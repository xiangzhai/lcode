#include "agent.h"
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *env_or(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return (v && *v) ? v : fallback;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  --model <name>          model id (default: deepseek-v4-flash)\n"
            "  --base-url <url>        API base url (default: https://api.deepseek.com)\n"
            "  --max-iterations <n>    max tool-call rounds per turn (default: 16)\n"
            "  -h, --help              show this help\n"
            "env: DEEPSEEK_API_KEY (required), DEEPSEEK_BASE_URL, DEEPSEEK_MODEL\n",
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
    cfg.max_iterations = 16;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            cfg.model = argv[++i];
        } else if (strcmp(argv[i], "--base-url") == 0 && i + 1 < argc) {
            cfg.base_url = argv[++i];
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

    http_global_init();
    int rc = agent_run(&cfg);
    http_global_cleanup();
    return rc;
}
