#include "tools.h"
#include "interrupt.h"
#include "skills.h"
#include "strbuf.h"
#include "trust.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAX_TOOL_OUTPUT (256 * 1024)
#define MAX_READ_SIZE (512 * 1024)
#define MAX_EDIT_SIZE (16 * 1024 * 1024)
#define DISPLAY_MAX_LINES 10

static int g_non_interactive = 0;

static int ui_choose(const char *question, char *const *opts, int n);

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

static int trust_prompt(const char *path);

/*
 * Confine the file tools to the directories the user trusted. Paths are
 * canonicalised (resolving symlinks and "..") before the check so neither
 * relative escapes nor symlinks can reach outside a trusted tree; a path that
 * falls outside every one of them is offered to the user for approval first.
 */
static char *path_guard(const char *path)
{
    const char *root = trust_active_root();
    if (!root)
        return xstrdup("error: no trusted directory is active; refusing file access");
    if (trust_path_allowed(path) || trust_prompt(path))
        return NULL;

    char *msg = NULL;
    if (asprintf(&msg, "error: '%s' is outside the trusted directory '%s'; refusing access",
                 path ? path : ".", root) < 0)
        return xstrdup("error: path is outside the trusted directory");
    return msg;
}

/*
 * A tool asked for a path outside every trusted directory: instead of refusing
 * outright, offer to record the directory holding the target, the same way
 * lcode asks for the working directory at startup. Answering no leaves the tree
 * untrusted, so the access stays refused. Only a terminal can answer, which
 * keeps non-interactive runs from blocking on a prompt.
 */
static int trust_prompt(const char *path)
{
    if (g_non_interactive || !isatty(STDIN_FILENO))
        return 0;

    char *dir = trust_containing_dir(path);
    if (!dir)
        return 0;

    char *question = NULL, *yes = NULL, *no = NULL;
    int built = asprintf(&question,
                         "'%s' is outside the trusted directories. "
                         "Trust '%s' and continue?",
                         path ? path : ".", dir) >= 0 &&
                asprintf(&yes, "Yes, trust '%s'", dir) >= 0 &&
                asprintf(&no, "No, keep refusing access") >= 0;

    int trusted = 0;
    if (built) {
        char *opts[2] = {yes, no};
        if (ui_choose(question, opts, 2) == 0) {
            if (trust_add(dir) == 0) {
                printf("\033[1;33mtrusted directory: %s (saved to %s)\033[0m\n",
                       dir, trust_config_path());
                fflush(stdout);
                trusted = 1;
            } else {
                fprintf(stderr, "error: failed to save trusted directory '%s'\n", dir);
            }
        }
    }

    free(question);
    free(yes);
    free(no);
    free(dir);
    return trusted;
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

    char *guard = path_guard(path);
    if (guard)
        return guard;

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

    char *guard = path_guard(path);
    if (guard)
        return guard;

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

/*
 * Read a whole file for editing. read_stream() caps its result, which is fine
 * for read_file but not here: rewriting a truncated buffer would silently drop
 * the tail of the file. Files larger than MAX_EDIT_SIZE are refused on purpose.
 */
static char *read_file_all(const char *path, int *err_out)
{
    *err_out = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        *err_out = errno;
        return NULL;
    }

    struct stat st;
    if (fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode) &&
        st.st_size > MAX_EDIT_SIZE) {
        *err_out = EFBIG;
        fclose(f);
        return NULL;
    }

    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        *err_out = ENOMEM;
        fclose(f);
        return NULL;
    }

    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (len + 1 >= cap) {
            if (cap >= MAX_EDIT_SIZE) {
                free(buf);
                fclose(f);
                *err_out = EFBIG;
                return NULL;
            }
            size_t ncap = cap * 2;
            char *tmp = realloc(buf, ncap);
            if (!tmp) {
                free(buf);
                fclose(f);
                *err_out = ENOMEM;
                return NULL;
            }
            buf = tmp;
            cap = ncap;
        }
    }

    if (ferror(f)) {
        free(buf);
        fclose(f);
        *err_out = EIO;
        return NULL;
    }

    fclose(f);
    buf[len] = '\0';
    return buf;
}

/*
 * Replace an exact string in a file. A single unambiguous match is required
 * unless replace_all is set, so a wrong edit fails loudly instead of silently
 * changing an unintended occurrence. Prefer this over write_file for changing
 * part of an existing file.
 */
static char *tool_edit_file(json_object *args)
{
    const char *path = arg_str(args, "path");
    const char *old_str = arg_str(args, "old_string");
    const char *new_str = arg_str(args, "new_string");

    if (!path)
        return xstrdup("error: missing required argument 'path'");
    if (!old_str)
        return xstrdup("error: missing required argument 'old_string'");
    if (!new_str)
        return xstrdup("error: missing required argument 'new_string'");
    if (!*old_str)
        return xstrdup("error: 'old_string' must not be empty");
    if (strcmp(old_str, new_str) == 0)
        return xstrdup("error: 'old_string' and 'new_string' are identical");

    int replace_all = 0;
    json_object *v;
    if (args && json_object_object_get_ex(args, "replace_all", &v) &&
        json_object_get_type(v) == json_type_boolean)
        replace_all = json_object_get_boolean(v);

    char *guard = path_guard(path);
    if (guard)
        return guard;

    int err = 0;
    char *content = read_file_all(path, &err);
    if (!content) {
        char *msg = NULL;
        if (asprintf(&msg, "error: cannot read '%s' for editing: %s",
                     path, strerror(err)) < 0)
            return xstrdup("error: cannot read file for editing");
        return msg;
    }

    size_t old_len = strlen(old_str);
    size_t new_len = strlen(new_str);

    int count = 0;
    for (const char *p = content; (p = strstr(p, old_str)) != NULL; p += old_len)
        count++;

    if (count == 0) {
        free(content);
        char *msg = NULL;
        if (asprintf(&msg, "error: 'old_string' not found in '%s'", path) < 0)
            return xstrdup("error: 'old_string' not found");
        return msg;
    }
    if (count > 1 && !replace_all) {
        free(content);
        char *msg = NULL;
        if (asprintf(&msg,
                     "error: 'old_string' matches %d times in '%s'; add more "
                     "surrounding context to make it unique, or set replace_all=true",
                     count, path) < 0)
            return xstrdup("error: 'old_string' is not unique");
        return msg;
    }

    StrBuf out;
    if (sb_init(&out, strlen(content) + 64) != 0) {
        free(content);
        return xstrdup("error: out of memory");
    }

    int replacements = 0;
    const char *cur = content;
    for (;;) {
        const char *hit = strstr(cur, old_str);
        if (!hit) {
            sb_add(&out, cur);
            break;
        }
        sb_addn(&out, cur, (size_t)(hit - cur));
        sb_addn(&out, new_str, new_len);
        replacements++;
        cur = hit + old_len;
        if (!replace_all) {
            sb_add(&out, cur);
            break;
        }
    }
    free(content);

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(out.buf);
        char *msg = NULL;
        if (asprintf(&msg, "error: cannot open '%s' for writing: %s",
                     path, strerror(errno)) < 0)
            return xstrdup("error: cannot open file for writing");
        return msg;
    }

    size_t written = fwrite(out.buf, 1, out.len, f);
    int failed = ferror(f);
    fclose(f);
    free(out.buf);

    if (failed || written != out.len) {
        char *msg = NULL;
        if (asprintf(&msg, "error: failed writing to '%s'", path) < 0)
            return xstrdup("error: write failed");
        return msg;
    }

    char *msg = NULL;
    if (asprintf(&msg, "edited %s: replaced %d occurrence%s",
                 path, replacements, replacements == 1 ? "" : "s") < 0)
        return xstrdup("edited file");
    return msg;
}

static char *tool_list_dir(json_object *args)
{
    const char *path = arg_str(args, "path");
    if (!path)
        path = ".";

    char *guard = path_guard(path);
    if (guard)
        return guard;

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

/*
 * Shared state between the thread that drains the command's output pipe and
 * the main thread that streams it to the terminal. The buffer, the completion
 * flag and the wait status are guarded by the mutex.
 */
typedef struct {
    int fd;
    pid_t pid;
    StrBuf sb;
    pthread_mutex_t lock;
    int status;
    int exited;
    int done;
    int stop;
} ShellJob;

static void *shell_reader_thread(void *arg)
{
    ShellJob *job = arg;
    char buf[4096];

    for (;;) {
        pthread_mutex_lock(&job->lock);
        int stop = job->stop;
        pthread_mutex_unlock(&job->lock);
        if (stop)
            break;

        struct pollfd pfd;
        pfd.fd = job->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 100);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
            continue;

        ssize_t n = read(job->fd, buf, sizeof(buf));
        if (n > 0) {
            pthread_mutex_lock(&job->lock);
            if (job->sb.len < MAX_TOOL_OUTPUT) {
                size_t room = MAX_TOOL_OUTPUT - job->sb.len;
                sb_addn(&job->sb, buf, (size_t)n > room ? room : (size_t)n);
            }
            pthread_mutex_unlock(&job->lock);
        } else if (n == 0) {
            break;
        } else if (errno != EINTR) {
            break;
        }
    }

    int status = 0;
    waitpid(job->pid, &status, 0);

    pthread_mutex_lock(&job->lock);
    job->status = status;
    job->exited = 1;
    job->done = 1;
    pthread_mutex_unlock(&job->lock);
    return NULL;
}

typedef struct {
    size_t shown;
    int lines;
    int hidden;
    int spinner_visible;
} DisplayState;

/*
 * Print newly received command output. At most DISPLAY_MAX_LINES lines are
 * shown live; anything further is counted and summarised once the command
 * finishes. The full output is still kept in the buffer for the model.
 */
static void display_drain(ShellJob *job, DisplayState *ds, int final)
{
    pthread_mutex_lock(&job->lock);
    size_t avail = job->sb.len;
    size_t pos = ds->shown;

    while (pos < avail) {
        char *nl = memchr(job->sb.buf + pos, '\n', avail - pos);
        size_t line_end;
        if (nl)
            line_end = (size_t)(nl - job->sb.buf) + 1;
        else if (final)
            line_end = avail;
        else
            break;

        if (ds->lines < DISPLAY_MAX_LINES) {
            if (ds->spinner_visible) {
                fputs("\r\033[2K", stdout);
                ds->spinner_visible = 0;
            }
            fputs("\033[0;90m", stdout);
            fwrite(job->sb.buf + pos, 1, line_end - pos, stdout);
            if (job->sb.buf[line_end - 1] != '\n')
                fputc('\n', stdout);
            fputs("\033[0m", stdout);
            fflush(stdout);
            ds->lines++;
        } else {
            ds->hidden++;
        }
        pos = line_end;
    }

    ds->shown = pos;
    pthread_mutex_unlock(&job->lock);
}

/*
 * Kill the command's whole process group. The child is made its own group
 * leader, so this also reaches grandchildren (pipelines, make, ...) that would
 * otherwise survive, keep the output pipe open and hang the reader thread.
 */
static void shell_job_kill(pid_t pid)
{
    if (kill(-pid, SIGKILL) != 0)
        kill(pid, SIGKILL);
}

static int g_last_output_streamed = 0;

static volatile sig_atomic_t g_shell_signaled = 0;

static void shell_signal_handler(int sig)
{
    (void)sig;
    g_shell_signaled = 1;
}

int tools_output_streamed(void)
{
    return g_last_output_streamed;
}

static char *tool_run_shell(json_object *args)
{
    const char *command = arg_str(args, "command");
    if (!command)
        return xstrdup("error: missing required argument 'command'");

    if (!trust_active_root())
        return xstrdup("error: no trusted directory is active; refusing to run commands");

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
        setpgid(0, 0);
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    close(fds[1]);
    setpgid(pid, pid);

    ShellJob job;
    memset(&job, 0, sizeof(job));
    job.fd = fds[0];
    job.pid = pid;
    pthread_mutex_init(&job.lock, NULL);

    if (sb_init(&job.sb, 1024) != 0) {
        shell_job_kill(pid);
        close(fds[0]);
        waitpid(pid, NULL, 0);
        pthread_mutex_destroy(&job.lock);
        return xstrdup("error: out of memory");
    }

    interrupt_watch_begin();

    struct sigaction sa, old_int;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = shell_signal_handler;
    sigemptyset(&sa.sa_mask);
    g_shell_signaled = 0;
    sigaction(SIGINT, &sa, &old_int);

    int streamed = isatty(STDOUT_FILENO);
    static const char spinner[] = "|/-\\";
    int spinner_i = 0;

    pthread_t th;
    int have_thread = (pthread_create(&th, NULL, shell_reader_thread, &job) == 0);
    if (!have_thread)
        shell_reader_thread(&job);

    DisplayState ds;
    memset(&ds, 0, sizeof(ds));

    struct timespec t_start, t_last;
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    t_last = t_start;

    int interrupted = 0;
    for (;;) {
        if (streamed)
            display_drain(&job, &ds, 0);

        pthread_mutex_lock(&job.lock);
        int done = job.done;
        pthread_mutex_unlock(&job.lock);

        if (done)
            break;

        if (interrupt_poll() || g_shell_signaled) {
            interrupted = 1;
            pthread_mutex_lock(&job.lock);
            job.stop = 1;
            pthread_mutex_unlock(&job.lock);
            shell_job_kill(pid);
            interrupt_request();
            break;
        }

        if (streamed) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double since = (double)(now.tv_sec - t_last.tv_sec) +
                           (double)(now.tv_nsec - t_last.tv_nsec) / 1e9;
            if (since >= 0.1) {
                double secs = (double)(now.tv_sec - t_start.tv_sec) +
                              (double)(now.tv_nsec - t_start.tv_nsec) / 1e9;
                t_last = now;
                fprintf(stdout,
                        "\r\033[0;90m%c running command... %.1fs\033[0m",
                        spinner[spinner_i++ & 3], secs);
                fflush(stdout);
                ds.spinner_visible = 1;
            }
        }

        usleep(50000);
    }

    if (have_thread)
        pthread_join(th, NULL);

    if (streamed)
        display_drain(&job, &ds, 1);
    if (ds.spinner_visible)
        fputs("\r\033[2K", stdout);

    close(fds[0]);
    interrupt_watch_end();
    sigaction(SIGINT, &old_int, NULL);

    int status = job.exited ? job.status : 0;

    if (streamed) {
        if (interrupted)
            fputs("\033[1;33m[interrupted by user]\033[0m\n", stdout);
        if (job.exited && WIFEXITED(status))
            fprintf(stdout, "\033[0;90mexit_status: %d\033[0m\n",
                    WEXITSTATUS(status));
        else if (job.exited && WIFSIGNALED(status))
            fprintf(stdout, "\033[0;90mexit_status: terminated by signal %d\033[0m\n",
                    WTERMSIG(status));
        else if (job.exited)
            fputs("\033[0;90mexit_status: unknown\033[0m\n", stdout);
        if (ds.hidden > 0)
            fprintf(stdout, "\033[0;90m... (%d more lines hidden)\033[0m\n",
                    ds.hidden);
        fflush(stdout);
    }

    g_last_output_streamed = streamed;

    StrBuf result;
    if (sb_init(&result, 256) != 0) {
        free(job.sb.buf);
        pthread_mutex_destroy(&job.lock);
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

    if (job.sb.buf)
        sb_add(&result, job.sb.buf);

    free(job.sb.buf);
    pthread_mutex_destroy(&job.lock);
    return sb_steal(&result);
}

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

/*
 * Draw the menu starting at the current line. The cursor is left on the last
 * option line so that callers can move back up with "\033[<n>A" and redraw in
 * place; relying on save/restore cursor instead breaks as soon as the menu is
 * printed near the bottom and the terminal scrolls.
 */
static void render_menu(const char *question, char *const *opts, int n, int sel)
{
    printf("\r\033[2K\033[1;36m? %s\033[0m\n", question);
    for (int i = 0; i < n; i++) {
        printf("\r\033[2K");
        if (i == sel)
            printf("\033[1;36m> %d) %s\033[0m", i + 1, opts[i]);
        else
            printf("  %d) %s", i + 1, opts[i]);
        if (i != n - 1)
            printf("\n");
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

/*
 * Returns the selected index, -1 when the user cancels and -2 when the input
 * is not a valid choice.
 */
static int ask_interactive(const char *question, char *const *opts, int n)
{
    struct termios orig, raw;
    if (tcgetattr(STDIN_FILENO, &orig) != 0)
        return -1;

    raw = orig;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
        return -1;

    int sel = 0;
    int chosen = -1;

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
        printf("\033[%dA", n);
        render_menu(question, opts, n, sel);
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    printf("\033[%dA\r\033[J", n);
    fflush(stdout);

    return chosen;
}

static int ask_plain(const char *question, char *const *opts, int n)
{
    printf("? %s\n", question);
    for (int i = 0; i < n; i++)
        printf("  %d) %s\n", i + 1, opts[i]);
    printf("Enter choice [1-%d]: ", n);
    fflush(stdout);

    char line[64];
    if (!fgets(line, sizeof(line), stdin))
        return -1;

    int idx = atoi(line) - 1;
    if (idx < 0 || idx >= n)
        return -2;
    return idx;
}

static int ui_choose(const char *question, char *const *opts, int n)
{
    if (menu_is_tty())
        return ask_interactive(question, opts, n);
    return ask_plain(question, opts, n);
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

    int choice = ui_choose(question, opts, n);

    char *result;
    if (choice == -1)
        result = xstrdup("user cancelled the selection");
    else if (choice == -2)
        result = xstrdup("user did not make a valid selection");
    else
        result = selection_result(choice, opts);

    free(opts);
    return result;
}

char *tools_execute(const char *name, json_object *args)
{
    if (!name)
        return xstrdup("error: missing tool name");

    g_last_output_streamed = 0;

    if (strcmp(name, "read_file") == 0)
        return tool_read_file(args);
    if (strcmp(name, "ask_user") == 0)
        return tool_ask_user(args);
    if (strcmp(name, "write_file") == 0)
        return tool_write_file(args);
    if (strcmp(name, "edit_file") == 0)
        return tool_edit_file(args);
    if (strcmp(name, "list_dir") == 0)
        return tool_list_dir(args);
    if (strcmp(name, "run_shell") == 0)
        return tool_run_shell(args);
    if (strcmp(name, "skill") == 0)
        return skills_execute(arg_str(args, "name"));

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

    json_object *p2e = json_object_new_object();
    json_object_object_add(p2e, "path", prop("string", "Path to the file to edit"));
    json_object_object_add(p2e, "old_string",
                           prop("string", "Exact text to replace; must occur in the file"));
    json_object_object_add(p2e, "new_string",
                           prop("string", "Replacement text (may be empty to delete)"));
    json_object_object_add(p2e, "replace_all",
                           prop("boolean", "Replace every occurrence instead of requiring "
                                           "a unique match (default: false)"));
    json_object *r2e = json_object_new_array();
    json_object_array_add(r2e, json_object_new_string("path"));
    json_object_array_add(r2e, json_object_new_string("old_string"));
    json_object_array_add(r2e, json_object_new_string("new_string"));
    json_object_array_add(tools, make_tool(
        "edit_file",
        "Replace an exact string in a text file. By default 'old_string' must occur "
        "exactly once; set replace_all to replace every occurrence. Prefer this over "
        "write_file when changing part of an existing file.",
        p2e, r2e));

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

    json_object *p6 = json_object_new_object();
    json_object_object_add(p6, "name", prop("string", "Name of the skill to load"));
    json_object_array_add(tools, make_tool(
        "skill",
        "Load the full instructions of an available skill by name. Use this when a "
        "task matches one of the skills listed in the system prompt.",
        p6, required1("name")));

    return tools;
}
