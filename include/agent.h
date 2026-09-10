#ifndef LCODE_AGENT_H
#define LCODE_AGENT_H

typedef struct {
    const char *api_key;
    const char *base_url;
    const char *model;
    const char *system_prompt;
    const char *append_system_prompt;
    const char *initial_prompt;
    int max_iterations;
} AgentConfig;

int agent_run(AgentConfig *cfg);

#endif
