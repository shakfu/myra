/* acp.c - myra as an Agent Client Protocol agent: JSON-RPC, one message per line, on
   stdin and stdout (https://agentclientprotocol.com). The client (an editor) serves
   file reads and writes, so the agent sees unsaved buffers, and answers permission
   questions in its own UI. stdout carries only protocol messages; notes go to stderr.

   One thread: while a turn runs, myra_poll drains stdin, so session/cancel is seen
   during a model request or shell command; other messages wait in a queue. */
#include "acp.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROTOCOL_VERSION 1

static struct {
    char *line;   /* bytes of an incomplete line */
    size_t n;
    cJSON *queue; /* parsed messages not yet handled */
    int eof;
    int next_id;
    int calls;    /* tool calls this session, for toolCallId */
    int waiting;  /* id of the outstanding call() to the client, or 0 */
    int initialized;
    char session[16]; /* empty until session/new */
} in;

static void send_msg(cJSON *msg) {
    cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
    char *s = cJSON_PrintUnformatted(msg); /* escapes newlines: one message, one line */
    cJSON_Delete(msg);
    if (!s) return;
    fputs(s, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    free(s);
}

static void reply(cJSON *id, cJSON *result) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddItemToObject(m, "id", cJSON_Duplicate(id, 1));
    cJSON_AddItemToObject(m, "result", result);
    send_msg(m);
}

static void reply_error(cJSON *id, int code, const char *message) {
    cJSON *m = cJSON_CreateObject(), *e = cJSON_CreateObject();
    cJSON_AddItemToObject(m, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    cJSON_AddItemToObject(m, "error", e);
    send_msg(m);
}

static int is_method(cJSON *m, const char *name) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(m, "method"));
    return s && !strcmp(s, name);
}

/* Parse complete lines into the queue; session/cancel acts at once. */
static void take_lines(void) {
    char *start = in.line, *nl;
    while ((nl = memchr(start, '\n', in.n - (size_t)(start - in.line)))) {
        *nl = 0;
        cJSON *m = cJSON_Parse(start);
        if (!m && in.waiting) {
            /* Most likely the reply call() awaits (a lone surrogate, say): fail that
               call rather than wait for an id that will never parse. */
            m = cJSON_CreateObject();
            cJSON_AddNumberToObject(m, "id", in.waiting);
            cJSON_AddItemToObject(m, "error", cJSON_CreateObject());
            cJSON_AddStringToObject(cJSON_GetObjectItem(m, "error"), "message",
                                    "unparseable reply from the client");
            cJSON_AddItemToArray(in.queue, m);
        } else if (!m) reply_error(NULL, -32700, "parse error");
        else if (is_method(m, "session/cancel")) myra_interrupted = 1, cJSON_Delete(m);
        else cJSON_AddItemToArray(in.queue, m);
        start = nl + 1;
    }
    in.n -= (size_t)(start - in.line);
    memmove(in.line, start, in.n);
}

/* Read what stdin has; wait for it when block is set. */
static void fill(int block) {
    if (in.eof) return;
    struct pollfd p = {.fd = STDIN_FILENO, .events = POLLIN};
    if (poll(&p, 1, block ? -1 : 0) != 1) return;
    char buf[65536];
    ssize_t k = read(STDIN_FILENO, buf, sizeof buf);
    if (k < 0 && errno == EINTR) return;
    if (k <= 0) { in.eof = 1; return; }
    char *grown = realloc(in.line, in.n + (size_t)k + 1);
    if (!grown) { perror("realloc"); exit(1); }
    in.line = grown;
    memcpy(in.line + in.n, buf, (size_t)k);
    in.n += (size_t)k;
    take_lines();
}

static void poll_stdin(void) { fill(0); }

/* The next queued message that is a response to id (id >= 0) or a request or
   notification (id < 0); NULL at EOF. */
static cJSON *next(int id) {
    for (;;) {
        cJSON *m;
        int i = 0;
        cJSON_ArrayForEach(m, in.queue) {
            cJSON *mid = cJSON_GetObjectItem(m, "id");
            int is_response = !cJSON_GetObjectItem(m, "method");
            if (id < 0 ? !is_response : is_response && cJSON_IsNumber(mid) && mid->valueint == id)
                return cJSON_DetachItemFromArray(in.queue, i);
            i++;
        }
        if (in.eof) return NULL;
        fill(1);
    }
}

/* Send a request to the client and wait for its response; NULL at EOF. */
static cJSON *call(const char *method, cJSON *params) {
    int id = ++in.next_id;
    cJSON *m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "id", id);
    cJSON_AddStringToObject(m, "method", method);
    cJSON_AddItemToObject(m, "params", params);
    send_msg(m);
    in.waiting = id;
    cJSON *r = next(id);
    in.waiting = 0;
    return r;
}

static cJSON *session_params(void) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "sessionId", in.session);
    return p;
}

static void update(cJSON *u) {
    cJSON *p = session_params(), *m = cJSON_CreateObject();
    cJSON_AddItemToObject(p, "update", u);
    cJSON_AddStringToObject(m, "method", "session/update");
    cJSON_AddItemToObject(m, "params", p);
    send_msg(m);
}

/* ---- myra_io hooks ---- */

static void on_text(void *ud, const char *s) {
    (void)ud;
    cJSON *u = cJSON_CreateObject(), *c = cJSON_CreateObject();
    cJSON_AddStringToObject(u, "sessionUpdate", "agent_message_chunk");
    cJSON_AddStringToObject(c, "type", "text");
    cJSON_AddStringToObject(c, "text", s);
    cJSON_AddItemToObject(u, "content", c);
    update(u);
}

static const char *tool_kind(const char *name) {
    return !strcmp(name, "read") ? "read" : !strcmp(name, "shell") ? "execute" : "edit";
}

/* "name detail", detail cut at its first line. */
static char *tool_title(const char *name, const char *detail) {
    size_t n = strcspn(detail, "\n");
    char *t = malloc(strlen(name) + n + 2);
    if (!t) { perror("malloc"); exit(1); }
    sprintf(t, "%s %.*s", name, (int)n, detail);
    return t;
}

static void on_tool(void *ud, const char *name, const char *detail) {
    (void)ud;
    char id[32], *title = tool_title(name, detail);
    snprintf(id, sizeof id, "call_%d", ++in.calls);
    cJSON *u = cJSON_CreateObject();
    cJSON_AddStringToObject(u, "sessionUpdate", "tool_call");
    cJSON_AddStringToObject(u, "toolCallId", id);
    cJSON_AddStringToObject(u, "title", title);
    cJSON_AddStringToObject(u, "kind", tool_kind(name));
    cJSON_AddStringToObject(u, "status", "in_progress");
    update(u);
    free(title);
}

/* The error message of a response, malloc'd, or NULL if it succeeded. */
static char *failure(cJSON *resp) {
    if (!resp) return strdup("the client closed the connection");
    cJSON *e = cJSON_GetObjectItem(resp, "error");
    if (!e) return NULL;
    const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(e, "message"));
    return strdup(msg ? msg : "error");
}

static char *on_read(void *ud, const char *path, char **why) {
    (void)ud;
    cJSON *p = session_params();
    cJSON_AddStringToObject(p, "path", path);
    cJSON *resp = call("fs/read_text_file", p);
    const char *text = cJSON_GetStringValue(
        cJSON_GetObjectItem(cJSON_GetObjectItem(resp, "result"), "content"));
    char *out = NULL;
    if ((*why = failure(resp))) {}
    else if (!text) *why = strdup("no content in the client's reply");
    else out = strdup(text);
    cJSON_Delete(resp);
    return out;
}

static char *on_write(void *ud, const char *path, const char *content) {
    (void)ud;
    cJSON *p = session_params();
    cJSON_AddStringToObject(p, "path", path);
    cJSON_AddStringToObject(p, "content", content);
    cJSON *resp = call("fs/write_text_file", p);
    char *why = failure(resp);
    cJSON_Delete(resp);
    return why;
}

static cJSON *option(const char *id, const char *name, const char *kind) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "optionId", id);
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddStringToObject(o, "kind", kind);
    return o;
}

static int on_allow(void *ud, const char *name, const char *detail, int outside) {
    (void)ud;
    char id[32], *title = tool_title(name, detail);
    snprintf(id, sizeof id, "call_%d", in.calls); /* the call on_tool just announced */
    cJSON *p = session_params(), *tc = cJSON_CreateObject(), *opts = cJSON_CreateArray();
    cJSON_AddStringToObject(tc, "toolCallId", id);
    if (outside) {
        char *t = malloc(strlen(title) + 40);
        if (!t) { perror("malloc"); exit(1); }
        sprintf(t, "%s (outside the working directory)", title);
        free(title);
        title = t;
    }
    cJSON_AddStringToObject(tc, "title", title);
    cJSON_AddStringToObject(tc, "kind", tool_kind(name));
    cJSON_AddItemToObject(p, "toolCall", tc);
    cJSON_AddItemToArray(opts, option("allow", "Allow", "allow_once"));
    cJSON_AddItemToArray(opts, option("reject", "Reject", "reject_once"));
    cJSON_AddItemToObject(p, "options", opts);
    free(title);
    cJSON *resp = call("session/request_permission", p);
    cJSON *outcome = cJSON_GetObjectItem(cJSON_GetObjectItem(resp, "result"), "outcome");
    const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(outcome, "outcome"));
    const char *chosen = cJSON_GetStringValue(cJSON_GetObjectItem(outcome, "optionId"));
    int ok = kind && !strcmp(kind, "selected") && chosen && !strcmp(chosen, "allow");
    cJSON_Delete(resp);
    return ok;
}

/* ---- requests ---- */

static void initialize(myra_agent *a, cJSON *id, cJSON *params) {
    cJSON *fs = cJSON_GetObjectItem(cJSON_GetObjectItem(params, "clientCapabilities"), "fs");
    a->io = (myra_io){.text = on_text, .tool = on_tool, .allow = on_allow};
    /* Files go through the client only if it serves both: edit reads, then writes. */
    if (cJSON_IsTrue(cJSON_GetObjectItem(fs, "readTextFile")) &&
        cJSON_IsTrue(cJSON_GetObjectItem(fs, "writeTextFile")))
        a->io.read = on_read, a->io.write = on_write;
    cJSON *r = cJSON_CreateObject(), *caps = cJSON_CreateObject(), *info = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "protocolVersion", PROTOCOL_VERSION);
    cJSON_AddFalseToObject(caps, "loadSession");
    cJSON_AddItemToObject(caps, "promptCapabilities", cJSON_CreateObject());
    cJSON_AddItemToObject(r, "agentCapabilities", caps);
    cJSON_AddStringToObject(info, "name", "myra");
    cJSON_AddStringToObject(info, "version", MYRA_VERSION);
    cJSON_AddItemToObject(r, "agentInfo", info);
    cJSON_AddItemToObject(r, "authMethods", cJSON_CreateArray());
    reply(id, r);
}

/* One session at a time: a new one starts a fresh conversation in cwd. */
static void new_session(myra_agent *a, cJSON *id, cJSON *params) {
    const char *cwd = cJSON_GetStringValue(cJSON_GetObjectItem(params, "cwd"));
    if (cwd && chdir(cwd) != 0) {
        reply_error(id, -32602, "cannot change to the session's cwd");
        return;
    }
    myra_set_cwd(a);
    myra_clear(a);
    in.calls = 0;
    snprintf(in.session, sizeof in.session, "%d", atoi(in.session) + 1);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "sessionId", in.session);
    reply(id, r);
}

/* The prompt's text blocks, with each resource link named, as one user message. */
static char *prompt_text(cJSON *blocks) {
    size_t cap = 1;
    cJSON *b;
    cJSON_ArrayForEach(b, blocks) {
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(b, "text"));
        const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(b, "uri"));
        cap += (t ? strlen(t) : 0) + (u ? strlen(u) + 32 : 0) + 2;
    }
    char *s = calloc(cap, 1);
    if (!s) { perror("calloc"); exit(1); }
    cJSON_ArrayForEach(b, blocks) {
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(b, "type"));
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(b, "text"));
        const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(b, "uri"));
        if (*s) strcat(s, "\n\n");
        if (type && !strcmp(type, "text") && t) strcat(s, t);
        else if (type && !strcmp(type, "resource_link") && u) {
            strcat(s, "Context: the user's open file ");
            strcat(s, MYRA_STARTS_WITH(u, "file://") ? u + 7 : u);
        }
    }
    return s;
}

static void prompt(myra_agent *a, cJSON *id, cJSON *params) {
    char *text = prompt_text(cJSON_GetObjectItem(params, "prompt"));
    int rc = myra_ask(a, text); /* clears myra_interrupted first */
    free(text);
    if (myra_interrupted) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "stopReason", "cancelled");
        reply(id, r);
    } else if (rc < 0) {
        reply_error(id, -32603, "the model request failed; the agent's stderr says why");
    } else {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "stopReason", "end_turn");
        reply(id, r);
    }
    myra_interrupted = 0;
}

int myra_acp(myra_agent *a) {
    in.queue = cJSON_CreateArray();
    myra_poll = poll_stdin;
    cJSON *m;
    while ((m = next(-1))) {
        cJSON *id = cJSON_GetObjectItem(m, "id"), *params = cJSON_GetObjectItem(m, "params");
        if (!id || cJSON_IsNull(id)) {
            /* a notification: none needs handling, and none gets a reply */
        } else if (is_method(m, "initialize")) {
            initialize(a, id, params);
            in.initialized = 1;
        } else if (!in.initialized) {
            /* before initialize the io hooks are unset: text would reach stdout raw */
            reply_error(id, -32600, "not initialized");
        } else if (is_method(m, "session/new")) {
            new_session(a, id, params);
        } else if (is_method(m, "session/prompt")) {
            const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(params, "sessionId"));
            if (!sid || !*in.session || strcmp(sid, in.session))
                reply_error(id, -32602, "unknown session");
            else prompt(a, id, params);
        } else {
            reply_error(id, -32601, "method not found");
        }
        cJSON_Delete(m);
    }
    myra_poll = NULL;
    cJSON_Delete(in.queue);
    free(in.line);
    return 0;
}
