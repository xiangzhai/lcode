#ifndef LCODE_SESSION_H
#define LCODE_SESSION_H

#include <json-c/json.h>

const char *session_dir(void);

int session_validate_name(const char *name);
char *session_generate_name(void);
char *session_path(const char *name);

int session_exists(const char *name);
int session_save(const char *name, const char *model, json_object *messages);
json_object *session_load(const char *name);
int session_list(void);
int session_delete(const char *name);
char *session_latest(void);

#endif
