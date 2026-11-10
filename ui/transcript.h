/* transcript.h - what the agent thread writes and the UI thread reads: the
   conversation, myra's notes, and the permission handshake. Writers lock
   internally; the UI holds tr_lock while it reads, for the whole frame. */
#ifndef TRANSCRIPT_H
#define TRANSCRIPT_H

#include <pthread.h>
#include <stddef.h>

typedef enum { ITEM_USER, ITEM_REPLY, ITEM_REASONING, ITEM_TOOL, ITEM_ERROR } item_kind;
typedef enum { TOOL_PENDING, TOOL_RUNNING, TOOL_DONE, TOOL_FAILED } tool_status;

typedef struct {
    item_kind kind;
    char *text;           /* message, reply or reasoning so far, or tool result; never NULL */
    size_t len, cap;
    char *name, *detail;  /* ITEM_TOOL: tool name and its path or command */
    char *input;          /* ITEM_TOOL: arguments as JSON, or NULL */
    char *before, *after; /* ITEM_TOOL: the file before and after a write or edit, or NULL */
    tool_status status;
    double t0, t1;        /* ITEM_TOOL, ITEM_REASONING: start and end; t1 is 0 while open */
} item;

typedef struct {
    int style; /* a myra_style */
    double time;
    char *text;
} note;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    item *items;
    int n, cap;
    note *notes;
    int nnotes, notes_cap;
    unsigned version; /* bumped on every change to items, so the UI can follow output */
    int asking;       /* the agent waits in tr_ask for tr_answer */
    int outside;      /* what it asks about is outside the working directory */
    int answer;       /* -1 until answered, then 0 or 1 */
    int closed;       /* tr_close was called: tr_ask rejects at once */
} transcript;

void tr_init(transcript *t);
void tr_free(transcript *t);
void tr_lock(transcript *t);
void tr_unlock(transcript *t);
/* Drop the conversation; notes stay. Only while no turn runs. */
void tr_clear(transcript *t);

void tr_user(transcript *t, const char *text);
void tr_error(transcript *t, const char *text);
/* Reply text; appends to the last item if it is a reply, else starts one. */
void tr_text(transcript *t, const char *s);
/* Reasoning text; appends to an open reasoning item, else starts one. Any other
   item closes it. */
void tr_reasoning(transcript *t, const char *s);
/* A tool call starts. input, before and after may be NULL; all are copied. */
void tr_tool(transcript *t, const char *name, const char *detail, const char *input,
             const char *before, const char *after, int pending);
/* Close the last open tool call with its result. */
void tr_done(transcript *t, int err, const char *result);
/* The turn ended: close an open reasoning item. */
void tr_turn_end(transcript *t);
void tr_note(transcript *t, int style, const char *text);

/* Agent side: block until the UI calls tr_answer. 1 allows. */
int tr_ask(transcript *t, int outside);
/* UI side: answer a pending tr_ask; no-op if none is pending. Takes the lock. */
void tr_answer(transcript *t, int ok);
/* UI side, on shutdown: reject a pending tr_ask and every later one. */
void tr_close(transcript *t);

double tr_now(void);

#endif
