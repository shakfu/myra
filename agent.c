/* agent.c - minimal code agent: an LLM API + read/write/edit/shell. */
#include <cJSON.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_TOKENS 16000 /* Anthropic only; chat completions uses the server default */
#define MAX_OUTPUT (100 * 1024) /* cap on one tool result, in bytes */
#define RETRIES 4

static const char *TOOLS_JSON =
    "[{\"name\":\"read\",\"description\":\"Read a text file and return its contents.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"],\"additionalProperties\":false}},"
    "{\"name\":\"write\",\"description\":\"Create or overwrite a file with the given content.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"],"
    "\"additionalProperties\":false}},"
    "{\"name\":\"edit\",\"description\":\"Replace old_string with new_string in a file. "
    "old_string must occur exactly once.\",\"input_schema\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"},\"old_string\":{\"type\":\"string\"},"
    "\"new_string\":{\"type\":\"string\"}},\"required\":[\"path\",\"old_string\",\"new_string\"],"
    "\"additionalProperties\":false}},"
    "{\"name\":\"shell\",\"description\":\"Run a command with /bin/sh in the working directory. "
    "Returns combined stdout and stderr and the exit status. stdin is /dev/null.\","
    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},"
    "\"required\":[\"command\"],\"additionalProperties\":false}}]";

/* anthropic speaks the Messages API; the rest speak OpenAI chat completions. */
typedef struct {
    const char *name, *key_env, *base_env, *base, *path, *model;
    int anthropic, key_optional;
} provider;

static const provider PROVIDERS[] = {
    {"openrouter", "OPENROUTER_API_KEY", "OPENROUTER_BASE_URL", "https://openrouter.ai/api/v1",
     "/chat/completions", "anthropic/claude-opus-5", 0, 0},
    {"anthropic", "ANTHROPIC_API_KEY", "ANTHROPIC_BASE_URL", "https://api.anthropic.com",
     "/v1/messages", "claude-opus-5", 1, 0},
    {"openai", "OPENAI_API_KEY", "OPENAI_BASE_URL", "https://api.openai.com/v1",
     "/chat/completions", NULL, 0, 0},
    /* Any OpenAI-compatible server; the default URL is llama-server's. */
    {"compat", "COMPAT_API_KEY", "COMPAT_BASE_URL", "http://localhost:8080/v1",
     "/chat/completions", "local", 0, 1},
};

static const provider *prov;
static const char *model, *api_key;
static int auto_yes; /* -y: run mutating tools without asking */
static cJSON *tools; /* in the provider's wire format */
static char system_prompt[4096 + 256];

/* ---- growable buffer ---- */

typedef struct { char *p; size_t n, cap; } buf;

static void buf_add(buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = realloc(b->p, b->cap);
        if (!b->p) { perror("realloc"); exit(1); }
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static char *xstrdup(const char *s) { buf b = {0}; buf_add(&b, s, strlen(s)); return b.p; }

static char *fmt(const char *f, const char *a) {
    size_t n = strlen(f) + strlen(a) + 1;
    char *s = malloc(n);
    snprintf(s, n, f, a);
    return s;
}

/* Truncate to MAX_OUTPUT and say so, rather than drop bytes silently. */
static char *capped(buf *b) {
    if (!b->p) return xstrdup("");
    if (b->n > MAX_OUTPUT) {
        char note[96];
        snprintf(note, sizeof note, "\n[truncated: %zu bytes total, first %d shown]", b->n, MAX_OUTPUT);
        b->n = MAX_OUTPUT;
        buf_add(b, note, strlen(note));
    }
    return b->p;
}

static int slurp(const char *path, buf *b) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_add(b, chunk, n);
    int err = ferror(f);
    fclose(f);
    if (!b->p) buf_add(b, "", 0);
    return err ? -1 : 0;
}

static int spit(const char *path, const char *s, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(s, 1, n, f) == n;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

/* ---- tools: each returns a malloc'd result and sets *err on failure ---- */

static char *tool_read(const char *path, int *err) {
    buf b = {0};
    if (slurp(path, &b) < 0) { free(b.p); *err = 1; return fmt("cannot read %s", path); }
    if (memchr(b.p, 0, b.n)) { free(b.p); *err = 1; return fmt("%s is binary", path); }
    return capped(&b);
}

static char *tool_write(const char *path, const char *content, int *err) {
    if (spit(path, content, strlen(content)) < 0) { *err = 1; return fmt("cannot write %s", path); }
    return fmt("wrote %s", path);
}

static char *tool_edit(const char *path, const char *old, const char *new, int *err) {
    *err = 1;
    if (!*old) return xstrdup("old_string is empty");
    buf b = {0};
    if (slurp(path, &b) < 0) { free(b.p); return fmt("cannot read %s", path); }
    char *hit = strstr(b.p, old);
    if (!hit) { free(b.p); return fmt("old_string not found in %s", path); }
    if (strstr(hit + 1, old)) { free(b.p); return fmt("old_string occurs more than once in %s", path); }
    buf out = {0};
    size_t pre = hit - b.p, olen = strlen(old);
    buf_add(&out, b.p, pre);
    buf_add(&out, new, strlen(new));
    buf_add(&out, hit + olen, b.n - pre - olen);
    int rc = spit(path, out.p, out.n);
    free(b.p);
    free(out.p);
    if (rc < 0) return fmt("cannot write %s", path);
    *err = 0;
    return fmt("edited %s", path);
}

static char *tool_shell(const char *cmd, int *err) {
    /* Braces and newline keep a trailing comment in cmd from eating the redirects. */
    char *wrapped = fmt("{ %s\n} </dev/null 2>&1", cmd);
    FILE *p = popen(wrapped, "r");
    free(wrapped);
    if (!p) { *err = 1; return xstrdup("popen failed"); }
    buf b = {0};
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, p)) > 0) buf_add(&b, chunk, n);
    int st = pclose(p);
    char tail[32];
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    snprintf(tail, sizeof tail, "%s[exit %d]", b.n && b.p[b.n - 1] != '\n' ? "\n" : "", code);
    char *out = capped(&b);
    buf r = {0};
    buf_add(&r, out, strlen(out));
    buf_add(&r, tail, strlen(tail));
    free(out);
    *err = code != 0;
    return r.p;
}

/* Ask on the controlling terminal so it works in headless mode and the REPL alike. */
static int confirm(const char *name, const char *detail) {
    if (auto_yes) return 1;
    FILE *tty = fopen("/dev/tty", "r+");
    if (!tty) return 0;
    fprintf(tty, "allow %s: %s ? [y/N] ", name, detail);
    fflush(tty);
    char line[16] = {0};
    int ok = fgets(line, sizeof line, tty) && (line[0] == 'y' || line[0] == 'Y');
    fclose(tty);
    return ok;
}

static const char *str_arg(cJSON *input, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(input, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static char *run_tool(const char *name, cJSON *input, int *err) {
    const char *path = str_arg(input, "path");
    const char *content = str_arg(input, "content");
    const char *old = str_arg(input, "old_string"), *new = str_arg(input, "new_string");
    const char *cmd = str_arg(input, "command");
    *err = 1;
    if (!strcmp(name, "read") && path) {
        fprintf(stderr, "> read %s\n", path);
        *err = 0;
        return tool_read(path, err);
    }
    if (!strcmp(name, "write") && path && content) {
        fprintf(stderr, "> write %s\n", path);
        if (!confirm("write", path)) return xstrdup("denied by user");
        *err = 0;
        return tool_write(path, content, err);
    }
    if (!strcmp(name, "edit") && path && old && new) {
        fprintf(stderr, "> edit %s\n", path);
        if (!confirm("edit", path)) return xstrdup("denied by user");
        return tool_edit(path, old, new, err);
    }
    if (!strcmp(name, "shell") && cmd) {
        fprintf(stderr, "> shell %s\n", cmd);
        if (!confirm("shell", cmd)) return xstrdup("denied by user");
        return tool_shell(cmd, err);
    }
    return fmt("unknown tool or missing arguments: %s", name);
}

/* ---- API ---- */

static size_t on_body(char *p, size_t sz, size_t n, void *ud) {
    buf_add(ud, p, sz * n);
    return sz * n;
}

static char *request_body(cJSON *messages) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", model);
    if (prov->anthropic) {
        cJSON_AddNumberToObject(req, "max_tokens", MAX_TOKENS);
        cJSON_AddStringToObject(req, "system", system_prompt);
        cJSON_AddItemToObject(req, "cache_control", cJSON_Parse("{\"type\":\"ephemeral\"}"));
        cJSON_AddStringToObject(req, "fallbacks", "default"); /* reroute refusals server-side */
    }
    cJSON_AddItemReferenceToObject(req, "tools", tools);
    cJSON_AddItemReferenceToObject(req, "messages", messages);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return body;
}

static cJSON *call_api(cJSON *messages) {
    const char *base = getenv(prov->base_env);
    buf url = {0};
    base = base && *base ? base : prov->base;
    buf_add(&url, base, strlen(base));
    buf_add(&url, prov->path, strlen(prov->path));
    char *body = request_body(messages);

    struct curl_slist *h = curl_slist_append(NULL, "content-type: application/json");
    char *auth = NULL;
    if (prov->anthropic) {
        h = curl_slist_append(h, "anthropic-version: 2023-06-01");
        h = curl_slist_append(h, "anthropic-beta: server-side-fallback-2026-07-01");
        auth = fmt("x-api-key: %s", api_key);
    } else if (api_key && *api_key) {
        auth = fmt("authorization: Bearer %s", api_key);
    }
    if (auth) h = curl_slist_append(h, auth);

    cJSON *resp = NULL;
    for (int attempt = 0; attempt <= RETRIES; attempt++) {
        if (attempt) sleep(1u << (attempt - 1));
        buf out = {0};
        CURL *c = curl_easy_init();
        curl_easy_setopt(c, CURLOPT_URL, url.p);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 600L);
        CURLcode rc = curl_easy_perform(c);
        long status = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(c);
        if (rc == CURLE_OK && status == 200) {
            resp = cJSON_Parse(out.p ? out.p : "");
            if (!resp) fprintf(stderr, "error: response is not JSON\n");
            free(out.p);
            break;
        }
        int retryable = rc != CURLE_OK || status == 429 || status >= 500;
        fprintf(stderr, "api error (%s, http %ld)%s: %s\n", curl_easy_strerror(rc), status,
                retryable && attempt < RETRIES ? ", retrying" : "", out.p ? out.p : "");
        free(out.p);
        if (!retryable) break;
    }
    curl_slist_free_all(h);
    free(auth);
    free(body);
    free(url.p);
    return resp;
}

static void print_text(const char *s) {
    if (!s || !*s) return;
    printf("%s\n", s);
    fflush(stdout);
}

static const char *get_str(cJSON *o, const char *key) {
    return cJSON_GetStringValue(cJSON_GetObjectItem(o, key));
}

static void add_message(cJSON *messages, const char *role, cJSON *content) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", role);
    cJSON_AddItemToObject(m, "content", content);
    cJSON_AddItemToArray(messages, m);
}

/* Each step consumes one response: -1 failed, 0 done, 1 tools ran and the loop continues. */

static int step_anthropic(cJSON *resp, cJSON *messages) {
    const char *stop = get_str(resp, "stop_reason");
    stop = stop ? stop : "";
    if (!strcmp(stop, "refusal")) {
        const char *why = get_str(cJSON_GetObjectItem(resp, "stop_details"), "explanation");
        fprintf(stderr, "refused: %s\n", why ? why : "(no explanation)");
        return -1;
    }
    cJSON *content = cJSON_DetachItemFromObject(resp, "content");
    if (!cJSON_IsArray(content)) { cJSON_Delete(content); content = cJSON_CreateArray(); }
    int want_tools = !strcmp(stop, "tool_use");
    cJSON *results = cJSON_CreateArray();
    for (int i = 0; i < cJSON_GetArraySize(content);) {
        cJSON *b = cJSON_GetArrayItem(content, i);
        const char *type = get_str(b, "type");
        if (type && !strcmp(type, "text")) {
            print_text(get_str(b, "text"));
        } else if (type && !strcmp(type, "tool_use")) {
            /* A tool_use without stop_reason tool_use (e.g. max_tokens) may be
               truncated and would need a tool_result the next request lacks. */
            if (!want_tools) { cJSON_DeleteItemFromArray(content, i); continue; }
            int err = 0;
            const char *name = get_str(b, "name");
            char *out = run_tool(name ? name : "", cJSON_GetObjectItem(b, "input"), &err);
            cJSON *r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "type", "tool_result");
            cJSON_AddStringToObject(r, "tool_use_id", get_str(b, "id"));
            cJSON_AddStringToObject(r, "content", out);
            if (err) cJSON_AddBoolToObject(r, "is_error", 1);
            cJSON_AddItemToArray(results, r);
            free(out);
        }
        i++;
    }
    if (!strcmp(stop, "max_tokens")) fprintf(stderr, "warning: hit max_tokens\n");
    if (cJSON_GetArraySize(content)) add_message(messages, "assistant", content);
    else cJSON_Delete(content);
    if (!cJSON_GetArraySize(results)) { cJSON_Delete(results); return 0; }
    add_message(messages, "user", results); /* all results in one message */
    return 1;
}

static int step_openai(cJSON *resp, cJSON *messages) {
    cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItem(resp, "choices"), 0);
    cJSON *msg = cJSON_DetachItemFromObject(choice, "message");
    if (!cJSON_IsObject(msg)) {
        char *s = cJSON_PrintUnformatted(resp);
        fprintf(stderr, "error: no message in response: %s\n", s);
        free(s);
        cJSON_Delete(msg);
        return -1;
    }
    const char *finish = get_str(choice, "finish_reason");
    finish = finish ? finish : "";
    if (!strcmp(finish, "content_filter")) {
        fprintf(stderr, "refused: content_filter\n");
        cJSON_Delete(msg);
        return -1;
    }
    /* Same reasoning as max_tokens above: calls cut off by length are dropped. */
    if (!strcmp(finish, "length")) {
        fprintf(stderr, "warning: hit max_tokens\n");
        cJSON_DeleteItemFromObject(msg, "tool_calls");
    }
    print_text(get_str(msg, "content"));
    cJSON *calls = cJSON_GetObjectItem(msg, "tool_calls");
    int ncalls = cJSON_GetArraySize(calls);
    if (!ncalls && !get_str(msg, "content")) { /* null content is only valid with tool_calls */
        cJSON_DeleteItemFromObject(msg, "tool_calls");
        cJSON_DeleteItemFromObject(msg, "content");
        cJSON_AddStringToObject(msg, "content", "");
    }
    cJSON_AddItemToArray(messages, msg);
    for (int i = 0; i < ncalls; i++) {
        cJSON *call = cJSON_GetArrayItem(calls, i), *fn = cJSON_GetObjectItem(call, "function");
        const char *name = get_str(fn, "name"), *args = get_str(fn, "arguments");
        cJSON *input = cJSON_Parse(args ? args : "");
        int err = 0;
        char *out = cJSON_IsObject(input) ? run_tool(name ? name : "", input, &err)
                                          : (err = 1, fmt("invalid JSON arguments for %s", name ? name : "?"));
        cJSON_Delete(input);
        /* Chat completions has no is_error field, so mark failures in the text. */
        char *content = err ? fmt("error: %s", out) : fmt("%s", out);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "role", "tool");
        cJSON_AddStringToObject(r, "tool_call_id", get_str(call, "id"));
        cJSON_AddStringToObject(r, "content", content);
        cJSON_AddItemToArray(messages, r);
        free(content);
        free(out);
    }
    return ncalls > 0;
}

/* One user turn: call the model and run tools until it stops asking for them. */
static int run_turn(cJSON *messages) {
    int rc;
    do {
        cJSON *resp = call_api(messages);
        if (!resp) return -1;
        rc = prov->anthropic ? step_anthropic(resp, messages) : step_openai(resp, messages);
        cJSON_Delete(resp);
    } while (rc == 1);
    return rc;
}

/* Add a user message and run the turn; on failure roll history back to a valid state. */
static int ask(cJSON *messages, const char *text) {
    int before = cJSON_GetArraySize(messages);
    add_message(messages, "user", cJSON_CreateString(text));
    int rc = run_turn(messages);
    if (rc < 0)
        while (cJSON_GetArraySize(messages) > before)
            cJSON_DeleteItemFromArray(messages, cJSON_GetArraySize(messages) - 1);
    return rc;
}

/* Chat completions wraps each tool as {"type":"function","function":{...parameters}}. */
static cJSON *openai_tools(cJSON *defs) {
    cJSON *out = cJSON_CreateArray(), *d;
    cJSON_ArrayForEach(d, defs) {
        cJSON *fn = cJSON_Duplicate(d, 1);
        cJSON *schema = cJSON_DetachItemFromObject(fn, "input_schema");
        cJSON_AddItemToObject(fn, "parameters", schema);
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "function");
        cJSON_AddItemToObject(t, "function", fn);
        cJSON_AddItemToArray(out, t);
    }
    return out;
}

static int usage(const char *argv0, int rc) {
    fprintf(rc ? stderr : stdout,
            "usage: %s [-y] [-P provider] [-m model] [-p prompt]\n"
            "  -P  openrouter (default), anthropic, openai, compat\n"
            "  -m  model id (default depends on provider; required for openai)\n"
            "  -p  run one prompt headless and exit (default: REPL)\n"
            "  -y  run write/edit/shell without asking\n", argv0);
    return rc;
}

int main(int argc, char **argv) {
    const char *prompt = NULL, *pname = "openrouter";
    int opt;
    while ((opt = getopt(argc, argv, "yp:P:m:h")) != -1) {
        if (opt == 'y') auto_yes = 1;
        else if (opt == 'p') prompt = optarg;
        else if (opt == 'P') pname = optarg;
        else if (opt == 'm') model = optarg;
        else return usage(argv[0], opt == 'h' ? 0 : 2);
    }
    if (optind < argc) return usage(argv[0], 2);
    for (size_t i = 0; i < sizeof PROVIDERS / sizeof *PROVIDERS; i++)
        if (!strcmp(pname, PROVIDERS[i].name)) prov = &PROVIDERS[i];
    if (!prov) { fprintf(stderr, "error: unknown provider %s\n", pname); return usage(argv[0], 2); }
    if (!model) model = prov->model;
    if (!model) { fprintf(stderr, "error: provider %s needs -m MODEL\n", prov->name); return 2; }
    api_key = getenv(prov->key_env);
    if ((!api_key || !*api_key) && !prov->key_optional) {
        fprintf(stderr, "error: %s is not set\n", prov->key_env);
        return 1;
    }

    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, ".");
    snprintf(system_prompt, sizeof system_prompt,
             "You are a coding agent. The working directory is %s. Use the read, write, "
             "edit and shell tools to inspect and change code. Verify changes where you can. "
             "Be concise.", cwd);
    tools = cJSON_Parse(TOOLS_JSON);
    if (!prov->anthropic) {
        cJSON *t = openai_tools(tools);
        cJSON_Delete(tools);
        tools = t;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    cJSON *messages = cJSON_CreateArray();
    if (!prov->anthropic) { /* rollback in ask() never reaches index 0 */
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "role", "system");
        cJSON_AddStringToObject(s, "content", system_prompt);
        cJSON_AddItemToArray(messages, s);
    }
    int rc = 0;

    if (prompt) {
        rc = ask(messages, prompt) < 0;
    } else {
        char *line = NULL;
        size_t cap = 0;
        ssize_t n;
        for (;;) {
            fputs("> ", stderr);
            if ((n = getline(&line, &cap, stdin)) < 0) break;
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            if (!n) continue;
            if (!strcmp(line, "/exit")) break;
            ask(messages, line);
        }
        free(line);
    }
    cJSON_Delete(messages);
    cJSON_Delete(tools);
    curl_global_cleanup();
    return rc;
}
