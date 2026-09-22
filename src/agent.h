/* agent.h - agentlib: providers, the four tools, and the chat-completions tool loop. */
#ifndef AGENT_H
#define AGENT_H

#include <cJSON.h>
#include <signal.h>
#include <stddef.h>

#define AGENT_SHELL_TIMEOUT 120 /* seconds, when the model gives none */
#define AGENT_SHELL_TIMEOUT_MAX 600

/* Set it from a SIGINT handler installed without SA_RESTART. It stops the running
   shell command (killing its process group) or model request, and ends the turn.
   agent_ask clears it on entry. */
extern volatile sig_atomic_t agent_interrupted;

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
    int auto_yes;        /* run write/edit/shell without asking */
    size_t max_output;   /* cap on one tool result, in bytes */
    int context_full;    /* the last request failed as too long for the model's context */
    int streamed;        /* the last reply's text was printed while it streamed */
    cJSON *tools, *messages;
} agent;

const agent_provider *agent_provider_named(const char *name);
/* The remembered provider if it can run (a cloud one needs its key set), else the
   first cloud provider whose key is set, else NULL. */
const agent_provider *agent_provider_default(void);

/* model NULL means the remembered one, else the provider default. -1 if the key is unset. */
int agent_init(agent *a, const agent_provider *p, const char *model, int auto_yes);
void agent_free(agent *a);

/* Run one user turn. -1 on failure, with history rolled back to before the turn. */
int agent_ask(agent *a, const char *text);
/* Drop the conversation, keeping the system prompt. */
void agent_clear(agent *a);
/* Switch model; it is remembered after the next successful response. */
void agent_set_model(agent *a, const char *model);
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
