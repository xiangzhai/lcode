#include "http.h"
#include "interrupt.h"
#include "version.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char *data;
    size_t size;
} HttpBuffer;

typedef struct {
    pthread_mutex_t lock;
    int stop;
} Spinner;

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

/*
 * State for a streaming (SSE) request: the raw body is buffered for error
 * reporting while complete "data:" lines are split out and dispatched. The
 * spinner is stopped the moment the first event arrives so streamed text and
 * the spinner never interleave.
 */
typedef struct {
    HttpBuffer raw;
    HttpStreamCallback cb;
    void *userdata;
    Spinner *spinner;
    int started;
    char *line;
    size_t line_len;
    size_t line_cap;
} SseContext;

static void sse_emit(SseContext *ctx, const char *payload)
{
    if (ctx->spinner && !ctx->started) {
        /* The spinner holds its lock while drawing, so once we take it no
           further frame can be printed after the first line of output. */
        pthread_mutex_lock(&ctx->spinner->lock);
        ctx->spinner->stop = 1;
        fputs("\r\033[2K", stdout);
        fflush(stdout);
        pthread_mutex_unlock(&ctx->spinner->lock);
    }
    ctx->started = 1;
    if (ctx->cb)
        ctx->cb(payload, ctx->userdata);
}

static void sse_handle_line(SseContext *ctx)
{
    size_t len = ctx->line_len;
    while (len > 0 && (ctx->line[len - 1] == '\r' || ctx->line[len - 1] == '\n'))
        len--;
    if (len < 5 || strncmp(ctx->line, "data:", 5) != 0)
        return;

    size_t start = 5;
    if (start < len && ctx->line[start] == ' ')
        start++;
    size_t plen = len - start;
    if (plen == 0)
        return;

    char *payload = strndup(ctx->line + start, plen);
    if (!payload)
        return;
    if (strcmp(payload, "[DONE]") != 0)
        sse_emit(ctx, payload);
    free(payload);
}

static size_t stream_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t total = size * nmemb;
    SseContext *ctx = userdata;

    char *tmp = realloc(ctx->raw.data, ctx->raw.size + total + 1);
    if (!tmp)
        return 0;
    ctx->raw.data = tmp;
    memcpy(ctx->raw.data + ctx->raw.size, ptr, total);
    ctx->raw.size += total;
    ctx->raw.data[ctx->raw.size] = '\0';

    for (size_t i = 0; i < total; i++) {
        char c = ptr[i];
        if (c == '\n') {
            sse_handle_line(ctx);
            ctx->line_len = 0;
            if (ctx->line)
                ctx->line[0] = '\0';
            continue;
        }
        if (ctx->line_len + 2 > ctx->line_cap) {
            size_t ncap = ctx->line_cap ? ctx->line_cap * 2 : 256;
            char *lt = realloc(ctx->line, ncap);
            if (!lt)
                return 0;
            ctx->line = lt;
            ctx->line_cap = ncap;
        }
        ctx->line[ctx->line_len++] = c;
        ctx->line[ctx->line_len] = '\0';
    }
    return total;
}

static int progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    (void)clientp;
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    return interrupt_poll() ? 1 : 0;
}

static void *spinner_thread(void *arg)
{
    Spinner *sp = arg;
    static const char frames[] = "|/-\\";
    int i = 0;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double secs = (double)(now.tv_sec - t0.tv_sec) +
                      (double)(now.tv_nsec - t0.tv_nsec) / 1e9;

        pthread_mutex_lock(&sp->lock);
        if (sp->stop) {
            pthread_mutex_unlock(&sp->lock);
            break;
        }
        fprintf(stdout, "\r\033[0;36m%c thinking... %.1fs\033[0m",
                frames[i++ & 3], secs);
        fflush(stdout);
        pthread_mutex_unlock(&sp->lock);

        usleep(100000);
    }
    return NULL;
}

void http_global_init(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void http_global_cleanup(void)
{
    curl_global_cleanup();
}

static HttpResponse http_perform(const char *url, const char *api_key,
                                 const char *json_body, int stream,
                                 HttpStreamCallback cb, void *userdata)
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

    SseContext sctx;
    memset(&sctx, 0, sizeof(sctx));
    sctx.cb = cb;
    sctx.userdata = userdata;

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
    headers = curl_slist_append(headers, stream ? "Accept: text/event-stream"
                                                 : "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json_body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (stream) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sctx);
    } else {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    }
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "lcode/" LCODE_VERSION);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);

    Spinner spinner;
    memset(&spinner, 0, sizeof(spinner));
    pthread_mutex_init(&spinner.lock, NULL);
    pthread_t th;
    int have_spinner = isatty(STDOUT_FILENO) &&
                       pthread_create(&th, NULL, spinner_thread, &spinner) == 0;
    if (stream)
        sctx.spinner = have_spinner ? &spinner : NULL;

    interrupt_watch_begin();
    CURLcode rc = curl_easy_perform(curl);
    int interrupted = interrupt_poll();
    interrupt_watch_end();

    if (have_spinner) {
        pthread_mutex_lock(&spinner.lock);
        spinner.stop = 1;
        pthread_mutex_unlock(&spinner.lock);
        pthread_join(th, NULL);
        /* Leave the spinner line alone if streamed output already replaced it. */
        if (!(stream && sctx.started)) {
            fputs("\r\033[2K", stdout);
            fflush(stdout);
        }
    }
    pthread_mutex_destroy(&spinner.lock);

    if (interrupted) {
        res.interrupted = 1;
    } else if (rc != CURLE_OK) {
        res.error = strdup(curl_easy_strerror(rc));
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.status);
    }
    res.body = buf.data;
    free(sctx.line);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return res;
}

HttpResponse http_post_json(const char *url, const char *api_key, const char *json_body)
{
    return http_perform(url, api_key, json_body, 0, NULL, NULL);
}

HttpResponse http_post_json_stream(const char *url, const char *api_key,
                                   const char *json_body, HttpStreamCallback cb,
                                   void *userdata)
{
    return http_perform(url, api_key, json_body, 1, cb, userdata);
}

void http_response_free(HttpResponse *res)
{
    free(res->body);
    free(res->error);
    res->body = NULL;
    res->error = NULL;
}
