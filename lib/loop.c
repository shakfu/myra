/* loop.c - myralib: one turn: send the conversation, run the tools the model asks for, repeat. */
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

/* ---- the loop ---- */

int myra_step(myra_agent *a, cJSON *resp) {
    cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItem(resp, "choices"), 0);
    cJSON *msg = cJSON_DetachItemFromObject(choice, "message");
    if (!cJSON_IsObject(msg)) {
        char *s = cJSON_PrintUnformatted(resp);
        myra_note(MYRA_ERROR, "error: no message in response: %s\n", s);
        free(s);
        cJSON_Delete(msg);
        return -1;
    }
    const char *finish = myra__get_str(choice, "finish_reason");
    finish = finish ? finish : "";
    if (!strcmp(finish, "content_filter")) {
        myra_note(MYRA_ERROR, "refused: content_filter\n");
        cJSON_Delete(msg);
        return -1;
    }
    /* Calls cut off by length may be truncated JSON, and dropping them keeps the
       history valid: every tool call must be followed by its result. */
    if (!strcmp(finish, "length")) {
        myra_note(MYRA_WARN, "warning: reply hit the length limit\n");
        a->truncated = 1;
        cJSON_DeleteItemFromObject(msg, "tool_calls");
    }
    const char *text = myra__get_str(msg, "content");
    if (!a->streamed && a->io.text && text && *text) { /* a streamed reply was shown already */
        a->io.text(a->io.ud, text);
        if (a->io.text_end) a->io.text_end(a->io.ud);
    }
    cJSON *calls = cJSON_GetObjectItem(msg, "tool_calls");
    int ncalls = cJSON_GetArraySize(calls);
    if (!ncalls && !myra__get_str(msg, "content")) { /* null content is only valid with tool_calls */
        cJSON_DeleteItemFromObject(msg, "tool_calls");
        cJSON_DeleteItemFromObject(msg, "content");
        cJSON_AddStringToObject(msg, "content", "");
    }
    cJSON_AddItemToArray(a->messages, msg);
    int budget = MYRA_MAX_TOOL_CALLS - a->turn_calls, skipped = 0; /* the cap holds within a batch */
    a->turn_calls += ncalls;
    for (int i = 0; i < ncalls; i++) {
        cJSON *call = cJSON_GetArrayItem(calls, i), *fn = cJSON_GetObjectItem(call, "function");
        const char *id = myra__get_str(call, "id");
        if (!id || !*id) { /* some servers omit it; the result must still name its call */
            char gen[32];
            snprintf(gen, sizeof gen, "myra_call_%d", i);
            if (cJSON_GetObjectItem(call, "id"))
                cJSON_ReplaceItemInObject(call, "id", cJSON_CreateString(gen));
            else
                cJSON_AddStringToObject(call, "id", gen);
        }
        const char *name = myra__get_str(fn, "name"), *args = myra__get_str(fn, "arguments");
        /* Some servers send "" for a tool without parameters. */
        cJSON *input = cJSON_Parse(args && *args ? args : "{}");
        int err = 1;
        a->announced = 0;
        /* Every call still gets a result, so the history stays valid. */
        char *out;
        if (myra_interrupted) out = xstrdup("skipped: interrupted by user");
        else if (i >= budget) out = (skipped++, xstrdup("skipped: the turn reached its tool call limit"));
        else if (cJSON_IsObject(input)) out = myra_run_tool(a, name ? name : "", input, &err);
        else out = myra__fmt("invalid JSON arguments for %s", name ? name : "?");
        cJSON_Delete(input);
        if (a->announced && a->io.done) a->io.done(a->io.ud, err, out);
        /* Chat completions has no is_error field, so mark failures in the text. */
        char *content = err ? myra__fmt("error: %s", out) : myra__fmt("%s", out);
        if (a->verbose) myra_note(MYRA_DIM, "%s\n", content);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "role", "tool");
        cJSON_AddStringToObject(r, "tool_call_id", myra__get_str(call, "id"));
        cJSON_AddStringToObject(r, "content", content);
        cJSON_AddItemToArray(a->messages, r);
        free(content);
        free(out);
    }
    char *note = ncalls && a->family.note ? a->family.note(a->family.ud) : NULL;
    if (note) { /* tool output, so a child's news never carries the user's authority */
        cJSON *last = cJSON_GetArrayItem(a->messages, cJSON_GetArraySize(a->messages) - 1);
        const char *content = myra__get_str(last, "content");
        buf b = {0};
        buf_add(&b, content, strlen(content));
        buf_lit(&b, "\n\n");
        buf_add(&b, note, strlen(note));
        cJSON_ReplaceItemInObject(last, "content", cJSON_CreateString(b.p));
        free(b.p);
        free(note);
    }
    if (myra_interrupted) {
        myra_note(MYRA_WARN, "interrupted\n");
        return 0; /* end the turn without asking the model again */
    }
    if (skipped) {
        myra_note(MYRA_WARN, "stopped: %d tool calls in one turn; skipped %d\n", MYRA_MAX_TOOL_CALLS,
                  skipped);
        a->truncated = 1;
        return 0;
    }
    return ncalls > 0;
}

static int is_tool(cJSON *m) {
    const char *role = myra__get_str(m, "role");
    return role && !strcmp(role, "tool");
}

/* Blank every tool result except the trailing batch, which the next reply needs.
   Returns how many were blanked. */
static int drop_old_tool_output(myra_agent *a) {
    static const char *const dropped = "[output dropped to fit the context]";
    int n = cJSON_GetArraySize(a->messages), last = n, count = 0;
    while (last > 0 && is_tool(cJSON_GetArrayItem(a->messages, last - 1))) last--;
    for (int i = 0; i < last; i++) {
        cJSON *m = cJSON_GetArrayItem(a->messages, i);
        const char *content = myra__get_str(m, "content");
        if (!is_tool(m) || !content || !strcmp(content, dropped)) continue;
        cJSON_ReplaceItemInObject(m, "content", cJSON_CreateString(dropped));
        count++;
    }
    if (count) /* the edit invalidates later signed reasoning, which Anthropic then rejects */
        for (int i = 0; i < n; i++)
            cJSON_DeleteItemFromObject(cJSON_GetArrayItem(a->messages, i), "reasoning_details");
    return count;
}

/* Add one response's usage; OpenRouter's cost is in credits, which are dollars. */
static void usage_add(myra_usage *u, cJSON *usage) {
    if (!cJSON_IsObject(usage)) return;
    u->requests++;
    u->in += myra__get_num(usage, "prompt_tokens");
    u->out += myra__get_num(usage, "completion_tokens");
    u->cached += myra__get_num(cJSON_GetObjectItem(usage, "prompt_tokens_details"), "cached_tokens");
    cJSON *cost = cJSON_GetObjectItem(usage, "cost");
    if (cJSON_IsNumber(cost)) {
        u->cost += cost->valuedouble;
        u->costed = 1;
    }
}

static void usage_print(const char *label, const myra_usage *u) {
    if (!u->requests) return;
    char cost[32] = "";
    if (u->costed) snprintf(cost, sizeof cost, ", $%.4f", u->cost);
    myra_note(MYRA_DIM, "%s %d request%s: %.0f in (%.0f cached), %.0f out%s\n", label,
               u->requests, u->requests == 1 ? "" : "s", u->in, u->cached, u->out, cost);
}

void myra_report_session(myra_agent *a) { usage_print("[session]", &a->session); }

int myra_ask(myra_agent *a, const char *text) {
    myra_interrupted = 0;
    a->turn_calls = a->truncated = 0;
    int before = cJSON_GetArraySize(a->messages);
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", text);
    cJSON_AddItemToArray(a->messages, m);
    int rc;
    myra_usage turn = {0};
    cJSON *undropped = NULL; /* history before the first drop, restored if the turn fails */
    for (;;) { /* call the model and run tools until it stops asking for them */
        cJSON *resp = myra__call_api(a);
        usage_add(&turn, cJSON_GetObjectItem(resp, "usage"));
        usage_add(&a->session, cJSON_GetObjectItem(resp, "usage"));
        cJSON *usage = cJSON_GetObjectItem(resp, "usage");
        if (cJSON_IsObject(usage))
            a->context_used = myra__get_num(usage, "prompt_tokens") + myra__get_num(usage, "completion_tokens");
        if (!resp && a->context_full) {
            if (!undropped) undropped = cJSON_Duplicate(a->messages, 1);
            int n = drop_old_tool_output(a);
            if (n) {
                myra_note(MYRA_WARN, "context full: dropped %d old tool output%s, retrying\n",
                           n, n == 1 ? "" : "s");
                continue;
            }
            myra_note(MYRA_WARN, "hint: the conversation no longer fits the model's context; "
                                   "/clear starts a new one\n");
        }
        if (!resp) { rc = -1; break; }
        rc = myra_step(a, resp);
        cJSON_Delete(resp);
        if (rc != 1) break;
        if (a->turn_calls >= MYRA_MAX_TOOL_CALLS) { /* a model looping on tools */
            myra_note(MYRA_WARN, "stopped: %d tool calls in one turn\n", a->turn_calls);
            a->truncated = 1;
            rc = 0;
            break;
        }
    }
    usage_print("[usage]", &turn);
    if (rc < 0 && undropped) { /* a misread error must not blank the conversation for good */
        cJSON_Delete(a->messages);
        a->messages = undropped;
        undropped = NULL;
    }
    cJSON_Delete(undropped);
    if (rc < 0)
        while (cJSON_GetArraySize(a->messages) > before)
            cJSON_DeleteItemFromArray(a->messages, cJSON_GetArraySize(a->messages) - 1);
    return rc;
}

