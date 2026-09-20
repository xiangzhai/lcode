#ifndef LCODE_CONTEXT_H
#define LCODE_CONTEXT_H

#include <json-c/json.h>

typedef struct {
    long max_tokens;  /* prompt token budget; <= 0 disables compaction */
    int keep_recent;  /* trailing messages always kept when compacting */
    int summarize;    /* 1 = summarize dropped history, 0 = truncate it */
} ContextConfig;

/*
 * Produces a replacement summary for the messages about to be dropped. The
 * callee owns nothing passed in and must return a malloc'd string (or NULL on
 * failure, in which case the caller falls back to plain truncation).
 */
typedef char *(*ContextSummarizeFn)(json_object *dropped, void *userdata);

/* Rough token estimate for a message array or a single message. */
long context_estimate_tokens(json_object *messages);
long context_estimate_message(json_object *message);

/*
 * Shrink `*messages` when its estimated size exceeds cfg->max_tokens. The first
 * message (the system prompt) and at least cfg->keep_recent trailing messages
 * are kept; the older prefix is dropped and, when cfg->summarize is set and
 * `summarize` succeeds, replaced by a single system message holding the summary.
 * Never splits a tool call from its result.
 *
 * The array is rebuilt from scratch: on success `*messages` points at a new
 * array and the old one is released. Returns the number of messages removed, or
 * 0 when nothing had to be done (leaving `*messages` untouched).
 */
int context_compact(json_object **messages, const ContextConfig *cfg,
                    ContextSummarizeFn summarize, void *userdata);

#endif
