#include "trust.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <json-c/json.h>

static char **g_trusted = NULL;
static int g_trusted_count = 0;
static char *g_active_root = NULL;

/* Directory holding lcode's trust config; other policy files live here too. */
const char *trust_config_dir(void)
{
    static char dir[4096];
    static int ready = 0;
    if (ready)
        return dir;

    const char *override = getenv("LCODE_HOME");
    if (override && *override) {
        snprintf(dir, sizeof(dir), "%s", override);
    } else {
        const char *cfg = getenv("XDG_CONFIG_HOME");
        if (cfg && *cfg) {
            snprintf(dir, sizeof(dir), "%s/lcode", cfg);
        } else {
            const char *home = getenv("HOME");
            if (!home || !*home)
                home = ".";
            snprintf(dir, sizeof(dir), "%s/.config/lcode", home);
        }
    }
    ready = 1;
    return dir;
}

const char *trust_config_path(void)
{
    static char path[4096];
    static int ready = 0;
    if (!ready) {
        const char *dir = trust_config_dir();
        const char *suffix = "/trusted.json";
        size_t n = strlen(dir);
        if (n > sizeof(path) - strlen(suffix) - 1)
            n = sizeof(path) - strlen(suffix) - 1;
        memcpy(path, dir, n);
        strcpy(path + n, suffix);
        ready = 1;
    }
    return path;
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

/* Resolve '.' and '..' lexically; the input must be an absolute path. */
static char *path_normalize(const char *path)
{
    if (!path || path[0] != '/')
        return NULL;

    char *copy = strdup(path);
    if (!copy)
        return NULL;

    char **comp = NULL;
    int n = 0, cap = 0;
    char *save = NULL;
    for (char *tok = strtok_r(copy, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (tok[0] == '\0' || strcmp(tok, ".") == 0)
            continue;
        if (strcmp(tok, "..") == 0) {
            if (n > 0)
                n--;
            continue;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            char **tmp = realloc(comp, (size_t)cap * sizeof(char *));
            if (!tmp) {
                free(comp);
                free(copy);
                return NULL;
            }
            comp = tmp;
        }
        comp[n++] = tok;
    }

    size_t len = 1;
    for (int i = 0; i < n; i++)
        len += strlen(comp[i]) + 1;
    char *out = malloc(len + 1);
    if (!out) {
        free(comp);
        free(copy);
        return NULL;
    }
    char *p = out;
    *p++ = '/';
    for (int i = 0; i < n; i++) {
        size_t l = strlen(comp[i]);
        memcpy(p, comp[i], l);
        p += l;
        if (i != n - 1)
            *p++ = '/';
    }
    *p = '\0';

    free(comp);
    free(copy);
    return out;
}

char *trust_canonical(const char *path)
{
    if (!path || !*path)
        return NULL;

    char *abs = NULL;
    if (path[0] == '/') {
        abs = strdup(path);
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd)))
            return NULL;
        if (asprintf(&abs, "%s/%s", cwd, path) < 0)
            return NULL;
    }
    if (!abs)
        return NULL;

    char *norm = path_normalize(abs);
    free(abs);
    if (!norm)
        return NULL;

    char *rp = realpath(norm, NULL);
    if (rp) {
        free(norm);
        return rp;
    }

    /* The target may not exist yet (e.g. write_file): resolve its parent and
       re-append the final component. */
    char *slash = strrchr(norm, '/');
    if (!slash) {
        free(norm);
        return NULL;
    }
    char *base = strdup(slash + 1);
    if (!base) {
        free(norm);
        return NULL;
    }
    if (slash == norm)
        norm[1] = '\0';
    else
        *slash = '\0';

    char *rpdir = realpath(norm, NULL);
    free(norm);
    if (!rpdir) {
        free(base);
        return NULL;
    }

    char *full = NULL;
    size_t rl = strlen(rpdir);
    if (asprintf(&full, "%s%s%s", rpdir,
                 (rl > 0 && rpdir[rl - 1] == '/') ? "" : "/", base) < 0)
        full = NULL;
    free(rpdir);
    free(base);
    return full;
}

/* 1 if path is root itself or lies underneath it. Both are canonical. */
static int path_within(const char *path, const char *root)
{
    size_t rl = strlen(root);
    if (rl == 1 && root[0] == '/')
        return path[0] == '/';
    if (strncmp(path, root, rl) != 0)
        return 0;
    return path[rl] == '\0' || path[rl] == '/';
}

static void list_clear(void)
{
    for (int i = 0; i < g_trusted_count; i++)
        free(g_trusted[i]);
    free(g_trusted);
    g_trusted = NULL;
    g_trusted_count = 0;
}

int trust_load(void)
{
    list_clear();

    const char *path = trust_config_path();
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;

    json_object *root = json_object_from_file(path);
    if (!root)
        return 0;

    json_object *arr;
    if (json_object_object_get_ex(root, "trusted_dirs", &arr) &&
        json_object_get_type(arr) == json_type_array) {
        int n = (int)json_object_array_length(arr);
        for (int i = 0; i < n; i++) {
            json_object *e = json_object_array_get_idx(arr, i);
            if (json_object_get_type(e) != json_type_string)
                continue;
            const char *s = json_object_get_string(e);
            if (!s || !*s)
                continue;

            char *copy = strdup(s);
            if (!copy)
                break;
            char **tmp = realloc(g_trusted, (size_t)(g_trusted_count + 1) * sizeof(char *));
            if (!tmp) {
                free(copy);
                break;
            }
            g_trusted = tmp;
            g_trusted[g_trusted_count++] = copy;
        }
    }

    json_object_put(root);
    return g_trusted_count;
}

static int trust_save(void)
{
    const char *path = trust_config_path();
    if (ensure_parent_dir(path) != 0) {
        fprintf(stderr, "error: cannot create config directory for %s\n", path);
        return -1;
    }

    json_object *root = json_object_new_object();
    json_object *arr = json_object_new_array();
    for (int i = 0; i < g_trusted_count; i++)
        json_object_array_add(arr, json_object_new_string(g_trusted[i]));
    json_object_object_add(root, "trusted_dirs", arr);

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

void trust_free(void)
{
    list_clear();
    free(g_active_root);
    g_active_root = NULL;
}

const char *trust_matching_root(const char *dir)
{
    char *canon = trust_canonical(dir);
    if (!canon)
        return NULL;

    const char *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < g_trusted_count; i++) {
        if (!path_within(canon, g_trusted[i]))
            continue;
        size_t len = strlen(g_trusted[i]);
        if (len > best_len) {
            best = g_trusted[i];
            best_len = len;
        }
    }
    free(canon);
    return best;
}

int trust_add(const char *dir)
{
    char *canon = trust_canonical(dir);
    if (!canon)
        return -1;

    for (int i = 0; i < g_trusted_count; i++) {
        if (strcmp(g_trusted[i], canon) == 0) {
            free(canon);
            return 0;
        }
    }

    char **tmp = realloc(g_trusted, (size_t)(g_trusted_count + 1) * sizeof(char *));
    if (!tmp) {
        free(canon);
        return -1;
    }
    g_trusted = tmp;
    g_trusted[g_trusted_count++] = canon;
    return trust_save();
}

int trust_set_active(const char *dir)
{
    char *canon = trust_canonical(dir);
    if (!canon)
        return -1;
    free(g_active_root);
    g_active_root = canon;
    return 0;
}

const char *trust_active_root(void)
{
    return g_active_root;
}

/*
 * Any recorded trusted directory grants access, not only the one lcode started
 * in: directories trusted on demand during a run have to stay reachable.
 */
int trust_path_allowed(const char *path)
{
    if (!g_active_root)
        return 0;
    char *canon = trust_canonical(path);
    if (!canon)
        return 0;

    int ok = 0;
    for (int i = 0; i < g_trusted_count && !ok; i++) {
        if (path_within(canon, g_trusted[i]))
            ok = 1;
    }
    free(canon);
    return ok;
}

/*
 * The directory to record when the user approves a path that is not covered
 * yet: the target itself when it is a directory, otherwise its parent.
 */
char *trust_containing_dir(const char *path)
{
    char *canon = trust_canonical(path);
    if (!canon)
        return NULL;

    struct stat st;
    if (stat(canon, &st) == 0 && S_ISDIR(st.st_mode))
        return canon;

    char *slash = strrchr(canon, '/');
    if (!slash) {
        free(canon);
        return NULL;
    }
    if (slash == canon)
        canon[1] = '\0';
    else
        *slash = '\0';

    if (stat(canon, &st) != 0 || !S_ISDIR(st.st_mode)) {
        free(canon);
        return NULL;
    }
    return canon;
}

int trust_ask(const char *dir)
{
    printf("\n\033[1;33mlcode: '%s' is not a trusted directory.\033[0m\n", dir);
    printf("lcode will only read, write and run commands inside a directory you trust.\n");
    printf("Trust this directory and its contents? [y/N] ");
    fflush(stdout);

    char line[64];
    if (!fgets(line, sizeof(line), stdin))
        return 0;
    return line[0] == 'y' || line[0] == 'Y';
}

int trust_list(void)
{
    printf("trusted directories (config: %s):\n", trust_config_path());
    if (g_trusted_count == 0)
        printf("  (none)\n");
    for (int i = 0; i < g_trusted_count; i++)
        printf("  %s\n", g_trusted[i]);
    return g_trusted_count;
}
