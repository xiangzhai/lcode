#include "agent.h"
#include "http.h"
#include "tools.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <readline/history.h>
#include <readline/readline.h>

static const char *SYSTEM_PROMPT =
    "You are lcode, a coding agent running in the user's terminal. "
    "You can read and write files, list directories, and run shell commands to help "
    "with software engineering tasks. Use the provided tools whenever you need to "
    "inspect or modify the workspace. Think step by step, then act. "
    "When the task is complete, reply to the user with a concise summary.";

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

    json_object *sys = json_object_new_object();
    json_object_object_add(sys, "role", json_object_new_string("system"));
    json_object_object_add(sys, "content",
                           json_object_new_string(merged ? merged : base));
    json_object_array_add(messages, sys);
    free(merged);

    return messages;
}

static json_object *build_request(AgentConfig *cfg, json_object *messages)
{
    json_object *req = json_object_new_object();
    json_object_object_add(req, "model", json_object_new_string(cfg->model));
    json_object_object_add(req, "messages", json_object_get(messages));
    json_object_object_add(req, "tools", tools_schema());
    json_object_object_add(req, "tool_choice", json_object_new_string("auto"));
    json_object_object_add(req, "temperature", json_object_new_double(0.2));
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

static int run_turn(AgentConfig *cfg, json_object *messages, const char *url)
{
    for (int iter = 0; iter < cfg->max_iterations; iter++) {
        json_object *req = build_request(cfg, messages);
        const char *body = json_object_to_json_string_ext(req, JSON_C_TO_STRING_PLAIN);

        HttpResponse res = http_post_json(url, cfg->api_key, body);
        json_object_put(req);

        if (res.error) {
            fprintf(stderr, "request failed: %s\n", res.error);
            http_response_free(&res);
            return -1;
        }
        if (res.status < 200 || res.status >= 300) {
            fprintf(stderr, "API error (HTTP %ld): %s\n", res.status, res.body ? res.body : "");
            http_response_free(&res);
            return -1;
        }

        json_object *root = json_tokener_parse(res.body);
        http_response_free(&res);
        if (!root) {
            fprintf(stderr, "failed to parse API response\n");
            return -1;
        }

        json_object *choices, *choice, *message;
        if (!json_object_object_get_ex(root, "choices", &choices) ||
            json_object_get_type(choices) != json_type_array ||
            json_object_array_length(choices) == 0) {
            fprintf(stderr, "unexpected response: %s\n", json_object_to_json_string(root));
            json_object_put(root);
            return -1;
        }
        choice = json_object_array_get_idx(choices, 0);
        if (!json_object_object_get_ex(choice, "message", &message)) {
            fprintf(stderr, "response has no message\n");
            json_object_put(root);
            return -1;
        }

        const char *text = json_string_at(message, "content");
        if (text && *text)
            printf("\n%s\n", text);

        json_object *tool_calls;
        if (!json_object_object_get_ex(message, "tool_calls", &tool_calls) ||
            !tool_calls ||
            json_object_get_type(tool_calls) != json_type_array ||
            json_object_array_length(tool_calls) == 0) {
            json_object_array_add(messages, json_object_get(message));
            json_object_put(root);
            return 0;
        }

        json_object_array_add(messages, json_object_get(message));

        int n = json_object_array_length(tool_calls);
        for (int t = 0; t < n; t++) {
            json_object *tc = json_object_array_get_idx(tool_calls, t);
            const char *id = json_string_at(tc, "id");
            const char *name = NULL;
            const char *args_str = "{}";

            json_object *fn;
            if (json_object_object_get_ex(tc, "function", &fn) && fn) {
                name = json_string_at(fn, "name");
                const char *a = json_string_at(fn, "arguments");
                if (a)
                    args_str = a;
            }

            printf("\n\033[1;33m[tool] %s %s\033[0m\n", name ? name : "(unknown)", args_str);

            json_object *args = json_tokener_parse(args_str);
            char *result = tools_execute(name, args);
            if (args)
                json_object_put(args);

            if (result && *result)
                printf("\033[0;90m%s\033[0m\n", result);

            json_object *tm = json_object_new_object();
            json_object_object_add(tm, "role", json_object_new_string("tool"));
            json_object_object_add(tm, "tool_call_id", json_object_new_string(id ? id : ""));
            json_object_object_add(tm, "content", json_object_new_string(result ? result : ""));
            json_object_array_add(messages, tm);
            free(result);
        }

        json_object_put(root);
    }

    fprintf(stderr, "reached max tool iterations (%d)\n", cfg->max_iterations);
    return 0;
}

int agent_run(AgentConfig *cfg)
{
    char url[1024];
    snprintf(url, sizeof(url), "%s/chat/completions", cfg->base_url);

    json_object *messages = build_messages(cfg);

    printf("lcode - coding agent (%s)\n", cfg->model);
    printf("Type your request, Ctrl-D to exit.\n");

    if (cfg->initial_prompt && *cfg->initial_prompt) {
        json_object *um = json_object_new_object();
        json_object_object_add(um, "role", json_object_new_string("user"));
        json_object_object_add(um, "content",
                               json_object_new_string(cfg->initial_prompt));
        json_object_array_add(messages, um);
        run_turn(cfg, messages, url);
    }

    while (1) {
        char *line = readline("\n\033[1;36m> \033[0m");
        if (!line) {
            printf("\n");
            break;
        }
        if (*line == '\0') {
            free(line);
            continue;
        }
        add_history(line);

        json_object *um = json_object_new_object();
        json_object_object_add(um, "role", json_object_new_string("user"));
        json_object_object_add(um, "content", json_object_new_string(line));
        json_object_array_add(messages, um);
        free(line);

        run_turn(cfg, messages, url);
    }

    json_object_put(messages);
    return 0;
}
