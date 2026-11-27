/* myra.h - myralib, installed as libmyra: providers, pluggable tools with the coding
   agent's four, and the chat-completions tool loop. */
#ifndef MYRA_H
#define MYRA_H

#include "cJSON.h" /* quoted: installed beside this header */
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>

#define MYRA_SHELL_TIMEOUT 120 /* seconds, when the model gives none */
#define MYRA_SHELL_TIMEOUT_MAX 600
#define MYRA_MAX_RAW (64 * 1024 * 1024) /* bytes read before a tool gives up */
#define MYRA_MAX_RESPONSE (64 * 1024 * 1024) /* bytes received before a request gives up */
#define MYRA_MAX_TOOL_CALLS 100 /* per turn, so a looping model stops */

/* Nonzero if s starts with the string literal lit; needs <string.h>. */
#define MYRA_STARTS_WITH(s, lit) (!strncmp((s), "" lit, sizeof(lit) - 1))

/* Set it from a SIGINT handler installed without SA_RESTART. It stops the running
   shell command (killing its process group) or model request, and ends the turn.
   myra_ask clears it on entry. Atomic, so another thread may also set it; lock-free
   atomics are safe in a signal handler too. */
extern atomic_int myra_interrupted;

/* The signal number, set with myra_interrupted by a SIGTERM or SIGHUP handler: the
   front end ends once the turn has stopped. */
extern atomic_int myra_terminated;

/* Kill the process group of every shell call that still has members: SIGTERM, then
   SIGKILL after 1 s. myra_free calls it. */
void myra_kill_jobs(void);

/* Called often while a model request or shell command runs, so a front end can
   read its input and set myra_interrupted. NULL: none. */
extern void (*myra_poll)(void);

typedef enum { MYRA_PLAIN, MYRA_TOOL, MYRA_ERROR, MYRA_WARN, MYRA_DIM, MYRA_BOLD } myra_style;
/* Format a note (progress, warnings, errors) and pass it to myra_note_hook. Without a
   hook it is dropped; an error is still kept in myra_last_error. libmyra-term's hook
   prints notes on stderr (myra/term.h). */
void myra_note(myra_style style, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Receives each formatted, unescaped note. It may be called from the agent's thread. */
extern void (*myra_note_hook)(myra_style style, const char *text);
/* The text of the last MYRA_ERROR note, trailing newline dropped; "" if none. */
extern char myra_last_error[1024];
/* s with each invalid UTF-8 byte replaced by U+FFFD; malloc'd. */
char *myra_utf8(const char *s);

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

/* How a front end sees and steers a turn, and file access it may serve itself (ACP
   mode). A NULL member keeps the default: output is dropped, files are local. */
typedef struct {
    void *ud;
    void (*text)(void *ud, const char *s); /* reply text, as it streams; NULL: dropped */
    void (*text_end)(void *ud);            /* the reply text that text received is complete */
    /* A tool call starts; pending: it waits for allow before it runs. input is the
       call's parsed arguments, valid only during the call. */
    void (*tool)(void *ud, const char *name, const char *detail, const cJSON *input, int pending);
    /* The call tool announced has finished: its result, err set if it failed. */
    void (*done)(void *ud, int err, const char *result);
    /* The file at absolute path, malloc'd; NULL on failure, with *why malloc'd. */
    char *(*read)(void *ud, const char *path, char **why);
    /* Write content to absolute path: NULL on success, else why, malloc'd. */
    char *(*write)(void *ud, const char *path, const char *content);
    /* Whether a mutating tool may run, asked where --permissions would ask. 1 allows,
       0 denies, -1: no one can answer. NULL: denied, as for -1. */
    int (*allow)(void *ud, const char *name, const char *detail, int outside);
    /* Reasoning text, as it streams: the reasoning or reasoning_content delta. */
    void (*reasoning)(void *ud, const char *s);
} myra_io;

typedef struct myra_mcp myra_mcp; /* mcp.h */
typedef struct myra_agent myra_agent;

/* How a tool's calls are confined, which decides when --permissions asks. */
typedef enum {
    MYRA_TOOL_READS,  /* changes nothing: never asks, runs under read-only */
    MYRA_TOOL_WRITES, /* changes the file its string argument "path" names: asks per
                         permissions, and auto asks outside the working directory */
    MYRA_TOOL_RUNS,   /* may change anything, as shell: asks per permissions, as if inside */
} myra_tool_access;

/* A tool the model can call. The strings are not copied: they must outlive the agent. */
typedef struct myra_tool {
    const char *name, *description;
    const char *parameters; /* JSON Schema of its arguments: an object */
    const char *detail;     /* the string argument shown for a call; NULL: all, as JSON */
    myra_tool_access access;
    /* Run a call whose required arguments are present with their schema's type, once
       permission is granted. A malloc'd result; *err is 1 on entry, 0 for success. */
    char *(*run)(myra_agent *a, const struct myra_tool *t, cJSON *input, int *err);
    void *ud;
} myra_tool;

typedef struct myra_tool_entry myra_tool_entry; /* a registered tool */

/* Set by a front end that makes the agent a member of a family of agents (the CLI's
   -r). A NULL member: none. */
typedef struct {
    void *ud;
    const char *prompt;      /* appended to the system prompt; not owned */
    /* After a step's tool calls: text appended to the last result, malloc'd, or NULL. */
    char *(*note)(void *ud);
    /* Before each model request: its output cap in tokens, sent as max_tokens, or -1 to
       refuse it (with an error note). input estimates the request's input tokens. */
    long (*reserve)(void *ud, myra_agent *a, long input);
    /* After each reserved request: its usage object, NULL if it did not complete. */
    void (*settle)(void *ud, const cJSON *usage);
} myra_family_hooks;

struct myra_agent {
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
    int truncated;       /* this turn ended early: a reply hit the length limit or the call cap */
    int announced;       /* io.tool was told of the current call, so io.done follows */
    myra_usage session; /* totals over every turn */
    double context_used; /* prompt plus completion tokens of the last response */
    long context_window; /* the model's, in tokens; 0 unknown */
    myra_io io;
    myra_family_hooks family;
    myra_mcp *mcp;       /* running MCP servers, newest first */
    /* The system prompt; "{cwd}" becomes the working directory. Not owned. NULL: none,
       besides the family's. myra_add_coding_tools sets the coding agent's. */
    const char *system;
    myra_tool_entry *registry; /* tools offered, in order */
    size_t ntools;
    char session_id[64]; /* the saved session this conversation is, "" if none */
    cJSON *tools, *messages; /* tools: registered tools and MCP tools, as sent */
};

const myra_provider *myra_provider_named(const char *name);
/* The remembered provider if it can run (a cloud one needs its key set), else the
   first cloud provider whose key is set, else NULL. */
const myra_provider *myra_provider_default(void);

/* model NULL means the remembered one, else the provider default. -1 if the key is unset.
   The agent has no tools: add them with myra_add_tool or myra_add_coding_tools. */
int myra_init(myra_agent *a, const myra_provider *p, const char *model, myra_permissions perms);

/* Offer t to the model. -1 with an error note if it has no name or run, its name is
   taken, or its parameters are not a JSON object. */
int myra_add_tool(myra_agent *a, const myra_tool *t);
/* The coding agent: read, write, edit and shell, and its system prompt. */
void myra_add_coding_tools(myra_agent *a);
void myra_free(myra_agent *a);

/* Run one user turn. -1 on failure, with history rolled back to before the turn. */
int myra_ask(myra_agent *a, const char *text);
/* The text of the last assistant message that has any, or NULL. Points into the history. */
const char *myra_last_reply(myra_agent *a);
/* Drop the conversation, keeping the system prompt. */
void myra_clear(myra_agent *a);
/* Switch model; it is remembered after the next successful response. */
void myra_set_model(myra_agent *a, const char *model);
/* Print the session's usage totals, if the server reported any. */
void myra_report_session(myra_agent *a);
/* The model's context window from the provider's /models entry; 0 if it gives none. Fetches. */
long myra_context_window(myra_agent *a);
/* The model's entry in the provider's /models list, a copy; NULL if none. Fetches. */
cJSON *myra_model_entry(myra_agent *a);

/* The provider's model ids, NULL-terminated; NULL if the request failed. Fetches. */
char **myra_model_ids(myra_agent *a);
void myra_free_model_ids(char **ids);

/* Rebuild the system prompt for the current working directory. */
void myra_set_cwd(myra_agent *a);

/* Saved sessions: $XDG_STATE_HOME/myra/sessions/<id>.json, mode 0600, without the
   system prompt. The session saved as id, parsed; NULL with an error note if there is
   none, it cannot be read, or it was saved in another working directory. */
cJSON *myra_session_load(const char *id);
/* Replace a's conversation with that of s, from myra_session_load, and take its id. */
void myra_session_resume(myra_agent *a, const cJSON *s, const char *id);
/* Save a's conversation as a->session_id, first picking a new id if it is "". -1 with a
   warning if it cannot, and a->session_id is then "". */
int myra_session_save(myra_agent *a);

/* Lower-level pieces, exposed for tests. */

/* Consume one chat-completions response: -1 failed, 0 done, 1 tools ran. */
int myra_step(myra_agent *a, cJSON *resp);
/* Run a registered or MCP tool. Returns a malloc'd result; *err is set on failure. */
char *myra_run_tool(myra_agent *a, const char *name, cJSON *input, int *err);
/* $XDG_STATE_HOME/myra/<name>, default ~/.local/state/myra/<name>; NULL without either
   variable. make_dir creates the parent directory. */
char *myra_state_path(const char *name, int make_dir);
/* Read and write that file's first line. */
char *myra_load_state(const char *name);
void myra_store_state(const char *name, const char *value);

#endif
