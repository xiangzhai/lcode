#ifndef LCODE_TOOLS_H
#define LCODE_TOOLS_H

#include <json-c/json.h>

#define SHELL_TIMEOUT_DEFAULT 120

void tools_set_non_interactive(int enabled);
void tools_set_shell_timeout(int seconds);
json_object *tools_schema(void);
char *tools_execute(const char *name, json_object *args);
int tools_output_streamed(void);

/*
 * One tool call from an assistant message together with its result. Independent
 * calls in a batch are executed concurrently by tools_execute_batch(); results
 * are filled in the original order and owned by the caller, which frees them.
 */
typedef struct {
    const char *name;
    json_object *args;
    char *result;
    int streamed; /* the tool already printed its output live */
} ToolCallJob;

/*
 * Run a batch of tool calls. Calls that cannot touch the terminal run together
 * in worker threads; the rest run one at a time on the calling thread so that
 * prompts and streamed shell output stay readable. Returns the number of calls
 * that ran before an interrupt stopped the batch (n when all of them ran).
 */
int tools_execute_batch(ToolCallJob *jobs, int n);

#endif
