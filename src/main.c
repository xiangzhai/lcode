#include "agent.h"
#include "http.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define APPEND_PROMPT_FILE ".lcode.md"

static const char *env_or(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return (v && *v) ? v : fallback;
}

static char *read_whole_file(const char *path, const char *what)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open %s '%s': %s\n", what, path, strerror(errno));
        return NULL;
    }

    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        fclose(f);
        fprintf(stderr, "error: out of memory reading %s '%s'\n", what, path);
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
                fprintf(stderr, "error: out of memory reading %s '%s'\n", what, path);
                return NULL;
            }
            buf = tmp;
            cap *= 2;
        }
    }

    if (ferror(f)) {
        fprintf(stderr, "error: failed reading %s '%s'\n", what, path);
        free(buf);
        fclose(f);
        return NULL;
    }

    fclose(f);
    buf[len] = '\0';
    return buf;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  --model <name>              model id (default: deepseek-v4-flash)\n"
            "  --base-url <url>            API base url (default: https://api.deepseek.com)\n"
            "  --system-prompt <text>      replace the built-in system prompt\n"
            "  --system-prompt-file <path> read the system prompt from a file\n"
            "  --append-system-prompt <text>      append extra instructions to the system prompt\n"
            "  --append-system-prompt-file <path> append file contents to the system prompt\n"
            "  --prompt <text>             preload an initial user prompt\n"
            "  --prompt-file <path>        read the initial user prompt from a file\n"
            "  --non-interactive, -y       let the model decide; never ask, exit after --prompt\n"
            "  --max-iterations <n>        max tool-call rounds per turn (default: 16)\n"
            "  -h, --help                  show this help\n"
            "env: DEEPSEEK_API_KEY (required), DEEPSEEK_BASE_URL, DEEPSEEK_MODEL\n"
            "note: .lcode.md in the current directory is auto-appended to the system prompt\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *api_key = getenv("DEEPSEEK_API_KEY");
    if (!api_key || !*api_key) {
        fprintf(stderr, "error: DEEPSEEK_API_KEY is not set\n");
        return 1;
    }

    AgentConfig cfg;
    cfg.api_key = api_key;
    cfg.base_url = env_or("DEEPSEEK_BASE_URL", "https://api.deepseek.com");
    cfg.model = env_or("DEEPSEEK_MODEL", "deepseek-v4-flash");
    cfg.system_prompt = NULL;
    cfg.append_system_prompt = NULL;
    cfg.initial_prompt = NULL;
    cfg.non_interactive = 0;
    cfg.max_iterations = 16;

    char *owned[8];
    int owned_n = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            cfg.model = argv[++i];
        } else if (strcmp(argv[i], "--base-url") == 0 && i + 1 < argc) {
            cfg.base_url = argv[++i];
        } else if (strcmp(argv[i], "--system-prompt") == 0 && i + 1 < argc) {
            cfg.system_prompt = argv[++i];
        } else if (strcmp(argv[i], "--append-system-prompt") == 0 && i + 1 < argc) {
            cfg.append_system_prompt = argv[++i];
        } else if (strcmp(argv[i], "--prompt") == 0 && i + 1 < argc) {
            cfg.initial_prompt = argv[++i];
        } else if ((strcmp(argv[i], "--system-prompt-file") == 0 ||
                    strcmp(argv[i], "--append-system-prompt-file") == 0 ||
                    strcmp(argv[i], "--prompt-file") == 0) &&
                   i + 1 < argc) {
            const char *flag = argv[i];
            const char *path = argv[++i];
            const char *what = strcmp(flag, "--prompt-file") == 0 ? "prompt file" : "system prompt file";
            char *content = read_whole_file(path, what);
            if (!content) {
                for (int k = 0; k < owned_n; k++)
                    free(owned[k]);
                return 1;
            }
            owned[owned_n++] = content;
            if (strcmp(flag, "--system-prompt-file") == 0)
                cfg.system_prompt = content;
            else if (strcmp(flag, "--append-system-prompt-file") == 0)
                cfg.append_system_prompt = content;
            else
                cfg.initial_prompt = content;
        } else if (strcmp(argv[i], "--non-interactive") == 0 || strcmp(argv[i], "-y") == 0) {
            cfg.non_interactive = 1;
        } else if (strcmp(argv[i], "--max-iterations") == 0 && i + 1 < argc) {
            cfg.max_iterations = atoi(argv[++i]);
            if (cfg.max_iterations <= 0)
                cfg.max_iterations = 16;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "error: unknown argument '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!cfg.append_system_prompt &&
        access(APPEND_PROMPT_FILE, R_OK) == 0) {
        char *content = read_whole_file(APPEND_PROMPT_FILE, "append system prompt file");
        if (!content) {
            for (int k = 0; k < owned_n; k++)
                free(owned[k]);
            return 1;
        }
        owned[owned_n++] = content;
        cfg.append_system_prompt = content;
    }

    http_global_init();
    int rc = agent_run(&cfg);
    http_global_cleanup();

    for (int k = 0; k < owned_n; k++)
        free(owned[k]);
    return rc;
}
