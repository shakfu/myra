/* agent.c - myralib: the agent: notes, the tool registry, dispatch and permission, and its lifecycle. */
#include "internal.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
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

atomic_int myra_interrupted, myra_terminated;
void (*myra_poll)(void);
void (*myra_note_hook)(myra_style style, const char *text);
char myra_last_error[1024];

void myra_note(myra_style style, const char *fmt, ...) {
    va_list ap, again;
    va_start(ap, fmt);
    va_copy(again, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    char *s = malloc(n < 0 ? 1 : (size_t)n + 1);
    if (!s) { perror("malloc"); exit(1); }
    vsnprintf(s, n < 0 ? 1 : (size_t)n + 1, fmt, again);
    va_end(again);
    va_end(ap);
    if (style == MYRA_ERROR) {
        size_t k = (size_t)snprintf(myra_last_error, sizeof myra_last_error, "%s", s);
        if (k >= sizeof myra_last_error) k = sizeof myra_last_error - 1;
        while (k && myra_last_error[k - 1] == '\n') myra_last_error[--k] = 0;
    }
    if (myra_note_hook) myra_note_hook(style, s);
    free(s);
}

/* Whether path, with symlinks resolved, lies outside the working directory. A path
   whose directory does not exist counts as inside: writing there fails anyway. */
int myra__outside_cwd(const char *path) {
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

/* Whether a mutating call must be confirmed before it runs. */
static int asks(myra_permissions mode, int outside) {
    return mode == MYRA_ASK || (mode == MYRA_AUTO && outside);
}

/* NULL if the call may run, else why not, for the model to relay. Asks io.allow when
   the mode asks; -1 from it, or no hook, means no one can answer. */
static const char *denied(myra_agent *a, int outside, const char *name, const char *detail) {
    myra_permissions mode = a->permissions;
    if (mode == MYRA_READ_ONLY) return "denied: the agent runs with --permissions read-only";
    if (!asks(mode, outside)) return NULL;
    int answer = a->io.allow ? a->io.allow(a->io.ud, name, detail, outside) : -1;
    if (answer > 0) return NULL;
    if (answer == 0) return "denied by user";
    return mode == MYRA_AUTO ? "denied: outside the working directory; --permissions all allows it"
                              : "denied: no terminal to confirm; --permissions auto skips asking";
}

const char *myra__str_arg(cJSON *input, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(input, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* The call's first line, unless verbose; a front end may cut it further. */
static void tool_log(myra_agent *a, const char *name, const char *detail) {
    size_t n = a->verbose ? strlen(detail) : strcspn(detail, "\n");
    myra_note(MYRA_TOOL, "[tool] %s %.*s%s\n", name, (int)n, detail, detail[n] ? " ..." : "");
}

/* Log the call, and tell the front end. */
static void tool_start(myra_agent *a, const char *name, const char *detail, const cJSON *input,
                       int outside, int asking) {
    tool_log(a, name, detail);
    a->announced = 1;
    int pending = asking && asks(a->permissions, outside);
    if (a->io.tool) a->io.tool(a->io.ud, name, detail, input, pending);
}

/* text, freed, cut to the tool result cap as a local read would be. */
char *myra__capped(myra_agent *a, char *text) {
    sink s;
    myra__sink_init(&s, a->max_output);
    myra__sink_add(&s, text, strlen(text));
    free(text);
    char *out = myra__sink_take(&s);
    myra__sink_free(&s);
    return out;
}

/* ---- the tool registry ---- */

int myra_add_tool(myra_agent *a, const myra_tool *t) {
    if (!t->name || !*t->name || !t->run || !t->parameters) {
        myra_note(MYRA_ERROR, "error: tool %s: it needs a name, parameters and run\n",
                  t->name && *t->name ? t->name : "(unnamed)");
        return -1;
    }
    cJSON *schema = cJSON_Parse(t->parameters);
    int taken = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, a->tools) {
        const char *n = myra__get_str(cJSON_GetObjectItem(it, "function"), "name");
        if (n && !strcmp(n, t->name)) taken = 1;
    }
    if (!cJSON_IsObject(schema) || taken) {
        myra_note(MYRA_ERROR, taken ? "error: tool %s: the name is taken\n"
                                    : "error: tool %s: parameters is not a JSON object\n", t->name);
        cJSON_Delete(schema);
        return -1;
    }
    myra_tool_entry *grown = realloc(a->registry, (a->ntools + 1) * sizeof *grown);
    if (!grown) { perror("realloc"); exit(1); }
    a->registry = grown;
    a->registry[a->ntools++] = (myra_tool_entry){.tool = *t, .schema = schema};
    cJSON *fn = cJSON_CreateObject(), *def = cJSON_CreateObject();
    cJSON_AddStringToObject(def, "type", "function");
    cJSON_AddStringToObject(fn, "name", t->name);
    cJSON_AddStringToObject(fn, "description", t->description ? t->description : "");
    cJSON_AddItemToObject(fn, "parameters", cJSON_Duplicate(schema, 1));
    cJSON_AddItemToObject(def, "function", fn);
    cJSON_AddItemToArray(a->tools, def);
    return 0;
}

/* Whether input has each of the schema's required arguments, of its declared type. */
static int has_required(const cJSON *schema, cJSON *input) {
    cJSON *props = cJSON_GetObjectItemCaseSensitive(schema, "properties"), *req;
    cJSON_ArrayForEach(req, cJSON_GetObjectItemCaseSensitive(schema, "required")) {
        if (!cJSON_IsString(req)) continue;
        cJSON *v = cJSON_GetObjectItemCaseSensitive(input, req->valuestring);
        const char *type = myra__get_str(cJSON_GetObjectItemCaseSensitive(props, req->valuestring), "type");
        if (!v || cJSON_IsNull(v)) return 0;
        if (!type) continue;
        if ((!strcmp(type, "string") && !cJSON_IsString(v)) ||
            ((!strcmp(type, "integer") || !strcmp(type, "number")) && !cJSON_IsNumber(v)) ||
            (!strcmp(type, "integer") &&
             !(isfinite(v->valuedouble) && v->valuedouble == floor(v->valuedouble))) ||
            (!strcmp(type, "boolean") && !cJSON_IsBool(v)) ||
            (!strcmp(type, "object") && !cJSON_IsObject(v)) ||
            (!strcmp(type, "array") && !cJSON_IsArray(v)))
            return 0;
    }
    return 1;
}

char *myra_run_tool(myra_agent *a, const char *name, cJSON *input, int *err) {
    *err = 1;
    for (size_t i = 0; i < a->ntools; i++) {
        const myra_tool *t = &a->registry[i].tool;
        if (strcmp(t->name, name)) continue;
        if (!cJSON_IsObject(input) || !has_required(a->registry[i].schema, input)) break;
        char *json = t->detail ? NULL : cJSON_PrintUnformatted(input);
        const char *detail = t->detail ? myra__str_arg(input, t->detail) : json;
        if (!detail) detail = "";
        const char *path = myra__str_arg(input, "path");
        int outside = t->access == MYRA_TOOL_WRITES && (!path || myra__outside_cwd(path));
        tool_start(a, name, detail, input, outside, t->access != MYRA_TOOL_READS);
        const char *why = t->access == MYRA_TOOL_READS ? NULL : denied(a, outside, name, detail);
        free(json);
        if (why) return xstrdup(why);
        return t->run(a, t, input, err);
    }
    if (myra_mcp_has(a, name)) {
        char *args = cJSON_PrintUnformatted(input);
        tool_start(a, name, args ? args : "", input, 0, 1);
        const char *why = denied(a, 0, name, args ? args : ""); /* unconfinable, like shell */
        free(args);
        if (why) return xstrdup(why);
        return myra__capped(a, myra_mcp_call(a, name, input, err));
    }
    return myra__fmt("unknown tool or missing arguments: %s", name);
}

/* ---- session ---- */

int myra_init(myra_agent *a, const myra_provider *p, const char *model, myra_permissions perms) {
    memset(a, 0, sizeof *a);
    a->prov = p;
    a->permissions = perms;
    const char *cap = getenv("MYRA_MAX_OUTPUT");
    a->max_output = cap && atol(cap) >= 1024 ? (size_t)atol(cap) : p->max_output;
    /* read takes the whole-file path for a file up to the cap, and that path stops at MYRA_MAX_RAW */
    if (a->max_output > MYRA_MAX_RAW) a->max_output = MYRA_MAX_RAW;
    a->api_key = getenv(p->key_env);
    if ((!a->api_key || !*a->api_key) && !p->key_optional) {
        myra_note(MYRA_ERROR, "error: %s is not set\n", p->key_env);
        return -1;
    }
    char *key = myra__fmt("%s.model", p->name);
    char *remembered = myra_load_state(key);
    free(key);
    if (model) a->save_model = !remembered || strcmp(model, remembered);
    a->model = xstrdup(model ? model : remembered ? remembered : p->model);
    free(remembered);

    a->tools = cJSON_CreateArray();
    a->messages = cJSON_CreateArray();
    cJSON *sys = cJSON_CreateObject(); /* rollback in myra_ask never reaches index 0 */
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", "");
    cJSON_AddItemToArray(a->messages, sys);
    myra_set_cwd(a);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    return 0;
}

void myra_set_cwd(myra_agent *a) {
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, ".");
    const char *sys = a->system ? a->system : "", *at = strstr(sys, "{cwd}");
    buf b = {0};
    buf_add(&b, sys, at ? (size_t)(at - sys) : strlen(sys));
    if (at) {
        buf_add(&b, cwd, strlen(cwd));
        buf_add(&b, at + 5, strlen(at + 5));
    }
    if (a->family.prompt) {
        if (b.n) buf_lit(&b, "\n\n");
        buf_add(&b, a->family.prompt, strlen(a->family.prompt));
    }
    cJSON_ReplaceItemInObject(cJSON_GetArrayItem(a->messages, 0), "content",
                              cJSON_CreateString(b.p ? b.p : ""));
    free(b.p);
}

void myra_free(myra_agent *a) {
    myra_kill_jobs();
    myra_mcp_stop(a);
    if (a->messages) curl_global_cleanup(); /* paired with a successful myra_init */
    cJSON_Delete(a->messages);
    cJSON_Delete(a->tools);
    for (size_t i = 0; i < a->ntools; i++) cJSON_Delete(a->registry[i].schema);
    free(a->registry);
    free(a->model);
    memset(a, 0, sizeof *a);
}

const char *myra_last_reply(myra_agent *a) {
    for (int i = cJSON_GetArraySize(a->messages) - 1; i > 0; i--) {
        cJSON *m = cJSON_GetArrayItem(a->messages, i);
        const char *text = myra__get_str(m, "content"), *role = myra__get_str(m, "role");
        if (role && !strcmp(role, "assistant") && text && *text) return text;
    }
    return NULL;
}

void myra_clear(myra_agent *a) {
    while (cJSON_GetArraySize(a->messages) > 1)
        cJSON_DeleteItemFromArray(a->messages, cJSON_GetArraySize(a->messages) - 1);
}

void myra_set_model(myra_agent *a, const char *model) {
    free(a->model);
    a->model = xstrdup(model);
    a->save_model = 1;
}

