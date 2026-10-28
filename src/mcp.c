/* mcp.c - a Model Context Protocol client over stdio (https://modelcontextprotocol.io).
   Each server's tools join the model's tools as mcp__<server>__<tool>. JSON-RPC, one
   message per line, on the server's stdin and stdout; its stderr is left as ours. */
#include "mcp.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define PROTOCOL_VERSION "2025-06-18"
#define START_TIMEOUT 60 /* seconds for initialize and tools/list: npx may download first */
#define NAME_MAX_LEN 64  /* OpenAI function names: [A-Za-z0-9_-]{1,64} */
#define MAX_PAGES 100    /* of tools/list: a server whose cursor never ends would hang startup */

/* What a server inherits of our environment, besides its own env: the MCP SDKs'
   POSIX default, so the provider key and other secrets do not reach it. */
static const char *const INHERITED[] = {"HOME", "LOGNAME", "PATH", "SHELL", "TERM", "USER"};

extern char **environ;

struct myra_mcp {
    myra_mcp *next;
    char *name;
    pid_t pid;      /* 0 once it has exited */
    pid_t pgid;     /* its process group, 0 once that is empty */
    int in, out;    /* its stdin, which we write; its stdout, which we read */
    char *line;     /* bytes of an incomplete line */
    size_t n;
    int next_id;
    cJSON *tools;   /* exposed name -> the server's own name */
};

/* Stop s, or with all set s and every server after it, together. Closing stdin asks
   each to exit; then SIGTERM, then SIGKILL, as the spec's shutdown. Each goes to the
   whole process group, so what a server started does not outlive it. */
static void reap(myra_mcp *s, int all) {
    for (myra_mcp *p = s; p; p = all ? p->next : NULL) {
        if (p->in >= 0) close(p->in);
        if (p->out >= 0) close(p->out);
        p->in = p->out = -1;
    }
    int sig[] = {0, SIGTERM, SIGKILL};
    for (int i = 0; i < 3; i++) {
        for (myra_mcp *p = s; p; p = all ? p->next : NULL)
            if (sig[i] && p->pgid) kill(-p->pgid, sig[i]);
        /* On the clock: counting sleeps stretches with each oversleep, as on a loaded VM. */
        for (double end = now() + 1;;) {
            int left = 0;
            for (myra_mcp *p = s; p; p = all ? p->next : NULL) {
                if (p->pid) {
                    pid_t w = waitpid(p->pid, NULL, WNOHANG);
                    if (w == p->pid || (w < 0 && errno != EINTR)) p->pid = 0;
                }
                /* Signal 0 finds no process once the group is empty. Checked after the
                   leader is reaped: its zombie would count. */
                if (p->pgid && kill(-p->pgid, 0) != 0) p->pgid = 0;
                left |= p->pid || p->pgid;
            }
            if (!left) return;
            if (now() >= end) break;
            usleep(10000);
        }
    }
    for (myra_mcp *p = s; p; p = all ? p->next : NULL) p->pgid = 0; /* its id may be reused */
}

/* Send one message: -1 if the server has exited, has not read it by deadline, or
   Ctrl-C came first. A message cut short would corrupt the next, so the server's
   stdin is then closed. */
static int send_line(myra_mcp *s, cJSON *msg, double deadline) {
    cJSON_AddStringToObject(msg, "jsonrpc", "2.0");
    char *text = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (!text || s->in < 0) { free(text); return -1; }
    /* Ignored only for this write: shell children would inherit a lasting SIG_IGN. */
    struct sigaction ign = {.sa_handler = SIG_IGN}, old;
    sigaction(SIGPIPE, &ign, &old);
    size_t len = strlen(text), off = 0;
    text[len++] = '\n'; /* replaces the NUL; len now counts the newline */
    while (off < len) {
        ssize_t k = write(s->in, text + off, len - off); /* s->in is non-blocking */
        if (k > 0) { off += (size_t)k; continue; }
        if (k < 0 && errno == EINTR) continue;
        if (k == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) break;
        if (myra_poll) myra_poll();
        if (myra_interrupted || now() >= deadline) break;
        struct pollfd p = {.fd = s->in, .events = POLLOUT};
        poll(&p, 1, 200);
    }
    sigaction(SIGPIPE, &old, NULL);
    free(text);
    if (off == len) return 0;
    if (off) { close(s->in); s->in = -1; }
    return -1;
}

static void notify(myra_mcp *s, const char *method, cJSON *params, double deadline) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "method", method);
    if (params) cJSON_AddItemToObject(m, "params", params);
    send_line(s, m, deadline);
}

/* Answer a request from the server: ping is the only one a client must serve. */
static void serve(myra_mcp *s, cJSON *req, double deadline) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddItemToObject(m, "id", cJSON_Duplicate(cJSON_GetObjectItem(req, "id"), 1));
    const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(req, "method"));
    if (method && !strcmp(method, "ping")) {
        cJSON_AddItemToObject(m, "result", cJSON_CreateObject());
    } else {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "code", -32601);
        cJSON_AddStringToObject(e, "message", "method not found");
        cJSON_AddItemToObject(m, "error", e);
    }
    send_line(s, m, deadline);
}

/* The next whole line from the server, parsed, or NULL with *why set. */
static cJSON *take(myra_mcp *s, double deadline, const char **why) {
    for (;;) {
        char *nl = s->line ? memchr(s->line, '\n', s->n) : NULL;
        if (nl) {
            *nl = 0;
            cJSON *m = cJSON_Parse(s->line);
            s->n -= (size_t)(nl + 1 - s->line);
            memmove(s->line, nl + 1, s->n);
            if (m) return m;
            continue; /* not JSON: a server's stray output, skipped */
        }
        if (s->out < 0) { *why = "the server has exited"; return NULL; }
        if (myra_poll) myra_poll();
        if (myra_interrupted) { *why = "interrupted by user"; return NULL; }
        double left = deadline - now();
        if (left <= 0) { *why = "timed out"; return NULL; }
        struct pollfd p = {.fd = s->out, .events = POLLIN};
        if (poll(&p, 1, left < 0.2 ? (int)(left * 1000) + 1 : 200) <= 0) continue;
        char buf[65536];
        ssize_t k = read(s->out, buf, sizeof buf);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) { reap(s, 0); continue; }
        if (s->n + (size_t)k > MYRA_MAX_RAW) { reap(s, 0); *why = "the server's reply is too large"; return NULL; }
        char *grown = realloc(s->line, s->n + (size_t)k + 1);
        if (!grown) { perror("realloc"); exit(1); }
        s->line = grown;
        memcpy(s->line + s->n, buf, (size_t)k);
        s->n += (size_t)k;
    }
}

/* Send a request and wait for its result; NULL with *why set (malloc'd) if it failed. */
static cJSON *rpc(myra_mcp *s, const char *method, cJSON *params, int timeout, char **why) {
    int id = ++s->next_id;
    cJSON *m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "id", id);
    cJSON_AddStringToObject(m, "method", method);
    cJSON_AddItemToObject(m, "params", params ? params : cJSON_CreateObject());
    double deadline = now() + timeout;
    if (send_line(s, m, deadline) < 0) {
        *why = xstrdup(myra_interrupted     ? "interrupted by user"
                       : now() >= deadline ? "timed out"
                                           : "the server has exited");
        return NULL;
    }
    for (;;) {
        const char *fail = NULL;
        cJSON *r = take(s, deadline, &fail);
        if (!r) {
            /* still running: tell it to stop; initialize is the one it must not be sent */
            if (s->out >= 0 && strcmp(method, "initialize")) {
                cJSON *p = cJSON_CreateObject();
                cJSON_AddNumberToObject(p, "requestId", id);
                cJSON_AddStringToObject(p, "reason", fail);
                notify(s, "notifications/cancelled", p, deadline);
            }
            *why = xstrdup(fail);
            return NULL;
        }
        cJSON *rid = cJSON_GetObjectItem(r, "id");
        if (cJSON_GetObjectItem(r, "method")) {
            if (rid && !cJSON_IsNull(rid)) serve(s, r, deadline); /* else a notification: ignored */
        } else if (cJSON_IsNumber(rid) && rid->valueint == id) {
            cJSON *e = cJSON_GetObjectItem(r, "error");
            cJSON *result = e ? NULL : cJSON_DetachItemFromObject(r, "result");
            if (e) {
                const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(e, "message"));
                *why = xstrdup(msg ? msg : "error");
            } else if (!result) {
                *why = xstrdup("no result in the server's reply");
            }
            cJSON_Delete(r);
            return result;
        } /* else a late response to a cancelled request */
        cJSON_Delete(r);
    }
}

/* s with each character outside [A-Za-z0-9_-] as '_', appended to out. */
static void add_safe(char *out, size_t cap, const char *s) {
    size_t n = strlen(out);
    for (; *s && n + 1 < cap; s++, n++) {
        char c = *s;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '_' || c == '-';
        out[n] = ok ? c : '_';
    }
    out[n] = 0;
}

static int has_tool(myra_agent *a, const char *name) {
    cJSON *t;
    cJSON_ArrayForEach(t, a->tools) {
        const char *n = cJSON_GetStringValue(
            cJSON_GetObjectItem(cJSON_GetObjectItem(t, "function"), "name"));
        if (n && !strcmp(n, name)) return 1;
    }
    return 0;
}

/* Stop a server, unlink it and remove its tools from the model's. */
static void drop(myra_agent *a, myra_mcp *s) {
    for (myra_mcp **p = &a->mcp; *p; p = &(*p)->next)
        if (*p == s) { *p = s->next; break; }
    reap(s, 0);
    for (int i = cJSON_GetArraySize(a->tools) - 1; i >= 0; i--) {
        const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(
            cJSON_GetObjectItem(cJSON_GetArrayItem(a->tools, i), "function"), "name"));
        if (n && cJSON_GetObjectItemCaseSensitive(s->tools, n))
            cJSON_DeleteItemFromArray(a->tools, i);
    }
    free(s->name);
    free(s->line);
    cJSON_Delete(s->tools);
    free(s);
}

/* Add the server's tools to the model's: -1 if tools/list failed. */
static int list_tools(myra_agent *a, myra_mcp *s) {
    char *cursor = NULL;
    int pages = 0;
    do {
        if (++pages > MAX_PAGES) {
            myra_note(MYRA_ERROR, "error: mcp %s: tools/list: more than %d pages\n", s->name,
                      MAX_PAGES);
            free(cursor);
            return -1;
        }
        cJSON *p = cJSON_CreateObject();
        if (cursor) cJSON_AddStringToObject(p, "cursor", cursor);
        free(cursor);
        cursor = NULL;
        char *why = NULL;
        cJSON *r = rpc(s, "tools/list", p, START_TIMEOUT, &why);
        if (!r) {
            myra_note(MYRA_ERROR, "error: mcp %s: tools/list: %s\n", s->name, why);
            free(why);
            return -1;
        }
        cJSON *t;
        cJSON_ArrayForEach(t, cJSON_GetObjectItem(r, "tools")) {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(t, "name"));
            if (!name) continue;
            char exposed[NAME_MAX_LEN + 1] = "mcp__";
            add_safe(exposed, sizeof exposed, s->name);
            add_safe(exposed, sizeof exposed, "__");
            add_safe(exposed, sizeof exposed, name);
            if (has_tool(a, exposed)) {
                myra_note(MYRA_WARN, "warning: mcp %s: skipped %s: %s is taken\n", s->name, name,
                          exposed);
                continue;
            }
            cJSON *schema = cJSON_GetObjectItem(t, "inputSchema");
            const char *desc = cJSON_GetStringValue(cJSON_GetObjectItem(t, "description"));
            cJSON *fn = cJSON_CreateObject(), *tool = cJSON_CreateObject();
            cJSON_AddStringToObject(fn, "name", exposed);
            cJSON_AddStringToObject(fn, "description", desc ? desc : "");
            cJSON_AddItemToObject(fn, "parameters", cJSON_IsObject(schema)
                                                        ? cJSON_Duplicate(schema, 1)
                                                        : cJSON_Parse("{\"type\":\"object\"}"));
            cJSON_AddStringToObject(tool, "type", "function");
            cJSON_AddItemToObject(tool, "function", fn);
            cJSON_AddItemToArray(a->tools, tool);
            cJSON_AddStringToObject(s->tools, exposed, name);
        }
        const char *next = cJSON_GetStringValue(cJSON_GetObjectItem(r, "nextCursor"));
        if (next && *next) cursor = xstrdup(next);
        cJSON_Delete(r);
    } while (cursor);
    return 0;
}

static void free_strings(char **v);

/* A server's environment: INHERITED's variables that are set, less any shell function
   exported as one, then env, which overrides them. The array and strings are malloc'd. */
static char **server_env(char *const *env) {
    size_t n = 0, ninh = sizeof INHERITED / sizeof *INHERITED, k = 0;
    while (env && env[n]) n++;
    char **v = calloc(ninh + n + 1, sizeof *v);
    if (!v) { perror("calloc"); exit(1); }
    for (size_t i = 0; i < ninh; i++) {
        const char *val = getenv(INHERITED[i]);
        size_t len = strlen(INHERITED[i]);
        int set = 0;
        for (size_t j = 0; j < n; j++)
            set |= !strncmp(env[j], INHERITED[i], len) && env[j][len] == '=';
        if (!val || !strncmp(val, "()", 2) || set) continue;
        buf b = {0};
        buf_add(&b, INHERITED[i], len);
        buf_lit(&b, "=");
        buf_add(&b, val, strlen(val));
        v[k++] = b.p;
    }
    for (size_t j = 0; j < n; j++) v[k++] = xstrdup(env[j]);
    return v;
}

/* Start a server and add its tools; -1 if it fails, with the reason on stderr. */
static int start(myra_agent *a, const char *name, const char *command, char *const *args,
                 char *const *env) {
    /* Built before fork: another thread may hold malloc's lock, so the child must not
       allocate. */
    size_t n = 0;
    while (args && args[n]) n++;
    char **argv = calloc(n + 2, sizeof *argv), **envp = server_env(env);
    if (!argv) { perror("calloc"); exit(1); }
    argv[0] = (char *)command;
    for (size_t i = 0; i < n; i++) argv[i + 1] = args[i];
    buf failed = {0};
    buf_lit(&failed, "error: mcp ");
    buf_add(&failed, name, strlen(name));
    buf_lit(&failed, ": cannot run ");
    buf_add(&failed, command, strlen(command));
    buf_lit(&failed, ": ");

    int to[2], from[2];
    pid_t pid = -1;
    if (pipe(to) < 0) goto fail;
    if (pipe(from) < 0) { close(to[0]); close(to[1]); goto fail; }
    if ((pid = fork()) < 0) {
        close(to[0]), close(to[1]), close(from[0]), close(from[1]);
        goto fail;
    }
    if (pid == 0) {
        setpgid(0, 0); /* the terminal's Ctrl-C reaches only the agent */
        dup2(to[0], 0);
        dup2(from[1], 1);
        close(to[0]), close(to[1]), close(from[0]), close(from[1]);
        environ = envp;
        execvp(command, argv);
        const char *why = strerror(errno);
        if (write(2, failed.p, failed.n) < 0 || write(2, why, strlen(why)) < 0 ||
            write(2, "\n", 1) < 0) {}
        _exit(127);
    }
    free(argv);
    free_strings(envp);
    free(failed.p);
    setpgid(pid, pid);
    close(to[0]);
    close(from[1]);
    /* else shell commands and later servers inherit them, and stdin never reaches EOF */
    fcntl(to[1], F_SETFD, FD_CLOEXEC);
    fcntl(from[0], F_SETFD, FD_CLOEXEC);
    fcntl(to[1], F_SETFL, O_NONBLOCK); /* so send_line can give up on a server not reading */
    myra_mcp *s = calloc(1, sizeof *s);
    if (!s) { perror("calloc"); exit(1); }
    *s = (myra_mcp){.name = xstrdup(name), .pid = pid, .pgid = pid, .in = to[1], .out = from[0],
                    .tools = cJSON_CreateObject()};
    s->next = a->mcp;
    a->mcp = s;

    cJSON *p = cJSON_CreateObject(), *info = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "protocolVersion", PROTOCOL_VERSION);
    cJSON_AddItemToObject(p, "capabilities", cJSON_CreateObject());
    cJSON_AddStringToObject(info, "name", "myra");
    cJSON_AddStringToObject(info, "version", MYRA_VERSION);
    cJSON_AddItemToObject(p, "clientInfo", info);
    char *why = NULL;
    cJSON *r = rpc(s, "initialize", p, START_TIMEOUT, &why);
    if (!r) {
        myra_note(MYRA_ERROR, "error: mcp %s: initialize: %s\n", name, why);
        free(why);
        drop(a, s);
        return -1;
    }
    cJSON_Delete(r);
    notify(s, "notifications/initialized", NULL, now() + START_TIMEOUT);
    if (list_tools(a, s) < 0) { drop(a, s); return -1; } /* with any earlier pages' tools */
    return 0;
fail:
    myra_note(MYRA_ERROR, "error: mcp %s: %s\n", name, strerror(errno));
    free(argv);
    free_strings(envp);
    free(failed.p);
    return -1;
}

/* The strings of a JSON array, NULL-terminated; each made by f from one item. The
   array and strings are malloc'd. */
static char **strings(cJSON *items, char *(*f)(cJSON *)) {
    char **v = calloc((size_t)cJSON_GetArraySize(items) + 1, sizeof *v);
    if (!v) { perror("calloc"); exit(1); }
    size_t n = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, items) if ((v[n] = f(it))) n++;
    return v;
}

static char *arg(cJSON *it) {
    return cJSON_IsString(it) ? xstrdup(it->valuestring) : NULL;
}

static char *env_var(cJSON *it) { /* {"name", "value"} as NAME=value */
    const char *k = cJSON_GetStringValue(cJSON_GetObjectItem(it, "name"));
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(it, "value"));
    if (!k || !v) return NULL;
    size_t n = strlen(k) + strlen(v) + 2;
    char *s = malloc(n);
    if (!s) { perror("malloc"); exit(1); }
    snprintf(s, n, "%s=%s", k, v);
    return s;
}

static void free_strings(char **v) {
    for (char **p = v; *p; p++) free(*p);
    free(v);
}

void myra_mcp_start_list(myra_agent *a, cJSON *servers) {
    cJSON *srv;
    cJSON_ArrayForEach(srv, servers) {
        if (myra_interrupted) break; /* Ctrl-C while starting: skip the rest */
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(srv, "type"));
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(srv, "name"));
        const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(srv, "command"));
        if (type && strcmp(type, "stdio")) {
            myra_note(MYRA_WARN, "warning: mcp %s: only stdio servers are supported\n",
                      name ? name : "?");
            continue;
        }
        if (!name || !cmd) {
            myra_note(MYRA_WARN, "warning: mcp %s: no command\n", name ? name : "?");
            continue;
        }
        char **args = strings(cJSON_GetObjectItem(srv, "args"), arg);
        char **env = strings(cJSON_GetObjectItem(srv, "env"), env_var);
        start(a, name, cmd, args, env);
        free_strings(args);
        free_strings(env);
    }
}

cJSON *myra_mcp_load(const char *path) {
    buf text = {0};
    if (slurp(path, &text) < 0) {
        myra_note(MYRA_ERROR, "error: cannot read %s: %s\n", path, strerror(errno));
        free(text.p);
        return NULL;
    }
    cJSON *doc = cJSON_Parse(text.p);
    free(text.p);
    cJSON *servers = cJSON_GetObjectItem(doc, "mcpServers");
    if (!cJSON_IsObject(servers)) {
        myra_note(MYRA_ERROR, "error: %s: expected {\"mcpServers\": {...}}\n", path);
        cJSON_Delete(doc);
        return NULL;
    }
    cJSON *list = cJSON_CreateArray(), *srv;
    cJSON_ArrayForEach(srv, servers) { /* {name: {command, args, env: {K: V}}} */
        cJSON *entry = cJSON_IsObject(srv) ? cJSON_Duplicate(srv, 1) : cJSON_CreateObject();
        cJSON *env = cJSON_DetachItemFromObject(entry, "env"), *pairs = cJSON_CreateArray(), *e;
        cJSON_ArrayForEach(e, env) {
            if (!cJSON_IsString(e)) continue;
            cJSON *pair = cJSON_CreateObject();
            cJSON_AddStringToObject(pair, "name", e->string);
            cJSON_AddStringToObject(pair, "value", e->valuestring);
            cJSON_AddItemToArray(pairs, pair);
        }
        cJSON_Delete(env);
        cJSON_AddItemToObject(entry, "env", pairs);
        cJSON_DeleteItemFromObject(entry, "name");
        cJSON_AddStringToObject(entry, "name", srv->string);
        cJSON_AddItemToArray(list, entry);
    }
    cJSON_Delete(doc);
    return list;
}

void myra_mcp_stop(myra_agent *a) {
    reap(a->mcp, 1); /* together: one slow server at a time would add up */
    while (a->mcp) drop(a, a->mcp);
}

/* The text of a tools/call result's content, one item per line. */
static char *result_text(cJSON *r) {
    buf out = {0};
    cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(r, "content")) {
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(c, "type"));
        cJSON *res = cJSON_GetObjectItem(c, "resource");
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(c, "text"));
        const char *uri = cJSON_GetStringValue(cJSON_GetObjectItem(c, "uri"));
        if (!text && res) text = cJSON_GetStringValue(cJSON_GetObjectItem(res, "text"));
        if (!uri && res) uri = cJSON_GetStringValue(cJSON_GetObjectItem(res, "uri"));
        if (out.n) buf_lit(&out, "\n");
        if (text) {
            buf_add(&out, text, strlen(text));
            continue;
        }
        /* images, audio, links: named, since the model gets text only */
        if (!type) type = "content";
        buf_lit(&out, "[");
        buf_add(&out, type, strlen(type));
        if (uri) buf_lit(&out, " "), buf_add(&out, uri, strlen(uri));
        buf_lit(&out, "]");
    }
    if (!out.n) { /* no content: the structured result, if any */
        cJSON *sc = cJSON_GetObjectItem(r, "structuredContent");
        char *s = sc ? cJSON_PrintUnformatted(sc) : NULL;
        if (s) { free(out.p); return s; }
    }
    return out.p ? out.p : xstrdup("");
}

/* The server that serves the exposed tool name, with *tool its own name for it. */
static myra_mcp *server_of(myra_agent *a, const char *name, const char **tool) {
    for (myra_mcp *s = a->mcp; s; s = s->next)
        if ((*tool = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(s->tools, name))))
            return s;
    return NULL;
}

int myra_mcp_has(myra_agent *a, const char *name) {
    const char *tool;
    return server_of(a, name, &tool) != NULL;
}

char *myra_mcp_call(myra_agent *a, const char *name, cJSON *input, int *err) {
    const char *tool;
    myra_mcp *s = server_of(a, name, &tool);
    *err = 1;
    if (!s) return xstrdup("unknown tool");
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "name", tool);
    cJSON_AddItemToObject(p, "arguments", cJSON_Duplicate(input, 1));
    char *why = NULL;
    cJSON *r = rpc(s, "tools/call", p, MYRA_SHELL_TIMEOUT_MAX, &why);
    if (!r) return why;
    char *out = result_text(r);
    *err = cJSON_IsTrue(cJSON_GetObjectItem(r, "isError"));
    cJSON_Delete(r);
    return out;
}
