#ifndef LCODE_HTTP_H
#define LCODE_HTTP_H

#include <stddef.h>

typedef struct {
    long status;
    char *body;
    char *error;
} HttpResponse;

void http_global_init(void);
void http_global_cleanup(void);

HttpResponse http_post_json(const char *url, const char *api_key, const char *json_body);
void http_response_free(HttpResponse *res);

#endif
