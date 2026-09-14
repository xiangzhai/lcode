#include "tools.h"
#include "interrupt.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define MAX_TOOL_OUTPUT (256 * 1024)
#define MAX_READ_SIZE (512 * 1024)

static const char *arg_str(json_object *args, const char *key)
{
    json_object *v;
    if (!args || json_object_get_type(args) != json_type_object)
        return NULL;
    if (!json_object_object_get_ex(args, key, &v))
        return NULL;
    if (json_object_get_type(v) != json_type_string)
        return NULL;
    return json_object_get_string(v);
}

static char *xstrdup(const char *s)
{
    return s ? strdup(s) : NULL;
}

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} StrBuf;

static int sb_init(StrBuf *sb, size_t cap)
{
    sb->buf = malloc(cap > 0 ? cap : 1);
    if (!sb->buf)
        return -1;
    sb->buf[0] = '\0';
    sb->len = 0;
    sb->cap = cap > 0 ? cap : 1;
    return 0;
}

static int sb_vaddf(StrBuf *sb, const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (need < 0)
        return -1;

    if (sb->len + (size_t)need + 1 > sb->cap) {
        size_t ncap = sb->cap ? sb->cap : 1;
        while (sb->len + (size_t)need + 1 > ncap)
            ncap *= 2;
        char *tmp = realloc(sb->buf, ncap);
        if (!tmp)
            return -1;
        sb->buf = tmp;
        sb->cap = ncap;
    }
    vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
    sb->len += (size_t)need;
    return 0;
}

static int sb_addf(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int rc = sb_vaddf(sb, fmt, ap);
    va_end(ap);
    return rc;
}

static int sb_add(StrBuf *sb, const char *s)
{
    return sb_addf(sb, "%s", s);
}

static char *sb_steal(StrBuf *sb)
{
    char *b = sb->buf;
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
    return b;
}

static int sb_addn(StrBuf *sb, const char *data, size_t n)
{
    if (sb->len + n + 1 > sb->cap) {
        size_t ncap = sb->cap ? sb->cap : 1;
        while (sb->len + n + 1 > ncap)
            ncap *= 2;
        char *tmp = realloc(sb->buf, ncap);
        if (!tmp)
            return -1;
        sb->buf = tmp;
        sb->cap = ncap;
    }
    memcpy(sb->buf + sb->len, data, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
    return 0;
}

static char *read_stream(FILE *f)
{
    size_t cap = 8192;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf)
        return NULL;

    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (len + 1 >= cap) {
            if (cap >= MAX_TOOL_OUTPUT)
                break;
            size_t ncap = cap * 2;
            if (ncap > MAX_TOOL_OUTPUT + 1024)
                ncap = MAX_TOOL_OUTPUT + 1024;
            char *tmp = realloc(buf, ncap);
            if (!tmp)
                break;
            buf = tmp;
            cap = ncap;
        }
    }
    buf[len] = '\0';
    return buf;
}

static char *tool_read_file(json_object *args)
{
    const char *path = arg_str(args, "path");
    if (!path)
        return xstrdup("error: missing required argument 'path'");

    FILE *f = fopen(path, "rb");
    if (!f) {
        char *msg = NULL;
        if (asprintf(&msg, "error: cannot open '%s': %s", path, strerror(errno)) < 0)
            return xstrdup("error: cannot open file");
        return msg;
    }

    char *content = read_stream(f);
    fclose(f);
    if (!content)
        return xstrdup("error: out of memory while reading file");
    return content;
}

static char *tool_write_file(json_object *args)
{
    const char *path = arg_str(args, "path");
    const char *content = arg_str(args, "content");
    if (!path)
        return xstrdup("error: missing required argument 'path'");
    if (!content)
        return xstrdup("error: missing required argument 'content'");

    FILE *f = fopen(path, "wb");
    if (!f) {
        char *msg = NULL;
        if (asprintf(&msg, "error: cannot open '%s' for writing: %s", path, strerror(errno)) < 0)
            return xstrdup("error: cannot open file for writing");
        return msg;
    }

    size_t n = fwrite(content, 1, strlen(content), f);
    int failed = ferror(f);
    fclose(f);

    char *msg = NULL;
    if (failed) {
        if (asprintf(&msg, "error: failed writing to '%s'", path) < 0)
            return xstrdup("error: write failed");
        return msg;
    }
    if (asprintf(&msg, "wrote %zu bytes to %s", n, path) < 0)
        return xstrdup("wrote file");
    return msg;
}

static char *tool_list_dir(json_object *args)
{
    const char *path = arg_str(args, "path");
    if (!path)
        path = ".";

    DIR *d = opendir(path);
    if (!d) {
        char *msg = NULL;
        if (asprintf(&msg, "error: cannot open directory '%s': %s", path, strerror(errno)) < 0)
            return xstrdup("error: cannot open directory");
        return msg;
    }

    StrBuf sb;
    if (sb_init(&sb, 1024) != 0) {
        closedir(d);
        return xstrdup("error: out of memory");
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;

        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);

        struct stat st;
        char type = '?';
        if (lstat(full, &st) == 0) {
            if (S_ISDIR(st.st_mode))
                type = 'd';
            else if (S_ISLNK(st.st_mode))
                type = 'l';
            else if (S_ISREG(st.st_mode))
                type = 'f';
            sb_addf(&sb, "%c %10lld  %s\n", type, (long long)st.st_size, e->d_name);
        } else {
            sb_addf(&sb, "? %10s  %s\n", "-", e->d_name);
        }
    }
    closedir(d);
    return sb_steal(&sb);
}

static char *tool_run_shell(json_object *args)
{
    const char *command = arg_str(args, "command");
    if (!command)
        return xstrdup("error: missing required argument 'command'");

    int fds[2];
    if (pipe(fds) != 0) {
        char *msg = NULL;
        if (asprintf(&msg, "error: failed to create pipe: %s", strerror(errno)) < 0)
            return xstrdup("error: failed to create pipe");
        return msg;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        char *msg = NULL;
        if (asprintf(&msg, "error: failed to run command: %s", strerror(errno)) < 0)
            return xstrdup("error: failed to run command");
        return msg;
    }

    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    close(fds[1]);

    StrBuf sb;
    if (sb_init(&sb, 1024) != 0) {
        kill(pid, SIGKILL);
        close(fds[0]);
        waitpid(pid, NULL, 0);
        return xstrdup("error: out of memory");
    }

    interrupt_watch_begin();

    int interrupted = 0;
    char buf[4096];
    for (;;) {
        if (interrupt_poll()) {
            interrupted = 1;
            break;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fds[0], &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;

        int s = select(fds[0] + 1, &rfds, NULL, NULL, &tv);
        if (s < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (s == 0)
            continue;

        ssize_t n = read(fds[0], buf, sizeof(buf));
        if (n > 0) {
            if (sb.len < MAX_TOOL_OUTPUT) {
                size_t room = MAX_TOOL_OUTPUT - sb.len;
                sb_addn(&sb, buf, (size_t)n > room ? room : (size_t)n);
            }
        } else if (n == 0) {
            break;
        } else if (errno != EINTR && errno != EAGAIN) {
            break;
        }
    }

    if (interrupted)
        kill(pid, SIGKILL);

    close(fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    interrupt_watch_end();

    StrBuf result;
    if (sb_init(&result, 256) != 0) {
        free(sb.buf);
        return xstrdup("error: out of memory");
    }

    if (interrupted)
        sb_add(&result, "[interrupted by user]\n");

    if (WIFEXITED(status))
        sb_addf(&result, "exit_status: %d\n", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        sb_addf(&result, "exit_status: terminated by signal %d\n", WTERMSIG(status));
    else
        sb_add(&result, "exit_status: unknown\n");

    if (sb.buf)
        sb_add(&result, sb.buf);
    free(sb.buf);
    return sb_steal(&result);
}

static int g_non_interactive = 0;

void tools_set_non_interactive(int enabled)
{
    g_non_interactive = enabled ? 1 : 0;
}

static int menu_is_tty(void)
{
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
}

static int wait_input(int ms)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);

    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
}

enum {
    KEY_UP = 0,
    KEY_DOWN,
    KEY_ENTER,
    KEY_CANCEL,
    KEY_OTHER,
    KEY_DIGIT = 100,
};

static int read_key(void)
{
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1)
        return KEY_CANCEL;

    if (c == 0x1b) {
        if (wait_input(30) <= 0)
            return KEY_CANCEL;
        unsigned char c2;
        if (read(STDIN_FILENO, &c2, 1) != 1)
            return KEY_CANCEL;
        if (c2 == '[' || c2 == 'O') {
            if (wait_input(30) <= 0)
                return KEY_CANCEL;
            unsigned char c3;
            if (read(STDIN_FILENO, &c3, 1) != 1)
                return KEY_CANCEL;
            if (c3 == 'A')
                return KEY_UP;
            if (c3 == 'B')
                return KEY_DOWN;
            return KEY_OTHER;
        }
        return KEY_CANCEL;
    }

    if (c == '\r' || c == '\n')
        return KEY_ENTER;
    if (c == 0x03 || c == 0x04 || c == 'q' || c == 'Q')
        return KEY_CANCEL;
    if (c >= '1' && c <= '9')
        return KEY_DIGIT + (c - '0');
    if (c == 'j')
        return KEY_DOWN;
    if (c == 'k')
        return KEY_UP;
    return KEY_OTHER;
}

static void render_menu(const char *question, char *const *opts, int n, int sel)
{
    printf("\r\033[2K\033[1;36m? %s\033[0m\n", question);
    for (int i = 0; i < n; i++) {
        printf("\033[2K");
        if (i == sel)
            printf("\033[1;36m> %d) %s\033[0m\n", i + 1, opts[i]);
        else
            printf("  %d) %s\n", i + 1, opts[i]);
    }
    fflush(stdout);
}

static char *selection_result(int idx, char *const *opts)
{
    char *msg = NULL;
    if (asprintf(&msg, "user selected option %d: %s", idx + 1, opts[idx]) < 0)
        return xstrdup("user made a selection");
    return msg;
}

static char *ask_interactive(const char *question, char *const *opts, int n)
{
    struct termios orig, raw;
    if (tcgetattr(STDIN_FILENO, &orig) != 0)
        return NULL;

    raw = orig;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
        return NULL;

    int sel = 0;
    int chosen = -1;

    printf("\0337");
    render_menu(question, opts, n, sel);

    while (1) {
        int k = read_key();
        if (k == KEY_UP) {
            sel = (sel + n - 1) % n;
        } else if (k == KEY_DOWN) {
            sel = (sel + 1) % n;
        } else if (k >= KEY_DIGIT + 1 && k <= KEY_DIGIT + n) {
            chosen = k - KEY_DIGIT - 1;
            break;
        } else if (k == KEY_ENTER) {
            chosen = sel;
            break;
        } else if (k == KEY_CANCEL) {
            break;
        } else {
            continue;
        }
        printf("\0338\033[J");
        render_menu(question, opts, n, sel);
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    printf("\0338\033[J");
    fflush(stdout);

    if (chosen < 0)
        return xstrdup("user cancelled the selection");
    return selection_result(chosen, opts);
}

static char *ask_plain(const char *question, char *const *opts, int n)
{
    printf("? %s\n", question);
    for (int i = 0; i < n; i++)
        printf("  %d) %s\n", i + 1, opts[i]);
    printf("Enter choice [1-%d]: ", n);
    fflush(stdout);

    char line[64];
    if (!fgets(line, sizeof(line), stdin))
        return xstrdup("user cancelled the selection");

    int idx = atoi(line) - 1;
    if (idx < 0 || idx >= n)
        return xstrdup("user did not make a valid selection");
    return selection_result(idx, opts);
}

static char *tool_ask_user(json_object *args)
{
    if (g_non_interactive)
        return xstrdup("non-interactive mode is enabled: the user has delegated this "
                       "decision to you. Pick the most reasonable option yourself and "
                       "continue the task without asking again.");

    const char *question = arg_str(args, "question");
    if (!question)
        return xstrdup("error: missing required argument 'question'");

    json_object *opts_json = NULL;
    if (!args || !json_object_object_get_ex(args, "options", &opts_json) ||
        !opts_json || json_object_get_type(opts_json) != json_type_array)
        return xstrdup("error: missing required argument 'options'");

    int n = (int)json_object_array_length(opts_json);
    if (n <= 0)
        return xstrdup("error: 'options' must contain at least one choice");

    char **opts = calloc((size_t)n, sizeof(char *));
    if (!opts)
        return xstrdup("error: out of memory");

    for (int i = 0; i < n; i++) {
        json_object *o = json_object_array_get_idx(opts_json, i);
        if (json_object_get_type(o) != json_type_string) {
            free(opts);
            return xstrdup("error: each option must be a string");
        }
        opts[i] = (char *)json_object_get_string(o);
    }

    char *result = NULL;
    if (menu_is_tty())
        result = ask_interactive(question, opts, n);
    if (!result)
        result = ask_plain(question, opts, n);

    free(opts);
    return result;
}

char *tools_execute(const char *name, json_object *args)
{
    if (!name)
        return xstrdup("error: missing tool name");

    if (strcmp(name, "read_file") == 0)
        return tool_read_file(args);
    if (strcmp(name, "ask_user") == 0)
        return tool_ask_user(args);
    if (strcmp(name, "write_file") == 0)
        return tool_write_file(args);
    if (strcmp(name, "list_dir") == 0)
        return tool_list_dir(args);
    if (strcmp(name, "run_shell") == 0)
        return tool_run_shell(args);

    char *msg = NULL;
    if (asprintf(&msg, "error: unknown tool '%s'", name) < 0)
        return xstrdup("error: unknown tool");
    return msg;
}

static json_object *prop(const char *type, const char *desc)
{
    json_object *p = json_object_new_object();
    json_object_object_add(p, "type", json_object_new_string(type));
    json_object_object_add(p, "description", json_object_new_string(desc));
    return p;
}

static json_object *make_tool(const char *name, const char *desc,
                              json_object *props, json_object *required)
{
    json_object *tool = json_object_new_object();
    json_object_object_add(tool, "type", json_object_new_string("function"));

    json_object *fn = json_object_new_object();
    json_object_object_add(fn, "name", json_object_new_string(name));
    json_object_object_add(fn, "description", json_object_new_string(desc));

    json_object *params = json_object_new_object();
    json_object_object_add(params, "type", json_object_new_string("object"));
    json_object_object_add(params, "properties", props);
    json_object_object_add(params, "required", required);

    json_object_object_add(fn, "parameters", params);
    json_object_object_add(tool, "function", fn);
    return tool;
}

static json_object *required1(const char *a)
{
    json_object *req = json_object_new_array();
    if (a)
        json_object_array_add(req, json_object_new_string(a));
    return req;
}

json_object *tools_schema(void)
{
    json_object *tools = json_object_new_array();

    json_object *p1 = json_object_new_object();
    json_object_object_add(p1, "path", prop("string", "Path to the file to read"));
    json_object_array_add(tools, make_tool(
        "read_file",
        "Read the contents of a text file at the given path.",
        p1, required1("path")));

    json_object *p2 = json_object_new_object();
    json_object_object_add(p2, "path", prop("string", "Path to the file to write"));
    json_object_object_add(p2, "content", prop("string", "Full content to write to the file"));
    json_object *r2 = json_object_new_array();
    json_object_array_add(r2, json_object_new_string("path"));
    json_object_array_add(r2, json_object_new_string("content"));
    json_object_array_add(tools, make_tool(
        "write_file",
        "Write (overwrite) a text file with the given content.",
        p2, r2));

    json_object *p3 = json_object_new_object();
    json_object_object_add(p3, "path", prop("string", "Directory path, defaults to current directory"));
    json_object_array_add(tools, make_tool(
        "list_dir",
        "List entries in a directory with type and size.",
        p3, required1(NULL)));

    json_object *p4 = json_object_new_object();
    json_object_object_add(p4, "command", prop("string", "Shell command to execute"));
    json_object_array_add(tools, make_tool(
        "run_shell",
        "Run a shell command and return its combined output and exit status.",
        p4, required1("command")));

    json_object *p5 = json_object_new_object();
    json_object_object_add(p5, "question", prop("string", "The question to ask the user"));
    json_object *opts_prop = json_object_new_object();
    json_object_object_add(opts_prop, "type", json_object_new_string("array"));
    json_object_object_add(opts_prop, "description",
                           json_object_new_string("The choices to present to the user"));
    json_object *items = json_object_new_object();
    json_object_object_add(items, "type", json_object_new_string("string"));
    json_object_object_add(opts_prop, "items", items);
    json_object_object_add(p5, "options", opts_prop);
    json_object *r5 = json_object_new_array();
    json_object_array_add(r5, json_object_new_string("question"));
    json_object_array_add(r5, json_object_new_string("options"));
    json_object_array_add(tools, make_tool(
        "ask_user",
        "Ask the user to choose one option from a list using an interactive menu. "
        "Use this whenever a decision needs the user's input before proceeding.",
        p5, r5));

    return tools;
}
