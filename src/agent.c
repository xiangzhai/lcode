#include "agent.h"
#include "http.h"
#include "session.h"
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
    "inspect or modify the workspace. When the user needs to choose between several "
    "options, call the ask_user tool to show an interactive menu instead of listing "
    "choices in plain text. Think step by step, then act. "
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
    const char *prompt = merged ? merged : base;

    char *with_mode = NULL;
    if (cfg->non_interactive) {
        if (asprintf(&with_mode, "%s\n\n%s", prompt,
                     "Non-interactive mode is enabled: the user has delegated decisions "
                     "to you. Do not ask the user questions; choose reasonable defaults "
                     "yourself and complete the task without further input.") < 0)
            with_mode = NULL;
    }

    json_object *sys = json_object_new_object();
    json_object_object_add(sys, "role", json_object_new_string("system"));
    json_object_object_add(sys, "content",
                           json_object_new_string(with_mode ? with_mode : prompt));
    json_object_array_add(messages, sys);
    free(merged);
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

static const char *SESSION_HELP =
    "session commands:\n"
    "  /new [name]      start a new session (saves the current one)\n"
    "  /list            list saved sessions\n"
    "  /switch <name>   save current and open another session\n"
    "  /delete <name>   delete a saved session\n"
    "  /session         show the current session name\n"
    "  /save            save the current session\n"
    "  /help            show this help\n";

static int handle_command(AgentConfig *cfg, char **session_name,
                          json_object **messages, const char *line)
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

    return 0;
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

    tools_set_non_interactive(cfg->non_interactive);

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
    printf("session: %s%s\n", session_name, resumed ? " (resumed)" : "");
    printf("Type your request, Ctrl-D to exit. /help for session commands.\n");

    if (cfg->initial_prompt && *cfg->initial_prompt) {
        json_object *um = json_object_new_object();
        json_object_object_add(um, "role", json_object_new_string("user"));
        json_object_object_add(um, "content",
                               json_object_new_string(cfg->initial_prompt));
        json_object_array_add(messages, um);
        run_turn(cfg, messages, url);
        session_save(session_name, cfg->model, messages);

        if (cfg->non_interactive) {
            json_object_put(messages);
            free(session_name);
            return 0;
        }
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

        if (handle_command(cfg, &session_name, &messages, line)) {
            free(line);
            continue;
        }

        json_object *um = json_object_new_object();
        json_object_object_add(um, "role", json_object_new_string("user"));
        json_object_object_add(um, "content", json_object_new_string(line));
        json_object_array_add(messages, um);
        free(line);

        run_turn(cfg, messages, url);
        session_save(session_name, cfg->model, messages);
    }

    session_save(session_name, cfg->model, messages);
    json_object_put(messages);
    free(session_name);
    return 0;
}
