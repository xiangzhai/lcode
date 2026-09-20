#include "agent.h"
#include "http.h"
#include "interrupt.h"
#include "session.h"
#include "skills.h"
#include "strbuf.h"
#include "tools.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <readline/history.h>
#include <readline/readline.h>

static const char *SYSTEM_PROMPT =
    "You are lcode, a coding agent running in the user's terminal. "
    "You can read and write files, list directories, and run shell commands to help "
    "with software engineering tasks. Use the provided tools whenever you need to "
    "inspect or modify the workspace. Some shell commands are classified as "
    "destructive: lcode asks the user to confirm them before running, and blocks the "
    "most dangerous ones outright. If run_shell reports that a command was refused, do "
    "not retry it unchanged; use a safer approach or explain what you need. "
    "When the user needs to choose between several "
    "options, call the ask_user tool to show an interactive menu instead of listing "
    "choices in plain text. Think step by step, then act. "
    "When the task is complete, reply to the user with a concise summary.";

#define TOOL_OUTPUT_MAX_LINES 10
#define REASONING_MAX_LINES 10

static void print_tool_result(const char *result)
{
    if (!result || !*result)
        return;

    int lines = 0;
    for (const char *p = result; *p; p++)
        if (*p == '\n')
            lines++;
    if (result[strlen(result) - 1] != '\n')
        lines++;

    if (lines <= TOOL_OUTPUT_MAX_LINES) {
        printf("\033[0;90m%s\033[0m\n", result);
        return;
    }

    const char *end = result;
    int seen = 0;
    while (*end && seen < TOOL_OUTPUT_MAX_LINES) {
        if (*end == '\n')
            seen++;
        end++;
    }

    printf("\033[0;90m%.*s\033[0m\n", (int)(end - result), result);
    printf("\033[0;90m... (%d more lines hidden)\033[0m\n",
           lines - TOOL_OUTPUT_MAX_LINES);
}

static void print_tool_call(const char *name, const char *args_str, json_object *args)
{
    if (name && strcmp(name, "write_file") == 0 && args &&
        json_object_get_type(args) == json_type_object) {
        json_object *v;
        const char *path = NULL;
        size_t bytes = 0;
        if (json_object_object_get_ex(args, "path", &v) &&
            json_object_get_type(v) == json_type_string)
            path = json_object_get_string(v);
        if (json_object_object_get_ex(args, "content", &v) &&
            json_object_get_type(v) == json_type_string)
            bytes = strlen(json_object_get_string(v));
        printf("\n\033[1;33m[tool] write_file %s (%zu bytes)\033[0m\n",
               path ? path : "(unknown)", bytes);
        return;
    }

    if (name && strcmp(name, "edit_file") == 0 && args &&
        json_object_get_type(args) == json_type_object) {
        json_object *v;
        const char *path = NULL;
        size_t old_bytes = 0, new_bytes = 0;
        int replace_all = 0;
        if (json_object_object_get_ex(args, "path", &v) &&
            json_object_get_type(v) == json_type_string)
            path = json_object_get_string(v);
        if (json_object_object_get_ex(args, "old_string", &v) &&
            json_object_get_type(v) == json_type_string)
            old_bytes = strlen(json_object_get_string(v));
        if (json_object_object_get_ex(args, "new_string", &v) &&
            json_object_get_type(v) == json_type_string)
            new_bytes = strlen(json_object_get_string(v));
        if (json_object_object_get_ex(args, "replace_all", &v) &&
            json_object_get_type(v) == json_type_boolean)
            replace_all = json_object_get_boolean(v);
        printf("\n\033[1;33m[tool] edit_file %s (%zu -> %zu bytes%s)\033[0m\n",
               path ? path : "(unknown)", old_bytes, new_bytes,
               replace_all ? ", all" : "");
        return;
    }

    printf("\n\033[1;33m[tool] %s %s\033[0m\n", name ? name : "(unknown)", args_str);
}

static void print_reasoning(const char *text)
{
    if (!text || !*text)
        return;

    int lines = 0;
    for (const char *p = text; *p; p++)
        if (*p == '\n')
            lines++;
    if (text[strlen(text) - 1] != '\n')
        lines++;

    printf("\n\033[0;90m[thinking]\033[0m\n");

    if (lines <= REASONING_MAX_LINES) {
        printf("\033[0;90m%s\033[0m\n", text);
        return;
    }

    const char *end = text;
    int seen = 0;
    while (*end && seen < REASONING_MAX_LINES) {
        if (*end == '\n')
            seen++;
        end++;
    }

    printf("\033[0;90m%.*s\033[0m\n", (int)(end - text), text);
    printf("\033[0;90m... (%d more lines hidden)\033[0m\n",
           lines - REASONING_MAX_LINES);
}

static json_object *build_messages(AgentConfig *cfg)
{
    json_object *messages = json_object_new_array();

    const char *base = (cfg->system_prompt && *cfg->system_prompt)
                           ? cfg->system_prompt
                           : SYSTEM_PROMPT;

    char *merged = NULL;
    if (cfg->append_system_prompt && *cfg->append_system_prompt) {
        if (asprintf(&merged, "%s\n\n%s", base, cfg->append_system_prompt) < 0)
            merged = NULL;
    }
    const char *prompt = merged ? merged : base;

    char *skills_prompt = skills_system_prompt();
    char *with_skills = NULL;
    if (skills_prompt) {
        if (asprintf(&with_skills, "%s\n\n%s", prompt, skills_prompt) < 0)
            with_skills = NULL;
    }
    const char *prompt_with_skills = with_skills ? with_skills : prompt;

    char *with_mode = NULL;
    if (cfg->non_interactive) {
        if (asprintf(&with_mode, "%s\n\n%s", prompt_with_skills,
                     "Non-interactive mode is enabled: the user has delegated decisions "
                     "to you. Do not ask the user questions; choose reasonable defaults "
                     "yourself and complete the task without further input.") < 0)
            with_mode = NULL;
    }

    json_object *sys = json_object_new_object();
    json_object_object_add(sys, "role", json_object_new_string("system"));
    json_object_object_add(sys, "content",
                           json_object_new_string(with_mode ? with_mode : prompt_with_skills));
    json_object_array_add(messages, sys);
    free(merged);
    free(skills_prompt);
    free(with_skills);
    free(with_mode);

    return messages;
}

static json_object *build_request(AgentConfig *cfg, json_object *messages)
{
    json_object *req = json_object_new_object();
    json_object_object_add(req, "model", json_object_new_string(cfg->model));
    json_object_object_add(req, "messages", json_object_get(messages));
    json_object_object_add(req, "tools", tools_schema());
    json_object_object_add(req, "tool_choice", json_object_new_string("auto"));

    if (cfg->stream) {
        json_object_object_add(req, "stream", json_object_new_boolean(1));
        json_object *stream_opts = json_object_new_object();
        json_object_object_add(stream_opts, "include_usage", json_object_new_boolean(1));
        json_object_object_add(req, "stream_options", stream_opts);
    }

    json_object *thinking = json_object_new_object();
    json_object_object_add(thinking, "type",
                           json_object_new_string(cfg->thinking ? "enabled" : "disabled"));
    json_object_object_add(req, "thinking", thinking);

    if (cfg->thinking) {
        if (cfg->reasoning_effort && *cfg->reasoning_effort)
            json_object_object_add(req, "reasoning_effort",
                                   json_object_new_string(cfg->reasoning_effort));
    } else {
        json_object_object_add(req, "temperature", json_object_new_double(0.2));
    }
    return req;
}

static const char *json_string_at(json_object *obj, const char *key)
{
    json_object *v;
    if (!obj || json_object_get_type(obj) != json_type_object)
        return NULL;
    if (!json_object_object_get_ex(obj, key, &v) || !v)
        return NULL;
    if (json_object_get_type(v) != json_type_string)
        return NULL;
    return json_object_get_string(v);
}

static json_object *merge_loaded_messages(AgentConfig *cfg, json_object *loaded)
{
    json_object *messages = build_messages(cfg);
    int n = (int)json_object_array_length(loaded);
    int start = 0;
    if (n > 0) {
        const char *role = json_string_at(json_object_array_get_idx(loaded, 0), "role");
        if (role && strcmp(role, "system") == 0)
            start = 1;
    }
    for (int i = start; i < n; i++)
        json_object_array_add(messages, json_object_get(json_object_array_get_idx(loaded, i)));
    return messages;
}

typedef struct {
    long long prompt;
    long long completion;
    long long total;
    long long cache_hit;
    long long cache_miss;
    int requests;
} UsageTotals;

static long long usage_ll(json_object *obj, const char *key)
{
    json_object *v;
    if (!obj || json_object_get_type(obj) != json_type_object)
        return 0;
    if (!json_object_object_get_ex(obj, key, &v) || !v)
        return 0;
    if (json_object_get_type(v) != json_type_int)
        return 0;
    return json_object_get_int64(v);
}

static void usage_track(UsageTotals *acc, json_object *usage)
{
    if (!usage)
        return;

    long long prompt = usage_ll(usage, "prompt_tokens");
    long long completion = usage_ll(usage, "completion_tokens");
    long long total = usage_ll(usage, "total_tokens");
    long long hit = usage_ll(usage, "prompt_cache_hit_tokens");
    long long miss = usage_ll(usage, "prompt_cache_miss_tokens");

    if (hit == 0 && miss == 0 && prompt > 0) {
        json_object *details;
        if (json_object_object_get_ex(usage, "prompt_tokens_details", &details)) {
            hit = usage_ll(details, "cached_tokens");
            if (hit > 0)
                miss = prompt - hit;
        }
    }

    if (prompt == 0 && completion == 0 && total == 0 && hit == 0 && miss == 0)
        return;
    if (total == 0)
        total = prompt + completion;

    acc->prompt += prompt;
    acc->completion += completion;
    acc->total += total;
    acc->cache_hit += hit;
    acc->cache_miss += miss;
    acc->requests++;

    double rate = (hit + miss) > 0 ? (100.0 * (double)hit / (double)(hit + miss)) : 0.0;
    fprintf(stderr,
            "\033[0;90m[usage] prompt %lld (cache hit %lld / miss %lld, %.1f%%), "
            "completion %lld, total %lld\033[0m\n",
            prompt, hit, miss, rate, completion, total);
}

static void print_usage(const UsageTotals *u)
{
    if (u->requests == 0) {
        printf("no API requests yet\n");
        return;
    }
    double rate = (u->cache_hit + u->cache_miss) > 0
                      ? (100.0 * (double)u->cache_hit / (double)(u->cache_hit + u->cache_miss))
                      : 0.0;
    printf("usage over %d request%s:\n", u->requests, u->requests == 1 ? "" : "s");
    printf("  prompt tokens : %lld\n", u->prompt);
    printf("  cache hit     : %lld (%.1f%%)\n", u->cache_hit, rate);
    printf("  cache miss    : %lld\n", u->cache_miss);
    printf("  completion    : %lld\n", u->completion);
    printf("  total         : %lld\n", u->total);
}

enum {
    TURN_OK = 0,
    TURN_ERROR = -1,
    TURN_INTERRUPTED = 1,
};

static const char *SESSION_HELP =
    "session commands:\n"
    "  /new [name]      start a new session (saves the current one)\n"
    "  /list            list saved sessions\n"
    "  /switch <name>   save current and open another session\n"
    "  /delete <name>   delete a saved session\n"
    "  /session         show the current session name\n"
    "  /save            save the current session\n"
    "  /usage           show token usage and cache hit rate\n"
    "  /skills          list available skills\n"
    "  /exit            save and exit (same as Ctrl-D)\n"
    "  /help            show this help\n";

static int handle_command(AgentConfig *cfg, char **session_name,
                          json_object **messages, UsageTotals *usage,
                          const char *line)
{
    if (line[0] != '/')
        return 0;

    char cmd[64];
    size_t clen = strcspn(line, " \t");
    if (clen == 0 || clen >= sizeof(cmd))
        return 0;
    memcpy(cmd, line, clen);
    cmd[clen] = '\0';

    const char *arg = line + clen;
    while (*arg == ' ' || *arg == '\t')
        arg++;

    char name[128] = "";
    if (*arg) {
        snprintf(name, sizeof(name), "%s", arg);
        size_t nlen = strlen(name);
        while (nlen > 0 && (name[nlen - 1] == ' ' || name[nlen - 1] == '\t'))
            name[--nlen] = '\0';
    }

    if (strcmp(cmd, "/help") == 0) {
        fputs(SESSION_HELP, stdout);
        return 1;
    }

    if (strcmp(cmd, "/session") == 0) {
        printf("current session: %s\n", *session_name);
        return 1;
    }

    if (strcmp(cmd, "/list") == 0 || strcmp(cmd, "/sessions") == 0) {
        session_list();
        return 1;
    }

    if (strcmp(cmd, "/usage") == 0) {
        print_usage(usage);
        return 1;
    }

    if (strcmp(cmd, "/skills") == 0) {
        skills_list();
        return 1;
    }

    if (strcmp(cmd, "/save") == 0) {
        if (session_save(*session_name, cfg->model, *messages) == 0)
            printf("saved session '%s'\n", *session_name);
        else
            printf("failed to save session '%s'\n", *session_name);
        return 1;
    }

    if (strcmp(cmd, "/new") == 0) {
        char *newname;
        if (*name) {
            if (!session_validate_name(name)) {
                printf("invalid session name '%s'\n", name);
                return 1;
            }
            newname = strdup(name);
        } else {
            newname = session_generate_name();
        }
        if (!newname) {
            printf("out of memory\n");
            return 1;
        }
        session_save(*session_name, cfg->model, *messages);
        free(*session_name);
        *session_name = newname;
        json_object_put(*messages);
        *messages = build_messages(cfg);
        printf("started new session '%s'\n", *session_name);
        return 1;
    }

    if (strcmp(cmd, "/switch") == 0 || strcmp(cmd, "/resume") == 0) {
        if (!*name) {
            printf("usage: %s <name>\n", cmd);
            return 1;
        }
        if (!session_validate_name(name)) {
            printf("invalid session name '%s'\n", name);
            return 1;
        }
        json_object *loaded = session_load(name);
        if (!loaded) {
            printf("session '%s' not found\n", name);
            return 1;
        }
        session_save(*session_name, cfg->model, *messages);
        free(*session_name);
        *session_name = strdup(name);
        json_object_put(*messages);
        *messages = merge_loaded_messages(cfg, loaded);
        json_object_put(loaded);
        printf("switched to session '%s'\n", *session_name);
        return 1;
    }

    if (strcmp(cmd, "/delete") == 0) {
        if (!*name) {
            printf("usage: /delete <name>\n");
            return 1;
        }
        if (strcmp(name, *session_name) == 0) {
            printf("cannot delete the current session\n");
            return 1;
        }
        if (session_delete(name) == 0)
            printf("deleted session '%s'\n", name);
        else
            printf("session '%s' not found\n", name);
        return 1;
    }

    if (strcmp(cmd, "/exit") == 0)
        return 2;

    return 0;
}

/*
 * Streaming (SSE) reassembly. The API splits one assistant message across many
 * deltas: content and reasoning arrive as text fragments, and tool calls arrive
 * in pieces keyed by an index. Text is rendered as it arrives while the pieces
 * are accumulated into a single assistant message that can be fed back to the
 * API.
 */
typedef struct {
    char *id;
    StrBuf name;
    StrBuf args;
} ToolCallAcc;

typedef struct {
    AgentConfig *cfg;
    StrBuf content;
    StrBuf reasoning;
    ToolCallAcc *calls;
    int ncalls;
    int cap;
    json_object *usage;
    int reasoning_started;
    int content_started;
    int got_reasoning;
    int show_reasoning;
    int think_shown;
    int think_frame;
    struct timespec think_start;
} StreamState;

static ToolCallAcc *stream_call(StreamState *st, int index)
{
    while (st->ncalls <= index) {
        if (st->ncalls == st->cap) {
            int ncap = st->cap ? st->cap * 2 : 4;
            ToolCallAcc *nt = realloc(st->calls, sizeof(*nt) * (size_t)ncap);
            if (!nt)
                return NULL;
            st->calls = nt;
            st->cap = ncap;
        }
        ToolCallAcc *c = &st->calls[st->ncalls++];
        memset(c, 0, sizeof(*c));
        sb_init(&c->name, 32);
        sb_init(&c->args, 64);
    }
    return &st->calls[index];
}

static void stream_apply_tool_delta(StreamState *st, json_object *tc)
{
    json_object *v;
    int index = st->ncalls;
    if (json_object_object_get_ex(tc, "index", &v) &&
        json_object_get_type(v) == json_type_int)
        index = json_object_get_int(v);
    if (index < 0)
        return;

    ToolCallAcc *c = stream_call(st, index);
    if (!c)
        return;

    if (json_object_object_get_ex(tc, "id", &v) &&
        json_object_get_type(v) == json_type_string) {
        free(c->id);
        c->id = strdup(json_object_get_string(v));
    }

    json_object *fn;
    if (json_object_object_get_ex(tc, "function", &fn) && fn) {
        if (json_object_object_get_ex(fn, "name", &v) &&
            json_object_get_type(v) == json_type_string)
            sb_add(&c->name, json_object_get_string(v));
        if (json_object_object_get_ex(fn, "arguments", &v) &&
            json_object_get_type(v) == json_type_string)
            sb_add(&c->args, json_object_get_string(v));
    }
}

/*
 * While the model is only producing reasoning, keep the screen to a single
 * status line that refreshes in place instead of scrolling the whole chain of
 * thought past the user. It is cleared as soon as real content or a tool call
 * has something to show.
 */
static void stream_show_thinking(StreamState *st)
{
    if (st->content_started || !isatty(STDOUT_FILENO))
        return;

    static const char frames[] = "|/-\\";
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double secs = (double)(now.tv_sec - st->think_start.tv_sec) +
                  (double)(now.tv_nsec - st->think_start.tv_nsec) / 1e9;

    fprintf(stdout, "\r\033[2K\033[0;36m%c thinking... %.1fs\033[0m",
            frames[st->think_frame++ & 3], secs);
    fflush(stdout);
    st->think_shown = 1;
}

static void stream_clear_thinking(StreamState *st)
{
    if (!st->think_shown)
        return;
    fputs("\r\033[2K", stdout);
    fflush(stdout);
    st->think_shown = 0;
}

static void stream_apply_delta(StreamState *st, json_object *delta)
{
    json_object *v;

    if (json_object_object_get_ex(delta, "reasoning_content", &v) &&
        json_object_get_type(v) == json_type_string) {
        const char *s = json_object_get_string(v);
        if (*s) {
            if (st->show_reasoning) {
                if (!st->reasoning_started) {
                    printf("\n\033[0;90m[thinking]\033[0m\n");
                    st->reasoning_started = 1;
                }
                printf("\033[0;90m%s\033[0m", s);
                fflush(stdout);
            } else {
                stream_show_thinking(st);
            }
            sb_add(&st->reasoning, s);
            st->got_reasoning = 1;
        }
    }

    if (json_object_object_get_ex(delta, "content", &v) &&
        json_object_get_type(v) == json_type_string) {
        const char *s = json_object_get_string(v);
        if (*s) {
            stream_clear_thinking(st);
            if (!st->content_started) {
                printf("\n");
                st->content_started = 1;
            }
            fputs(s, stdout);
            fflush(stdout);
            sb_add(&st->content, s);
        }
    }

    json_object *tcs;
    if (json_object_object_get_ex(delta, "tool_calls", &tcs) &&
        json_object_get_type(tcs) == json_type_array) {
        int n = (int)json_object_array_length(tcs);
        for (int i = 0; i < n; i++)
            stream_apply_tool_delta(st, json_object_array_get_idx(tcs, i));
    }
}

static void stream_on_event(const char *data, void *userdata)
{
    StreamState *st = userdata;
    json_object *root = json_tokener_parse(data);
    if (!root)
        return;

    json_object *usage;
    if (json_object_object_get_ex(root, "usage", &usage) && usage &&
        json_object_get_type(usage) == json_type_object) {
        if (st->usage)
            json_object_put(st->usage);
        st->usage = json_object_get(usage);
    }

    json_object *choices;
    if (json_object_object_get_ex(root, "choices", &choices) &&
        json_object_get_type(choices) == json_type_array &&
        json_object_array_length(choices) > 0) {
        json_object *choice = json_object_array_get_idx(choices, 0);
        json_object *delta;
        if (json_object_object_get_ex(choice, "delta", &delta) && delta)
            stream_apply_delta(st, delta);
    }

    json_object_put(root);
}

static json_object *stream_build_message(AgentConfig *cfg, StreamState *st)
{
    json_object *msg = json_object_new_object();
    json_object_object_add(msg, "role", json_object_new_string("assistant"));

    if (cfg->thinking && st->got_reasoning)
        json_object_object_add(msg, "reasoning_content",
                               json_object_new_string(st->reasoning.buf ? st->reasoning.buf : ""));

    json_object_object_add(msg, "content",
                           json_object_new_string(st->content.buf ? st->content.buf : ""));

    if (st->ncalls > 0) {
        json_object *tcs = json_object_new_array();
        for (int i = 0; i < st->ncalls; i++) {
            ToolCallAcc *c = &st->calls[i];
            json_object *tc = json_object_new_object();
            json_object_object_add(tc, "id", json_object_new_string(c->id ? c->id : ""));
            json_object_object_add(tc, "type", json_object_new_string("function"));
            json_object *fn = json_object_new_object();
            json_object_object_add(fn, "name",
                                   json_object_new_string(c->name.buf ? c->name.buf : ""));
            json_object_object_add(fn, "arguments",
                                   json_object_new_string(c->args.buf ? c->args.buf : ""));
            json_object_object_add(tc, "function", fn);
            json_object_array_add(tcs, tc);
        }
        json_object_object_add(msg, "tool_calls", tcs);
    }
    return msg;
}

static void stream_state_free(StreamState *st)
{
    sb_free(&st->content);
    sb_free(&st->reasoning);
    for (int i = 0; i < st->ncalls; i++) {
        free(st->calls[i].id);
        sb_free(&st->calls[i].name);
        sb_free(&st->calls[i].args);
    }
    free(st->calls);
    if (st->usage)
        json_object_put(st->usage);
    memset(st, 0, sizeof(*st));
}

static void stream_end_line(StreamState *st)
{
    if (st->think_shown) {
        stream_clear_thinking(st);
        fputc('\n', stdout);
        fflush(stdout);
        return;
    }
    if (st->reasoning_started || st->content_started) {
        fputc('\n', stdout);
        fflush(stdout);
    }
}

static int run_turn(AgentConfig *cfg, json_object *messages, const char *url,
                    UsageTotals *usage)
{
    interrupt_clear();
    for (int iter = 0;; iter++) {
        if (cfg->max_iterations > 0 && iter > 0 && iter % cfg->max_iterations == 0)
            fprintf(stderr,
                    "\033[1;33m[notice]\033[0m reached %d tool-call rounds, "
                    "continuing until the model stops calling tools\n",
                    cfg->max_iterations);
        if (interrupt_pending())
            return TURN_INTERRUPTED;

        json_object *req = build_request(cfg, messages);
        const char *body = json_object_to_json_string_ext(req, JSON_C_TO_STRING_PLAIN);

        json_object *message = NULL;

        if (cfg->stream) {
            StreamState st;
            memset(&st, 0, sizeof(st));
            st.show_reasoning = cfg->show_reasoning;
            clock_gettime(CLOCK_MONOTONIC, &st.think_start);

            HttpResponse res = http_post_json_stream(url, cfg->api_key, body,
                                                     stream_on_event, &st);
            json_object_put(req);

            if (res.interrupted) {
                stream_end_line(&st);
                stream_state_free(&st);
                http_response_free(&res);
                return TURN_INTERRUPTED;
            }
            if (res.error) {
                stream_end_line(&st);
                fprintf(stderr, "request failed: %s\n", res.error);
                stream_state_free(&st);
                http_response_free(&res);
                return TURN_ERROR;
            }
            if (res.status < 200 || res.status >= 300) {
                stream_end_line(&st);
                fprintf(stderr, "API error (HTTP %ld): %s\n", res.status,
                        res.body ? res.body : "");
                stream_state_free(&st);
                http_response_free(&res);
                return TURN_ERROR;
            }
            http_response_free(&res);

            stream_end_line(&st);
            if (st.usage)
                usage_track(usage, st.usage);

            message = stream_build_message(cfg, &st);
            stream_state_free(&st);
        } else {
            HttpResponse res = http_post_json(url, cfg->api_key, body);
            json_object_put(req);

            if (res.interrupted) {
                http_response_free(&res);
                return TURN_INTERRUPTED;
            }
            if (res.error) {
                fprintf(stderr, "request failed: %s\n", res.error);
                http_response_free(&res);
                return TURN_ERROR;
            }
            if (res.status < 200 || res.status >= 300) {
                fprintf(stderr, "API error (HTTP %ld): %s\n", res.status,
                        res.body ? res.body : "");
                http_response_free(&res);
                return TURN_ERROR;
            }

            json_object *root = json_tokener_parse(res.body);
            http_response_free(&res);
            if (!root) {
                fprintf(stderr, "failed to parse API response\n");
                return TURN_ERROR;
            }

            json_object *usage_obj;
            if (json_object_object_get_ex(root, "usage", &usage_obj))
                usage_track(usage, usage_obj);

            json_object *choices, *choice;
            if (!json_object_object_get_ex(root, "choices", &choices) ||
                json_object_get_type(choices) != json_type_array ||
                json_object_array_length(choices) == 0) {
                fprintf(stderr, "unexpected response: %s\n",
                        json_object_to_json_string(root));
                json_object_put(root);
                return TURN_ERROR;
            }
            choice = json_object_array_get_idx(choices, 0);
            json_object *m;
            if (!json_object_object_get_ex(choice, "message", &m)) {
                fprintf(stderr, "response has no message\n");
                json_object_put(root);
                return TURN_ERROR;
            }

            if (cfg->thinking && cfg->show_reasoning)
                print_reasoning(json_string_at(m, "reasoning_content"));

            const char *text = json_string_at(m, "content");
            if (text && *text)
                printf("\n%s\n", text);

            message = json_object_get(m);
            json_object_put(root);
        }

        json_object *tool_calls;
        int has_tools = json_object_object_get_ex(message, "tool_calls", &tool_calls) &&
                        tool_calls &&
                        json_object_get_type(tool_calls) == json_type_array &&
                        json_object_array_length(tool_calls) > 0;

        /* The array takes over the message reference in both branches below. */
        json_object_array_add(messages, message);

        if (!has_tools)
            return TURN_OK;

        int n = (int)json_object_array_length(tool_calls);
        ToolCallJob *jobs = calloc((size_t)n, sizeof(*jobs));
        if (!jobs) {
            fprintf(stderr, "error: out of memory\n");
            return TURN_ERROR;
        }

        /* Independent calls run concurrently, so every call is announced first
           and the results are printed afterwards in the original order. */
        for (int t = 0; t < n; t++) {
            json_object *tc = json_object_array_get_idx(tool_calls, t);
            const char *name = NULL;
            const char *args_str = "{}";

            json_object *fn;
            if (json_object_object_get_ex(tc, "function", &fn) && fn) {
                name = json_string_at(fn, "name");
                const char *a = json_string_at(fn, "arguments");
                if (a)
                    args_str = a;
            }

            jobs[t].name = name;
            jobs[t].args = json_tokener_parse(args_str);
            print_tool_call(name, args_str, jobs[t].args);
        }

        int executed = tools_execute_batch(jobs, n);

        for (int t = 0; t < n; t++) {
            json_object *tc = json_object_array_get_idx(tool_calls, t);
            const char *id = json_string_at(tc, "id");
            const char *content = "error: interrupted by user";
            char *result = jobs[t].result;

            if (t < executed) {
                if (!jobs[t].streamed)
                    print_tool_result(result);
                content = result ? result : "";
            }

            json_object *tm = json_object_new_object();
            json_object_object_add(tm, "role", json_object_new_string("tool"));
            json_object_object_add(tm, "tool_call_id", json_object_new_string(id ? id : ""));
            json_object_object_add(tm, "content", json_object_new_string(content));
            json_object_array_add(messages, tm);

            if (jobs[t].args)
                json_object_put(jobs[t].args);
            free(result);
        }
        free(jobs);

        if (executed < n)
            return TURN_INTERRUPTED;
    }
}

static int run_turn_with_redirect(AgentConfig *cfg, json_object *messages,
                                  const char *url, UsageTotals *usage)
{
    while (1) {
        int rc = run_turn(cfg, messages, url, usage);
        if (rc != TURN_INTERRUPTED)
            return rc;

        interrupt_clear();
        fprintf(stderr,
                "\n\033[1;33m[interrupted]\033[0m the model was stopped. "
                "给出一条新的指示来改变思路（直接回车跳过）。\n");
        if (cfg->non_interactive)
            return rc;

        char *line = readline("\001\033[1;33m\002redirect> \001\033[0m\002");
        if (!line || !*line) {
            free(line);
            return rc;
        }
        add_history(line);

        json_object *um = json_object_new_object();
        json_object_object_add(um, "role", json_object_new_string("user"));
        json_object_object_add(um, "content", json_object_new_string(line));
        json_object_array_add(messages, um);
        free(line);
        interrupt_clear();
    }
}

int agent_run(AgentConfig *cfg)
{
    char url[1024];
    snprintf(url, sizeof(url), "%s/chat/completions", cfg->base_url);

    tools_set_non_interactive(cfg->non_interactive);
    tools_set_shell_timeout(cfg->shell_timeout);
    skills_reload();

    char *session_name = NULL;
    if (cfg->session && *cfg->session) {
        if (!session_validate_name(cfg->session)) {
            fprintf(stderr, "error: invalid session name '%s'\n", cfg->session);
            return 1;
        }
        session_name = strdup(cfg->session);
    } else {
        session_name = session_generate_name();
    }
    if (!session_name) {
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }

    UsageTotals usage = {0};

    int resumed = 0;
    json_object *messages = NULL;
    json_object *loaded = session_load(session_name);
    if (loaded) {
        messages = merge_loaded_messages(cfg, loaded);
        json_object_put(loaded);
        resumed = 1;
    } else {
        messages = build_messages(cfg);
    }

    printf("lcode - coding agent (%s)\n", cfg->model);
    if (cfg->thinking && cfg->reasoning_effort && *cfg->reasoning_effort)
        printf("thinking: enabled (effort %s)\n", cfg->reasoning_effort);
    else if (cfg->thinking)
        printf("thinking: enabled\n");
    else
        printf("thinking: disabled\n");
    printf("session: %s%s\n", session_name, resumed ? " (resumed)" : "");
    printf("Type your request, Ctrl-D to exit. Press ESC to interrupt the model. "
           "/help for session commands.\n");

    if (cfg->initial_prompt && *cfg->initial_prompt) {
        json_object *um = json_object_new_object();
        json_object_object_add(um, "role", json_object_new_string("user"));
        json_object_object_add(um, "content",
                               json_object_new_string(cfg->initial_prompt));
        json_object_array_add(messages, um);
        run_turn_with_redirect(cfg, messages, url, &usage);
        session_save(session_name, cfg->model, messages);

        if (cfg->non_interactive) {
            json_object_put(messages);
            free(session_name);
            return 0;
        }
    }

    while (1) {
        /* \001/\002 mark the ANSI escapes as non-printing so readline
           computes the prompt width correctly when the line wraps. */
        char *line = readline("\n\001\033[1;36m\002> \001\033[0m\002");
        if (!line) {
            printf("\n");
            break;
        }
        if (*line == '\0') {
            free(line);
            continue;
        }
        add_history(line);

        int handled = handle_command(cfg, &session_name, &messages, &usage, line);
        if (handled == 2) {
            free(line);
            break;
        }
        if (handled == 1) {
            free(line);
            continue;
        }

        json_object *um = json_object_new_object();
        json_object_object_add(um, "role", json_object_new_string("user"));
        json_object_object_add(um, "content", json_object_new_string(line));
        json_object_array_add(messages, um);
        free(line);

        run_turn_with_redirect(cfg, messages, url, &usage);
        session_save(session_name, cfg->model, messages);
    }

    session_save(session_name, cfg->model, messages);
    json_object_put(messages);
    free(session_name);
    return 0;
}
