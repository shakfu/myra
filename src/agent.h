/* agent.h - agentlib: providers, the four tools, and the chat-completions tool loop. */
#ifndef AGENT_H
#define AGENT_H

#include <cJSON.h>
#include <signal.h>
#include <stddef.h>

#define AGENT_SHELL_TIMEOUT 120 /* seconds, when the model gives none */
#define AGENT_SHELL_TIMEOUT_MAX 600
#define AGENT_MAX_RAW (64 * 1024 * 1024) /* bytes read before a tool gives up */
#define AGENT_MAX_TOOL_CALLS 100 /* per turn, so a looping model stops */

/* Set it from a SIGINT handler installed without SA_RESTART. It stops the running
   shell command (killing its process group) or model request, and ends the turn.
   agent_ask clears it on entry. */
extern volatile sig_atomic_t agent_interrupted;

/* Color stderr messages with ANSI codes. The CLI sets it for a terminal. */
extern int agent_color;
typedef enum { AGENT_PLAIN, AGENT_TOOL, AGENT_ERROR, AGENT_WARN, AGENT_DIM, AGENT_BOLD } agent_style;
/* fprintf to stderr, colored by style when agent_color is set. */
void agent_note(agent_style style, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Token counts and cost, as the server reports them. */
typedef struct {
    int requests;
    double in, cached, out, cost;
    int costed; /* the server reported a cost: OpenRouter does, llama-server does not */
} agent_usage;

/* When write, edit and shell may run without asking on the terminal. */
typedef enum {
    AGENT_AUTO,      /* always, except write/edit outside the working directory */
    AGENT_ASK,       /* never: ask each time */
    AGENT_ALL,       /* always */
    AGENT_READ_ONLY, /* refuse them; read still runs */
} agent_permissions;

typedef struct {
    const char *name, *key_env, *base_env, *base, *model;
    int key_optional;
    int cache_control;  /* send OpenRouter's top-level cache_control */
    size_t max_output;  /* cap on one tool result, in bytes; AGENT_MAX_OUTPUT overrides */
} agent_provider;

extern const agent_provider AGENT_PROVIDERS[];
extern const size_t AGENT_NPROVIDERS;

typedef struct {
    const agent_provider *prov;
    char *model;         /* owned */
    const char *api_key; /* NULL or empty: no authorization header */
    int save_model;      /* remember model after the next successful response */
    int save_provider;   /* remember provider likewise; set for an explicit -P */
    agent_permissions permissions;
    size_t max_output;   /* cap on one tool result, in bytes */
    int context_full;    /* the last request failed as too long for the model's context */
    int streamed;        /* the last reply's text was printed while it streamed */
    int verbose;         /* full tool arguments and results on stderr, not one line per call */
    int turn_calls;      /* tool calls run in this turn */
    agent_usage session; /* totals over every turn */
    cJSON *tools, *messages;
} agent;

const agent_provider *agent_provider_named(const char *name);
/* The remembered provider if it can run (a cloud one needs its key set), else the
   first cloud provider whose key is set, else NULL. */
const agent_provider *agent_provider_default(void);

/* model NULL means the remembered one, else the provider default. -1 if the key is unset. */
int agent_init(agent *a, const agent_provider *p, const char *model, agent_permissions perms);
void agent_free(agent *a);

/* Run one user turn. -1 on failure, with history rolled back to before the turn. */
int agent_ask(agent *a, const char *text);
/* Drop the conversation, keeping the system prompt. */
void agent_clear(agent *a);
/* Switch model; it is remembered after the next successful response. */
void agent_set_model(agent *a, const char *model);
/* Print the session's usage totals, if the server reported any. */
void agent_report_session(agent *a);
/* Print the provider's model ids that contain filter, ignoring case; the current one is starred. */
void agent_list_models(agent *a, const char *filter);

/* Lower-level pieces, exposed for tests. */

/* Consume one chat-completions response: -1 failed, 0 done, 1 tools ran. */
int agent_step(agent *a, cJSON *resp);
/* Returns a malloc'd result; *err is set on failure. */
char *agent_run_tool(agent *a, const char *name, cJSON *input, int *err);
/* $XDG_STATE_HOME/ant/<name>, default ~/.local/state/ant/<name>; NULL without either
   variable. make_dir creates the parent directory. */
char *agent_state_path(const char *name, int make_dir);
/* Read and write that file's first line. */
char *agent_load_state(const char *name);
void agent_store_state(const char *name, const char *value);
/* If line is "name" or "name args", return args with leading spaces skipped; else NULL. */
const char *agent_command(const char *line, const char *name);

#endif
