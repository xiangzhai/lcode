#ifndef LCODE_HTTP_H
#define LCODE_HTTP_H

#include <stddef.h>

typedef struct {
    long status;
    char *body;
    char *error;
    int interrupted;
} HttpResponse;

typedef void (*HttpStreamCallback)(const char *data, void *userdata);

void http_global_init(void);
void http_global_cleanup(void);

HttpResponse http_post_json(const char *url, const char *api_key, const char *json_body);

/*
 * Like http_post_json() but requests a Server-Sent Events response: each
 * "data:" payload is handed to cb as it arrives (so callers can render the
 * stream live). [DONE] markers are consumed here and never passed on. The raw
 * body is still buffered in HttpResponse.body for error reporting.
 */
HttpResponse http_post_json_stream(const char *url, const char *api_key,
                                   const char *json_body, HttpStreamCallback cb,
                                   void *userdata);
void http_response_free(HttpResponse *res);

#endif
