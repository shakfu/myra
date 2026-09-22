/* agent.c - agentlib: providers, the four tools, and the chat-completions tool loop. */
#include "agent.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
    "Returns combined stdout and stderr and the exit status. stdin is /dev/null. "
    "The command and its children are killed after timeout seconds (default 120, max 600). "
    "Background jobs must redirect their output, or the call waits for them.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},"
    "\"timeout\":{\"type\":\"integer\"}},"
    "\"required\":[\"command\"],\"additionalProperties\":false}}}]";

volatile sig_atomic_t agent_interrupted;

const agent_provider AGENT_PROVIDERS[] = {
    {"openrouter", "OPENROUTER_API_KEY", "OPENROUTER_BASE_URL", "https://openrouter.ai/api/v1",
     "anthropic/claude-opus-5", 0, 1, 100 * 1024},
    /* Any OpenAI-compatible server; the default URL is llama-server's. */
    /* 16 KB is ~4-5K tokens: a quarter of llama-server's usual 16K context. */
    {"local", "LOCAL_API_KEY", "LOCAL_BASE_URL", "http://localhost:8080/v1", "local", 1, 0,
     16 * 1024},
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

static const char *get_str(cJSON *o, const char *key) {
    return cJSON_GetStringValue(cJSON_GetObjectItem(o, key));
}

static double get_num(cJSON *o, const char *key) { /* 0 when absent */
    cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

/* Length of the valid UTF-8 sequence at p (at most n bytes), or 0 if invalid.
   NUL counts as invalid: results become C strings, where it would end them. */
static size_t utf8_len(const unsigned char *p, size_t n) {
    unsigned char c = p[0];
    if (c >= 0x01 && c <= 0x7F) return 1;
    size_t len;
    unsigned char lo = 0x80, hi = 0xBF; /* allowed range of the second byte */
    if (c >= 0xC2 && c <= 0xDF) len = 2;
    else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        if (c == 0xE0) lo = 0xA0;      /* no overlong forms */
        else if (c == 0xED) hi = 0x9F; /* no UTF-16 surrogates */
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        if (c == 0xF0) lo = 0x90;
        else if (c == 0xF4) hi = 0x8F; /* nothing above U+10FFFF */
    } else return 0;
    if (n < len || p[1] < lo || p[1] > hi) return 0;
    for (size_t i = 2; i < len; i++)
        if ((p[i] & 0xC0) != 0x80) return 0;
    return len;
}

static int valid_utf8(const buf *b) {
    size_t i = 0, k;
    while (i < b->n && (k = utf8_len((const unsigned char *)b->p + i, b->n - i))) i += k;
    return i == b->n;
}

/* Replace each invalid byte with U+FFFD, so the JSON request stays valid. */
static void sanitize_utf8(buf *b) {
    const unsigned char *p = (const unsigned char *)b->p;
    size_t i = 0, k;
    while (i < b->n && (k = utf8_len(p + i, b->n - i))) i += k;
    if (i == b->n) return; /* the common case: already valid */
    buf out = {0};
    buf_add(&out, b->p, i);
    while (i < b->n) {
        if ((k = utf8_len(p + i, b->n - i))) buf_add(&out, b->p + i, k), i += k;
        else buf_add(&out, "\xEF\xBF\xBD", 3), i++;
    }
    free(b->p);
    *b = out;
}

static int is_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

/* Sanitize, then keep the first fifth and the last four fifths of cap bytes, cut on
   character boundaries. The end is favoured: errors and summaries come last. */
static char *capped(buf *b, size_t cap) {
    if (!b->p) return xstrdup("");
    sanitize_utf8(b);
    if (b->n <= cap) return b->p;
    size_t head = cap / 5, tail = b->n - (cap - head);
    while (head && is_cont(b->p[head])) head--;
    while (tail < b->n && is_cont(b->p[tail])) tail++;
    char note[80];
    snprintf(note, sizeof note, "\n[... %zu bytes omitted ...]\n", tail - head);
    buf out = {0};
    buf_add(&out, b->p, head);
    buf_add(&out, note, strlen(note));
    buf_add(&out, b->p + tail, b->n - tail);
    free(b->p);
    *b = out;
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

static char *tool_read(const char *path, size_t cap, int *err) {
    buf b = {0};
    if (slurp(path, &b) < 0) { free(b.p); *err = 1; return fmt("cannot read %s", path); }
    if (memchr(b.p, 0, b.n)) { free(b.p); *err = 1; return fmt("%s is binary", path); }
    return capped(&b, cap);
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
    if (!hit && strstr(old, "\xEF\xBF\xBD") && !valid_utf8(&b)) { /* U+FFFD from read */
        free(b.p);
        return fmt("old_string not found in %s: the file is not valid UTF-8, and read showed "
                   "its invalid bytes as U+FFFD, which edit cannot match. Use shell (sed, iconv).",
                   path);
    }
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

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void reap(pid_t pid, int *st) {
    while (waitpid(pid, st, 0) < 0 && errno == EINTR) {}
}

/* SIGTERM the process group, SIGKILL whatever is left after 2 s. */
static void kill_group(pid_t pid, int *st) {
    kill(-pid, SIGTERM);
    for (int i = 0; i < 20 && waitpid(pid, st, WNOHANG) == 0; i++) usleep(100000);
    kill(-pid, SIGKILL); /* also children that outlived sh */
    if (waitpid(pid, st, WNOHANG) == 0) reap(pid, st);
}

/* sh runs in its own process group, so the terminal's Ctrl-C reaches only the agent,
   which then decides to kill the group. */
static char *tool_shell(const char *cmd, int timeout, size_t cap, int *err) {
    int fds[2];
    if (pipe(fds) < 0) { *err = 1; return fmt("pipe failed: %s", strerror(errno)); }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        *err = 1;
        return fmt("fork failed: %s", strerror(errno));
    }
    if (pid == 0) {
        setpgid(0, 0);
        int nul = open("/dev/null", O_RDONLY);
        dup2(nul, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        close(nul);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid); /* also here, so the group exists before any kill */
    close(fds[1]);

    buf b = {0};
    char chunk[65536];
    const char *stopped = NULL;
    double deadline = now() + timeout;
    for (;;) {
        if (agent_interrupted) { stopped = "interrupted by user"; break; }
        double left = deadline - now();
        if (left <= 0) { stopped = "timed out"; break; }
        struct pollfd pfd = {fds[0], POLLIN, 0};
        int r = poll(&pfd, 1, left < 0.2 ? (int)(left * 1000) + 1 : 200);
        if (r <= 0) continue; /* timeout tick or EINTR: recheck both conditions */
        ssize_t n = read(fds[0], chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break; /* EOF: every writer has exited */
        buf_add(&b, chunk, (size_t)n);
    }
    close(fds[0]);
    int st = 0;
    if (stopped) kill_group(pid, &st);
    else reap(pid, &st);

    char tail[64];
    const char *nl = b.n && b.p[b.n - 1] != '\n' ? "\n" : "";
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    if (stopped && !strcmp(stopped, "timed out"))
        snprintf(tail, sizeof tail, "%s[timed out after %ds; killed]", nl, timeout);
    else if (stopped)
        snprintf(tail, sizeof tail, "%s[interrupted by user; killed]", nl);
    else
        snprintf(tail, sizeof tail, "%s[exit %d]", nl, code);
    char *out = capped(&b, cap);
    buf r = {0};
    buf_add(&r, out, strlen(out));
    buf_add(&r, tail, strlen(tail));
    free(out);
    *err = stopped || code != 0;
    return r.p;
}

/* Whether path, with symlinks resolved, lies outside the working directory. A path
   whose directory does not exist counts as inside: writing there fails anyway. */
static int outside_cwd(const char *path) {
    char cwd[PATH_MAX], real[PATH_MAX];
    if (!realpath(".", cwd)) return 1;
    if (!realpath(path, real)) { /* a new file: resolve its directory */
        char *dir = xstrdup(path), *slash = strrchr(dir, '/');
        const char *d = slash ? (slash == dir ? "/" : (*slash = 0, dir)) : ".";
        int ok = realpath(d, real) != NULL;
        free(dir);
        if (!ok) return 0;
    }
    size_t n = strlen(cwd);
    if (n == 1) return 0; /* cwd is / */
    return strncmp(real, cwd, n) || (real[n] && real[n] != '/');
}

/* Ask on the controlling terminal so it works in headless mode and the REPL alike.
   Returns NULL if allowed, else why not, for the model to relay. */
static const char *denied(int auto_yes, int outside, const char *name, const char *detail) {
    if (auto_yes && !outside) return NULL;
    FILE *tty = fopen("/dev/tty", "r+");
    if (!tty)
        return outside && auto_yes ? "denied: outside the working directory, which -y does not cover"
                                   : "denied: no terminal to confirm; run with -y";
    fprintf(tty, "allow %s%s: %s ? [y/N] ", name, outside ? " outside the working directory" : "",
            detail);
    fflush(tty);
    char line[16] = {0};
    int ok = fgets(line, sizeof line, tty) && (line[0] == 'y' || line[0] == 'Y');
    fclose(tty);
    return ok ? NULL : "denied by user";
}

static const char *str_arg(cJSON *input, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(input, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

char *agent_run_tool(agent *a, const char *name, cJSON *input, int *err) {
    const char *path = str_arg(input, "path");
    const char *content = str_arg(input, "content");
    const char *old = str_arg(input, "old_string"), *new = str_arg(input, "new_string");
    const char *cmd = str_arg(input, "command"), *why;
    *err = 1;
    if (!strcmp(name, "read") && path) {
        fprintf(stderr, "[tool] read %s\n", path);
        *err = 0;
        return tool_read(path, a->max_output, err);
    }
    if (!strcmp(name, "write") && path && content) {
        fprintf(stderr, "[tool] write %s\n", path);
        if ((why = denied(a->auto_yes, outside_cwd(path), "write", path))) return xstrdup(why);
        *err = 0;
        return tool_write(path, content, err);
    }
    if (!strcmp(name, "edit") && path && old && new) {
        fprintf(stderr, "[tool] edit %s\n", path);
        if ((why = denied(a->auto_yes, outside_cwd(path), "edit", path))) return xstrdup(why);
        return tool_edit(path, old, new, err);
    }
    if (!strcmp(name, "shell") && cmd) {
        fprintf(stderr, "[tool] shell %s\n", cmd);
        if ((why = denied(a->auto_yes, 0, "shell", cmd))) return xstrdup(why); /* unconfinable */
        cJSON *t = cJSON_GetObjectItemCaseSensitive(input, "timeout");
        int secs = cJSON_IsNumber(t) ? (int)t->valuedouble : AGENT_SHELL_TIMEOUT;
        if (secs < 1) secs = 1;
        if (secs > AGENT_SHELL_TIMEOUT_MAX) secs = AGENT_SHELL_TIMEOUT_MAX;
        return tool_shell(cmd, secs, a->max_output, err);
    }
    return fmt("unknown tool or missing arguments: %s", name);
}

/* ---- remembered state ---- */

char *agent_state_path(const char *name, int make_dir) {
    const char *xdg = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    buf b = {0};
    if (xdg && *xdg) buf_add(&b, xdg, strlen(xdg));
    else if (home && *home) { buf_add(&b, home, strlen(home)); buf_add(&b, "/.local/state", 13); }
    else return NULL;
    buf_add(&b, "/ant/", 5);
    buf_add(&b, name, strlen(name));
    for (char *p = b.p + 1; make_dir && *p; p++) /* mkdir -p the parent */
        if (*p == '/') { *p = 0; mkdir(b.p, 0755); *p = '/'; }
    return b.p;
}

char *agent_load_state(const char *name) {
    char *path = agent_state_path(name, 0);
    buf b = {0};
    int rc = path ? slurp(path, &b) : -1;
    free(path);
    if (rc < 0) { free(b.p); return NULL; }
    while (b.n && strchr(" \t\r\n", b.p[b.n - 1])) b.p[--b.n] = 0;
    if (!b.n) { free(b.p); return NULL; }
    return b.p;
}

void agent_store_state(const char *name, const char *value) {
    char *path = agent_state_path(name, 1);
    if (!path) return;
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

static int has_key(const agent_provider *p) {
    const char *k = getenv(p->key_env);
    return k && *k;
}

const agent_provider *agent_provider_default(void) {
    char *last = agent_load_state("provider");
    const agent_provider *p = last ? agent_provider_named(last) : NULL;
    free(last);
    if (p && (p->key_optional || has_key(p))) return p;
    for (size_t i = 0; i < AGENT_NPROVIDERS; i++) /* local is never picked here */
        if (!AGENT_PROVIDERS[i].key_optional && has_key(&AGENT_PROVIDERS[i]))
            return &AGENT_PROVIDERS[i];
    return NULL;
}

/* ---- API ---- */

/* curl calls this about once a second, and sooner on activity; nonzero aborts. */
static int on_progress(void *ud, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
    (void)ud, (void)dt, (void)dn, (void)ut, (void)un;
    return agent_interrupted;
}

static size_t on_body(char *p, size_t sz, size_t n, void *ud) {
    buf_add(ud, p, sz * n);
    return sz * n;
}

/* Case-insensitive strstr; strcasestr needs _GNU_SOURCE on glibc. */
static int contains_ci(const char *s, const char *sub) {
    size_t n = strlen(sub);
    for (; *s; s++)
        if (!strncasecmp(s, sub, n)) return 1;
    return !n;
}

/* Wording varies by server: llama-server, OpenAI-style APIs, Anthropic. */
static int is_context_error(const char *body) {
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
    int events, printed;
} stream;

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
        int idx = (int)get_num(d, "index");
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
        const char *id = get_str(d, "id"), *name = get_str(fn, "name"), *args = get_str(fn, "arguments");
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
    if (!strcmp(data, "[DONE]")) return;
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
    const char *finish = get_str(choice, "finish_reason");
    if (finish) snprintf(st->finish, sizeof st->finish, "%s", finish);
    cJSON_ArrayForEach(f, cJSON_GetObjectItem(choice, "delta")) {
        if (!strcmp(f->string, "content") && cJSON_IsString(f) && *f->valuestring) {
            fputs(f->valuestring, stdout);
            fflush(stdout);
            st->printed = 1;
            buf_add(&st->content, f->valuestring, strlen(f->valuestring));
        } else if (!strcmp(f->string, "tool_calls") && cJSON_IsArray(f)) {
            merge_calls(st, f);
        } else if (!strcmp(f->string, "reasoning_details") && cJSON_IsArray(f)) {
            merge_details(st, f);
        } else if (strcmp(f->string, "role") && cJSON_IsString(f) && *f->valuestring) {
            add_text(st, f->string, f->valuestring);
        }
    }
    cJSON_Delete(chunk);
}

static size_t on_stream(char *p, size_t sz, size_t n, void *ud) {
    stream *st = ud;
    size_t len = sz * n;
    buf_add(&st->raw, p, len);
    long status = 0;
    curl_easy_getinfo(st->c, CURLINFO_RESPONSE_CODE, &status);
    if (status != 200) return len; /* an error body: plain JSON, kept in raw */
    buf_add(&st->line, p, len);
    char *start = st->line.p, *nl;
    while ((nl = memchr(start, '\n', st->line.n - (size_t)(start - st->line.p)))) {
        *nl = 0;
        if (nl > start && nl[-1] == '\r') nl[-1] = 0;
        if (!strncmp(start, "data:", 5)) stream_event(st, start + 5 + (start[5] == ' '));
        start = nl + 1; /* comments (":") and other fields are ignored */
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
static cJSON *request(agent *a, const char *path, const char *body, int streaming) {
    const char *base = getenv(a->prov->base_env);
    buf url = {0};
    base = base && *base ? base : a->prov->base;
    buf_add(&url, base, strlen(base));
    buf_add(&url, path, strlen(path));

    struct curl_slist *h = curl_slist_append(NULL, "content-type: application/json");
    char *auth = a->api_key && *a->api_key ? fmt("authorization: Bearer %s", a->api_key) : NULL;
    if (auth) h = curl_slist_append(h, auth);

    const char *d = getenv("AGENT_RETRY_DELAY_MS"); /* first back-off; doubles each retry */
    long delay_ms = d && atol(d) > 0 ? atol(d) : 1000;
    cJSON *resp = NULL;
    a->streamed = 0;
    for (int attempt = 0; attempt <= RETRIES; attempt++) {
        if (attempt) { /* a signal ends the wait early */
            long ms = delay_ms << (attempt - 1);
            struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
            nanosleep(&ts, NULL);
        }
        if (agent_interrupted) break;
        stream st = {0};
        CURL *c = st.c = curl_easy_init();
        curl_easy_setopt(c, CURLOPT_URL, url.p);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        if (body) curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, streaming ? on_stream : on_body);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, streaming ? (void *)&st : (void *)&st.raw);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 600L);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
        CURLcode rc = curl_easy_perform(c);
        long status = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(c);
        if (st.printed) putchar('\n'), fflush(stdout); /* end the streamed line */
        a->streamed = st.printed;
        const char *out = st.raw.p ? st.raw.p : "";
        if (rc == CURLE_OK && status == 200 && !st.error) {
            resp = st.events ? stream_result(&st) : cJSON_Parse(out); /* "stream" ignored */
            if (!resp) fprintf(stderr, "error: response is not JSON\n");
            stream_free(&st);
            break;
        }
        if (rc == CURLE_ABORTED_BY_CALLBACK) {
            fprintf(stderr, "interrupted\n");
            stream_free(&st);
            break;
        }
        if (rc == CURLE_COULDNT_CONNECT) { /* nothing listening: retrying cannot help */
            fprintf(stderr, "error: cannot connect to %s; is the server running?\n", url.p);
            stream_free(&st);
            break;
        }
        char *err = st.error ? cJSON_PrintUnformatted(st.error) : NULL;
        if (err) out = err;
        a->context_full = (status == 400 || st.error) && is_context_error(out);
        int retryable = (rc != CURLE_OK || status == 429 || status >= 500 || st.error) &&
                        !st.printed && !a->context_full; /* retrying cannot shrink the context */
        const char *again = retryable && attempt < RETRIES ? ", retrying" : "";
        if (st.error) fprintf(stderr, "stream error%s: %s\n", again, out);
        else fprintf(stderr, "api error (%s, http %ld)%s: %s\n", curl_easy_strerror(rc), status,
                     again, out);
        free(err);
        stream_free(&st);
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
    cJSON_AddTrueToObject(req, "stream");
    cJSON_AddItemToObject(req, "stream_options", cJSON_Parse("{\"include_usage\":true}"));
    /* Turns on prompt caching for Anthropic models, where it is opt-in. Providers
       that cache automatically or lack support ignore unknown fields. */
    if (a->prov->cache_control)
        cJSON_AddItemToObject(req, "cache_control", cJSON_Parse("{\"type\":\"ephemeral\"}"));
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    cJSON *resp = request(a, "/chat/completions", body, 1);
    free(body);
    /* Saved only once the server accepts them, so a mistyped choice is not kept. */
    if (resp && a->save_provider) {
        agent_store_state("provider", a->prov->name);
        a->save_provider = 0;
    }
    if (resp && a->save_model) {
        char *key = fmt("%s.model", a->prov->name);
        agent_store_state(key, a->model);
        free(key);
        a->save_model = 0;
    }
    return resp;
}

void agent_list_models(agent *a, const char *filter) {
    cJSON *resp = request(a, "/models", NULL, 0), *m;
    cJSON *data = cJSON_GetObjectItem(resp, "data");
    if (resp && !cJSON_IsArray(data)) fprintf(stderr, "error: no model list in response\n");
    cJSON_ArrayForEach(m, data) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(m, "id"));
        if (id && contains_ci(id, filter)) printf("%s %s\n", strcmp(id, a->model) ? " " : "*", id);
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
    if (!a->streamed) print_text(get_str(msg, "content")); /* else already shown */
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
        /* Every call still gets a result, so the history stays valid. */
        char *out = agent_interrupted       ? xstrdup("skipped: interrupted by user")
                    : cJSON_IsObject(input) ? agent_run_tool(a, name ? name : "", input, &err)
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
    if (agent_interrupted) {
        fprintf(stderr, "interrupted\n");
        return 0; /* end the turn without asking the model again */
    }
    return ncalls > 0;
}

static int is_tool(cJSON *m) {
    const char *role = get_str(m, "role");
    return role && !strcmp(role, "tool");
}

/* Blank every tool result except the trailing batch, which the next reply needs.
   Returns how many were blanked. */
static int drop_old_tool_output(agent *a) {
    static const char *const dropped = "[output dropped to fit the context]";
    int n = cJSON_GetArraySize(a->messages), last = n, count = 0;
    while (last > 0 && is_tool(cJSON_GetArrayItem(a->messages, last - 1))) last--;
    for (int i = 0; i < last; i++) {
        cJSON *m = cJSON_GetArrayItem(a->messages, i);
        const char *content = get_str(m, "content");
        if (!is_tool(m) || !content || !strcmp(content, dropped)) continue;
        cJSON_ReplaceItemInObject(m, "content", cJSON_CreateString(dropped));
        count++;
    }
    return count;
}

int agent_ask(agent *a, const char *text) {
    agent_interrupted = 0;
    int before = cJSON_GetArraySize(a->messages);
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", text);
    cJSON_AddItemToArray(a->messages, m);
    int rc, requests = 0;
    double in = 0, out = 0, cached = 0; /* token counts, when the server reports them */
    for (;;) { /* call the model and run tools until it stops asking for them */
        cJSON *resp = call_api(a);
        cJSON *u = cJSON_GetObjectItem(resp, "usage");
        if (cJSON_IsObject(u)) {
            requests++;
            in += get_num(u, "prompt_tokens");
            out += get_num(u, "completion_tokens");
            cached += get_num(cJSON_GetObjectItem(u, "prompt_tokens_details"), "cached_tokens");
        }
        if (!resp && a->context_full) {
            int n = drop_old_tool_output(a);
            if (n) {
                fprintf(stderr, "context full: dropped %d old tool output%s, retrying\n", n,
                        n == 1 ? "" : "s");
                continue;
            }
            fprintf(stderr, "hint: the conversation no longer fits the model's context; "
                            "/clear starts a new one\n");
        }
        if (!resp) { rc = -1; break; }
        rc = agent_step(a, resp);
        cJSON_Delete(resp);
        if (rc != 1) break;
    }
    if (requests)
        fprintf(stderr, "[usage] %d request%s: %.0f in (%.0f cached), %.0f out\n", requests,
                requests == 1 ? "" : "s", in, cached, out);
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
    const char *cap = getenv("AGENT_MAX_OUTPUT");
    a->max_output = cap && atol(cap) >= 1024 ? (size_t)atol(cap) : p->max_output;
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

void agent_clear(agent *a) {
    while (cJSON_GetArraySize(a->messages) > 1)
        cJSON_DeleteItemFromArray(a->messages, cJSON_GetArraySize(a->messages) - 1);
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
