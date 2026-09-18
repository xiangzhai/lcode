#include "agent.h"
#include "http.h"
#include "policy.h"
#include "session.h"
#include "skills.h"
#include "tools.h"
#include "trust.h"
#include "version.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define APPEND_PROMPT_FILE "AGENTS.md"
#define DEFAULT_MAX_ITERATIONS 0
#define DEFAULT_CONTEXT_TOKENS 60000
#define DEFAULT_CONTEXT_KEEP 8

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

static int valid_reasoning_effort(const char *v)
{
    static const char *allowed[] = {
        "minimal", "low", "medium", "high", "xhigh", "max", "ultra", NULL
    };
    for (int i = 0; allowed[i]; i++)
        if (strcmp(v, allowed[i]) == 0)
            return 1;
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  --model <name>              model id (default: deepseek-flash)\n"
            "  --base-url <url>            API base url (default: https://api.deepseek.com)\n"
            "  --system-prompt <text>      replace the built-in system prompt\n"
            "  --system-prompt-file <path> read the system prompt from a file\n"
            "  --append-system-prompt <text>      append extra instructions to the system prompt\n"
            "  --append-system-prompt-file <path> append file contents to the system prompt\n"
            "  --prompt <text>             preload an initial user prompt\n"
            "  --prompt-file <path>        read the initial user prompt from a file\n"
            "  --session <name>            open or create a named session\n"
            "  --resume                    resume the most recently updated session\n"
            "  --list-sessions             list saved sessions and exit\n"
            "  --list-skills               list available skills and exit\n"
            "  --trust                     trust the current directory without asking\n"
            "  --list-trusted              list trusted directories and exit\n"
            "  --list-permissions          list remembered shell command permissions and exit\n"
            "  --reset-permissions         forget all remembered shell command permissions\n"
            "  --allow-dangerous           run shell commands without the dangerous-command\n"
            "                              confirmation policy (this run only)\n"
            "  --non-interactive, -y       let the model decide; never ask, exit after --prompt\n"
            "  --stream                    stream the response over SSE (default)\n"
            "  --no-stream                 buffer the whole response before printing\n"
            "  --thinking                  enable DeepSeek thinking mode (default)\n"
            "  --no-thinking               disable thinking mode\n"
            "  --reasoning-effort <level>  thinking effort: minimal|low|medium|high|xhigh|max|ultra\n"
            "                              (default: high)\n"
            "  --max-iterations <n>        print a notice every n tool-call rounds; the turn\n"
            "                              runs until the model stops (0 = unlimited, default)\n"
            "  --shell-timeout <seconds>   kill a run_shell command that runs longer than this\n"
            "                              (0 = no timeout, default: %d)\n"
            "  --context-tokens <n>        compact the history once the prompt is estimated to\n"
            "                              exceed n tokens (0 = disabled, default: 60000)\n"
            "  --context-keep <n>          keep at least the last n messages when compacting\n"
            "                              (default: 8)\n"
            "  --context-strategy <s>      how to shrink history: summarize | truncate\n"
            "                              (default: summarize)\n"
            "  -h, --help                  show this help\n"
            "  -V, --version               show version and exit\n"
            "env: DEEPSEEK_API_KEY (required), DEEPSEEK_BASE_URL, DEEPSEEK_MODEL,\n"
            "     DEEPSEEK_THINKING, DEEPSEEK_REASONING_EFFORT, LCODE_HOME, LCODE_SHELL_TIMEOUT,\n"
            "     LCODE_CONTEXT_TOKENS, LCODE_CONTEXT_KEEP, LCODE_CONTEXT_STRATEGY\n"
            "note: AGENTS.md in the current directory is auto-appended to the system prompt\n"
            "note: skills are discovered in .lcode/skills and $LCODE_HOME/skills\n"
            "note: lcode only accesses directories you have trusted; it asks per\n"
            "      directory (at startup and when a tool reaches outside) and saves\n"
            "      the answer in the trust config file\n"
            "note: run_shell confirms potentially destructive commands before running\n"
            "      them; refused ones are blocked outright, and approvals can be\n"
            "      remembered per command or per kind of command\n",
            prog, SHELL_TIMEOUT_DEFAULT);
}

/*
 * Make sure lcode is allowed to work in the current directory. A directory is
 * trusted when it, or one of its parents, was approved before and recorded in
 * the config file; otherwise the user is asked once and the answer is saved.
 * Refusing trust aborts the run so the directory is never touched.
 */
static int ensure_trust(int non_interactive, int force_trust)
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "error: cannot determine the current directory: %s\n",
                strerror(errno));
        return -1;
    }

    const char *root = trust_matching_root(cwd);
    if (root) {
        if (trust_set_active(root) != 0) {
            fprintf(stderr, "error: out of memory\n");
            return -1;
        }
        printf("trusted directory: %s\n", root);
        return 0;
    }

    if (force_trust) {
        if (trust_add(cwd) != 0 || trust_set_active(cwd) != 0) {
            fprintf(stderr, "error: failed to save trusted directory '%s'\n", cwd);
            return -1;
        }
        printf("trusted directory: %s (saved to %s)\n", cwd, trust_config_path());
        return 0;
    }

    if (non_interactive || !isatty(STDIN_FILENO)) {
        fprintf(stderr,
                "error: '%s' is not a trusted directory and trust cannot be "
                "requested in this mode\n"
                "       run lcode interactively to approve it, or pass --trust\n",
                cwd);
        return -1;
    }

    if (!trust_ask(cwd)) {
        fprintf(stderr,
                "lcode: refusing to access untrusted directory '%s'\n"
                "       re-run and answer 'y', or pass --trust to trust it\n",
                cwd);
        return -1;
    }

    if (trust_add(cwd) != 0 || trust_set_active(cwd) != 0) {
        fprintf(stderr, "error: failed to save trusted directory '%s'\n", cwd);
        return -1;
    }
    printf("trusted directory: %s (saved to %s)\n", cwd, trust_config_path());
    return 0;
}

int main(int argc, char **argv)
{
    AgentConfig cfg;
    cfg.api_key = getenv("DEEPSEEK_API_KEY");
    cfg.base_url = env_or("DEEPSEEK_BASE_URL", "https://api.deepseek.com");
    cfg.model = env_or("DEEPSEEK_MODEL", "deepseek-flash");
    cfg.system_prompt = NULL;
    cfg.append_system_prompt = NULL;
    cfg.initial_prompt = NULL;
    cfg.session = NULL;
    cfg.non_interactive = 0;
    cfg.max_iterations = DEFAULT_MAX_ITERATIONS;
    cfg.shell_timeout = SHELL_TIMEOUT_DEFAULT;
    const char *timeout_env = getenv("LCODE_SHELL_TIMEOUT");
    if (timeout_env && *timeout_env) {
        cfg.shell_timeout = atoi(timeout_env);
        if (cfg.shell_timeout < 0)
            cfg.shell_timeout = 0;
    }
    cfg.stream = 1;
    cfg.thinking = 1;
    cfg.reasoning_effort = env_or("DEEPSEEK_REASONING_EFFORT", "high");

    cfg.context_max_tokens = DEFAULT_CONTEXT_TOKENS;
    const char *ctx_tokens_env = getenv("LCODE_CONTEXT_TOKENS");
    if (ctx_tokens_env && *ctx_tokens_env) {
        cfg.context_max_tokens = atoi(ctx_tokens_env);
        if (cfg.context_max_tokens < 0)
            cfg.context_max_tokens = 0;
    }
    cfg.context_keep_recent = DEFAULT_CONTEXT_KEEP;
    const char *ctx_keep_env = getenv("LCODE_CONTEXT_KEEP");
    if (ctx_keep_env && *ctx_keep_env) {
        cfg.context_keep_recent = atoi(ctx_keep_env);
        if (cfg.context_keep_recent < 1)
            cfg.context_keep_recent = 1;
    }
    cfg.context_summarize = 1;
    const char *ctx_strategy_env = getenv("LCODE_CONTEXT_STRATEGY");
    if (ctx_strategy_env && strcmp(ctx_strategy_env, "truncate") == 0)
        cfg.context_summarize = 0;

    const char *think_env = getenv("DEEPSEEK_THINKING");
    if (think_env && *think_env)
        cfg.thinking = !(strcmp(think_env, "disabled") == 0 ||
                         strcmp(think_env, "off") == 0 ||
                         strcmp(think_env, "false") == 0 ||
                         strcmp(think_env, "0") == 0);

    char *owned[10];
    int owned_n = 0;
    int list_sessions = 0;
    int list_skills = 0;
    int list_trusted = 0;
    int list_permissions = 0;
    int reset_permissions = 0;
    int allow_dangerous = 0;
    int force_trust = 0;
    int resume = 0;
    const char *context_strategy = NULL;

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
        } else if (strcmp(argv[i], "--session") == 0 && i + 1 < argc) {
            cfg.session = argv[++i];
        } else if (strcmp(argv[i], "--resume") == 0) {
            resume = 1;
        } else if (strcmp(argv[i], "--list-sessions") == 0) {
            list_sessions = 1;
        } else if (strcmp(argv[i], "--list-skills") == 0) {
            list_skills = 1;
        } else if (strcmp(argv[i], "--trust") == 0) {
            force_trust = 1;
        } else if (strcmp(argv[i], "--list-trusted") == 0) {
            list_trusted = 1;
        } else if (strcmp(argv[i], "--list-permissions") == 0) {
            list_permissions = 1;
        } else if (strcmp(argv[i], "--reset-permissions") == 0) {
            reset_permissions = 1;
        } else if (strcmp(argv[i], "--allow-dangerous") == 0) {
            allow_dangerous = 1;
        } else if (strcmp(argv[i], "--non-interactive") == 0 || strcmp(argv[i], "-y") == 0) {
            cfg.non_interactive = 1;
        } else if (strcmp(argv[i], "--stream") == 0) {
            cfg.stream = 1;
        } else if (strcmp(argv[i], "--no-stream") == 0) {
            cfg.stream = 0;
        } else if (strcmp(argv[i], "--thinking") == 0) {
            cfg.thinking = 1;
        } else if (strcmp(argv[i], "--no-thinking") == 0) {
            cfg.thinking = 0;
        } else if (strcmp(argv[i], "--reasoning-effort") == 0 && i + 1 < argc) {
            cfg.reasoning_effort = argv[++i];
        } else if (strcmp(argv[i], "--max-iterations") == 0 && i + 1 < argc) {
            cfg.max_iterations = atoi(argv[++i]);
            if (cfg.max_iterations < 0)
                cfg.max_iterations = DEFAULT_MAX_ITERATIONS;
        } else if (strcmp(argv[i], "--shell-timeout") == 0 && i + 1 < argc) {
            cfg.shell_timeout = atoi(argv[++i]);
            if (cfg.shell_timeout < 0)
                cfg.shell_timeout = 0;
        } else if (strcmp(argv[i], "--context-tokens") == 0 && i + 1 < argc) {
            cfg.context_max_tokens = atoi(argv[++i]);
            if (cfg.context_max_tokens < 0)
                cfg.context_max_tokens = 0;
        } else if (strcmp(argv[i], "--context-keep") == 0 && i + 1 < argc) {
            cfg.context_keep_recent = atoi(argv[++i]);
            if (cfg.context_keep_recent < 1)
                cfg.context_keep_recent = 1;
        } else if (strcmp(argv[i], "--context-strategy") == 0 && i + 1 < argc) {
            context_strategy = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("lcode %s\n", LCODE_VERSION);
            return 0;
        } else {
            fprintf(stderr, "error: unknown argument '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (cfg.reasoning_effort && !valid_reasoning_effort(cfg.reasoning_effort)) {
        fprintf(stderr,
                "error: invalid reasoning effort '%s' "
                "(expected minimal, low, medium, high, xhigh, max or ultra)\n",
                cfg.reasoning_effort);
        for (int k = 0; k < owned_n; k++)
            free(owned[k]);
        return 1;
    }

    if (context_strategy) {
        if (strcmp(context_strategy, "summarize") == 0)
            cfg.context_summarize = 1;
        else if (strcmp(context_strategy, "truncate") == 0)
            cfg.context_summarize = 0;
        else {
            fprintf(stderr,
                    "error: invalid context strategy '%s' (expected summarize or truncate)\n",
                    context_strategy);
            for (int k = 0; k < owned_n; k++)
                free(owned[k]);
            return 1;
        }
    }

    if (list_sessions) {
        session_list();
        for (int k = 0; k < owned_n; k++)
            free(owned[k]);
        return 0;
    }

    if (list_skills) {
        skills_reload();
        skills_list();
        skills_free();
        for (int k = 0; k < owned_n; k++)
            free(owned[k]);
        return 0;
    }

    if (list_trusted) {
        trust_load();
        trust_list();
        trust_free();
        for (int k = 0; k < owned_n; k++)
            free(owned[k]);
        return 0;
    }

    if (list_permissions) {
        policy_load();
        policy_list();
        policy_free();
        for (int k = 0; k < owned_n; k++)
            free(owned[k]);
        return 0;
    }

    if (reset_permissions) {
        policy_load();
        if (policy_reset() != 0)
            fprintf(stderr, "error: failed to clear %s\n", policy_config_path());
        else
            printf("cleared shell command permissions (config: %s)\n",
                   policy_config_path());
        policy_free();
        for (int k = 0; k < owned_n; k++)
            free(owned[k]);
        return 0;
    }

    if (cfg.session && !session_validate_name(cfg.session)) {
        fprintf(stderr, "error: invalid session name '%s'\n", cfg.session);
        for (int k = 0; k < owned_n; k++)
            free(owned[k]);
        return 1;
    }

    if (resume && !cfg.session) {
        char *latest = session_latest();
        if (!latest) {
            fprintf(stderr, "error: no sessions to resume\n");
            for (int k = 0; k < owned_n; k++)
                free(owned[k]);
            return 1;
        }
        owned[owned_n++] = latest;
        cfg.session = latest;
    }

    if (!cfg.api_key || !*cfg.api_key) {
        fprintf(stderr, "error: DEEPSEEK_API_KEY is not set\n");
        for (int k = 0; k < owned_n; k++)
            free(owned[k]);
        return 1;
    }

    trust_load();
    policy_load();
    if (allow_dangerous)
        policy_set_allow_all(1);
    if (ensure_trust(cfg.non_interactive, force_trust) != 0) {
        policy_free();
        trust_free();
        for (int k = 0; k < owned_n; k++)
            free(owned[k]);
        return 1;
    }

    if (!cfg.append_system_prompt &&
        access(APPEND_PROMPT_FILE, R_OK) == 0) {
        char *content = read_whole_file(APPEND_PROMPT_FILE, "append system prompt file");
        if (!content) {
            policy_free();
            trust_free();
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
    skills_free();
    policy_free();
    trust_free();

    for (int k = 0; k < owned_n; k++)
        free(owned[k]);
    return rc;
}
