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

typedef struct {
    pthread_mutex_t lock;
    int stop;
} Spinner;

static void *spinner_thread(void *arg)
{
    Spinner *sp = arg;
    static const char frames[] = "|/-\\";
    int i = 0;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (;;) {
        pthread_mutex_lock(&sp->lock);
        int stop = sp->stop;
        pthread_mutex_unlock(&sp->lock);
        if (stop)
            break;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double secs = (double)(now.tv_sec - t0.tv_sec) +
                      (double)(now.tv_nsec - t0.tv_nsec) / 1e9;

        fprintf(stdout, "\r\033[0;36m%c thinking... %.1fs\033[0m",
                frames[i++ & 3], secs);
        fflush(stdout);

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
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);

    Spinner spinner;
    spinner.stop = 0;
    pthread_mutex_init(&spinner.lock, NULL);
    pthread_t th;
    int have_spinner = isatty(STDOUT_FILENO) &&
                       pthread_create(&th, NULL, spinner_thread, &spinner) == 0;

    interrupt_watch_begin();
    CURLcode rc = curl_easy_perform(curl);
    int interrupted = interrupt_poll();
    interrupt_watch_end();

    if (have_spinner) {
        pthread_mutex_lock(&spinner.lock);
        spinner.stop = 1;
        pthread_mutex_unlock(&spinner.lock);
        pthread_join(th, NULL);
        fputs("\r\033[2K", stdout);
        fflush(stdout);
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
