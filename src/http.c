#include "http.h"
#include "version.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t size;
} HttpBuffer;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t total = size * nmemb;
    HttpBuffer *buf = (HttpBuffer *)userdata;

    char *tmp = realloc(buf->data, buf->size + total + 1);
    if (!tmp)
        return 0;

    buf->data = tmp;
    memcpy(buf->data + buf->size, ptr, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

void http_global_init(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void http_global_cleanup(void)
{
    curl_global_cleanup();
}

HttpResponse http_post_json(const char *url, const char *api_key, const char *json_body)
{
    HttpResponse res;
    memset(&res, 0, sizeof(res));

    HttpBuffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.data = malloc(1);
    if (!buf.data) {
        res.error = strdup("out of memory");
        return res;
    }
    buf.data[0] = '\0';

    CURL *curl = curl_easy_init();
    if (!curl) {
        free(buf.data);
        res.error = strdup("failed to initialize curl");
        return res;
    }

    struct curl_slist *headers = NULL;
    char auth[4096];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json_body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "lcode/" LCODE_VERSION);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        res.error = strdup(curl_easy_strerror(rc));
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.status);
    }
    res.body = buf.data;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return res;
}

void http_response_free(HttpResponse *res)
{
    free(res->body);
    free(res->error);
    res->body = NULL;
    res->error = NULL;
}
