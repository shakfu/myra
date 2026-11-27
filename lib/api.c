/* api.c - myralib: providers, HTTP requests, server-sent events and the model list. */
#include "internal.h"

#define RETRIES 4

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

const myra_provider MYRA_PROVIDERS[] = {
    {"openrouter", "OPENROUTER_API_KEY", "OPENROUTER_BASE_URL", "https://openrouter.ai/api/v1",
     "anthropic/claude-opus-5", 0, 1, 100 * 1024},
    /* Any OpenAI-compatible server; the default URL is llama-server's. */
    /* 16 KB is ~4-5K tokens: a quarter of llama-server's usual 16K context. */
    {"local", "LOCAL_API_KEY", "LOCAL_BASE_URL", "http://localhost:8080/v1", "local", 1, 0,
     16 * 1024},
};
const size_t MYRA_NPROVIDERS = sizeof MYRA_PROVIDERS / sizeof *MYRA_PROVIDERS;

/* ---- providers ---- */

const myra_provider *myra_provider_named(const char *name) {
    for (size_t i = 0; i < MYRA_NPROVIDERS; i++)
        if (!strcmp(name, MYRA_PROVIDERS[i].name)) return &MYRA_PROVIDERS[i];
    return NULL;
}

static int has_key(const myra_provider *p) {
    const char *k = getenv(p->key_env);
    return k && *k;
}

const myra_provider *myra_provider_default(void) {
    char *last = myra_load_state("provider");
    const myra_provider *p = last ? myra_provider_named(last) : NULL;
    free(last);
    if (p && (p->key_optional || has_key(p))) return p;
    for (size_t i = 0; i < MYRA_NPROVIDERS; i++) /* local is never picked here */
        if (!MYRA_PROVIDERS[i].key_optional && has_key(&MYRA_PROVIDERS[i]))
            return &MYRA_PROVIDERS[i];
    return NULL;
}

/* ---- API ---- */

/* curl calls this about once a second, and sooner on activity; nonzero aborts. */
static int on_progress(void *ud, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
    (void)ud, (void)dt, (void)dn, (void)ut, (void)un;
    if (myra_poll) myra_poll();
    return myra_interrupted;
}


/* Case-insensitive strstr; strcasestr needs _GNU_SOURCE on glibc. */
static int contains_ci(const char *s, const char *sub) {
    size_t n = strlen(sub);
    for (; *s; s++)
        if (!strncasecmp(s, sub, n)) return 1;
    return !n;
}

/* OpenAI's code and llama-server's type first; wording, which varies by server, after. */
static int is_context_error(const char *body) {
    cJSON *j = cJSON_Parse(body), *e = cJSON_GetObjectItem(j, "error");
    if (!cJSON_IsObject(e)) e = j; /* a mid-stream error is the error object itself */
    const char *code = myra__get_str(e, "code"), *type = myra__get_str(e, "type");
    int hit = (code && !strcmp(code, "context_length_exceeded")) ||
              (type && !strcmp(type, "exceed_context_size_error"));
    cJSON_Delete(j);
    if (hit) return 1;
    static const char *const hints[] = {"context size", "context length", "context window",
                                        "maximum context", "prompt is too long", "too many tokens"};
    for (size_t i = 0; i < sizeof hints / sizeof *hints; i++)
        if (contains_ci(body, hints[i])) return 1;
    return 0;
}

/* ---- streaming (server-sent events) ---- */

typedef struct {
    char key[24];
    buf val;
} text_field;

/* One streamed reply, rebuilt into the shape of a non-streamed one. */
typedef struct {
    CURL *c;
    buf line, raw; /* raw: every byte, for error bodies and servers that ignore "stream" */
    buf content;
    text_field extra[4]; /* other text deltas: reasoning, reasoning_content, refusal */
    cJSON *calls;        /* tool calls; arguments are kept in args */
    buf *args;
    int ncalls;
    cJSON *details; /* reasoning_details, merged by index */
    cJSON *usage, *error;
    char finish[32];
    int events, printed, done; /* done: the server sent [DONE] */
    size_t received;
    int oversize; /* received passed MYRA_MAX_RESPONSE; the transfer was aborted */
    const myra_io *io; /* io->text, when set, takes the reply text */
} stream;

/* Count len more bytes; 0 once over the cap, which curl takes as an abort. */
static int within_cap(stream *st, size_t len) {
    st->received += len;
    if (st->received > MYRA_MAX_RESPONSE) st->oversize = 1;
    return !st->oversize;
}

static size_t on_body(char *p, size_t sz, size_t n, void *ud) {
    stream *st = ud;
    if (!within_cap(st, sz * n)) return 0;
    buf_add(&st->raw, p, sz * n);
    return sz * n;
}

static void stream_free(stream *st) {
    free(st->line.p);
    free(st->raw.p);
    free(st->content.p);
    for (size_t i = 0; i < sizeof st->extra / sizeof *st->extra; i++) free(st->extra[i].val.p);
    for (int i = 0; i < st->ncalls; i++) free(st->args[i].p);
    free(st->args);
    cJSON_Delete(st->calls);
    cJSON_Delete(st->details);
    cJSON_Delete(st->usage);
    cJSON_Delete(st->error);
}

static void add_text(stream *st, const char *key, const char *s) {
    size_t i, n = sizeof st->extra / sizeof *st->extra;
    for (i = 0; i < n && st->extra[i].key[0] && strcmp(st->extra[i].key, key); i++) {}
    if (i == n || strlen(key) >= sizeof st->extra[i].key) return; /* unknown extras: dropped */
    snprintf(st->extra[i].key, sizeof st->extra[i].key, "%s", key);
    buf_add(&st->extra[i].val, s, strlen(s));
}

/* Tool calls arrive in pieces keyed by index: arguments are appended, the rest is set. */
static void merge_calls(stream *st, cJSON *deltas) {
    cJSON *d;
    if (!st->calls) st->calls = cJSON_CreateArray();
    cJSON_ArrayForEach(d, deltas) {
        int idx = (int)myra__get_num(d, "index");
        if (idx < 0 || idx > 255) continue;
        while (st->ncalls <= idx) {
            cJSON *c = cJSON_CreateObject();
            cJSON_AddStringToObject(c, "id", "");
            cJSON_AddStringToObject(c, "type", "function");
            cJSON_AddItemToObject(c, "function", cJSON_CreateObject());
            cJSON_AddStringToObject(cJSON_GetObjectItem(c, "function"), "name", "");
            cJSON_AddItemToArray(st->calls, c);
            st->args = realloc(st->args, (st->ncalls + 1) * sizeof *st->args);
            st->args[st->ncalls++] = (buf){0};
        }
        cJSON *c = cJSON_GetArrayItem(st->calls, idx), *fn = cJSON_GetObjectItem(d, "function");
        const char *id = myra__get_str(d, "id"), *name = myra__get_str(fn, "name"), *args = myra__get_str(fn, "arguments");
        if (id && *id) cJSON_ReplaceItemInObject(c, "id", cJSON_CreateString(id));
        if (name && *name)
            cJSON_ReplaceItemInObject(cJSON_GetObjectItem(c, "function"), "name", cJSON_CreateString(name));
        if (args) buf_add(&st->args[idx], args, strlen(args));
    }
}

/* reasoning_details pieces: text fields are appended, others set, merged by index. */
static void merge_details(stream *st, cJSON *deltas) {
    cJSON *d;
    if (!st->details) st->details = cJSON_CreateArray();
    cJSON_ArrayForEach(d, deltas) {
        cJSON *idx = cJSON_GetObjectItem(d, "index"), *into = NULL, *e;
        cJSON_ArrayForEach(e, st->details)
            if (cJSON_IsNumber(idx) && cJSON_Compare(cJSON_GetObjectItem(e, "index"), idx, 1)) into = e;
        if (!into) {
            cJSON_AddItemToArray(st->details, cJSON_Duplicate(d, 1));
            continue;
        }
        cJSON *f;
        cJSON_ArrayForEach(f, d) {
            cJSON *old = cJSON_GetObjectItem(into, f->string);
            if (cJSON_IsString(f) && cJSON_IsString(old) && strcmp(f->string, "type")) {
                buf j = {0};
                buf_add(&j, old->valuestring, strlen(old->valuestring));
                buf_add(&j, f->valuestring, strlen(f->valuestring));
                cJSON_ReplaceItemInObject(into, f->string, cJSON_CreateString(j.p));
                free(j.p);
            } else if (old) {
                cJSON_ReplaceItemInObject(into, f->string, cJSON_Duplicate(f, 1));
            } else {
                cJSON_AddItemToObject(into, f->string, cJSON_Duplicate(f, 1));
            }
        }
    }
}

static void stream_event(stream *st, const char *data) {
    cJSON *chunk = cJSON_Parse(data), *f;
    if (!chunk) return;
    cJSON *err = cJSON_DetachItemFromObject(chunk, "error");
    if (err) { /* mid-stream failure */
        cJSON_Delete(st->error);
        st->error = err;
    }
    st->events++;
    cJSON *usage = cJSON_GetObjectItem(chunk, "usage");
    if (cJSON_IsObject(usage)) {
        cJSON_Delete(st->usage);
        st->usage = cJSON_Duplicate(usage, 1);
    }
    cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItem(chunk, "choices"), 0);
    const char *finish = myra__get_str(choice, "finish_reason");
    if (finish) snprintf(st->finish, sizeof st->finish, "%s", finish);
    cJSON_ArrayForEach(f, cJSON_GetObjectItem(choice, "delta")) {
        if (!strcmp(f->string, "content") && cJSON_IsString(f) && *f->valuestring) {
            if (st->io->text) st->io->text(st->io->ud, f->valuestring), st->printed = 1;
            buf_add(&st->content, f->valuestring, strlen(f->valuestring));
        } else if (!strcmp(f->string, "tool_calls") && cJSON_IsArray(f)) {
            merge_calls(st, f);
        } else if (!strcmp(f->string, "reasoning_details") && cJSON_IsArray(f)) {
            merge_details(st, f);
        } else if (strcmp(f->string, "role") && cJSON_IsString(f) && *f->valuestring) {
            if (st->io->reasoning && (!strcmp(f->string, "reasoning") ||
                                      !strcmp(f->string, "reasoning_content")))
                st->io->reasoning(st->io->ud, f->valuestring);
            add_text(st, f->string, f->valuestring);
        }
    }
    cJSON_Delete(chunk);
}

static void stream_line(stream *st, char *line) {
    if (!MYRA_STARTS_WITH(line, "data:")) return; /* comments (":") and other fields are ignored */
    const char *data = line + 5 + (line[5] == ' ');
    if (!strcmp(data, "[DONE]")) st->done = 1;
    else stream_event(st, data);
}

static size_t on_stream(char *p, size_t sz, size_t n, void *ud) {
    stream *st = ud;
    size_t len = sz * n;
    if (!within_cap(st, len)) return 0;
    /* raw serves error bodies and servers that ignore "stream"; an event stream needs none */
    if (!st->events) buf_add(&st->raw, p, len);
    long status = 0;
    curl_easy_getinfo(st->c, CURLINFO_RESPONSE_CODE, &status);
    if (status != 200) return len; /* an error body: plain JSON, kept in raw */
    size_t old = st->line.n;
    buf_add(&st->line, p, len);
    char *start = st->line.p, *from = start + old, *nl; /* earlier bytes hold no '\n' */
    while ((nl = memchr(from, '\n', st->line.n - (size_t)(from - st->line.p)))) {
        *nl = 0;
        if (nl > start && nl[-1] == '\r') nl[-1] = 0;
        stream_line(st, start);
        start = from = nl + 1;
    }
    st->line.n -= (size_t)(start - st->line.p);
    memmove(st->line.p, start, st->line.n);
    st->line.p[st->line.n] = 0;
    return len;
}

/* The streamed reply as a non-streamed response object. */
static cJSON *stream_result(stream *st) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "assistant");
    if (st->content.n) cJSON_AddStringToObject(msg, "content", st->content.p);
    else if (st->ncalls) cJSON_AddNullToObject(msg, "content");
    else cJSON_AddStringToObject(msg, "content", "");
    for (size_t i = 0; i < sizeof st->extra / sizeof *st->extra && st->extra[i].key[0]; i++)
        cJSON_AddStringToObject(msg, st->extra[i].key, st->extra[i].val.p);
    if (cJSON_GetArraySize(st->details)) {
        cJSON_AddItemToObject(msg, "reasoning_details", st->details);
        st->details = NULL;
    }
    for (int i = 0; i < st->ncalls; i++)
        cJSON_AddStringToObject(cJSON_GetObjectItem(cJSON_GetArrayItem(st->calls, i), "function"),
                                "arguments", st->args[i].p ? st->args[i].p : "");
    if (st->ncalls) {
        cJSON_AddItemToObject(msg, "tool_calls", st->calls);
        st->calls = NULL;
    }
    cJSON *choice = cJSON_CreateObject();
    cJSON_AddItemToObject(choice, "message", msg);
    if (st->finish[0]) cJSON_AddStringToObject(choice, "finish_reason", st->finish);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddItemToObject(resp, "choices", cJSON_CreateArray());
    cJSON_AddItemToArray(cJSON_GetObjectItem(resp, "choices"), choice);
    if (st->usage) {
        cJSON_AddItemToObject(resp, "usage", st->usage);
        st->usage = NULL;
    }
    return resp;
}

/* POST body to path (streamed when streaming is set), or GET path when body is NULL.
   Retries 429, 5xx and network errors, unless streamed text was already printed. */
static cJSON *request(myra_agent *a, const char *path, const char *body, int streaming) {
    const char *base = getenv(a->prov->base_env);
    buf url = {0};
    base = base && *base ? base : a->prov->base;
    buf_add(&url, base, strlen(base));
    buf_add(&url, path, strlen(path));

    struct curl_slist *h = curl_slist_append(NULL, "content-type: application/json");
    char *auth = a->api_key && *a->api_key ? myra__fmt("authorization: Bearer %s", a->api_key) : NULL;
    if (auth) h = curl_slist_append(h, auth);

    const char *d = getenv("MYRA_RETRY_DELAY_MS"); /* first back-off; doubles each retry */
    long delay_ms = d ? strtol(d, NULL, 10) : 0;
    if (delay_ms < 1) delay_ms = 1000;
    if (delay_ms > 60000) delay_ms = 60000; /* a minute of back-off is plenty */
    cJSON *resp = NULL;
    a->streamed = a->context_full = 0;
    for (int attempt = 0; attempt <= RETRIES; attempt++) {
        if (attempt) { /* a signal ends the wait early */
            long ms = delay_ms << (attempt - 1);
            struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
            nanosleep(&ts, NULL);
        }
        if (myra_interrupted) break;
        stream st = {.io = &a->io};
        CURL *c = st.c = curl_easy_init();
        curl_easy_setopt(c, CURLOPT_URL, url.p);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        if (body) curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, streaming ? on_stream : on_body);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &st);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 600L);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
        CURLcode rc = curl_easy_perform(c);
        if (streaming && st.line.n) stream_line(&st, st.line.p); /* a last event without "\n" */
        long status = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(c);
        if (st.printed && a->io.text_end) a->io.text_end(a->io.ud);
        a->streamed = st.printed;
        const char *out = st.raw.p ? st.raw.p : "";
        /* No finish_reason and no [DONE]: the stream was cut off, so the reply is partial. */
        int truncated = st.events && !st.finish[0] && !st.done;
        if (rc == CURLE_OK && status == 200 && !st.error && !truncated) {
            resp = st.events ? stream_result(&st) : cJSON_Parse(out); /* "stream" ignored */
            if (!resp) myra_note(MYRA_ERROR, "error: response is not JSON\n");
            stream_free(&st);
            break;
        }
        if (st.oversize) { /* a retry would receive the same */
            myra_note(MYRA_ERROR, "error: response exceeds %d MB\n", MYRA_MAX_RESPONSE / (1024 * 1024));
            stream_free(&st);
            break;
        }
        if (rc == CURLE_ABORTED_BY_CALLBACK) {
            myra_note(MYRA_WARN, "interrupted\n");
            stream_free(&st);
            break;
        }
        if (rc == CURLE_COULDNT_CONNECT) { /* nothing listening: retrying cannot help */
            myra_note(MYRA_ERROR, "error: cannot connect to %s; is the server running?\n", url.p);
            stream_free(&st);
            break;
        }
        char *err = st.error ? cJSON_PrintUnformatted(st.error) : NULL;
        if (err) out = err;
        else if (truncated) out = "the stream ended early";
        a->context_full = (status == 400 || st.error) && is_context_error(out);
        int retryable = (rc != CURLE_OK || status == 429 || status >= 500 || st.error || truncated) &&
                        !st.printed && !a->context_full; /* retrying cannot shrink the context */
        const char *again = retryable && attempt < RETRIES ? ", retrying" : "";
        if (st.error || truncated) myra_note(MYRA_ERROR, "stream error%s: %s\n", again, out);
        else if (rc != CURLE_OK)
            myra_note(MYRA_ERROR, "api error (%s, http %ld)%s: %s\n", curl_easy_strerror(rc),
                       status, again, out);
        else myra_note(MYRA_ERROR, "api error (http %ld)%s: %s\n", status, again, out);
        free(err);
        stream_free(&st);
        if (!retryable) break;
    }
    curl_slist_free_all(h);
    free(auth);
    free(url.p);
    return resp;
}

cJSON *myra__call_api(myra_agent *a) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", a->model);
    if (cJSON_GetArraySize(a->tools)) /* an empty list is refused by some servers */
        cJSON_AddItemReferenceToObject(req, "tools", a->tools);
    cJSON_AddItemReferenceToObject(req, "messages", a->messages);
    cJSON_AddTrueToObject(req, "stream");
    cJSON_AddItemToObject(req, "stream_options", cJSON_Parse("{\"include_usage\":true}"));
    /* Turns on prompt caching for Anthropic models, where it is opt-in. Providers
       that cache automatically or lack support ignore unknown fields. */
    if (a->prov->cache_control)
        cJSON_AddItemToObject(req, "cache_control", cJSON_Parse("{\"type\":\"ephemeral\"}"));
    char *body = cJSON_PrintUnformatted(req);
    long grant = 0;
    if (a->family.reserve) { /* about 3 bytes of JSON per token: more tokens than most text */
        grant = a->family.reserve(a->family.ud, a, (long)(strlen(body) / 3 + 1));
        if (grant < 0) {
            a->context_full = 0;
            free(body);
            cJSON_Delete(req);
            return NULL;
        }
        if (grant) { /* the provider enforces the grant */
            cJSON_AddNumberToObject(req, "max_tokens", (double)grant);
            free(body);
            body = cJSON_PrintUnformatted(req);
        }
    }
    cJSON_Delete(req);
    cJSON *resp = request(a, "/chat/completions", body, 1);
    free(body);
    if (a->family.settle) a->family.settle(a->family.ud, cJSON_GetObjectItem(resp, "usage"));
    /* Saved only once the server accepts them, so a mistyped choice is not kept. */
    if (resp && a->save_provider) {
        myra_store_state("provider", a->prov->name);
        a->save_provider = 0;
    }
    if (resp && a->save_model) {
        char *key = myra__fmt("%s.model", a->prov->name);
        myra_store_state(key, a->model);
        free(key);
        a->save_model = 0;
    }
    return resp;
}

char **myra_model_ids(myra_agent *a) {
    cJSON *resp = request(a, "/models", NULL, 0), *m;
    cJSON *data = cJSON_GetObjectItem(resp, "data");
    if (resp && !cJSON_IsArray(data)) myra_note(MYRA_ERROR, "error: no model list in response\n");
    char **ids = NULL;
    size_t n = 0, cap = 0;
    cJSON_ArrayForEach(m, data) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(m, "id"));
        if (!id) continue;
        if (n + 2 > cap) {
            cap = cap ? cap * 2 : 32;
            ids = realloc(ids, cap * sizeof *ids);
            if (!ids) { perror("realloc"); exit(1); }
        }
        ids[n++] = xstrdup(id);
    }
    if (ids) ids[n] = NULL;
    cJSON_Delete(resp);
    return ids;
}

cJSON *myra_model_entry(myra_agent *a) {
    cJSON *resp = request(a, "/models", NULL, 0), *m, *entry = NULL;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(resp, "data")) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(m, "id"));
        if (id && !strcmp(id, a->model)) {
            entry = cJSON_Duplicate(m, 1);
            break;
        }
    }
    cJSON_Delete(resp);
    return entry;
}

long myra_context_window(myra_agent *a) {
    cJSON *m = myra_model_entry(a);
    double n = myra__get_num(m, "context_length"); /* OpenRouter's field */
    cJSON_Delete(m);
    return n >= 1 ? (long)n : 0;
}

void myra_free_model_ids(char **ids) {
    for (char **p = ids; p && *p; p++) free(*p);
    free(ids);
}

