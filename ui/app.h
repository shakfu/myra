/* app.h - myra-ui's state, shared by the worker (worker.c) and the frame (view.c). */
#ifndef MYRA_UI_APP_H
#define MYRA_UI_APP_H

#include "cui.h"
#include "myra.h"
#include "transcript.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

typedef enum { JOB_TURN, JOB_MODELS } job_kind;

/* Work the frame asks for; run after it releases the transcript lock, since
   each one may call into myra, whose notes take that lock. */
typedef enum { ACT_NONE, ACT_SEND, ACT_NEW_CHAT, ACT_SET_MODEL, ACT_FETCH_MODELS, ACT_CHDIR } action;

typedef struct {
    myra_agent agent;
    transcript tr;

    /* The worker runs one job at a time. While busy, only it touches agent; the
       UI reads agent.session and the model list after busy turns false. */
    atomic_bool busy;
    pthread_t worker;
    bool started; /* worker was created and not yet joined */
    job_kind job;
    char *prompt;           /* JOB_TURN: the sent draft */
    char **models;          /* JOB_MODELS result, NULL-terminated; NULL before or on failure */
    int nmodels;
    bool models_failed;

    myra_permissions permissions; /* applied to agent when a turn starts */
    char draft[8192];
    char cwd[4096];
    char browse[4096];      /* the folder the file browser shows */
    char chosen[4096];      /* the folder picker's result */
    int model_index;        /* in models, for the picker */
    action act;

    unsigned seen;          /* tr.version the chat last scrolled for */
    int notes_seen;         /* tr.nnotes already copied into log */
    double started_at;
    cui_log_buffer log;
    cui_log_line log_lines[4096];
    char log_text[512 * 1024];

    bool palette, choose_model, choose_folder;
    bool show_files, show_log;
    bool dark;
    char layout[1024];      /* the dock layout last saved */
    double layout_changed;  /* when it last changed, 0 when saved */
} app;

/* worker.c */
void worker_install(app *s);  /* myra_io hooks and the note hook */
bool worker_start(app *s, job_kind job);
void worker_shutdown(app *s); /* interrupt, reject any question, join */

/* view.c */
void view_frame(cui_ctx *ui, void *user);
/* Run the action the last frame asked for; the transcript lock is not held. */
void view_act(app *s);

#endif
