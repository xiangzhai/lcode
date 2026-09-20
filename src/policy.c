#include "policy.h"
#include "strbuf.h"
#include "trust.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <json-c/json.h>

/*
 * Command categories. "deny" categories are refused outright (they are almost
 * always catastrophic and have no place in a coding agent); the rest are
 * confirmed interactively and can be remembered so they stop asking. The desc
 * is shown to the user and is what a remembered category rule matches.
 */
typedef struct {
    const char *key;
    const char *desc;
    int deny;
} PolicyCategory;

static const PolicyCategory CATEGORIES[] = {
    { "fork-bomb",         "run a fork bomb", 1 },
    { "system-power",      "shut down or reboot the machine", 1 },
    { "delete-root",       "delete the filesystem root or a critical system path", 1 },
    { "system-write",      "overwrite a protected system file or device", 1 },
    { "disk-format",       "format a filesystem or repartition a disk", 1 },
    { "device-write",      "write to a raw block device", 1 },
    { "recursive-delete",  "recursively delete files (rm -r)", 0 },
    { "disk-write",        "overwrite a file or disk with raw data (dd/shred)", 0 },
    { "pipe-to-shell",     "pipe a command's output into a shell interpreter", 0 },
    { "privilege",         "run a command with elevated privileges", 0 },
    { "permission-change", "change permissions or ownership recursively", 0 },
    { "git-destructive",   "perform a destructive git operation", 0 },
    { "eval",              "eval an arbitrary string as code", 0 },
    { "outside-write",     "write to a path outside the trusted directories", 0 },
    { NULL, NULL, 0 },
};

static const PolicyCategory *cat_find(const char *key)
{
    if (!key)
        return NULL;
    for (int i = 0; CATEGORIES[i].key; i++)
        if (strcmp(CATEGORIES[i].key, key) == 0)
            return &CATEGORIES[i];
    return NULL;
}

static int cat_deny(const char *key)
{
    const PolicyCategory *c = cat_find(key);
    return c ? c->deny : 0;
}

const char *policy_category_desc(const char *key)
{
    const PolicyCategory *c = cat_find(key);
    return c ? c->desc : NULL;
}

/* Keep the most severe finding: a deny always wins over a confirm. */
static void note(const char **cat, const char *cand)
{
    if (!cand)
        return;
    if (!*cat || (cat_deny(cand) && !cat_deny(*cat)))
        *cat = cand;
}

/* ------------------------------------------------------------------ */
/* Command tokenizer                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    char **argv;
    int argc;
    int piped_in; /* this segment follows a '|' operator */
} Segment;

typedef struct {
    Segment *seg;
    int n;
    int cap;
} SegmentList;

static void seg_add_arg(Segment *s, const char *word)
{
    char **tmp = realloc(s->argv, (size_t)(s->argc + 2) * sizeof(char *));
    if (!tmp)
        return;
    s->argv = tmp;
    s->argv[s->argc] = strdup(word ? word : "");
    if (!s->argv[s->argc])
        return;
    s->argc++;
    s->argv[s->argc] = NULL;
}

static void sl_add(SegmentList *sl, Segment seg)
{
    if (sl->n == sl->cap) {
        int ncap = sl->cap ? sl->cap * 2 : 8;
        Segment *tmp = realloc(sl->seg, (size_t)ncap * sizeof(Segment));
        if (!tmp)
            return;
        sl->seg = tmp;
        sl->cap = ncap;
    }
    sl->seg[sl->n++] = seg;
}

static void sl_free(SegmentList *sl)
{
    if (!sl)
        return;
    for (int i = 0; i < sl->n; i++) {
        for (int j = 0; j < sl->seg[i].argc; j++)
            free(sl->seg[i].argv[j]);
        free(sl->seg[i].argv);
    }
    free(sl->seg);
    free(sl);
}

/*
 * Split a command line into pipeline segments on the shell operators
 * |, &, ;, newline, ( ) and backtick, honouring single/double quotes so that
 * quoted text is not mistaken for syntax. Quotes are stripped from the words.
 * This is deliberately simple: it only needs to expose the individual commands
 * for the risk checks below, not to be a faithful shell parser.
 */
static SegmentList *split_command(const char *cmd)
{
    SegmentList *sl = calloc(1, sizeof(*sl));
    if (!sl)
        return NULL;

    StrBuf tok;
    if (sb_init(&tok, 64) != 0) {
        free(sl);
        return NULL;
    }

    int has_tok = 0, in_single = 0, in_double = 0;
    Segment cur;
    memset(&cur, 0, sizeof(cur));

#define FLUSH_TOKEN()                 \
    do {                              \
        if (has_tok) {                \
            seg_add_arg(&cur, tok.buf); \
            tok.len = 0;              \
            tok.buf[0] = '\0';        \
            has_tok = 0;              \
        }                             \
    } while (0)

    for (const char *p = cmd;; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\0') {
            FLUSH_TOKEN();
            sl_add(sl, cur);
            break;
        }
        if (in_single) {
            if (c == '\'')
                in_single = 0;
            else {
                sb_addn(&tok, (const char *)&c, 1);
                has_tok = 1;
            }
            continue;
        }
        if (in_double) {
            if (c == '"')
                in_double = 0;
            else {
                sb_addn(&tok, (const char *)&c, 1);
                has_tok = 1;
            }
            continue;
        }
        if (c == '\\') {
            if (p[1]) {
                p++;
                sb_addn(&tok, p, 1);
                has_tok = 1;
            }
            continue;
        }
        if (c == '\'') {
            in_single = 1;
            has_tok = 1;
            continue;
        }
        if (c == '"') {
            in_double = 1;
            has_tok = 1;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            FLUSH_TOKEN();
            continue;
        }
        if (c == '\n' || c == '|' || c == '&' || c == ';' ||
            c == '(' || c == ')' || c == '`') {
            FLUSH_TOKEN();
            sl_add(sl, cur);
            memset(&cur, 0, sizeof(cur));
            cur.piped_in = (c == '|') ? 1 : 0;
            continue;
        }
        sb_addn(&tok, (const char *)&c, 1);
        has_tok = 1;
    }
#undef FLUSH_TOKEN

    sb_free(&tok);
    return sl;
}

/* ------------------------------------------------------------------ */
/* Risk classification                                                 */
/* ------------------------------------------------------------------ */

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int in_list(const char *s, const char *const *list)
{
    for (int i = 0; list[i]; i++)
        if (strcmp(s, list[i]) == 0)
            return 1;
    return 0;
}

static const char *SHELLS[] = {
    "sh", "bash", "zsh", "dash", "ksh", "ash", "fish", "csh", "tcsh", NULL
};

static const char *SCRIPT_INTERPRETERS[] = {
    "python", "python2", "python3", "perl", "ruby", "node", "nodejs",
    "php", "lua", "tclsh", "wish", "Rscript", NULL
};

static const char *SUDO_PROGS[] = { "sudo", "doas", "pkexec", "su", NULL };

static const char *SIMPLE_WRAPPERS[] = {
    "command", "builtin", "exec", "setsid", "nohup", "time", "nice",
    "ionice", "stdbuf", "xargs", NULL
};

static const char *POWER_PROGS[] = {
    "shutdown", "reboot", "halt", "poweroff", "telinit", NULL
};

static const char *FORMAT_PROGS[] = {
    "mkswap", "fdisk", "gdisk", "sfdisk", "parted", "partx", "cfdisk", NULL
};

static const char *DEVICE_PROGS[] = { "blkdiscard", "wipefs", NULL };

static const char *SYS_DIRS[] = {
    "etc", "usr", "bin", "sbin", "boot", "lib", "lib64", "lib32",
    "var", "dev", "proc", "sys", "root", "opt", "srv", NULL
};

static const char *SAFE_DEVICES[] = {
    "null", "zero", "stdout", "stderr", "stdin", "tty", "full",
    "random", "urandom", NULL
};

static int is_interpreter(const char *p)
{
    return in_list(p, SHELLS) || in_list(p, SCRIPT_INTERPRETERS);
}

static int short_flag_has(const char *a, char ch)
{
    if (a[0] != '-' || a[1] == '\0' || a[1] == '-')
        return 0;
    for (const char *p = a + 1; *p; p++)
        if (*p == ch)
            return 1;
    return 0;
}

/* A wrapper flag or a duration/niceness argument such as 5, 2.5s, 10m. */
static int is_wrapper_arg(const char *a)
{
    if (a[0] == '-')
        return 1;
    const char *p = a;
    int digits = 0;
    while (*p && (isdigit((unsigned char)*p) || *p == '.')) {
        p++;
        digits++;
    }
    if (digits == 0)
        return 0;
    if (*p == '\0')
        return 1;
    return p[1] == '\0' && strchr("smhd", *p) != NULL;
}

static int is_safe_device(const char *t)
{
    if (!t || strncmp(t, "/dev/", 5) != 0)
        return 0;
    char rest[64];
    size_t n = 0;
    for (const char *p = t + 5; *p && *p != '/' && n + 1 < sizeof(rest); p++)
        rest[n++] = *p;
    rest[n] = '\0';
    return in_list(rest, SAFE_DEVICES);
}

/*
 * True for the filesystem root, a home directory, or a system directory that
 * no coding task should be deleting or overwriting.
 */
static int is_protected_path(const char *t)
{
    if (!t || !*t)
        return 0;

    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", t);
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = '\0';

    if (strcmp(tmp, "/") == 0 || strcmp(tmp, "/*") == 0)
        return 1;
    if (strcmp(tmp, "~") == 0 || strcmp(tmp, "$HOME") == 0 ||
        strcmp(tmp, "${HOME}") == 0)
        return 1;
    if (tmp[0] != '/')
        return 0;

    char comp[64];
    size_t ci = 0;
    const char *p = tmp + 1;
    while (*p && *p != '/' && ci + 1 < sizeof(comp))
        comp[ci++] = *p++;
    comp[ci] = '\0';
    const char *rest = p;

    if (strcmp(comp, "dev") == 0 && is_safe_device(tmp))
        return 0;
    if (in_list(comp, SYS_DIRS))
        return 1;

    if (strcmp(comp, "home") == 0) {
        if (*rest == '\0')
            return 1;
        const char *q = rest + 1; /* skip '/' */
        return *q == '\0' || strchr(q, '/') == NULL;
    }
    if (strcmp(comp, "mnt") == 0 || strcmp(comp, "media") == 0)
        return *rest == '\0';
    return 0;
}

static int is_system_power(const char *prog, int argc, char **argv)
{
    if (in_list(prog, POWER_PROGS))
        return 1;
    if (strcmp(prog, "init") == 0)
        for (int j = 1; j < argc; j++)
            if (strcmp(argv[j], "0") == 0 || strcmp(argv[j], "6") == 0)
                return 1;
    return 0;
}

static int looks_like_fork_bomb(const char *cmd)
{
    char buf[512];
    size_t n = 0;
    for (const char *p = cmd; *p && n + 1 < sizeof(buf); p++)
        if (!isspace((unsigned char)*p))
            buf[n++] = *p;
    buf[n] = '\0';
    return strstr(buf, "(){:|:&") != NULL;
}

/*
 * Peel off wrappers (sudo, env, command, timeout, xargs, ...) so the checks
 * below see the command that will actually run. Sets *priv when an elevated
 * wrapper is present and returns 1 when a command remains.
 */
static int find_effective(const Segment *seg, int *start, int *priv)
{
    int i = 0;
    *priv = 0;
    for (int guard = 0; guard < 16 && i < seg->argc; guard++) {
        const char *p = base_name(seg->argv[i]);
        if (in_list(p, SUDO_PROGS)) {
            *priv = 1;
            i++;
            while (i < seg->argc && seg->argv[i][0] == '-') {
                const char *f = seg->argv[i];
                int takes = strcmp(f, "-u") == 0 || strcmp(f, "--user") == 0 ||
                            strcmp(f, "-g") == 0 || strcmp(f, "--group") == 0 ||
                            strcmp(f, "-p") == 0 || strcmp(f, "--prompt") == 0 ||
                            strcmp(f, "-C") == 0 || strcmp(f, "--chdir") == 0 ||
                            strcmp(f, "-h") == 0 || strcmp(f, "--host") == 0 ||
                            strcmp(f, "-t") == 0 || strcmp(f, "--type") == 0;
                i++;
                if (takes && i < seg->argc)
                    i++;
            }
            continue;
        }
        if (strcmp(p, "env") == 0) {
            i++;
            while (i < seg->argc) {
                const char *a = seg->argv[i];
                if (a[0] == '-' || strchr(a, '=')) {
                    i++;
                    continue;
                }
                break;
            }
            continue;
        }
        if (strcmp(p, "timeout") == 0) {
            i++;
            while (i < seg->argc && seg->argv[i][0] == '-')
                i++;
            if (i < seg->argc)
                i++; /* duration */
            continue;
        }
        if (in_list(p, SIMPLE_WRAPPERS)) {
            i++;
            while (i < seg->argc && is_wrapper_arg(seg->argv[i]))
                i++;
            continue;
        }
        break;
    }
    *start = i;
    return i < seg->argc;
}

static void classify_git(const char **cat, int argc, char **argv)
{
    int si = -1;
    for (int j = 1; j < argc; j++)
        if (argv[j][0] != '-') {
            si = j;
            break;
        }
    if (si < 0)
        return;

    const char *sub = argv[si];
    if (strcmp(sub, "push") == 0) {
        for (int j = si + 1; j < argc; j++)
            if (strcmp(argv[j], "--force") == 0 || strcmp(argv[j], "-f") == 0 ||
                strcmp(argv[j], "--force-with-lease") == 0 ||
                strcmp(argv[j], "--force-if-includes") == 0)
                note(cat, "git-destructive");
    } else if (strcmp(sub, "reset") == 0) {
        for (int j = si + 1; j < argc; j++)
            if (strcmp(argv[j], "--hard") == 0)
                note(cat, "git-destructive");
    } else if (strcmp(sub, "clean") == 0) {
        int dry = 0, force = 0;
        for (int j = si + 1; j < argc; j++) {
            const char *a = argv[j];
            if (a[0] != '-')
                continue;
            if (strcmp(a, "--dry-run") == 0 || short_flag_has(a, 'n'))
                dry = 1;
            if (strcmp(a, "--force") == 0 || short_flag_has(a, 'f'))
                force = 1;
        }
        if (force && !dry)
            note(cat, "git-destructive");
    }
}

static const char *classify_command(const char *cmd, int depth);

static const char *classify_segment(const Segment *seg, int piped_in, int depth)
{
    if (seg->argc == 0)
        return NULL;

    int start = 0, priv = 0;
    if (!find_effective(seg, &start, &priv))
        return priv ? "privilege" : NULL;

    int argc = seg->argc - start;
    char **argv = seg->argv + start;
    const char *prog = base_name(argv[0]);
    const char *cat = NULL;

    if (priv)
        note(&cat, "privilege");
    if (piped_in && is_interpreter(prog))
        note(&cat, "pipe-to-shell");
    if (is_system_power(prog, argc, argv))
        note(&cat, "system-power");

    if (in_list(prog, SHELLS)) {
        for (int j = 1; j < argc; j++)
            if (strcmp(argv[j], "-c") == 0 && j + 1 < argc) {
                note(&cat, classify_command(argv[j + 1], depth + 1));
                break;
            }
    } else if (strcmp(prog, "rm") == 0) {
        int recursive = 0, prot = 0;
        for (int j = 1; j < argc; j++) {
            const char *a = argv[j];
            if (a[0] == '-' && a[1] != '\0') {
                if (strcmp(a, "--recursive") == 0 || short_flag_has(a, 'r') ||
                    short_flag_has(a, 'R'))
                    recursive = 1;
                continue;
            }
            if (is_protected_path(a))
                prot = 1;
        }
        if (prot)
            note(&cat, "delete-root");
        else if (recursive)
            note(&cat, "recursive-delete");
    } else if (strcmp(prog, "rmdir") == 0) {
        for (int j = 1; j < argc; j++)
            if (argv[j][0] != '-' && is_protected_path(argv[j]))
                note(&cat, "delete-root");
    } else if (strcmp(prog, "dd") == 0) {
        for (int j = 1; j < argc; j++)
            if (strncmp(argv[j], "of=", 3) == 0) {
                const char *t = argv[j] + 3;
                if (strncmp(t, "/dev/", 5) == 0 && !is_safe_device(t))
                    note(&cat, "device-write");
                else
                    note(&cat, "disk-write");
            }
    } else if (strncmp(prog, "mkfs", 4) == 0 || in_list(prog, FORMAT_PROGS)) {
        note(&cat, "disk-format");
    } else if (in_list(prog, DEVICE_PROGS)) {
        note(&cat, "device-write");
    } else if (strcmp(prog, "shred") == 0) {
        note(&cat, "disk-write");
    } else if (strcmp(prog, "chmod") == 0 || strcmp(prog, "chown") == 0 ||
               strcmp(prog, "chgrp") == 0) {
        int recursive = 0, prot = 0;
        for (int j = 1; j < argc; j++) {
            const char *a = argv[j];
            if (a[0] == '-' && a[1] != '\0') {
                if (strcmp(a, "--recursive") == 0 || short_flag_has(a, 'R'))
                    recursive = 1;
                continue;
            }
            if (is_protected_path(a))
                prot = 1;
        }
        if (prot)
            note(&cat, "system-write");
        else if (recursive)
            note(&cat, "permission-change");
    } else if (strcmp(prog, "git") == 0) {
        classify_git(&cat, argc, argv);
    } else if (strcmp(prog, "find") == 0) {
        for (int j = 1; j < argc; j++) {
            if (strcmp(argv[j], "-delete") == 0)
                note(&cat, "recursive-delete");
            else if (strcmp(argv[j], "-exec") == 0 && j + 1 < argc) {
                const char *inner = base_name(argv[j + 1]);
                if (strcmp(inner, "rm") == 0 || strcmp(inner, "rmdir") == 0 ||
                    strcmp(inner, "shred") == 0)
                    note(&cat, "recursive-delete");
            }
        }
    } else if (strcmp(prog, "eval") == 0) {
        note(&cat, "eval");
    }

    /* redirections that leave the trusted tree or hit a system path */
    for (int j = 0; j < argc; j++) {
        const char *gt = strchr(argv[j], '>');
        if (!gt)
            continue;
        const char *t = gt;
        while (*t == '>')
            t++;
        const char *target = *t ? t : (j + 1 < argc ? argv[j + 1] : NULL);
        if (!target || !*target || is_safe_device(target))
            continue;
        if (is_protected_path(target))
            note(&cat, "system-write");
        else if (!trust_path_allowed(target))
            note(&cat, "outside-write");
    }

    return cat;
}

static const char *classify_command(const char *cmd, int depth)
{
    if (!cmd || !*cmd || depth > 6)
        return NULL;
    if (looks_like_fork_bomb(cmd))
        return "fork-bomb";

    SegmentList *sl = split_command(cmd);
    if (!sl)
        return NULL;

    const char *cat = NULL;
    for (int i = 0; i < sl->n; i++)
        note(&cat, classify_segment(&sl->seg[i], sl->seg[i].piped_in, depth));
    sl_free(sl);
    return cat;
}

/* ------------------------------------------------------------------ */
/* Remembered rules                                                    */
/* ------------------------------------------------------------------ */

static char **g_cmds = NULL;
static int g_cmds_n = 0;
static char **g_cats = NULL;
static int g_cats_n = 0;
static int g_allow_all = 0;

const char *policy_config_path(void)
{
    static char path[4096];
    static int ready = 0;
    if (!ready) {
        const char *dir = trust_config_dir();
        const char *suffix = "/permissions.json";
        size_t n = strlen(dir);
        if (n > sizeof(path) - strlen(suffix) - 1)
            n = sizeof(path) - strlen(suffix) - 1;
        memcpy(path, dir, n);
        strcpy(path + n, suffix);
        ready = 1;
    }
    return path;
}

static void rules_clear(void)
{
    for (int i = 0; i < g_cmds_n; i++)
        free(g_cmds[i]);
    free(g_cmds);
    g_cmds = NULL;
    g_cmds_n = 0;

    for (int i = 0; i < g_cats_n; i++)
        free(g_cats[i]);
    free(g_cats);
    g_cats = NULL;
    g_cats_n = 0;
}

static int list_add(char ***list, int *n, const char *s)
{
    char **tmp = realloc(*list, (size_t)(*n + 1) * sizeof(char *));
    if (!tmp)
        return -1;
    *list = tmp;
    char *copy = strdup(s);
    if (!copy)
        return -1;
    (*list)[(*n)++] = copy;
    return 0;
}

static int list_has(char *const *list, int n, const char *s)
{
    for (int i = 0; i < n; i++)
        if (strcmp(list[i], s) == 0)
            return 1;
    return 0;
}

static int mkdir_p(const char *path)
{
    char tmp[4096];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
        return -1;

    size_t len = strlen(tmp);
    if (len == 0)
        return -1;
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int ensure_parent_dir(const char *file)
{
    char tmp[4096];
    if (snprintf(tmp, sizeof(tmp), "%s", file) >= (int)sizeof(tmp))
        return -1;
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp)
        return 0;
    *slash = '\0';
    return mkdir_p(tmp);
}

int policy_load(void)
{
    rules_clear();

    const char *path = policy_config_path();
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;

    json_object *root = json_object_from_file(path);
    if (!root)
        return 0;

    json_object *arr;
    if (json_object_object_get_ex(root, "commands", &arr) &&
        json_object_get_type(arr) == json_type_array)
        for (int i = 0; i < (int)json_object_array_length(arr); i++) {
            json_object *e = json_object_array_get_idx(arr, i);
            if (json_object_get_type(e) == json_type_string)
                list_add(&g_cmds, &g_cmds_n, json_object_get_string(e));
        }

    if (json_object_object_get_ex(root, "categories", &arr) &&
        json_object_get_type(arr) == json_type_array)
        for (int i = 0; i < (int)json_object_array_length(arr); i++) {
            json_object *e = json_object_array_get_idx(arr, i);
            if (json_object_get_type(e) == json_type_string)
                list_add(&g_cats, &g_cats_n, json_object_get_string(e));
        }

    json_object_put(root);
    return g_cmds_n + g_cats_n;
}

static int policy_save(void)
{
    const char *path = policy_config_path();
    if (ensure_parent_dir(path) != 0) {
        fprintf(stderr, "error: cannot create config directory for %s\n", path);
        return -1;
    }

    json_object *root = json_object_new_object();
    json_object *cmds = json_object_new_array();
    for (int i = 0; i < g_cmds_n; i++)
        json_object_array_add(cmds, json_object_new_string(g_cmds[i]));
    json_object *cats = json_object_new_array();
    for (int i = 0; i < g_cats_n; i++)
        json_object_array_add(cats, json_object_new_string(g_cats[i]));
    json_object_object_add(root, "commands", cmds);
    json_object_object_add(root, "categories", cats);

    char *tmp = NULL;
    if (asprintf(&tmp, "%s.tmp.%d", path, (int)getpid()) < 0) {
        json_object_put(root);
        return -1;
    }

    int rc = json_object_to_file_ext(tmp, root, JSON_C_TO_STRING_PRETTY);
    if (rc == 0 && rename(tmp, path) != 0)
        rc = -1;
    if (rc != 0)
        unlink(tmp);

    free(tmp);
    json_object_put(root);
    return rc == 0 ? 0 : -1;
}

void policy_free(void)
{
    rules_clear();
}

void policy_set_allow_all(int enabled)
{
    g_allow_all = enabled ? 1 : 0;
}

static char *normalize_spaces(const char *s);

int policy_remember_command(const char *command)
{
    char *norm = normalize_spaces(command);
    if (!norm)
        return -1;
    int rc = 0;
    if (!list_has(g_cmds, g_cmds_n, norm))
        rc = list_add(&g_cmds, &g_cmds_n, norm);
    free(norm);
    return rc == 0 ? policy_save() : -1;
}

int policy_remember_category(const char *category)
{
    if (!cat_find(category))
        return -1;
    int rc = 0;
    if (!list_has(g_cats, g_cats_n, category))
        rc = list_add(&g_cats, &g_cats_n, category);
    return rc == 0 ? policy_save() : -1;
}

static char *normalize_spaces(const char *s)
{
    StrBuf sb;
    if (sb_init(&sb, strlen(s) + 1) != 0)
        return NULL;
    int last_space = 1;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isspace(c)) {
            if (!last_space) {
                sb_addn(&sb, " ", 1);
                last_space = 1;
            }
        } else {
            sb_addn(&sb, (const char *)&c, 1);
            last_space = 0;
        }
    }
    return sb_steal(&sb);
}

int policy_check(const char *command, const char **category)
{
    if (category)
        *category = NULL;
    if (!command || !*command || g_allow_all)
        return POLICY_ALLOW;

    const char *cat = classify_command(command, 0);
    if (!cat)
        return POLICY_ALLOW;

    if (cat_deny(cat)) {
        if (category)
            *category = cat;
        return POLICY_DENY;
    }

    char *norm = normalize_spaces(command);
    if (norm) {
        if (list_has(g_cmds, g_cmds_n, norm)) {
            free(norm);
            return POLICY_ALLOW;
        }
        free(norm);
    }
    if (list_has(g_cats, g_cats_n, cat))
        return POLICY_ALLOW;

    if (category)
        *category = cat;
    return POLICY_CONFIRM;
}

int policy_list(void)
{
    printf("shell command permissions (config: %s):\n", policy_config_path());
    if (g_cmds_n == 0 && g_cats_n == 0) {
        printf("  (none: dangerous commands are confirmed each time)\n");
        return 0;
    }
    for (int i = 0; i < g_cats_n; i++) {
        const char *desc = policy_category_desc(g_cats[i]);
        printf("  kind: commands that %s\n", desc ? desc : g_cats[i]);
    }
    for (int i = 0; i < g_cmds_n; i++)
        printf("  cmd : %s\n", g_cmds[i]);
    return g_cmds_n + g_cats_n;
}

int policy_reset(void)
{
    rules_clear();
    if (unlink(policy_config_path()) != 0 && errno != ENOENT)
        return -1;
    return 0;
}
