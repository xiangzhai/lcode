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
    int stream;                    /* 1 = stream the response over SSE, 0 = buffer it */
    int thinking;                  /* 1 = DeepSeek thinking mode, 0 = disabled */
    const char *reasoning_effort;  /* low / medium / high / max ... */
} AgentConfig;

int agent_run(AgentConfig *cfg);

#endif
