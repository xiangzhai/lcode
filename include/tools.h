#ifndef LCODE_TOOLS_H
#define LCODE_TOOLS_H

#include <json-c/json.h>

json_object *tools_schema(void);
char *tools_execute(const char *name, json_object *args);

#endif
