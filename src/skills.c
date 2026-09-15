#include "skills.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    char *name;
    char *description;
    char *path;
    char *dir;
} Skill;

static Skill *g_skills = NULL;
static int g_skill_count = 0;
static int g_skill_cap = 0;

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (len + 1 >= cap) {
            char *tmp = realloc(buf, cap * 2);
            if (!tmp) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = tmp;
            cap *= 2;
        }
    }

    if (ferror(f)) {
        free(buf);
        fclose(f);
        return NULL;
    }

    fclose(f);
    buf[len] = '\0';
    return buf;
}

static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/*
 * Split an optional YAML frontmatter block from the skill body. The name and
 * description are copied out (either may stay NULL) and *body points into the
 * original buffer at the first line after the closing "---" line.
 */
static void parse_skill(const char *content, char **out_name,
                        char **out_desc, const char **out_body)
{
    *out_name = NULL;
    *out_desc = NULL;
    *out_body = content;

    if (!starts_with(content, "---"))
        return;

    const char *nl = strchr(content, '\n');
    if (!nl)
        return;
    size_t first_len = (size_t)(nl - content);
    if (first_len > 0 && content[first_len - 1] == '\r')
        first_len--;
    if (first_len != 3)
        return;

    const char *fm = nl + 1;
    const char *end = NULL;
    const char *p = fm;
    while (*p) {
        const char *line_end = strchr(p, '\n');
        size_t llen = line_end ? (size_t)(line_end - p) : strlen(p);
        size_t lc = llen;
        if (lc > 0 && p[lc - 1] == '\r')
            lc--;
        if (lc == 3 && strncmp(p, "---", 3) == 0) {
            end = line_end ? line_end + 1 : p + llen;
            break;
        }
        if (!line_end)
            break;
        p = line_end + 1;
    }
    if (!end)
        return;

    p = fm;
    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        const char *stop = line_end ? line_end : end;
        const char *colon = memchr(p, ':', (size_t)(stop - p));
        if (colon) {
            const char *k = p;
            const char *ke = colon;
            while (k < ke && (*k == ' ' || *k == '\t'))
                k++;
            while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t' || ke[-1] == '\r'))
                ke--;

            const char *v = colon + 1;
            while (v < stop && (*v == ' ' || *v == '\t'))
                v++;
            const char *ve = stop;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\r'))
                ve--;
            if (ve - v >= 2 &&
                ((v[0] == '"' && ve[-1] == '"') ||
                 (v[0] == '\'' && ve[-1] == '\''))) {
                v++;
                ve--;
            }

            size_t klen = (size_t)(ke - k);
            if (klen == 4 && strncmp(k, "name", 4) == 0 && !*out_name)
                *out_name = strndup(v, (size_t)(ve - v));
            else if (klen == 11 && strncmp(k, "description", 11) == 0 && !*out_desc)
                *out_desc = strndup(v, (size_t)(ve - v));
        }
        if (!line_end)
            break;
        p = line_end + 1;
    }

    *out_body = end;
}

static char *first_line(const char *body)
{
    const char *p = body;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        size_t s = 0;
        while (s < l && (p[s] == ' ' || p[s] == '\t' || p[s] == '\r'))
            s++;
        size_t ee = l;
        while (ee > s && (p[ee - 1] == '\r' || p[ee - 1] == ' ' || p[ee - 1] == '\t'))
            ee--;
        if (ee > s) {
            while (s < ee && p[s] == '#')
                s++;
            while (s < ee && (p[s] == ' ' || p[s] == '\t'))
                s++;
            return strndup(p + s, ee - s);
        }
        if (!e)
            break;
        p = e + 1;
    }
    return NULL;
}

static int name_taken(const char *name)
{
    for (int i = 0; i < g_skill_count; i++)
        if (strcmp(g_skills[i].name, name) == 0)
            return 1;
    return 0;
}

static void add_skill(const char *fallback_name, const char *path, const char *dir)
{
    char *content = read_file(path);
    if (!content)
        return;

    char *fm_name = NULL, *desc = NULL;
    const char *body = NULL;
    parse_skill(content, &fm_name, &desc, &body);

    const char *name = (fm_name && *fm_name) ? fm_name : fallback_name;
    if (name_taken(name)) {
        free(fm_name);
        free(desc);
        free(content);
        return;
    }

    char *name_copy = strdup(name);
    char *desc_copy = (desc && *desc) ? strdup(desc) : first_line(body);
    char *path_copy = strdup(path);
    char *dir_copy = strdup(dir);

    if (!name_copy || !path_copy || !dir_copy) {
        free(fm_name);
        free(desc);
        free(name_copy);
        free(desc_copy);
        free(path_copy);
        free(dir_copy);
        free(content);
        return;
    }

    if (g_skill_count == g_skill_cap) {
        int ncap = g_skill_cap ? g_skill_cap * 2 : 8;
        Skill *tmp = realloc(g_skills, (size_t)ncap * sizeof(*tmp));
        if (!tmp) {
            free(fm_name);
            free(desc);
            free(name_copy);
            free(desc_copy);
            free(path_copy);
            free(dir_copy);
            free(content);
            return;
        }
        g_skills = tmp;
        g_skill_cap = ncap;
    }

    g_skills[g_skill_count].name = name_copy;
    g_skills[g_skill_count].description = desc_copy;
    g_skills[g_skill_count].path = path_copy;
    g_skills[g_skill_count].dir = dir_copy;
    g_skill_count++;

    free(fm_name);
    free(desc);
    free(content);
}

static void scan_dir(const char *base)
{
    DIR *d = opendir(base);
    if (!d)
        return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;

        char path[4096];
        if (snprintf(path, sizeof(path), "%s/%s", base, e->d_name) >= (int)sizeof(path))
            continue;

        struct stat st;
        if (stat(path, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            char md[4096];
            if (snprintf(md, sizeof(md), "%s/SKILL.md", path) >= (int)sizeof(md))
                continue;
            struct stat ms;
            if (stat(md, &ms) == 0 && S_ISREG(ms.st_mode))
                add_skill(e->d_name, md, path);
        } else if (S_ISREG(st.st_mode)) {
            size_t l = strlen(e->d_name);
            if (l > 3 && strcmp(e->d_name + l - 3, ".md") == 0) {
                char *name = strndup(e->d_name, l - 3);
                if (name) {
                    add_skill(name, path, base);
                    free(name);
                }
            }
        }
    }
    closedir(d);
}

static void scan_global(void)
{
    char dir[4096];
    const char *home = getenv("LCODE_HOME");
    if (home && *home) {
        snprintf(dir, sizeof(dir), "%s/skills", home);
    } else {
        const char *xdg = getenv("XDG_DATA_HOME");
        if (xdg && *xdg)
            snprintf(dir, sizeof(dir), "%s/lcode/skills", xdg);
        else {
            const char *h = getenv("HOME");
            if (!h || !*h)
                h = ".";
            snprintf(dir, sizeof(dir), "%s/.local/share/lcode/skills", h);
        }
    }
    scan_dir(dir);
}

static int cmp_skill_name(const void *a, const void *b)
{
    const Skill *x = a;
    const Skill *y = b;
    return strcmp(x->name, y->name);
}

void skills_reload(void)
{
    skills_free();
    scan_dir(".lcode/skills");
    scan_global();
    if (g_skill_count > 1)
        qsort(g_skills, (size_t)g_skill_count, sizeof(*g_skills), cmp_skill_name);
}

void skills_free(void)
{
    for (int i = 0; i < g_skill_count; i++) {
        free(g_skills[i].name);
        free(g_skills[i].description);
        free(g_skills[i].path);
        free(g_skills[i].dir);
    }
    free(g_skills);
    g_skills = NULL;
    g_skill_count = 0;
    g_skill_cap = 0;
}

char *skills_system_prompt(void)
{
    if (g_skill_count == 0)
        return NULL;

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("## Skills\n\n", f);
    fputs("Skills provide specialized instructions for specific tasks. When a task "
          "matches one of the skills below, call the `skill` tool with the skill "
          "name to load its full instructions before proceeding.\n\n", f);
    fputs("<available_skills>\n", f);
    for (int i = 0; i < g_skill_count; i++) {
        fprintf(f, "- %s: %s (location: %s)\n",
                g_skills[i].name,
                g_skills[i].description ? g_skills[i].description : "",
                g_skills[i].path);
    }
    fputs("</available_skills>\n", f);
    fclose(f);
    return buf;
}

char *skills_execute(const char *name)
{
    if (!name)
        return strdup("error: missing required argument 'name'");

    for (int i = 0; i < g_skill_count; i++) {
        if (strcmp(g_skills[i].name, name) != 0)
            continue;

        char *content = read_file(g_skills[i].path);
        if (!content) {
            char *msg = NULL;
            if (asprintf(&msg, "error: failed to read skill '%s'", name) < 0)
                return strdup("error: failed to read skill");
            return msg;
        }

        char *fm_name = NULL, *desc = NULL;
        const char *body = NULL;
        parse_skill(content, &fm_name, &desc, &body);

        char *out = NULL;
        size_t olen = 0;
        FILE *f = open_memstream(&out, &olen);
        char *result = NULL;
        if (f) {
            fprintf(f, "# Skill: %s\n\n", g_skills[i].name);
            fputs(body, f);
            size_t blen = strlen(body);
            if (blen > 0 && body[blen - 1] != '\n')
                fputc('\n', f);
            fprintf(f, "\nBase directory for this skill: %s\n", g_skills[i].dir);
            fclose(f);
            result = out;
        }

        free(fm_name);
        free(desc);
        free(content);
        return result ? result : strdup("error: out of memory");
    }

    char *msg = NULL;
    if (asprintf(&msg, "error: unknown skill '%s'", name) < 0)
        return strdup("error: unknown skill");
    return msg;
}

int skills_list(void)
{
    if (g_skill_count == 0) {
        printf("no skills found\n");
        return 0;
    }
    printf("skills:\n");
    for (int i = 0; i < g_skill_count; i++) {
        printf("  %-24s  %s\n", g_skills[i].name,
               g_skills[i].description ? g_skills[i].description : "");
        printf("  %-24s  %s\n", "", g_skills[i].path);
    }
    return g_skill_count;
}
