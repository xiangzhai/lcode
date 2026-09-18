#include "context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SUMMARY_PREFIX "[Earlier conversation compressed to save context]"

static const char *role_of(json_object *msg)
{
    json_object *v;
    if (!msg || json_object_get_type(msg) != json_type_object)
        return NULL;
    if (!json_object_object_get_ex(msg, "role", &v) || !v ||
        json_object_get_type(v) != json_type_string)
        return NULL;
    return json_object_get_string(v);
}

static int is_tool_message(json_object *messages, int idx)
{
    const char *role = role_of(json_object_array_get_idx(messages, idx));
    return role && strcmp(role, "tool") == 0;
}

/*
 * Token counting heuristic. A message is serialized the way it is sent and its
 * bytes are charged: ASCII text at roughly 4 characters per token, and each
 * non-ASCII character (CJK and friends) at roughly one token. A small constant
 * covers the per-message framing the API adds.
 */
static long estimate_string(const char *s)
{
    if (!s)
        return 0;
    long ascii = 0, wide = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x80)
            ascii++;
        else if ((*p & 0xC0) != 0x80)
            wide++;
    }
    return ascii / 4 + wide + 1;
}

long context_estimate_message(json_object *message)
{
    if (!message)
        return 0;
    const char *s = json_object_to_json_string_ext(message, JSON_C_TO_STRING_PLAIN);
    return estimate_string(s) + 4;
}

long context_estimate_tokens(json_object *messages)
{
    if (!messages || json_object_get_type(messages) != json_type_array)
        return 0;

    long total = 0;
    int n = (int)json_object_array_length(messages);
    for (int i = 0; i < n; i++)
        total += context_estimate_message(json_object_array_get_idx(messages, i));
    return total;
}

/*
 * Pick the first message of the surviving tail. Walking backwards from the end
 * we keep the longest suffix that fits the budget, but never fewer than
 * keep_recent messages and never a message that starts with an orphan `tool`
 * result (its assistant call would have been dropped).
 */
static int choose_cut(json_object *messages, int n, long budget, int keep_recent)
{
    int floor = n - (keep_recent > 0 ? keep_recent : 1);
    if (floor < 1)
        floor = 1;
    while (floor > 1 && is_tool_message(messages, floor))
        floor--;

    long acc = 0;
    int cut = n;
    for (int i = n - 1; i >= 1; i--) {
        acc += context_estimate_message(json_object_array_get_idx(messages, i));
        if (acc > budget)
            break;
        cut = i;
    }
    if (cut > floor)
        cut = floor;
    while (cut > 1 && is_tool_message(messages, cut))
        cut--;
    if (cut < 1)
        cut = 1;
    return cut;
}

int context_compact(json_object *messages, const ContextConfig *cfg,
                    ContextSummarizeFn summarize, void *userdata)
{
    if (!messages || json_object_get_type(messages) != json_type_array || !cfg)
        return 0;
    if (cfg->max_tokens <= 0)
        return 0;

    int n = (int)json_object_array_length(messages);
    if (n <= 1)
        return 0;
    if (context_estimate_tokens(messages) <= cfg->max_tokens)
        return 0;

    long budget = cfg->max_tokens -
                  context_estimate_message(json_object_array_get_idx(messages, 0));
    if (budget < 0)
        budget = 0;

    int cut = choose_cut(messages, n, budget, cfg->keep_recent);
    int dropped = cut - 1;
    if (dropped <= 0)
        return 0;

    json_object *doomed = json_object_new_array();
    if (doomed) {
        for (int i = 1; i < cut; i++)
            json_object_array_add(doomed, json_object_get(json_object_array_get_idx(messages, i)));
    }

    char *summary = NULL;
    if (cfg->summarize && summarize)
        summary = summarize(doomed, userdata);

    json_object_array_del_idx(messages, 1, (size_t)dropped);
    if (doomed)
        json_object_put(doomed);

    if (summary && *summary) {
        json_object *sm = json_object_new_object();
        json_object_object_add(sm, "role", json_object_new_string("system"));
        char *content = NULL;
        if (asprintf(&content, "%s\n%s", SUMMARY_PREFIX, summary) < 0)
            content = NULL;
        json_object_object_add(sm, "content",
                               json_object_new_string(content ? content : summary));
        free(content);
        json_object_array_insert_idx(messages, 1, sm);
    }
    free(summary);
    return dropped;
}
