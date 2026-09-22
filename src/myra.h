/* myra.h - myralib: providers, the four tools, and the chat-completions tool loop. */
#ifndef MYRA_H
#define MYRA_H

#include <cJSON.h>
#include <signal.h>
#include <stddef.h>

#define MYRA_SHELL_TIMEOUT 120 /* seconds, when the model gives none */
#define MYRA_SHELL_TIMEOUT_MAX 600
#define MYRA_MAX_RAW (64 * 1024 * 1024) /* bytes read before a tool gives up */
#define MYRA_MAX_TOOL_CALLS 100 /* per turn, so a looping model stops */

/* Nonzero if s starts with the string literal lit; needs <string.h>. */
#define MYRA_STARTS_WITH(s, lit) (!strncmp((s), "" lit, sizeof(lit) - 1))

/* Set it from a SIGINT handler installed without SA_RESTART. It stops the running
   shell command (killing its process group) or model request, and ends the turn.
   myra_ask clears it on entry. */
extern volatile sig_atomic_t myra_interrupted;

/* Color stderr messages with ANSI codes. The CLI sets it for a terminal. */
extern int myra_color;
typedef enum { MYRA_PLAIN, MYRA_TOOL, MYRA_ERROR, MYRA_WARN, MYRA_DIM, MYRA_BOLD } myra_style;
/* fprintf to stderr, colored by style when myra_color is set. */
void myra_note(myra_style style, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Token counts and cost, as the server reports them. */
typedef struct {
    int requests;
    double in, cached, out, cost;
    int costed; /* the server reported a cost: OpenRouter does, llama-server does not */
} myra_usage;

/* When write, edit and shell may run without asking on the terminal. */
typedef enum {
    MYRA_AUTO,      /* always, except write/edit outside the working directory */
    MYRA_ASK,       /* never: ask each time */
    MYRA_ALL,       /* always */
    MYRA_READ_ONLY, /* refuse them; read still runs */
} myra_permissions;

typedef struct {
    const char *name, *key_env, *base_env, *base, *model;
    int key_optional;
    int cache_control;  /* send OpenRouter's top-level cache_control */
    size_t max_output;  /* cap on one tool result, in bytes; MYRA_MAX_OUTPUT overrides */
} myra_provider;

extern const myra_provider MYRA_PROVIDERS[];
extern const size_t MYRA_NPROVIDERS;

typedef struct {
    const myra_provider *prov;
    char *model;         /* owned */
    const char *api_key; /* NULL or empty: no authorization header */
    int save_model;      /* remember model after the next successful response */
    int save_provider;   /* remember provider likewise; set for an explicit -P */
    myra_permissions permissions;
    size_t max_output;   /* cap on one tool result, in bytes */
    int context_full;    /* the last request failed as too long for the model's context */
    int streamed;        /* the last reply's text was printed while it streamed */
    int verbose;         /* full tool arguments and results on stderr, not one line per call */
    int turn_calls;      /* tool calls run in this turn */
    myra_usage session; /* totals over every turn */
    cJSON *tools, *messages;
} myra_agent;

const myra_provider *myra_provider_named(const char *name);
/* The remembered provider if it can run (a cloud one needs its key set), else the
   first cloud provider whose key is set, else NULL. */
const myra_provider *myra_provider_default(void);

/* model NULL means the remembered one, else the provider default. -1 if the key is unset. */
int myra_init(myra_agent *a, const myra_provider *p, const char *model, myra_permissions perms);
void myra_free(myra_agent *a);

/* Run one user turn. -1 on failure, with history rolled back to before the turn. */
int myra_ask(myra_agent *a, const char *text);
/* Drop the conversation, keeping the system prompt. */
void myra_clear(myra_agent *a);
/* Switch model; it is remembered after the next successful response. */
void myra_set_model(myra_agent *a, const char *model);
/* Print the session's usage totals, if the server reported any. */
void myra_report_session(myra_agent *a);
/* Print the provider's model ids that contain filter, ignoring case; the current one is starred. */
void myra_list_models(myra_agent *a, const char *filter);

/* Lower-level pieces, exposed for tests. */

/* Consume one chat-completions response: -1 failed, 0 done, 1 tools ran. */
int myra_step(myra_agent *a, cJSON *resp);
/* Returns a malloc'd result; *err is set on failure. */
char *myra_run_tool(myra_agent *a, const char *name, cJSON *input, int *err);
/* $XDG_STATE_HOME/myra/<name>, default ~/.local/state/myra/<name>; NULL without either
   variable. make_dir creates the parent directory. */
char *myra_state_path(const char *name, int make_dir);
/* Read and write that file's first line. */
char *myra_load_state(const char *name);
void myra_store_state(const char *name, const char *value);
/* If line is "name" or "name args", return args with leading spaces skipped; else NULL. */
const char *myra_command(const char *line, const char *name);

#endif
