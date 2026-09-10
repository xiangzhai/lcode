#include "session.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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

const char *session_dir(void)
{
    static char dir[4096];
    static int ready = 0;
    if (ready)
        return dir;

    const char *home = getenv("LCODE_HOME");
    if (home && *home) {
        snprintf(dir, sizeof(dir), "%s/sessions", home);
    } else {
        const char *xdg = getenv("XDG_DATA_HOME");
        if (xdg && *xdg) {
            snprintf(dir, sizeof(dir), "%s/lcode/sessions", xdg);
        } else {
            const char *h = getenv("HOME");
            if (!h || !*h)
                h = ".";
            snprintf(dir, sizeof(dir), "%s/.local/share/lcode/sessions", h);
        }
    }

    mkdir_p(dir);
    ready = 1;
    return dir;
}

int session_validate_name(const char *name)
{
    if (!name || !*name)
        return 0;
    size_t len = strlen(name);
    if (len > 120)
        return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
            continue;
        return 0;
    }
    return 1;
}

char *session_generate_name(void)
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char buf[64];
    strftime(buf, sizeof(buf), "session-%Y%m%d-%H%M%S", &tmv);
    return strdup(buf);
}

char *session_path(const char *name)
{
    char *path = NULL;
    if (asprintf(&path, "%s/%s.json", session_dir(), name) < 0)
        return NULL;
    return path;
}

int session_exists(const char *name)
{
    if (!session_validate_name(name))
        return 0;
    char *path = session_path(name);
    if (!path)
        return 0;
    struct stat st;
    int ok = (stat(path, &st) == 0 && S_ISREG(st.st_mode));
    free(path);
    return ok;
}

int session_save(const char *name, const char *model, json_object *messages)
{
    if (!session_validate_name(name) || !messages)
        return -1;

    char *path = session_path(name);
    if (!path)
        return -1;

    long long created = (long long)time(NULL);
    json_object *existing = json_object_from_file(path);
    if (existing) {
        json_object *v;
        if (json_object_object_get_ex(existing, "created_at", &v))
            created = json_object_get_int64(v);
        json_object_put(existing);
    }

    json_object *root = json_object_new_object();
    json_object_object_add(root, "id", json_object_new_string(name));
    if (model)
        json_object_object_add(root, "model", json_object_new_string(model));
    json_object_object_add(root, "created_at", json_object_new_int64(created));
    json_object_object_add(root, "updated_at", json_object_new_int64((long long)time(NULL)));
    json_object_object_add(root, "messages", json_object_get(messages));

    char *tmp = NULL;
    if (asprintf(&tmp, "%s.tmp.%d", path, (int)getpid()) < 0) {
        free(path);
        json_object_put(root);
        return -1;
    }

    int rc = json_object_to_file_ext(tmp, root, JSON_C_TO_STRING_PLAIN);
    if (rc == 0 && rename(tmp, path) != 0)
        rc = -1;
    if (rc != 0)
        unlink(tmp);

    free(tmp);
    free(path);
    json_object_put(root);
    return rc == 0 ? 0 : -1;
}

json_object *session_load(const char *name)
{
    if (!session_validate_name(name))
        return NULL;

    char *path = session_path(name);
    if (!path)
        return NULL;

    json_object *root = json_object_from_file(path);
    free(path);
    if (!root)
        return NULL;

    json_object *msgs;
    if (!json_object_object_get_ex(root, "messages", &msgs) ||
        json_object_get_type(msgs) != json_type_array) {
        json_object_put(root);
        return NULL;
    }

    json_object_get(msgs);
    json_object_put(root);
    return msgs;
}

typedef struct {
    char *name;
    long long updated;
    int count;
} SessionItem;

static int cmp_recent(const void *a, const void *b)
{
    const SessionItem *x = a;
    const SessionItem *y = b;
    if (x->updated < y->updated)
        return 1;
    if (x->updated > y->updated)
        return -1;
    return 0;
}

int session_list(void)
{
    const char *dir = session_dir();
    DIR *d = opendir(dir);
    if (!d) {
        printf("sessions directory: %s (empty)\n", dir);
        return 0;
    }

    SessionItem *items = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l <= 5 || strcmp(e->d_name + l - 5, ".json") != 0)
            continue;

        char *name = strndup(e->d_name, l - 5);
        char *path = session_path(name);
        json_object *root = path ? json_object_from_file(path) : NULL;
        free(path);

        long long updated = 0;
        int count = 0;
        if (root) {
            json_object *v;
            if (json_object_object_get_ex(root, "updated_at", &v))
                updated = json_object_get_int64(v);
            if (json_object_object_get_ex(root, "messages", &v) &&
                json_object_get_type(v) == json_type_array)
                count = (int)json_object_array_length(v);
            json_object_put(root);
        }

        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            SessionItem *tmp = realloc(items, (size_t)cap * sizeof(*items));
            if (!tmp) {
                free(name);
                break;
            }
            items = tmp;
        }
        items[n].name = name;
        items[n].updated = updated;
        items[n].count = count;
        n++;
    }
    closedir(d);

    if (n > 1)
        qsort(items, (size_t)n, sizeof(*items), cmp_recent);

    printf("sessions in %s:\n", dir);
    if (n == 0)
        printf("  (none)\n");
    for (int i = 0; i < n; i++) {
        char ts[64] = "-";
        if (items[i].updated) {
            time_t t = (time_t)items[i].updated;
            struct tm tmv;
            localtime_r(&t, &tmv);
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", &tmv);
        }
        printf("  %-24s  %3d msgs  %s\n", items[i].name, items[i].count, ts);
        free(items[i].name);
    }
    free(items);
    return n;
}

int session_delete(const char *name)
{
    if (!session_validate_name(name))
        return -1;
    char *path = session_path(name);
    if (!path)
        return -1;
    int rc = unlink(path);
    free(path);
    return rc == 0 ? 0 : -1;
}

char *session_latest(void)
{
    const char *dir = session_dir();
    DIR *d = opendir(dir);
    if (!d)
        return NULL;

    char *best = NULL;
    long long best_updated = -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l <= 5 || strcmp(e->d_name + l - 5, ".json") != 0)
            continue;

        char *name = strndup(e->d_name, l - 5);
        char *path = session_path(name);
        json_object *root = path ? json_object_from_file(path) : NULL;
        free(path);

        long long updated = 0;
        if (root) {
            json_object *v;
            if (json_object_object_get_ex(root, "updated_at", &v))
                updated = json_object_get_int64(v);
            json_object_put(root);
        }

        if (updated >= best_updated) {
            free(best);
            best = name;
            best_updated = updated;
        } else {
            free(name);
        }
    }
    closedir(d);
    return best;
}
