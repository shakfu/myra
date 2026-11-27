/* state.c - myralib: remembered state and saved sessions. */
#include "internal.h"

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

/* ---- remembered state ---- */

char *myra_state_path(const char *name, int make_dir) {
    const char *xdg = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    buf b = {0};
    if (xdg && *xdg) buf_add(&b, xdg, strlen(xdg));
    else if (home && *home) { buf_add(&b, home, strlen(home)); buf_lit(&b, "/.local/state"); }
    else return NULL;
    buf_lit(&b, "/myra/");
    buf_add(&b, name, strlen(name));
    for (char *p = b.p + 1; make_dir && *p; p++) /* mkdir -p the parent */
        if (*p == '/') { *p = 0; mkdir(b.p, 0755); *p = '/'; }
    return b.p;
}

char *myra_load_state(const char *name) {
    char *path = myra_state_path(name, 0);
    buf b = {0};
    int rc = path ? slurp(path, &b) : -1;
    free(path);
    if (rc < 0) { free(b.p); return NULL; }
    while (b.n && strchr(" \t\r\n", b.p[b.n - 1])) b.p[--b.n] = 0;
    if (!b.n) { free(b.p); return NULL; }
    return b.p;
}

void myra_store_state(const char *name, const char *value) {
    char *path = myra_state_path(name, 1);
    if (!path) return;
    char *line = myra__fmt("%s\n", value);
    if (myra__spit(path, line, strlen(line), -1) < 0) myra_note(MYRA_WARN, "warning: cannot save %s\n", path);
    free(line);
    free(path);
}

/* ---- saved sessions ---- */

/* Letters, digits, '-' and '_' only, so an id cannot name a path outside sessions/. */
static int valid_session_id(const char *id) {
    size_t n = strlen(id);
    return n && n < sizeof ((myra_agent *)0)->session_id &&
           strspn(id, "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_") == n;
}

static char *session_path(const char *id, int make_dir) {
    char *name = myra__fmt("sessions/%s.json", id);
    char *path = myra_state_path(name, make_dir);
    free(name);
    return path;
}

/* A random (version 4) UUID into out; -1 if /dev/urandom cannot be read. */
static int new_session_id(char out[37]) {
    unsigned char r[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    ssize_t n = fd < 0 ? -1 : read(fd, r, sizeof r);
    if (fd >= 0) close(fd);
    if (n != (ssize_t)sizeof r) return -1;
    r[6] = (r[6] & 0x0f) | 0x40;
    r[8] = (r[8] & 0x3f) | 0x80;
    for (int i = 0, at = 0; i < 16; i++)
        at += sprintf(out + at, "%s%02x", i == 4 || i == 6 || i == 8 || i == 10 ? "-" : "", r[i]);
    return 0;
}

cJSON *myra_session_load(const char *id) {
    char *path = valid_session_id(id) ? session_path(id, 0) : NULL;
    buf b = {0};
    int rc = path ? slurp(path, &b) : -1, e = path ? errno : ENOENT;
    free(path);
    if (rc < 0) {
        if (e == ENOENT) myra_note(MYRA_ERROR, "error: no session %s\n", id);
        else myra_note(MYRA_ERROR, "error: cannot read session %s: %s\n", id, strerror(e));
        free(b.p);
        return NULL;
    }
    cJSON *s = cJSON_Parse(b.p), *msgs = cJSON_GetObjectItem(s, "messages"), *m;
    free(b.p);
    int ok = cJSON_IsArray(msgs) && myra__get_str(s, "cwd");
    cJSON_ArrayForEach(m, msgs) ok = ok && cJSON_IsObject(m) && myra__get_str(m, "role");
    if (!ok) {
        myra_note(MYRA_ERROR, "error: cannot read session %s: not a myra session\n", id);
        cJSON_Delete(s);
        return NULL;
    }
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, ".");
    if (strcmp(myra__get_str(s, "cwd"), cwd)) { /* its history names files there */
        myra_note(MYRA_ERROR, "error: session %s was saved in %s, not %s\n", id, myra__get_str(s, "cwd"), cwd);
        cJSON_Delete(s);
        return NULL;
    }
    return s;
}

void myra_session_resume(myra_agent *a, const cJSON *s, const char *id) {
    myra_clear(a);
    cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(s, "messages"))
        cJSON_AddItemToArray(a->messages, cJSON_Duplicate(m, 1));
    snprintf(a->session_id, sizeof a->session_id, "%s", id);
}

int myra_session_save(myra_agent *a) {
    if (!*a->session_id && new_session_id(a->session_id) < 0) {
        myra_note(MYRA_WARN, "warning: cannot save the session: no random id\n");
        return -1;
    }
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, ".");
    cJSON *s = cJSON_CreateObject(), *msgs = cJSON_CreateArray();
    cJSON_AddNumberToObject(s, "version", 1);
    cJSON_AddStringToObject(s, "id", a->session_id);
    cJSON_AddStringToObject(s, "cwd", cwd);
    cJSON_AddStringToObject(s, "provider", a->prov->name);
    cJSON_AddStringToObject(s, "model", a->model);
    /* The system prompt is left out: resuming rebuilds it for the same directory. */
    for (int i = 1; i < cJSON_GetArraySize(a->messages); i++)
        cJSON_AddItemReferenceToArray(msgs, cJSON_GetArrayItem(a->messages, i));
    cJSON_AddItemToObject(s, "messages", msgs);
    char *text = cJSON_PrintUnformatted(s), *path = session_path(a->session_id, 1);
    cJSON_Delete(s);
    /* 0600: tool results can hold secrets */
    int rc = text && path ? myra__spit(path, text, strlen(text), 0600) : -1;
    if (rc < 0) {
        myra_note(MYRA_WARN, "warning: cannot save session %s\n", path ? path : a->session_id);
        a->session_id[0] = 0;
    }
    free(text);
    free(path);
    return rc;
}

