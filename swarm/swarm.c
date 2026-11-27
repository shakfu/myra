/* swarm.c - myra-swarm: parallel agents, as child myra processes or as threads in this
   one, on the public API only.

   myra-swarm [options] PLAN.md
     PLAN.md has one "# " heading, the objective, and one or more "## " headings, its
     parts. Text under a heading gives it context; "### " and deeper are text too. One
     agent works on each part. When all have finished, one more agent combines their
     answers to address the objective. Prints a markdown report, or JSON with --json.
     A part's "depends: TITLE, ..." line starts it only after those parts finish, and
     gives it their answers; a failed dependency skips it.

   myra-swarm [options] --tasks < TASKS
     TASKS: JSON lines, {"prompt": TEXT, "cwd": DIR, "stop_after": SECONDS}, one agent
     each. Prints one JSON object per task. For testing agents that share a process
     (swarm/tests/, docs/dev/agent-context.md).

   --mode processes (the default) runs each agent as "myra -p --json", so agents share
   nothing. With --budget-tokens, each is a member of one family that a "myra -r host"
   founds, and every request reserves against its ledger (docs/dev/recursion.md). --mode threads runs each on a thread of this process, on myralib, where they
   share its process-wide state (docs/dev/agent-context.md).

   Exit status: 0 if every agent succeeded, 1 if any failed, 2 on bad input, 130 when
   stopped by Ctrl-C. */
#include "myra/myra.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp */
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* ---- text ---- */

typedef struct {
    char *p;
    size_t n;
} text;

static void append_n(text *t, const char *s, size_t k) {
    char *p = realloc(t->p, t->n + k + 1);
    if (!p) { perror("realloc"); exit(1); }
    memcpy(p + t->n, s, k);
    p[t->n + k] = 0;
    t->p = p, t->n += k;
}

static void append(text *t, const char *s) { append_n(t, s, strlen(s)); }

static void appendf(text *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void appendf(text *t, const char *fmt, ...) {
    va_list ap, again;
    va_start(ap, fmt);
    va_copy(again, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *s = malloc(n < 0 ? 1 : (size_t)n + 1);
    if (!s) { perror("malloc"); exit(1); }
    vsnprintf(s, n < 0 ? 1 : (size_t)n + 1, fmt, again);
    va_end(again);
    append(t, s);
    free(s);
}

static const char *str(const text *t) { return t->p ? t->p : ""; }

/* ---- agents ---- */

typedef struct {
    char *title;       /* plan mode: the part's heading */
    char member[33];   /* processes mode with a budget: its name in the family */
    char member_id[64]; /* and its id, from the host; "" before add_members */
    char *prompt, *cwd;
    double stop_after; /* --tasks: seconds after the start; 0 never */
    myra_agent agent;
    int inited;        /* agent needs myra_free */
    text reply, notes;
    char *answer;      /* the last assistant text, owned */
    char error[1024];      /* this agent's last error note, seen by the note hook */
    char last_error[1024]; /* myra_last_error after the turn: should equal error */
    int rc, stopped;
    double started, finished;
    myra_usage usage;
    const size_t *deps; /* plan mode: tasks that must finish first; not owned */
    size_t ndeps;
    int state;          /* PENDING, RUNNING or FINISHED; under sched */
    atomic_int stop;    /* processes mode: interrupt this task's child */
    size_t shown;       /* processes mode, -v: notes bytes already echoed */
    atomic_int done;
} task;

enum { PENDING, RUNNING, FINISHED };

static const myra_provider *prov;   /* threads mode */
static const char *pname, *model;   /* -P and -m as given; processes mode forwards them */
static myra_permissions perms = MYRA_AUTO;
static const char *perms_name = "auto";
static const char *myra_bin = "myra"; /* processes mode: the agent, found in PATH */
static int verbose, plan_mode, threads;
static const char *budget_tokens, *budget_cost; /* as given; processes mode only */

static task *tasks;
static size_t ntasks, finished_tasks; /* finished_tasks: under sched */
static atomic_int stopping;           /* Ctrl-C: start nothing more */
static int words = 400;               /* plan mode: the answer length parts are asked for */

/* A task starts once every task in its deps has finished. */
static pthread_mutex_t sched = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sched_changed = PTHREAD_COND_INITIALIZER;
/* Plan mode: task i's prompt, built as it starts, since it quotes the answers of its deps. */
static char *(*prompt_for)(size_t i);

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* The note hook has no user data. Each agent notes only on its own thread, so a
   thread-local names the task. */
static _Thread_local task *current;

static void on_note(myra_style style, const char *s) {
    if (!current) { fputs(s, stderr); return; }
    append(&current->notes, s);
    if (verbose) fprintf(stderr, "[%s] %s", current->title ? current->title : "task", s);
    if (style == MYRA_ERROR) {
        snprintf(current->error, sizeof current->error, "%s", s);
        size_t k = strlen(current->error);
        while (k && current->error[k - 1] == '\n') current->error[--k] = 0;
    }
}

static void on_text(void *ud, const char *s) { append(&((task *)ud)->reply, s); }

static int on_allow(void *ud, const char *name, const char *detail, int outside) {
    (void)ud, (void)name, (void)detail, (void)outside;
    return -1; /* no one to ask */
}

/* Run t's turn on this thread. keep: leave the agent for the caller to free. */
static void run_task(task *t, int keep) {
    current = t;
    t->started = now();
    /* No per-agent working directory exists, so this changes it for every thread. */
    if (t->cwd && chdir(t->cwd) < 0) {
        snprintf(t->error, sizeof t->error, "cannot change to %s", t->cwd);
        t->rc = -1;
    } else if (myra_init(&t->agent, prov, model, perms) < 0) {
        t->rc = -1;
    } else {
        t->inited = 1;
        myra_add_coding_tools(&t->agent);
        t->agent.io = (myra_io){.ud = t, .text = on_text, .allow = on_allow};
        t->rc = myra_ask(&t->agent, t->prompt);
        if (myra_interrupted && !t->rc) t->rc = -1; /* a stopped turn has no answer */
        snprintf(t->last_error, sizeof t->last_error, "%s", myra_last_error);
        const char *r = myra_last_reply(&t->agent);
        t->answer = r ? strdup(r) : NULL;
        if (r && !t->reply.n) append(&t->reply, r); /* a reply that did not stream */
        t->usage = t->agent.session;
        if (!keep) myra_free(&t->agent), t->inited = 0;
    }
    if (t->rc && !*t->error) snprintf(t->error, sizeof t->error, "%s", "stopped");
    t->finished = now();
    current = NULL;
}

/* ---- processes mode ---- */

/* Every spawn holds this while its pipes exist without FD_CLOEXEC, so no child spawned
   by another worker inherits them. */
static pthread_mutex_t spawning = PTHREAD_MUTEX_INITIALIZER;

static int cloexec_pipe(int fd[2]) {
    if (pipe(fd) < 0) return -1;
    fcntl(fd[0], F_SETFD, FD_CLOEXEC);
    fcntl(fd[1], F_SETFD, FD_CLOEXEC);
    return 0;
}

/* Read what fd has into t; at EOF or a hard error, close it and clear *open. */
static void drain(int fd, text *t, int *open) {
    char chunk[65536];
    ssize_t n = read(fd, chunk, sizeof chunk);
    if (n > 0) append_n(t, chunk, (size_t)n);
    else if (n == 0 || errno != EINTR) close(fd), *open = 0;
}

/* -v: echo the child's new complete note lines, prefixed with the task's title. */
static void echo_notes(task *t) {
    for (char *nl; (nl = memchr(t->notes.p + t->shown, '\n', t->notes.n - t->shown));) {
        fprintf(stderr, "[%s] %.*s\n", t->title ? t->title : "task",
                (int)(nl - t->notes.p - t->shown), t->notes.p + t->shown);
        t->shown = (size_t)(nl - t->notes.p) + 1;
    }
}

/* The last stdout line of myra --json: its result, parsed, or NULL. */
static cJSON *result_line(const text *out) {
    if (!out->n) return NULL;
    const char *end = out->p + out->n;
    while (end > out->p && end[-1] == '\n') end--;
    const char *start = end;
    while (start > out->p && start[-1] != '\n') start--;
    char *line = strndup(start, (size_t)(end - start));
    cJSON *r = line ? cJSON_Parse(line) : NULL;
    free(line);
    return r;
}

/* ---- the family, with a budget ---- */

static struct {
    pid_t pid;
    FILE *in, *out; /* its stdin, for add lines; its stdout, for the replies */
    char fid[64];
} host = {.pid = -1};

/* Start "myra -r host" and read the family's id; -1 with the reason on stderr. */
static int start_host(const char *argv0) {
    const char *argv[16];
    int n = 0;
    argv[n++] = myra_bin, argv[n++] = "-r", argv[n++] = "host";
    argv[n++] = "--budget-tokens", argv[n++] = budget_tokens;
    if (budget_cost) argv[n++] = "--budget-cost", argv[n++] = budget_cost;
    argv[n++] = "--permissions", argv[n++] = perms_name;
    if (pname) argv[n++] = "-P", argv[n++] = pname;
    if (model) argv[n++] = "-m", argv[n++] = model;
    argv[n] = NULL;
    int in[2], out[2];
    if (cloexec_pipe(in) < 0 || cloexec_pipe(out) < 0) {
        perror("pipe");
        return -1;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    int e = posix_spawnp(&host.pid, myra_bin, &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]), close(out[1]);
    if (e) {
        fprintf(stderr, "%s: cannot run %s: %s\n", argv0, myra_bin, strerror(e));
        close(in[1]), close(out[0]);
        host.pid = -1;
        return -1;
    }
    host.in = fdopen(in[1], "w"), host.out = fdopen(out[0], "r");
    if (!host.in || !host.out || !fgets(host.fid, sizeof host.fid, host.out)) {
        fprintf(stderr, "%s: %s -r host did not start a family\n", argv0, myra_bin);
        return -1;
    }
    host.fid[strcspn(host.fid, "\n")] = 0;
    return 0;
}

/* Close the host's stdin, which ends the family, and reap it. */
static void stop_host(void) {
    if (host.pid < 0) return;
    if (host.in) fclose(host.in);
    if (host.out) fclose(host.out);
    while (waitpid(host.pid, NULL, 0) < 0 && errno == EINTR) {}
    host.pid = -1;
}

/* Add each of the n tasks to the family before any starts, on this one thread, so an
   add and its reply cannot interleave with another's. A task whose add fails keeps
   member_id "" and t->error, and fails when it runs. */
static void add_members(task *t, size_t n) {
    /* Ignored only here, so a dead host fails the write; children would inherit SIG_IGN. */
    struct sigaction ign = {.sa_handler = SIG_IGN}, old;
    sigaction(SIGPIPE, &ign, &old);
    for (size_t i = 0; i < n; i++) {
        char id[64];
        int ok = host.in && fprintf(host.in, "add %s\n", t[i].member) > 0 &&
                 fflush(host.in) == 0 && fgets(id, sizeof id, host.out);
        if (!ok && host.in) fclose(host.in), host.in = NULL; /* drops the unsent add */
        if (ok) id[strcspn(id, "\n")] = 0;
        if (ok && !MYRA_STARTS_WITH(id, "error: ")) memcpy(t[i].member_id, id, sizeof id);
        else snprintf(t[i].error, sizeof t[i].error, "cannot join the family: %s",
                      ok ? id + 7 : "the host exited");
    }
    sigaction(SIGPIPE, &old, NULL);
}

/* environ without the family variables, plus MYRA_FAMILY and MYRA_ID for member id;
   malloc'd, with its two new strings. */
static char **member_env(const char *id) {
    size_t n = 0;
    while (environ[n]) n++;
    char **env = calloc(n + 3, sizeof *env);
    if (!env) { perror("calloc"); exit(1); }
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (!MYRA_STARTS_WITH(environ[i], "MYRA_FAMILY=") && !MYRA_STARTS_WITH(environ[i], "MYRA_ID=") &&
            !MYRA_STARTS_WITH(environ[i], "MYRA_SELF=") && !MYRA_STARTS_WITH(environ[i], "MYRA_NAME=") &&
            !MYRA_STARTS_WITH(environ[i], "MYRA_PARENT="))
            env[k++] = environ[i];
    text a = {0}, b = {0};
    appendf(&a, "MYRA_FAMILY=%s", host.fid);
    appendf(&b, "MYRA_ID=%s", id);
    env[k++] = a.p, env[k++] = b.p;
    return env;
}

/* Free what member_env returned: its last two strings are its own. */
static void free_member_env(char **env) {
    size_t n = 0;
    while (env[n]) n++;
    free(env[n - 1]), free(env[n - 2]);
    free(env);
}

/* r's field key if it is a number in int range, else 0. */
static double number(const cJSON *r, const char *key) {
    const cJSON *v = cJSON_GetObjectItem(r, key);
    return cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble < 2e9 ? v->valuedouble : 0;
}

/* A prompt is one argument; Linux caps one at 128 KiB (MAX_ARG_STRLEN). */
#define MAX_PROMPT (120 * 1024)

/* Run t's turn as a child "myra -p --json". */
static void run_process(task *t) {
    t->started = now();
    size_t len = strlen(t->prompt);
    if (len > MAX_PROMPT) {
        snprintf(t->error, sizeof t->error,
                 "the prompt is %zu bytes, over the %d a child's argument may hold; "
                 "--mode threads has no such limit", len, MAX_PROMPT);
        t->rc = -1;
        t->finished = now();
        return;
    }
    const char *id = t->member_id;
    if (host.pid > 0 && !*id) { /* add_members failed for it */
        t->rc = -1;
        t->finished = now();
        return;
    }
    const char *argv[20];
    int n = 0;
    argv[n++] = myra_bin;
    if (*id) argv[n++] = "-r"; /* joins the family MYRA_FAMILY and MYRA_ID name */
    if (pname) argv[n++] = "-P", argv[n++] = pname;
    if (model) argv[n++] = "-m", argv[n++] = model;
    argv[n++] = "--permissions", argv[n++] = perms_name;
    argv[n++] = "--json", argv[n++] = "--no-save";
    if (t->cwd) argv[n++] = "-C", argv[n++] = t->cwd;
    argv[n++] = "-p", argv[n++] = t->prompt;
    argv[n] = NULL;

    int out[2] = {-1, -1}, err[2] = {-1, -1};
    pid_t pid = -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    pthread_mutex_lock(&spawning);
    if (cloexec_pipe(out) < 0 || cloexec_pipe(err) < 0) {
        snprintf(t->error, sizeof t->error, "pipe: %s", strerror(errno));
    } else {
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_adddup2(&fa, out[1], 1);
        posix_spawn_file_actions_adddup2(&fa, err[1], 2);
        char **env = *id ? member_env(id) : environ;
        int e = posix_spawnp(&pid, myra_bin, &fa, NULL, (char *const *)argv, env);
        if (env != environ) free_member_env(env);
        if (e) {
            snprintf(t->error, sizeof t->error, "cannot run %s: %s", myra_bin, strerror(e));
            pid = -1;
        }
    }
    for (int i = 0; i < 2; i++) /* the parent keeps only the read ends */
        if ((i ? err : out)[1] >= 0) close((i ? err : out)[1]);
    pthread_mutex_unlock(&spawning);
    posix_spawn_file_actions_destroy(&fa);
    if (pid < 0) {
        if (out[0] >= 0) close(out[0]);
        if (err[0] >= 0) close(err[0]);
        t->rc = -1;
        t->finished = now();
        return;
    }

    text stdout_text = {0};
    int open_out = 1, open_err = 1, signalled = 0;
    while (open_out || open_err) {
        if (!signalled && (atomic_load(&stopping) || atomic_load(&t->stop))) {
            kill(pid, SIGINT); /* as Ctrl-C would: the child ends its turn and reports */
            signalled = 1;
        }
        struct pollfd pf[2] = {{open_out ? out[0] : -1, POLLIN, 0},
                               {open_err ? err[0] : -1, POLLIN, 0}};
        if (poll(pf, 2, 100) <= 0) continue; /* the timeout rechecks the stop flags */
        if (pf[0].revents) drain(out[0], &stdout_text, &open_out);
        if (pf[1].revents) {
            drain(err[0], &t->notes, &open_err);
            if (verbose && t->notes.n) echo_notes(t);
        }
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}

    cJSON *r = result_line(&stdout_text);
    const char *outcome = cJSON_GetStringValue(cJSON_GetObjectItem(r, "outcome"));
    const char *reply = cJSON_GetStringValue(cJSON_GetObjectItem(r, "text"));
    const char *why = cJSON_GetStringValue(cJSON_GetObjectItem(r, "error"));
    t->rc = outcome && (!strcmp(outcome, "complete") || !strcmp(outcome, "truncated")) ? 0 : -1;
    if (reply && *reply) t->answer = strdup(reply), append(&t->reply, reply);
    if (why) snprintf(t->error, sizeof t->error, "%s", why);
    else if (outcome && !strcmp(outcome, "cancelled")) snprintf(t->error, sizeof t->error, "stopped");
    else if (!r)
        snprintf(t->error, sizeof t->error, "%s gave no result (exit status %d)", myra_bin,
                 WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st));
    memcpy(t->last_error, t->error, sizeof t->error); /* its own process: nothing to mix up */
    cJSON *cost = cJSON_GetObjectItem(r, "cost");
    t->usage = (myra_usage){.requests = (int)number(r, "requests"), .in = number(r, "input_tokens"),
                            .out = number(r, "output_tokens"), .cached = number(r, "cached_tokens"),
                            .cost = number(r, "cost"), .costed = cJSON_IsNumber(cost)};
    cJSON_Delete(r);
    free(stdout_text.p);
    if (t->rc && !*t->error) snprintf(t->error, sizeof t->error, "stopped");
    t->finished = now();
}

/* Run t's turn in the mode chosen. keep: threads mode only, see worker. */
static void run_one(task *t, int keep) {
    if (threads) run_task(t, keep);
    else run_process(t);
}

/* Under sched: mark t finished and report it. */
static void finish(task *t) {
    t->state = FINISHED;
    size_t k = ++finished_tasks;
    if (plan_mode)
        fprintf(stderr, "[myra-swarm] %zu/%zu %s in %.1f s: %s%s%s\n", k, ntasks,
                t->rc ? "failed" : "done", t->finished - t->started, t->title,
                t->rc ? ": " : "", t->rc ? t->error : "");
    atomic_store(&t->done, 1);
    pthread_cond_broadcast(&sched_changed);
}

/* Under sched: a task t that will never start, because of why. */
static void skip(task *t, const char *why) {
    t->rc = -1;
    snprintf(t->error, sizeof t->error, "%s", why);
    finish(t);
}

/* Under sched: the index of a pending task whose deps have all finished, ntasks if none
   is ready, or ntasks + 1 if one was skipped and the scan must restart. *pending: whether
   any task is still pending. */
static size_t next_ready(int *pending) {
    *pending = 0;
    for (size_t i = 0; i < ntasks; i++) {
        task *t = &tasks[i];
        if (t->state != PENDING) continue;
        *pending = 1;
        if (atomic_load(&stopping)) {
            skip(t, "not started: stopped");
            return ntasks + 1;
        }
        int ready = 1;
        for (size_t d = 0; d < t->ndeps; d++) {
            const task *dep = &tasks[t->deps[d]];
            if (dep->state != FINISHED) ready = 0;
            else if (dep->rc) {
                char why[1024];
                snprintf(why, sizeof why, "not started: it depends on \"%s\", which failed",
                         dep->title);
                skip(t, why);
                return ntasks + 1;
            }
        }
        if (ready) return i;
    }
    return ntasks;
}

static void *worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&sched);
    for (;;) {
        int pending;
        size_t i = next_ready(&pending);
        if (i == ntasks + 1) continue;
        if (i == ntasks) {
            if (!pending) break;
            /* Timed: the Ctrl-C handler cannot signal a condition variable. */
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += 100000000;
            if (until.tv_nsec >= 1000000000) until.tv_sec++, until.tv_nsec -= 1000000000;
            pthread_cond_timedwait(&sched_changed, &sched, &until);
            continue;
        }
        task *t = &tasks[i];
        t->state = RUNNING;
        pthread_mutex_unlock(&sched);
        if (prompt_for) t->prompt = prompt_for(i);
        /* A finishing agent's myra_free kills every agent's shell calls, running ones
           too (docs/dev/agent-context.md), so plan mode frees them all at the end.
           --tasks frees each at once, as a GUI closing a tab would, so its tests see it. */
        run_one(t, plan_mode);
        pthread_mutex_lock(&sched);
        finish(t);
    }
    pthread_mutex_unlock(&sched);
    return NULL;
}

static void on_sigint(int sig) {
    (void)sig;
    atomic_store(&stopping, 1);
    myra_interrupted = 1;
}

/* Run every task on min(jobs, ntasks) threads; jobs 0: one each. Returns once all are
   running; join with join_workers. */
static pthread_t *workers;
static size_t nworkers;

static void start_workers(size_t jobs) {
    nworkers = jobs && jobs < ntasks ? jobs : ntasks;
    workers = calloc(nworkers ? nworkers : 1, sizeof *workers);
    if (!workers) { perror("calloc"); exit(1); }
    for (size_t i = 0; i < nworkers; i++)
        if (pthread_create(&workers[i], NULL, worker, NULL)) { perror("pthread_create"); exit(1); }
}

static void join_workers(void) {
    for (size_t i = 0; i < nworkers; i++) pthread_join(workers[i], NULL);
    free(workers);
}

static void free_task(task *t) {
    if (t->inited) myra_free(&t->agent);
    free(t->title), free(t->prompt), free(t->cwd), free(t->answer);
    free(t->reply.p), free(t->notes.p);
}

/* ---- --tasks ---- */

static cJSON *task_result(const task *t, int i) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "task", i);
    cJSON_AddBoolToObject(o, "ok", t->rc == 0);
    cJSON_AddBoolToObject(o, "stopped", t->stopped);
    cJSON_AddStringToObject(o, "text", str(&t->reply));
    cJSON_AddStringToObject(o, "error", t->error);
    cJSON_AddStringToObject(o, "last_error", t->last_error);
    cJSON_AddStringToObject(o, "notes", str(&t->notes));
    cJSON_AddNumberToObject(o, "seconds", t->finished - t->started);
    cJSON_AddNumberToObject(o, "requests", t->usage.requests);
    cJSON_AddNumberToObject(o, "tokens_in", t->usage.in);
    cJSON_AddNumberToObject(o, "tokens_out", t->usage.out);
    return o;
}

static int run_tasks(const char *argv0, size_t jobs) {
    char *line = NULL;
    size_t cap = 0;
    for (ssize_t k; (k = getline(&line, &cap, stdin)) > 0;) {
        if (strspn(line, " \t\r\n") == (size_t)k) continue;
        cJSON *j = cJSON_Parse(line);
        const char *prompt = cJSON_GetStringValue(cJSON_GetObjectItem(j, "prompt"));
        task *grown = prompt ? realloc(tasks, (ntasks + 1) * sizeof *tasks) : NULL;
        if (!grown) {
            if (prompt) perror("realloc");
            else fprintf(stderr, "%s: task %zu: no \"prompt\" string\n", argv0, ntasks);
            cJSON_Delete(j);
            exit(2); /* the tasks so far are not yet running */
        }
        tasks = grown;
        task *t = &tasks[ntasks++];
        memset(t, 0, sizeof *t);
        t->prompt = strdup(prompt);
        const char *cwd = cJSON_GetStringValue(cJSON_GetObjectItem(j, "cwd"));
        t->cwd = cwd ? strdup(cwd) : NULL;
        cJSON *stop = cJSON_GetObjectItem(j, "stop_after");
        t->stop_after = cJSON_IsNumber(stop) ? stop->valuedouble : 0;
        snprintf(t->member, sizeof t->member, "task-%zu", ntasks);
        cJSON_Delete(j);
    }
    free(line);
    if (budget_tokens) {
        if (start_host(argv0) < 0) exit(1);
        add_members(tasks, ntasks);
    }

    start_workers(jobs);
    /* Stop each task whose stop_after has passed. Threads mode has no per-agent flag, so
       it sets the process-wide one. */
    double t0 = now();
    for (size_t left = ntasks; left;) {
        left = 0;
        for (size_t i = 0; i < ntasks; i++) {
            task *t = &tasks[i];
            if (atomic_load(&t->done)) continue;
            left++;
            if (t->stop_after > 0 && !t->stopped && now() - t0 >= t->stop_after) {
                t->stopped = 1;
                if (threads) myra_interrupted = 1;
                else atomic_store(&t->stop, 1);
            }
        }
        if (left) usleep(10000);
    }
    join_workers();
    stop_host();

    int failed = 0;
    for (size_t i = 0; i < ntasks; i++) {
        cJSON *o = task_result(&tasks[i], (int)i);
        char *s = cJSON_PrintUnformatted(o);
        puts(s);
        free(s);
        cJSON_Delete(o);
        failed |= tasks[i].rc != 0;
        free_task(&tasks[i]);
    }
    free(tasks);
    return failed;
}

/* ---- plans ---- */

typedef struct {
    char *title;
    text body;    /* without its depends: lines */
    char **deps;  /* the titles its depends: lines name */
    int *dep_lines;
    size_t ndeps;
    size_t *after; /* deps as part indices, once resolved */
} section;

typedef struct {
    section goal;
    section *parts;
    size_t n;
} plan;

/* The level (1 or 2) of an ATX heading line of n bytes, with its title in *title;
   0 if it is not one. Deeper headings count as text. */
static int heading(const char *s, size_t n, char **title) {
    size_t i = 0;
    while (i < n && i < 3 && s[i] == ' ') i++;
    size_t h = 0;
    while (i + h < n && s[i + h] == '#') h++;
    if (h < 1 || h > 2 || (i + h < n && s[i + h] != ' ' && s[i + h] != '\t')) return 0;
    const char *a = s + i + h, *b = s + n;
    while (a < b && (*a == ' ' || *a == '\t')) a++;
    while (b > a && (b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\r')) b--;
    const char *c = b; /* a closing run of #, after a space */
    while (c > a && c[-1] == '#') c--;
    if (c == a || (c < b && (c[-1] == ' ' || c[-1] == '\t'))) {
        b = c;
        while (b > a && (b[-1] == ' ' || b[-1] == '\t')) b--;
    }
    *title = strndup(a, (size_t)(b - a));
    return (int)h;
}

/* Whether the line opens or closes a fenced code block; updates *fence and *fence_n. */
static void fence_line(const char *s, size_t n, char *fence, size_t *fence_n) {
    size_t i = 0;
    while (i < n && i < 3 && s[i] == ' ') i++;
    char c = i < n ? s[i] : 0;
    if (c != '`' && c != '~') return;
    size_t k = 0;
    while (i + k < n && s[i + k] == c) k++;
    if (k < 3) return;
    if (!*fence) *fence = c, *fence_n = k;
    else if (c == *fence && k >= *fence_n && strspn(s + i + k, " \t\r") == n - i - k) *fence = 0;
}

/* If the line is "depends: A, B", add A and B to sec and return 1; 0 if it is not one,
   -1 if it names nothing. */
static int depends_line(const char *s, size_t n, section *sec, int line) {
    size_t i = 0;
    while (i < n && i < 3 && s[i] == ' ') i++;
    if (n - i < 8 || strncasecmp(s + i, "depends:", 8)) return 0;
    int named = 0;
    for (const char *a = s + i + 8, *end = s + n; a < end;) {
        const char *b = memchr(a, ',', (size_t)(end - a));
        if (!b) b = end;
        const char *x = a, *y = b;
        while (x < y && strchr(" \t\r", *x)) x++;
        while (y > x && strchr(" \t\r", y[-1])) y--;
        if (y > x) {
            char **d = realloc(sec->deps, (sec->ndeps + 1) * sizeof *d);
            int *l = d ? realloc(sec->dep_lines, (sec->ndeps + 1) * sizeof *l) : NULL;
            if (!l) { perror("realloc"); exit(1); }
            sec->deps = d, sec->dep_lines = l;
            d[sec->ndeps] = strndup(x, (size_t)(y - x));
            l[sec->ndeps++] = line;
            named = 1;
        }
        a = b + 1;
    }
    return named ? 1 : -1;
}

/* Whether part i reaches part i again through its deps; appends the cycle to path. */
static int cycle_from(const plan *p, size_t i, int *mark, text *path) {
    if (mark[i] == 2) return 0;
    if (mark[i] == 1) { appendf(path, "\"%s\"", p->parts[i].title); return 1; }
    mark[i] = 1;
    for (size_t d = 0; d < p->parts[i].ndeps; d++)
        if (cycle_from(p, p->parts[i].after[d], mark, path)) {
            text t = {0};
            appendf(&t, "\"%s\" -> %s", p->parts[i].title, str(path));
            free(path->p);
            *path = t;
            return 1;
        }
    mark[i] = 2;
    return 0;
}

/* Turn each part's dependency titles into indices; -1 with why set. */
static int resolve_deps(plan *p, char *why, size_t cap) {
    for (size_t i = 0; i < p->n; i++)
        for (size_t j = 0; j < i; j++)
            if (!strcmp(p->parts[i].title, p->parts[j].title)) {
                snprintf(why, cap, "two parts are titled \"%s\"", p->parts[i].title);
                return -1;
            }
    for (size_t i = 0; i < p->n; i++) {
        section *sec = &p->parts[i];
        sec->after = calloc(sec->ndeps ? sec->ndeps : 1, sizeof *sec->after);
        if (!sec->after) { perror("calloc"); exit(1); }
        for (size_t d = 0; d < sec->ndeps; d++) {
            size_t j = 0;
            while (j < p->n && strcmp(p->parts[j].title, sec->deps[d])) j++;
            if (j == p->n) {
                snprintf(why, cap, "line %d: depends: no part is titled \"%s\"",
                         sec->dep_lines[d], sec->deps[d]);
                return -1;
            }
            if (j == i) {
                snprintf(why, cap, "line %d: \"%s\" depends on itself", sec->dep_lines[d],
                         sec->title);
                return -1;
            }
            sec->after[d] = j;
        }
    }
    int *mark = calloc(p->n, sizeof *mark);
    if (!mark) { perror("calloc"); exit(1); }
    int rc = 0;
    for (size_t i = 0; i < p->n && !rc; i++) {
        text path = {0};
        if (cycle_from(p, i, mark, &path)) {
            snprintf(why, cap, "parts depend on each other: %s", str(&path));
            rc = -1;
        }
        free(path.p);
    }
    free(mark);
    return rc;
}

/* Parse src into p; -1 with why set. */
static int parse_plan(const char *src, plan *p, char *why, size_t cap) {
    memset(p, 0, sizeof *p);
    char fence = 0;
    size_t fence_n = 0;
    int line = 0;
    for (const char *s = src; *s;) {
        size_t n = strcspn(s, "\n");
        line++;
        char *title = NULL;
        int level = 0, in_fence = fence != 0;
        fence_line(s, n, &fence, &fence_n);
        if (!in_fence && !fence) level = heading(s, n, &title);
        if (level && !*title) {
            snprintf(why, cap, "line %d: a heading without a title", line);
            free(title);
            return -1;
        }
        if (level == 1) {
            if (p->goal.title) {
                snprintf(why, cap, "line %d: a second # heading; a plan has one objective", line);
                free(title);
                return -1;
            }
            p->goal.title = title;
        } else if (level == 2) {
            if (!p->goal.title) {
                snprintf(why, cap, "line %d: a ## heading before the # heading", line);
                free(title);
                return -1;
            }
            section *grown = realloc(p->parts, (p->n + 1) * sizeof *p->parts);
            if (!grown) { perror("realloc"); exit(1); }
            p->parts = grown;
            p->parts[p->n++] = (section){.title = title};
        } else if (!p->goal.title) {
            if (strspn(s, " \t\r") < n) {
                snprintf(why, cap, "line %d: text before the # heading", line);
                return -1;
            }
        } else {
            section *sec = p->n ? &p->parts[p->n - 1] : &p->goal;
            int dep = in_fence || fence ? 0 : depends_line(s, n, sec, line);
            if (dep && !p->n) {
                snprintf(why, cap, "line %d: depends: under the # heading; only parts have it",
                         line);
                return -1;
            }
            if (dep < 0) {
                snprintf(why, cap, "line %d: depends: names no part", line);
                return -1;
            }
            if (!dep) {
                append_n(&sec->body, s, n);
                append(&sec->body, "\n");
            }
        }
        s += n + (s[n] == '\n');
    }
    if (!p->goal.title) snprintf(why, cap, "no # heading: a plan needs one objective");
    else if (!p->n) snprintf(why, cap, "no ## headings: a plan needs at least one part");
    else return resolve_deps(p, why, cap);
    return -1;
}

static void free_section(section *s) {
    for (size_t d = 0; d < s->ndeps; d++) free(s->deps[d]);
    free(s->title), free(s->body.p), free(s->deps), free(s->dep_lines), free(s->after);
}

static void free_plan(plan *p) {
    free_section(&p->goal);
    for (size_t i = 0; i < p->n; i++) free_section(&p->parts[i]);
    free(p->parts);
}

/* s without leading and trailing blank lines, as length *n from the returned start. */
static const char *trimmed(const char *s, size_t *n) {
    const char *a = s, *b = s + strlen(s);
    while (a < b && strchr(" \t\r\n", *a)) a++;
    while (b > a && strchr(" \t\r\n", b[-1])) b--;
    *n = (size_t)(b - a);
    return a;
}

static void add_section(text *t, const char *label, const section *s) {
    size_t n;
    const char *body = trimmed(str(&s->body), &n);
    appendf(t, "%s%s\n", label, s->title);
    if (n) appendf(t, "\n%.*s\n", (int)n, body);
}

/* An <answer> element for task k, numbered as part n. */
static void add_answer(text *t, size_t n, const task *k) {
    appendf(t, "\n<answer part=\"%zu\">\nPart: %s\n\n", n, k->title);
    if (k->rc == 0 && k->answer) {
        size_t len;
        const char *a = trimmed(k->answer, &len);
        appendf(t, "%.*s\n", (int)len, a);
    } else appendf(t, "This part failed: %s\n", k->error);
    append(t, "</answer>\n");
}

static const plan *the_plan; /* for part_prompt_at */

static char *part_prompt(const plan *p, size_t i) {
    const section *me = &p->parts[i];
    text t = {0};
    appendf(&t, "You are one of %zu agents. Each works on one part of a shared objective.\n\n",
            p->n);
    add_section(&t, "# Objective: ", &p->goal);
    append(&t, "\n");
    add_section(&t, "## Your part: ", me);
    if (me->ndeps) {
        append(&t, "\n## Answers your part builds on\n\nThese parts finished before yours "
                   "started. Use their answers instead of redoing their work.\n");
        for (size_t d = 0; d < me->ndeps; d++) add_answer(&t, me->after[d] + 1, &tasks[me->after[d]]);
    }
    if (p->n > 1) {
        append(&t, "\n## Other parts, handled by other agents\n\n");
        for (size_t j = 0; j < p->n; j++)
            if (j != i) appendf(&t, "- %s\n", p->parts[j].title);
    }
    append(&t, "\nWork only on your part. End with a complete answer to it that stands on "
               "its own: another agent combines it with the other answers without seeing "
               "your work. Name the files, commands and sources it rests on.");
    if (words) appendf(&t, " Keep the answer under %d words: state findings, not the steps that "
                           "found them.", words);
    append(&t, "\n");
    return t.p;
}

static char *part_prompt_at(size_t i) { return part_prompt(the_plan, i); }

static char *combine_prompt(const plan *p) {
    text t = {0};
    add_section(&t, "# Objective: ", &p->goal);
    appendf(&t, "\n%zu agents each worked on one part of this objective. Their answers "
                "follow, one per <answer> element. A part that failed says so.\n", p->n);
    for (size_t i = 0; i < p->n; i++) add_answer(&t, i + 1, &tasks[i]);
    append(&t, "\nAddress the objective using these answers. Check them against each other. "
               "Where they disagree, or a part failed, say so, and resolve it if you can.\n");
    return t.p;
}

static void add_usage(myra_usage *sum, const myra_usage *u) {
    sum->requests += u->requests;
    sum->in += u->in, sum->cached += u->cached, sum->out += u->out, sum->cost += u->cost;
    sum->costed |= u->costed;
}

static cJSON *part_json(const task *t) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "title", t->title ? t->title : "");
    cJSON *deps = cJSON_AddArrayToObject(o, "depends");
    for (size_t d = 0; d < t->ndeps; d++)
        cJSON_AddItemToArray(deps, cJSON_CreateString(tasks[t->deps[d]].title));
    cJSON_AddBoolToObject(o, "ok", t->rc == 0);
    cJSON_AddStringToObject(o, "answer", t->answer && !t->rc ? t->answer : "");
    cJSON_AddStringToObject(o, "error", t->rc ? t->error : "");
    cJSON_AddNumberToObject(o, "seconds", t->finished - t->started);
    cJSON_AddNumberToObject(o, "requests", t->usage.requests);
    cJSON_AddNumberToObject(o, "tokens_in", t->usage.in);
    cJSON_AddNumberToObject(o, "tokens_out", t->usage.out);
    if (t->usage.costed) cJSON_AddNumberToObject(o, "cost", t->usage.cost);
    else cJSON_AddNullToObject(o, "cost");
    return o;
}

static void print_part(const char *title, const task *t) {
    size_t n = 0;
    const char *a = t->rc == 0 && t->answer ? trimmed(t->answer, &n) : NULL;
    printf("## %s\n\n", title);
    if (a) printf("%.*s\n\n", (int)n, a);
    else printf("*Failed: %s*\n\n", t->error);
}

static char *read_all(FILE *f) {
    text t = {0};
    char chunk[65536];
    size_t n;
    do { /* a short read means EOF or an error; reading on is undefined after an error */
        n = fread(chunk, 1, sizeof chunk, f);
        append_n(&t, chunk, n);
    } while (n == sizeof chunk);
    if (ferror(f)) { free(t.p); return NULL; }
    if (!t.p) append(&t, "");
    return t.p;
}

static int run_plan(const char *argv0, const char *path, size_t jobs, int json) {
    int from_stdin = !strcmp(path, "-");
    FILE *f = from_stdin ? stdin : fopen(path, "r");
    char *src = f ? read_all(f) : NULL;
    if (f && !from_stdin) fclose(f);
    if (!src) {
        fprintf(stderr, "%s: cannot read %s\n", argv0, path);
        return 2;
    }
    plan p;
    char why[256];
    int bad = parse_plan(src, &p, why, sizeof why);
    free(src);
    if (bad) {
        fprintf(stderr, "%s: %s: %s\n", argv0, path, why);
        free_plan(&p);
        return 2;
    }

    ntasks = p.n;
    tasks = calloc(ntasks, sizeof *tasks);
    if (!tasks) { perror("calloc"); exit(1); }
    for (size_t i = 0; i < p.n; i++) {
        tasks[i].title = strdup(p.parts[i].title);
        snprintf(tasks[i].member, sizeof tasks[i].member, "part-%zu", i + 1);
        tasks[i].deps = p.parts[i].after;
        tasks[i].ndeps = p.parts[i].ndeps;
    }
    the_plan = &p;
    prompt_for = part_prompt_at;
    task combine = {.title = strdup("combine"), .member = "combine"};
    if (budget_tokens) {
        if (start_host(argv0) < 0) exit(1);
        add_members(tasks, ntasks);
        add_members(&combine, 1);
    }
    fprintf(stderr, "[myra-swarm] %zu parts of: %s\n", p.n, p.goal.title);
    start_workers(jobs);
    join_workers();
    for (size_t i = 0; i < ntasks; i++) /* see worker */
        if (tasks[i].inited) myra_free(&tasks[i].agent), tasks[i].inited = 0;

    size_t ok = 0;
    for (size_t i = 0; i < ntasks; i++) ok += tasks[i].rc == 0;
    if (atomic_load(&stopping)) snprintf(combine.error, sizeof combine.error, "stopped");
    else if (!ok) snprintf(combine.error, sizeof combine.error, "every part failed");
    else {
        fprintf(stderr, "[myra-swarm] combining %zu answers\n", ok);
        prompt_for = NULL;
        combine.prompt = combine_prompt(&p);
        run_one(&combine, 0);
    }
    stop_host();
    int failed = combine.rc != 0 || !combine.prompt || ok < ntasks;

    myra_usage sum = {0};
    for (size_t i = 0; i < ntasks; i++) add_usage(&sum, &tasks[i].usage);
    add_usage(&sum, &combine.usage);
    if (json) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "objective", p.goal.title);
        cJSON_AddBoolToObject(o, "ok", !failed);
        cJSON_AddStringToObject(o, "answer", combine.answer && !combine.rc ? combine.answer : "");
        cJSON_AddStringToObject(o, "error", combine.rc || !combine.prompt ? combine.error : "");
        cJSON *parts = cJSON_AddArrayToObject(o, "parts");
        for (size_t i = 0; i < ntasks; i++) cJSON_AddItemToArray(parts, part_json(&tasks[i]));
        cJSON_AddItemToObject(o, "combine", part_json(&combine));
        char *s = cJSON_PrintUnformatted(o);
        puts(s);
        free(s);
        cJSON_Delete(o);
    } else {
        printf("# %s\n\n", p.goal.title);
        size_t n = 0;
        const char *a = combine.answer && !combine.rc ? trimmed(combine.answer, &n) : NULL;
        if (a) printf("%.*s\n\n", (int)n, a);
        else printf("*Not combined: %s*\n\n", combine.error);
        printf("---\n\n");
        for (size_t i = 0; i < ntasks; i++) print_part(tasks[i].title, &tasks[i]);
    }
    char cost[32] = "";
    if (sum.costed) snprintf(cost, sizeof cost, ", $%.4f", sum.cost);
    fprintf(stderr, "[myra-swarm] %zu of %zu parts done; %d requests: %.0f in (%.0f cached), "
                    "%.0f out%s\n", ok, ntasks, sum.requests, sum.in, sum.cached, sum.out, cost);

    for (size_t i = 0; i < ntasks; i++) free_task(&tasks[i]);
    free(tasks);
    free_task(&combine);
    free_plan(&p);
    the_plan = NULL; /* p dies on return */
    return atomic_load(&stopping) ? 130 : failed;
}

/* ---- main ---- */

static int usage(const char *argv0, int rc) {
    fprintf(rc ? stderr : stdout,
            "usage: %s [options] PLAN.md   (- reads the plan from stdin)\n"
            "       %s [options] --tasks < TASKS\n"
            "  -P PROVIDER, -m MODEL     as for myra\n"
            "  --permissions MODE        auto (default), all or read-only\n"
            "  -j N                      run at most N agents at once (default: all)\n"
            "  --mode MODE               processes (default): each agent a child myra;\n"
            "                            threads: each a thread of this process\n"
            "  --myra PATH               the myra a child runs (default: myra, in PATH)\n"
            "  --budget-tokens N         the most tokens every agent together may use; the\n"
            "                            agents share one family's ledger (myra -r)\n"
            "  --budget-cost USD         a second limit, in dollars\n"
            "  --words N                 ask each part for an answer under N words\n"
            "                            (default 400; 0 does not ask)\n"
            "  --json                    print the plan's result as JSON\n"
            "  -v                        print every agent's notes on stderr\n"
            "PLAN.md: one # objective, then a ## heading per part, each with any text.\n"
            "  A part's line \"depends: TITLE, ...\" starts it after those parts finish,\n"
            "  with their answers in its prompt.\n"
            "TASKS: JSON lines, {\"prompt\": TEXT, \"cwd\": DIR, \"stop_after\": SECONDS}\n",
            argv0, argv0);
    return rc;
}

int main(int argc, char **argv) {
    static const struct option longs[] = {{"permissions", required_argument, 0, 'p'},
                                          {"tasks", no_argument, 0, 't'},
                                          {"json", no_argument, 0, 'J'},
                                          {"words", required_argument, 0, 'w'},
                                          {"mode", required_argument, 0, 'M'},
                                          {"budget-tokens", required_argument, 0, 'T'},
                                          {"budget-cost", required_argument, 0, 'C'},
                                          {"myra", required_argument, 0, 'B'},
                                          {"help", no_argument, 0, 'h'},
                                          {0, 0, 0, 0}};
    int tasks_mode = 0, json = 0;
    long jobs = 0;
    for (int c; (c = getopt_long(argc, argv, "P:m:j:vh", longs, NULL)) != -1;) {
        if (c == 'P') pname = optarg;
        else if (c == 'm') model = optarg;
        else if (c == 'j') {
            char *end;
            jobs = strtol(optarg, &end, 10);
            if (*end || jobs < 1) return usage(argv[0], 2);
        } else if (c == 'w') {
            char *end;
            long w = strtol(optarg, &end, 10);
            if (*end || w < 0 || w > 100000) return usage(argv[0], 2);
            words = (int)w;
        } else if (c == 'M') {
            if (!strcmp(optarg, "threads")) threads = 1;
            else if (!strcmp(optarg, "processes")) threads = 0;
            else return usage(argv[0], 2);
        } else if (c == 'B') myra_bin = optarg;
        else if (c == 'T') budget_tokens = optarg; /* the host checks the values */
        else if (c == 'C') budget_cost = optarg;
        else if (c == 'v') verbose = 1;
        else if (c == 't') tasks_mode = 1;
        else if (c == 'J') json = 1;
        else if (c == 'p') {
            if (!strcmp(optarg, "auto")) perms = MYRA_AUTO;
            else if (!strcmp(optarg, "all")) perms = MYRA_ALL;
            else if (!strcmp(optarg, "read-only")) perms = MYRA_READ_ONLY;
            else return usage(argv[0], 2); /* ask: there is no one to ask */
            perms_name = optarg;
        } else if (c == 'h') return usage(argv[0], 0);
        else return usage(argv[0], 2);
    }
    if (tasks_mode ? optind != argc || json : optind != argc - 1) return usage(argv[0], 2);
    if (budget_cost && !budget_tokens) {
        fprintf(stderr, "%s: --budget-cost needs --budget-tokens too\n", argv[0]);
        return 2;
    }
    if (threads && budget_tokens) {
        fprintf(stderr, "%s: a budget needs --mode processes: its ledger is a family's\n", argv[0]);
        return 2;
    }
    if (threads) { /* processes mode: each child chooses, as myra does */
        prov = pname ? myra_provider_named(pname) : myra_provider_default();
        if (!prov) {
            fprintf(stderr, "%s: no usable provider\n", argv[0]);
            return 2;
        }
    } else if (pname && !myra_provider_named(pname)) {
        fprintf(stderr, "%s: unknown provider %s\n", argv[0], pname);
        return 2;
    }
    myra_note_hook = on_note;
    /* No SA_RESTART: a blocking wait returns EINTR, so Ctrl-C takes effect at once. */
    struct sigaction sa = {.sa_handler = on_sigint};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    if (tasks_mode) return run_tasks(argv[0], (size_t)jobs);
    plan_mode = 1;
    return run_plan(argv[0], argv[optind], (size_t)jobs, json);
}
