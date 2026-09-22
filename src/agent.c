/* agent.c - agentlib: providers, the four tools, and the chat-completions tool loop. */
#include "agent.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_OUTPUT (100 * 1024) /* cap on one tool result, in bytes */
#define RETRIES 4

/* OpenAI chat-completions tool definitions. */
static const char *TOOLS_JSON =
    "[{\"type\":\"function\",\"function\":{\"name\":\"read\","
    "\"description\":\"Read a text file and return its contents.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"],\"additionalProperties\":false}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"write\","
    "\"description\":\"Create or overwrite a file with the given content.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"],"
    "\"additionalProperties\":false}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"edit\","
    "\"description\":\"Replace old_string with new_string in a file. "
    "old_string must occur exactly once.\",\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"},\"old_string\":{\"type\":\"string\"},"
    "\"new_string\":{\"type\":\"string\"}},\"required\":[\"path\",\"old_string\",\"new_string\"],"
    "\"additionalProperties\":false}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"shell\","
    "\"description\":\"Run a command with /bin/sh in the working directory. "
    "Returns combined stdout and stderr and the exit status. stdin is /dev/null.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},"
    "\"required\":[\"command\"],\"additionalProperties\":false}}}]";

const agent_provider AGENT_PROVIDERS[] = {
    {"openrouter", "OPENROUTER_API_KEY", "OPENROUTER_BASE_URL", "https://openrouter.ai/api/v1",
     "anthropic/claude-opus-5", 0, 1},
    /* Any OpenAI-compatible server; the default URL is llama-server's. */
    {"local", "LOCAL_API_KEY", "LOCAL_BASE_URL", "http://localhost:8080/v1", "local", 1, 0},
};
const size_t AGENT_NPROVIDERS = sizeof AGENT_PROVIDERS / sizeof *AGENT_PROVIDERS;

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
static int confirm(int auto_yes, const char *name, const char *detail) {
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

char *agent_run_tool(agent *a, const char *name, cJSON *input, int *err) {
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
        if (!confirm(a->auto_yes, "write", path)) return xstrdup("denied by user");
        *err = 0;
        return tool_write(path, content, err);
    }
    if (!strcmp(name, "edit") && path && old && new) {
        fprintf(stderr, "> edit %s\n", path);
        if (!confirm(a->auto_yes, "edit", path)) return xstrdup("denied by user");
        return tool_edit(path, old, new, err);
    }
    if (!strcmp(name, "shell") && cmd) {
        fprintf(stderr, "> shell %s\n", cmd);
        if (!confirm(a->auto_yes, "shell", cmd)) return xstrdup("denied by user");
        return tool_shell(cmd, err);
    }
    return fmt("unknown tool or missing arguments: %s", name);
}

/* ---- remembered state ---- */

static char *state_path(const char *name) {
    const char *xdg = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    buf b = {0};
    if (xdg && *xdg) buf_add(&b, xdg, strlen(xdg));
    else if (home && *home) { buf_add(&b, home, strlen(home)); buf_add(&b, "/.local/state", 13); }
    else return NULL;
    buf_add(&b, "/ant/", 5);
    buf_add(&b, name, strlen(name));
    return b.p;
}

char *agent_load_state(const char *name) {
    char *path = state_path(name);
    buf b = {0};
    int rc = path ? slurp(path, &b) : -1;
    free(path);
    if (rc < 0) { free(b.p); return NULL; }
    while (b.n && strchr(" \t\r\n", b.p[b.n - 1])) b.p[--b.n] = 0;
    if (!b.n) { free(b.p); return NULL; }
    return b.p;
}

void agent_store_state(const char *name, const char *value) {
    char *path = state_path(name);
    if (!path) return;
    char *dir = xstrdup(path);
    for (char *p = dir + 1; *p; p++) /* mkdir -p the parent */
        if (*p == '/') { *p = 0; mkdir(dir, 0755); *p = '/'; }
    free(dir);
    char *line = fmt("%s\n", value);
    if (spit(path, line, strlen(line)) < 0) fprintf(stderr, "warning: cannot save %s\n", path);
    free(line);
    free(path);
}

/* ---- providers ---- */

const agent_provider *agent_provider_named(const char *name) {
    for (size_t i = 0; i < AGENT_NPROVIDERS; i++)
        if (!strcmp(name, AGENT_PROVIDERS[i].name)) return &AGENT_PROVIDERS[i];
    return NULL;
}

const agent_provider *agent_provider_default(void) {
    for (size_t i = 0; i < AGENT_NPROVIDERS; i++) {
        const char *k = getenv(AGENT_PROVIDERS[i].key_env);
        if (!AGENT_PROVIDERS[i].key_optional && k && *k) return &AGENT_PROVIDERS[i];
    }
    return NULL;
}

/* ---- API ---- */

static size_t on_body(char *p, size_t sz, size_t n, void *ud) {
    buf_add(ud, p, sz * n);
    return sz * n;
}

/* POST body to path, or GET it when body is NULL; retries 429, 5xx and network errors. */
static cJSON *request(agent *a, const char *path, const char *body) {
    const char *base = getenv(a->prov->base_env);
    buf url = {0};
    base = base && *base ? base : a->prov->base;
    buf_add(&url, base, strlen(base));
    buf_add(&url, path, strlen(path));

    struct curl_slist *h = curl_slist_append(NULL, "content-type: application/json");
    char *auth = a->api_key && *a->api_key ? fmt("authorization: Bearer %s", a->api_key) : NULL;
    if (auth) h = curl_slist_append(h, auth);

    cJSON *resp = NULL;
    for (int attempt = 0; attempt <= RETRIES; attempt++) {
        if (attempt) sleep(1u << (attempt - 1));
        buf out = {0};
        CURL *c = curl_easy_init();
        curl_easy_setopt(c, CURLOPT_URL, url.p);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        if (body) curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
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
    free(url.p);
    return resp;
}

static cJSON *call_api(agent *a) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", a->model);
    cJSON_AddItemReferenceToObject(req, "tools", a->tools);
    cJSON_AddItemReferenceToObject(req, "messages", a->messages);
    /* Turns on prompt caching for Anthropic models, where it is opt-in. Providers
       that cache automatically or lack support ignore unknown fields. */
    if (a->prov->cache_control)
        cJSON_AddItemToObject(req, "cache_control", cJSON_Parse("{\"type\":\"ephemeral\"}"));
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    cJSON *resp = request(a, "/chat/completions", body);
    free(body);
    /* Saved only once the server accepts it, so a mistyped model is not kept. */
    if (resp && a->save_model) {
        char *key = fmt("%s.model", a->prov->name);
        agent_store_state(key, a->model);
        free(key);
        a->save_model = 0;
    }
    return resp;
}

void agent_list_models(agent *a, const char *filter) {
    cJSON *resp = request(a, "/models", NULL), *m;
    cJSON *data = cJSON_GetObjectItem(resp, "data");
    if (resp && !cJSON_IsArray(data)) fprintf(stderr, "error: no model list in response\n");
    cJSON_ArrayForEach(m, data) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(m, "id"));
        if (id && strstr(id, filter)) printf("%s %s\n", strcmp(id, a->model) ? " " : "*", id);
    }
    fflush(stdout);
    cJSON_Delete(resp);
}

/* ---- the loop ---- */

static void print_text(const char *s) {
    if (!s || !*s) return;
    printf("%s\n", s);
    fflush(stdout);
}

static const char *get_str(cJSON *o, const char *key) {
    return cJSON_GetStringValue(cJSON_GetObjectItem(o, key));
}

int agent_step(agent *a, cJSON *resp) {
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
    /* Calls cut off by length may be truncated JSON, and dropping them keeps the
       history valid: every tool call must be followed by its result. */
    if (!strcmp(finish, "length")) {
        fprintf(stderr, "warning: reply hit the length limit\n");
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
    cJSON_AddItemToArray(a->messages, msg);
    for (int i = 0; i < ncalls; i++) {
        cJSON *call = cJSON_GetArrayItem(calls, i), *fn = cJSON_GetObjectItem(call, "function");
        const char *name = get_str(fn, "name"), *args = get_str(fn, "arguments");
        cJSON *input = cJSON_Parse(args ? args : "");
        int err = 1;
        char *out = cJSON_IsObject(input) ? agent_run_tool(a, name ? name : "", input, &err)
                                          : fmt("invalid JSON arguments for %s", name ? name : "?");
        cJSON_Delete(input);
        /* Chat completions has no is_error field, so mark failures in the text. */
        char *content = err ? fmt("error: %s", out) : fmt("%s", out);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "role", "tool");
        cJSON_AddStringToObject(r, "tool_call_id", get_str(call, "id"));
        cJSON_AddStringToObject(r, "content", content);
        cJSON_AddItemToArray(a->messages, r);
        free(content);
        free(out);
    }
    return ncalls > 0;
}

int agent_ask(agent *a, const char *text) {
    int before = cJSON_GetArraySize(a->messages);
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", text);
    cJSON_AddItemToArray(a->messages, m);
    int rc;
    do { /* call the model and run tools until it stops asking for them */
        cJSON *resp = call_api(a);
        if (!resp) { rc = -1; break; }
        rc = agent_step(a, resp);
        cJSON_Delete(resp);
    } while (rc == 1);
    if (rc < 0)
        while (cJSON_GetArraySize(a->messages) > before)
            cJSON_DeleteItemFromArray(a->messages, cJSON_GetArraySize(a->messages) - 1);
    return rc;
}

/* ---- session ---- */

int agent_init(agent *a, const agent_provider *p, const char *model, int auto_yes) {
    memset(a, 0, sizeof *a);
    a->prov = p;
    a->auto_yes = auto_yes;
    a->api_key = getenv(p->key_env);
    if ((!a->api_key || !*a->api_key) && !p->key_optional) {
        fprintf(stderr, "error: %s is not set\n", p->key_env);
        return -1;
    }
    char *key = fmt("%s.model", p->name);
    char *remembered = agent_load_state(key);
    free(key);
    if (model) a->save_model = !remembered || strcmp(model, remembered);
    a->model = xstrdup(model ? model : remembered ? remembered : p->model);
    free(remembered);

    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, ".");
    char *prompt = fmt("You are a coding agent. The working directory is %s. Use the read, write, "
                       "edit and shell tools to inspect and change code. Verify changes where you "
                       "can. Be concise.", cwd);
    a->tools = cJSON_Parse(TOOLS_JSON);
    a->messages = cJSON_CreateArray();
    cJSON *sys = cJSON_CreateObject(); /* rollback in agent_ask never reaches index 0 */
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", prompt);
    cJSON_AddItemToArray(a->messages, sys);
    free(prompt);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    return 0;
}

void agent_free(agent *a) {
    if (a->messages) curl_global_cleanup(); /* paired with a successful agent_init */
    cJSON_Delete(a->messages);
    cJSON_Delete(a->tools);
    free(a->model);
    memset(a, 0, sizeof *a);
}

void agent_set_model(agent *a, const char *model) {
    free(a->model);
    a->model = xstrdup(model);
    a->save_model = 1;
}

const char *agent_command(const char *line, const char *name) {
    size_t n = strlen(name);
    if (strncmp(line, name, n) || (line[n] && line[n] != ' ')) return NULL;
    for (line += n; *line == ' '; line++);
    return line;
}
