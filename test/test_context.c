#include "context.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static json_object *make_message(const char *role, const char *content)
{
    json_object *m = json_object_new_object();
    json_object_object_add(m, "role", json_object_new_string(role));
    json_object_object_add(m, "content", json_object_new_string(content));
    return m;
}

static void add(json_object *arr, const char *role, const char *content)
{
    json_object_array_add(arr, make_message(role, content));
}

static const char *role_at(json_object *arr, int idx)
{
    json_object *m = json_object_array_get_idx(arr, idx);
    json_object *v;
    if (!m || !json_object_object_get_ex(m, "role", &v))
        return NULL;
    return json_object_get_string(v);
}

static const char *content_at(json_object *arr, int idx)
{
    json_object *m = json_object_array_get_idx(arr, idx);
    json_object *v;
    if (!m || !json_object_object_get_ex(m, "content", &v))
        return NULL;
    return json_object_get_string(v);
}

static int test_estimate(void)
{
    json_object *m = json_object_new_array();
    add(m, "system", "you are lcode, a coding agent");
    add(m, "user", "read the file and explain it");
    long tokens = context_estimate_tokens(m);
    long single = context_estimate_message(json_object_array_get_idx(m, 0));
    int fails = 0;
    if (tokens <= 0 || single <= 0)
        fails++;
    printf("%-4s estimate: array %ld, one message %ld\n", fails ? "FAIL" : "ok",
           tokens, single);
    json_object_put(m);
    return fails;
}

static int test_noop_under_budget(void)
{
    json_object *m = json_object_new_array();
    add(m, "system", "sys");
    add(m, "user", "hi");

    ContextConfig cc = { 100000, 4, 1 };
    int dropped = context_compact(m, &cc, NULL, NULL);
    int fails = (dropped != 0) || (json_object_array_length(m) != 2);

    ContextConfig off = { 0, 4, 1 };
    int dropped_off = context_compact(m, &off, NULL, NULL);
    if (dropped_off != 0 || json_object_array_length(m) != 2)
        fails++;

    printf("%-4s no-op under budget / when disabled\n", fails ? "FAIL" : "ok");
    json_object_put(m);
    return fails;
}

static int test_truncate(void)
{
    json_object *m = json_object_new_array();
    add(m, "system", "you are lcode");
    for (int i = 0; i < 60; i++) {
        char buf[160];
        snprintf(buf, sizeof(buf), "user turn %d with enough padding words to cost tokens", i);
        add(m, "user", buf);
        snprintf(buf, sizeof(buf), "assistant reply %d with enough padding words to cost tokens", i);
        add(m, "assistant", buf);
    }

    int n0 = (int)json_object_array_length(m);
    long before = context_estimate_tokens(m);
    ContextConfig cc = { 300, 4, 0 };
    int dropped = context_compact(m, &cc, NULL, NULL);
    int n1 = (int)json_object_array_length(m);
    long after = context_estimate_tokens(m);

    int fails = 0;
    if (dropped <= 0 || n1 != n0 - dropped) {
        printf("FAIL truncate: dropped=%d n0=%d n1=%d\n", dropped, n0, n1);
        fails++;
    }
    if (!role_at(m, 0) || strcmp(role_at(m, 0), "system") != 0) {
        printf("FAIL truncate: system prompt not preserved\n");
        fails++;
    }
    if (after >= before) {
        printf("FAIL truncate: size did not shrink (%ld -> %ld)\n", before, after);
        fails++;
    }
    if (role_at(m, 1) && strcmp(role_at(m, 1), "tool") == 0) {
        printf("FAIL truncate: tail starts with an orphan tool message\n");
        fails++;
    }
    printf("%-4s truncate: dropped %d of %d messages, ~%ld -> ~%ld tokens\n",
           fails ? "FAIL" : "ok", dropped, n0, before, after);
    json_object_put(m);
    return fails;
}

static int test_tool_pairing(void)
{
    json_object *m = json_object_new_array();
    add(m, "system", "sys");
    add(m, "user", "please do the thing and keep going with padding words");
    add(m, "assistant", "working on it with padding words to cost some tokens");
    add(m, "user", "more instructions padded with words to cost some tokens");

    json_object *tc = json_object_new_object();
    json_object_object_add(tc, "role", json_object_new_string("assistant"));
    json_object_object_add(tc, "content", json_object_new_string("calling tool now"));
    json_object *tcs = json_object_new_array();
    json_object *call = json_object_new_object();
    json_object_object_add(call, "id", json_object_new_string("call_1"));
    json_object_object_add(call, "type", json_object_new_string("function"));
    json_object *fn = json_object_new_object();
    json_object_object_add(fn, "name", json_object_new_string("read_file"));
    json_object_object_add(fn, "arguments", json_object_new_string("{\"path\":\"x\"}"));
    json_object_object_add(call, "function", fn);
    json_object_array_add(tcs, call);
    json_object_object_add(tc, "tool_calls", tcs);
    json_object_array_add(m, tc);

    json_object *tr = json_object_new_object();
    json_object_object_add(tr, "role", json_object_new_string("tool"));
    json_object_object_add(tr, "tool_call_id", json_object_new_string("call_1"));
    json_object_object_add(tr, "content", json_object_new_string("file contents padded with words"));
    json_object_array_add(m, tr);

    /* keep_recent = 1 makes the naive cut land on the tool result, which must
       be skipped so a call is never separated from its result. */
    ContextConfig cc = { 40, 1, 0 };
    int dropped = context_compact(m, &cc, NULL, NULL);
    int fails = 0;
    if (dropped <= 0) {
        printf("FAIL tool pairing: nothing dropped (%d)\n", dropped);
        fails++;
    }
    if (role_at(m, 1) && strcmp(role_at(m, 1), "tool") == 0) {
        printf("FAIL tool pairing: kept tail starts with a tool result\n");
        fails++;
    }
    printf("%-4s tool pairing: dropped %d, first kept role '%s'\n",
           fails ? "FAIL" : "ok", dropped, role_at(m, 1) ? role_at(m, 1) : "?");
    json_object_put(m);
    return fails;
}

static char *fake_summary(json_object *dropped, void *userdata)
{
    int *calls = userdata;
    (*calls)++;
    char *s = NULL;
    if (asprintf(&s, "kept %d messages worth of facts",
                 (int)json_object_array_length(dropped)) < 0)
        return NULL;
    return s;
}

static int test_summarize(void)
{
    json_object *m = json_object_new_array();
    add(m, "system", "sys");
    for (int i = 0; i < 40; i++) {
        char buf[140];
        snprintf(buf, sizeof(buf), "user %d padded with enough words to cost tokens", i);
        add(m, "user", buf);
        snprintf(buf, sizeof(buf), "assistant %d padded with enough words to cost tokens", i);
        add(m, "assistant", buf);
    }

    ContextConfig cc = { 200, 4, 1 };
    int calls = 0;
    int dropped = context_compact(m, &cc, fake_summary, &calls);

    int fails = 0;
    if (calls != 1) {
        printf("FAIL summarize: callback calls=%d\n", calls);
        fails++;
    }
    if (dropped <= 0) {
        printf("FAIL summarize: nothing dropped\n");
        fails++;
    }
    const char *role = role_at(m, 1);
    if (!role || strcmp(role, "system") != 0) {
        printf("FAIL summarize: message at index 1 is '%s'\n", role ? role : "?");
        fails++;
    }
    const char *content = content_at(m, 1);
    if (!content || !strstr(content, "kept ")) {
        printf("FAIL summarize: summary text missing\n");
        fails++;
    }
    printf("%-4s summarize: dropped %d, inserted summary at index 1\n",
           fails ? "FAIL" : "ok", dropped);
    json_object_put(m);
    return fails;
}

static int test_summarize_fallback(void)
{
    json_object *m = json_object_new_array();
    add(m, "system", "sys");
    for (int i = 0; i < 40; i++) {
        char buf[140];
        snprintf(buf, sizeof(buf), "user %d padded with enough words to cost tokens", i);
        add(m, "user", buf);
        snprintf(buf, sizeof(buf), "assistant %d padded with enough words to cost tokens", i);
        add(m, "assistant", buf);
    }

    /* summarize requested but the callback fails: fall back to truncation */
    ContextConfig cc = { 200, 4, 1 };
    int dropped = context_compact(m, &cc, NULL, NULL);
    int fails = 0;
    if (dropped <= 0) {
        printf("FAIL fallback: nothing dropped\n");
        fails++;
    }
    if (role_at(m, 0) && strcmp(role_at(m, 0), "system") != 0) {
        printf("FAIL fallback: system prompt lost\n");
        fails++;
    }
    printf("%-4s summarize fallback to truncation: dropped %d\n",
           fails ? "FAIL" : "ok", dropped);
    json_object_put(m);
    return fails;
}

int main(void)
{
    int fails = 0;
    fails += test_estimate();
    fails += test_noop_under_budget();
    fails += test_truncate();
    fails += test_tool_pairing();
    fails += test_summarize();
    fails += test_summarize_fallback();
    printf("\n%s\n", fails ? "FAILED" : "all context tests passed");
    return fails ? 1 : 0;
}
