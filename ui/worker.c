/* worker.c - the agent thread. myra_ask and myra_model_ids block, so they run here;
   the myra_io hooks write what happens into the transcript. */
#include "app.h"
#include "util.h"

#include <sys/stat.h>

#define DIFF_MAX (1 << 20) /* files above this get no diff: the view would be slow */

static app *the_app; /* for myra_note_hook, which takes no user data */

/* path's contents if it is a regular file of at most DIFF_MAX bytes; NULL otherwise. */
static char *small_file(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size > DIFF_MAX) return NULL;
    buf b = {0};
    if (slurp(path, &b) < 0) { free(b.p); return NULL; }
    return b.p;
}

/* s with its only occurrence of old replaced by new, as edit will do; NULL if old
   is absent or not unique, since edit then fails. */
static char *replaced(const char *s, const char *old, const char *new) {
    const char *at = *old ? strstr(s, old) : NULL;
    if (!at || strstr(at + 1, old)) return NULL;
    buf b = {0};
    buf_add(&b, s, (size_t)(at - s));
    buf_add(&b, new, strlen(new));
    buf_add(&b, at + strlen(old), strlen(at + strlen(old)));
    return b.p;
}

static void on_text(void *ud, const char *s) { tr_text(ud, s); }
static void on_reasoning(void *ud, const char *s) { tr_reasoning(ud, s); }
static void on_done(void *ud, int err, const char *result) { tr_done(ud, err, result); }

/* The tool hook runs before the tool, so the file on disk is still the "before". */
static void on_tool(void *ud, const char *name, const char *detail, const cJSON *input,
                    int pending) {
    char *json = input ? cJSON_Print(input) : NULL, *before = NULL, *after = NULL;
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(input, "path"));
    if (!strcmp(name, "write") && path) {
        const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(input, "content"));
        if (content && strlen(content) <= DIFF_MAX) {
            before = small_file(path); /* NULL: a new file */
            after = xstrdup(content);
            if (!before) before = xstrdup("");
        }
    } else if (!strcmp(name, "edit") && path) {
        const char *old = cJSON_GetStringValue(cJSON_GetObjectItem(input, "old_string"));
        const char *new = cJSON_GetStringValue(cJSON_GetObjectItem(input, "new_string"));
        char *file = old && new ? small_file(path) : NULL;
        if (file && (after = replaced(file, old, new))) before = file;
        else if (old && new) { /* show the fragments: edit will report why it failed */
            free(file);
            before = xstrdup(old), after = xstrdup(new);
        }
    }
    tr_tool(ud, name, detail, json, before, after, pending);
    free(json), free(before), free(after);
}

static int on_allow(void *ud, const char *name, const char *detail, int outside) {
    (void)name, (void)detail; /* already in the pending tool item */
    return tr_ask(ud, outside);
}

static void on_note(myra_style style, const char *text) {
    if (the_app) tr_note(&the_app->tr, (int)style, text);
}

void worker_install(app *s) {
    the_app = s;
    s->agent.io = (myra_io){.ud = &s->tr, .text = on_text, .tool = on_tool, .done = on_done,
                            .allow = on_allow, .reasoning = on_reasoning};
    myra_note_hook = on_note;
}

static void *run(void *arg) {
    app *s = arg;
    if (s->job == JOB_TURN) {
        int rc = myra_ask(&s->agent, s->prompt);
        tr_turn_end(&s->tr);
        if (rc < 0)
            tr_error(&s->tr, myra_interrupted ? "Stopped."
                             : *myra_last_error ? myra_last_error
                                                : "The request failed; the log says why.");
        free(s->prompt);
        s->prompt = NULL;
    } else {
        myra_free_model_ids(s->models);
        s->models = myra_model_ids(&s->agent);
        s->nmodels = 0;
        while (s->models && s->models[s->nmodels]) s->nmodels++;
        s->models_failed = !s->models;
    }
    atomic_store(&s->busy, false);
    return NULL;
}

bool worker_start(app *s, job_kind job) {
    if (atomic_load(&s->busy)) return false;
    if (s->started) pthread_join(s->worker, NULL); /* finished: busy was false */
    s->started = false;
    s->job = job;
    myra_last_error[0] = 0;
    atomic_store(&s->busy, true);
    if (pthread_create(&s->worker, NULL, run, s) == 0) return s->started = true;
    atomic_store(&s->busy, false);
    return false;
}

void worker_shutdown(app *s) {
    if (!s->started) return;
    /* Let a running job end at its next interrupt check instead of exiting under it,
       which could cut a write or shell command short. */
    myra_interrupted = 1;
    tr_close(&s->tr);
    pthread_join(s->worker, NULL);
    s->started = false;
}
