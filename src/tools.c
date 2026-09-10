#include "tools.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>

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

    FILE *p = popen(command, "r");
    if (!p) {
        char *msg = NULL;
        if (asprintf(&msg, "error: failed to run command: %s", strerror(errno)) < 0)
            return xstrdup("error: failed to run command");
        return msg;
    }

    char *out = read_stream(p);
    int status = pclose(p);

    StrBuf sb;
    if (sb_init(&sb, 256) != 0) {
        free(out);
        return xstrdup("error: out of memory");
    }

    if (WIFEXITED(status))
        sb_addf(&sb, "exit_status: %d\n", WEXITSTATUS(status));
    else
        sb_addf(&sb, "exit_status: terminated by signal %d\n", WTERMSIG(status));

    if (out)
        sb_add(&sb, out);
    free(out);
    return sb_steal(&sb);
}

char *tools_execute(const char *name, json_object *args)
{
    if (!name)
        return xstrdup("error: missing tool name");

    if (strcmp(name, "read_file") == 0)
        return tool_read_file(args);
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

    return tools;
}
