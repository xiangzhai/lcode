#ifndef LCODE_TOOLS_H
#define LCODE_TOOLS_H

#include <json-c/json.h>

#define SHELL_TIMEOUT_DEFAULT 120

void tools_set_non_interactive(int enabled);
void tools_set_shell_timeout(int seconds);
json_object *tools_schema(void);
char *tools_execute(const char *name, json_object *args);
int tools_output_streamed(void);

#endif
