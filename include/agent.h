#ifndef LCODE_AGENT_H
#define LCODE_AGENT_H

typedef struct {
    const char *api_key;
    const char *base_url;
    const char *model;
    const char *system_prompt;
    const char *append_system_prompt;
    const char *initial_prompt;
    const char *session;
    int non_interactive;
    int max_iterations;
    int shell_timeout;             /* default run_shell timeout in seconds, 0 = disabled */
    int stream;                    /* 1 = stream the response over SSE, 0 = buffer it */
    int thinking;                  /* 1 = DeepSeek thinking mode, 0 = disabled */
    const char *reasoning_effort;  /* low / medium / high / max ... */
    long context_max_tokens;       /* prompt budget before history is compacted, 0 = off */
    int context_keep_recent;       /* messages kept when compacting */
    int context_summarize;         /* 1 = summarize dropped history, 0 = truncate */
} AgentConfig;

int agent_run(AgentConfig *cfg);

#endif
