#include "transcript.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) { perror("realloc"); exit(1); }
    return q;
}

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    return memcpy(xrealloc(NULL, n), s, n);
}

double tr_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void tr_init(transcript *t) {
    memset(t, 0, sizeof *t);
    pthread_mutex_init(&t->mu, NULL);
    pthread_cond_init(&t->cv, NULL);
    t->answer = -1;
}

static void free_items(transcript *t) {
    for (int i = 0; i < t->n; i++) {
        item *it = &t->items[i];
        free(it->text), free(it->name), free(it->detail);
        free(it->input), free(it->before), free(it->after);
    }
    t->n = 0;
}

void tr_free(transcript *t) {
    free_items(t);
    free(t->items);
    for (int i = 0; i < t->nnotes; i++) free(t->notes[i].text);
    free(t->notes);
    pthread_cond_destroy(&t->cv);
    pthread_mutex_destroy(&t->mu);
}

void tr_lock(transcript *t) { pthread_mutex_lock(&t->mu); }
void tr_unlock(transcript *t) { pthread_mutex_unlock(&t->mu); }

void tr_clear(transcript *t) {
    tr_lock(t);
    free_items(t);
    t->version++;
    tr_unlock(t);
}

static void append(item *it, const char *s) {
    size_t k = strlen(s);
    if (it->len + k + 1 > it->cap) {
        it->cap = (it->len + k + 1) * 2;
        it->text = xrealloc(it->text, it->cap);
    }
    memcpy(it->text + it->len, s, k + 1);
    it->len += k;
}

static item *last(transcript *t) { return t->n ? &t->items[t->n - 1] : NULL; }

/* Close an open reasoning item at the end; caller holds the lock. */
static void close_reasoning(transcript *t) {
    item *it = last(t);
    if (it && it->kind == ITEM_REASONING && !it->t1) it->t1 = tr_now();
}

/* A new item at the end; caller holds the lock. */
static item *push(transcript *t, item_kind kind) {
    if (kind != ITEM_REASONING) close_reasoning(t);
    if (!t->items || t->n >= t->cap) { /* items is NULL only before the first push */
        t->cap = t->items && t->cap >= 16 ? t->cap * 2 : 16;
        t->items = xrealloc(t->items, (size_t)t->cap * sizeof *t->items);
    }
    item *it = &t->items[t->n++];
    memset(it, 0, sizeof *it);
    it->kind = kind;
    append(it, "");
    return it;
}

static void add(transcript *t, item_kind kind, const char *text) {
    tr_lock(t);
    append(push(t, kind), text);
    t->version++;
    tr_unlock(t);
}

void tr_user(transcript *t, const char *text) { add(t, ITEM_USER, text); }
void tr_error(transcript *t, const char *text) { add(t, ITEM_ERROR, text); }

void tr_text(transcript *t, const char *s) {
    tr_lock(t);
    item *it = last(t);
    append(it && it->kind == ITEM_REPLY ? it : push(t, ITEM_REPLY), s);
    t->version++;
    tr_unlock(t);
}

void tr_reasoning(transcript *t, const char *s) {
    tr_lock(t);
    item *it = last(t);
    if (!it || it->kind != ITEM_REASONING || it->t1) {
        it = push(t, ITEM_REASONING);
        it->t0 = tr_now();
    }
    append(it, s);
    t->version++;
    tr_unlock(t);
}

void tr_tool(transcript *t, const char *name, const char *detail, const char *input,
             const char *before, const char *after, int pending) {
    tr_lock(t);
    item *it = push(t, ITEM_TOOL);
    it->name = xstrdup(name);
    it->detail = xstrdup(detail);
    it->input = xstrdup(input);
    it->before = xstrdup(before);
    it->after = xstrdup(after);
    it->status = pending ? TOOL_PENDING : TOOL_RUNNING;
    it->t0 = tr_now();
    t->version++;
    tr_unlock(t);
}

void tr_done(transcript *t, int err, const char *result) {
    tr_lock(t);
    for (int i = t->n - 1; i >= 0; i--) {
        item *it = &t->items[i];
        if (it->kind != ITEM_TOOL || it->t1) continue;
        it->status = err ? TOOL_FAILED : TOOL_DONE;
        it->t1 = tr_now();
        append(it, result);
        break;
    }
    t->version++;
    tr_unlock(t);
}

void tr_turn_end(transcript *t) {
    tr_lock(t);
    close_reasoning(t);
    t->version++;
    tr_unlock(t);
}

void tr_note(transcript *t, int style, const char *text) {
    tr_lock(t);
    if (t->nnotes == t->notes_cap) {
        t->notes_cap = t->notes_cap ? t->notes_cap * 2 : 64;
        t->notes = xrealloc(t->notes, (size_t)t->notes_cap * sizeof *t->notes);
    }
    t->notes[t->nnotes++] = (note){.style = style, .time = tr_now(), .text = xstrdup(text)};
    tr_unlock(t);
}

int tr_ask(transcript *t, int outside) {
    tr_lock(t);
    t->asking = 1;
    t->outside = outside;
    t->answer = t->closed ? 0 : -1;
    t->version++;
    while (t->answer < 0) pthread_cond_wait(&t->cv, &t->mu);
    int ok = t->answer;
    t->asking = 0;
    for (int i = t->n - 1; i >= 0; i--) /* the call that asked starts, or stays pending */
        if (t->items[i].kind == ITEM_TOOL && t->items[i].status == TOOL_PENDING) {
            if (ok) t->items[i].status = TOOL_RUNNING, t->items[i].t0 = tr_now();
            break;
        }
    t->version++;
    tr_unlock(t);
    return ok;
}

void tr_close(transcript *t) {
    tr_lock(t);
    t->closed = 1;
    if (t->asking && t->answer < 0) t->answer = 0;
    pthread_cond_signal(&t->cv);
    tr_unlock(t);
}

void tr_answer(transcript *t, int ok) {
    tr_lock(t);
    if (t->asking && t->answer < 0) {
        t->answer = ok != 0;
        pthread_cond_signal(&t->cv);
    }
    tr_unlock(t);
}
