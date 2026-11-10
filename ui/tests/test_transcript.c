/* Tests for transcript.c: plain asserts, one process; ctest runs it as `ui.transcript`. */
#undef NDEBUG
#include "transcript.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void test_reply_chunks_merge(void) {
    transcript t;
    tr_init(&t);
    tr_user(&t, "hi");
    tr_text(&t, "Hel");
    tr_text(&t, "lo");
    assert(t.n == 2);
    assert(t.items[1].kind == ITEM_REPLY);
    assert(!strcmp(t.items[1].text, "Hello"));
    tr_tool(&t, "read", "a.c", NULL, NULL, NULL, 0);
    tr_text(&t, "after");
    assert(t.n == 4 && t.items[3].kind == ITEM_REPLY); /* a tool call splits replies */
    tr_free(&t);
}

static void test_done_closes_last_open_call(void) {
    transcript t;
    tr_init(&t);
    tr_tool(&t, "shell", "true", NULL, NULL, NULL, 0);
    tr_done(&t, 0, "ok");
    tr_tool(&t, "shell", "false", NULL, NULL, NULL, 0);
    tr_done(&t, 1, "exit 1");
    assert(t.items[0].status == TOOL_DONE && !strcmp(t.items[0].text, "ok"));
    assert(t.items[1].status == TOOL_FAILED && !strcmp(t.items[1].text, "exit 1"));
    assert(t.items[1].t1 >= t.items[1].t0);
    tr_free(&t);
}

static void test_reasoning_closes_when_anything_follows(void) {
    transcript t;
    tr_init(&t);
    tr_reasoning(&t, "think ");
    tr_reasoning(&t, "more");
    assert(t.n == 1 && t.items[0].kind == ITEM_REASONING && !strcmp(t.items[0].text, "think more"));
    assert(!t.items[0].t1);
    tr_text(&t, "answer");
    assert(t.items[0].t1 >= t.items[0].t0); /* closed by the reply */
    tr_reasoning(&t, "again");
    assert(t.n == 3); /* a closed one is not reopened */
    tr_turn_end(&t);
    assert(t.items[2].t1);
    tr_free(&t);
}

static void test_tool_keeps_input_and_diff(void) {
    transcript t;
    tr_init(&t);
    tr_tool(&t, "edit", "a.c", "{}", "old\n", "new\n", 0);
    assert(!strcmp(t.items[0].input, "{}"));
    assert(!strcmp(t.items[0].before, "old\n") && !strcmp(t.items[0].after, "new\n"));
    tr_free(&t);
}

static void test_clear_keeps_notes(void) {
    transcript t;
    tr_init(&t);
    tr_user(&t, "hi");
    tr_note(&t, 2, "error: x\n");
    unsigned v = t.version;
    tr_clear(&t);
    assert(t.n == 0 && t.nnotes == 1 && !strcmp(t.notes[0].text, "error: x\n"));
    assert(t.version != v); /* the view rescrolls */
    tr_free(&t);
}

static void test_answer_without_ask_is_ignored(void) {
    transcript t;
    tr_init(&t);
    tr_answer(&t, 1);
    assert(t.answer == -1 && !t.asking);
    tr_free(&t);
}

typedef struct { transcript *t; int result; } asker;

static void *ask(void *arg) {
    asker *a = arg;
    a->result = tr_ask(a->t, 1);
    return NULL;
}

static void wait_asking(transcript *t) {
    for (;;) {
        tr_lock(t);
        int asking = t->asking;
        tr_unlock(t);
        if (asking) return;
        usleep(1000);
    }
}

static void check_ask(int ok) {
    transcript t;
    tr_init(&t);
    tr_tool(&t, "write", "/tmp/x", NULL, NULL, NULL, 1);
    asker a = {&t, -1};
    pthread_t th;
    assert(pthread_create(&th, NULL, ask, &a) == 0);
    wait_asking(&t);
    assert(t.outside == 1);
    tr_answer(&t, ok);
    pthread_join(th, NULL);
    assert(a.result == ok);
    assert(!t.asking);
    assert(t.items[0].status == (ok ? TOOL_RUNNING : TOOL_PENDING));
    tr_free(&t);
}

static void test_close_rejects_pending_and_later_asks(void) {
    transcript t;
    tr_init(&t);
    tr_tool(&t, "shell", "rm -rf build", NULL, NULL, NULL, 1);
    asker a = {&t, -1};
    pthread_t th;
    assert(pthread_create(&th, NULL, ask, &a) == 0);
    wait_asking(&t);
    tr_close(&t);
    pthread_join(th, NULL);
    assert(a.result == 0);
    assert(tr_ask(&t, 0) == 0); /* an ask after close must not block */
    tr_free(&t);
}

int main(void) {
    test_reply_chunks_merge();
    test_done_closes_last_open_call();
    test_answer_without_ask_is_ignored();
    test_reasoning_closes_when_anything_follows();
    test_tool_keeps_input_and_diff();
    test_clear_keeps_notes();
    check_ask(1);
    check_ask(0);
    test_close_rejects_pending_and_later_asks();
    puts("transcript: ok");
    return 0;
}
