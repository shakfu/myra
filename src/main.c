/* main.c - the agent CLI: options, provider choice, headless mode and the REPL. */
#include "myra.h"

#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "linenoise.h"

static const struct { const char *name, *args, *help; } COMMANDS[] = {
    {"/model", " [id]", "show or switch the model"},
    {"/models", " [filter]", "list the provider's models whose id contains filter"},
    {"/clear", "", "start a new conversation"},
    {"/help", "", "show this list"},
    {"/exit", "", "quit (also Ctrl-D)"},
};
#define NCOMMANDS (sizeof COMMANDS / sizeof *COMMANDS)

static void repl_help(FILE *f) {
    fputs("REPL commands:\n", f);
    for (size_t i = 0; i < NCOMMANDS; i++) {
        char sig[32];
        snprintf(sig, sizeof sig, "%s%s", COMMANDS[i].name, COMMANDS[i].args);
        fprintf(f, "  %-18s %s\n", sig, COMMANDS[i].help);
    }
}

static int usage(const char *argv0, int rc) {
    FILE *f = rc ? stderr : stdout;
    fprintf(f,
            "usage: %s [options] [-P provider] [-m model] [-p prompt]\n"
            "  -P  openrouter or local; remembered. Default: the remembered one if usable,\n"
            "      else the first cloud provider whose key is set\n"
            "  -m  model id; remembered per provider\n"
            "  -p  run one prompt headless and exit (default: REPL)\n"
            "  -V  print the version\n"
            "  -h, --help  print this help\n"
            "  --verbose   show each tool call's full arguments and result, not one line\n"
            "  --no-color  plain stderr; also when NO_COLOR is set or stderr is not a terminal\n"
            "  --permissions  when write, edit and shell run without asking:\n"
            "      auto       always, except write/edit outside the working directory (default)\n"
            "      ask        never: ask on the terminal each time\n"
            "      all        always\n"
            "      read-only  refuse them; read still runs\n", argv0);
    repl_help(f);
    return rc;
}

static void on_sigint(int sig) {
    (void)sig;
    myra_interrupted = 1;
}

/* The next input line in *line, trailing whitespace trimmed; NULL at EOF, or at
   Ctrl-C with *interrupted set. ctx is NULL when input is piped: no prompt, no editing.
   linenoise owns the returned string, but its default allocator is malloc, so *line
   stays free()-able either way. */
static char *next_line(linenoise_context_t *ctx, char **line, size_t *cap, int *interrupted) {
    ssize_t n;
    *interrupted = 0;
    if (ctx) {
        /* The editing loop is ours, so Ctrl-C needs no signal: linenoise clears ISIG
           and reports the keystroke. The prompt goes to stderr, like the other chatter. */
        linenoise_state_t st;
        if (linenoise_edit_start_dynamic(ctx, &st, STDIN_FILENO, STDERR_FILENO, 128, "> ") < 0) {
            /* ends the REPL like EOF; without this it would exit silently */
            myra_note(MYRA_ERROR, "error: cannot start line editing: %s\n", strerror(errno));
            return NULL;
        }
        char *l;
        while ((l = linenoise_edit_feed(&st)) == linenoise_edit_more) continue;
        linenoise_edit_stop(&st);
        if (!l) {
            *interrupted = linenoise_get_error() == LINENOISE_ERR_INTERRUPTED;
            return NULL;
        }
        if (*l) linenoise_history_add(ctx, l);
        free(*line);
        *line = l;
        *cap = strlen(l) + 1;
        n = (ssize_t)strlen(l);
    } else {
        if ((n = getline(line, cap, stdin)) < 0) {
            *interrupted = myra_interrupted; /* SIGINT makes getline fail with EINTR */
            return NULL;
        }
    }
    while (n && strchr(" \t\r\n", (*line)[n - 1])) (*line)[--n] = 0;
    return *line;
}

/* ---- tab completion ---- */

/* linenoise replaces the whole line with the chosen candidate, so each candidate
   repeats the text before the token. Its callback takes no user data, hence the statics. */
static myra_agent *completing;
static char **completion_models; /* the provider's ids: one fetch, on first use */
static int completion_models_tried;

static size_t last_token(const char *buf) {
    const char *p = buf + strlen(buf);
    while (p > buf && !strchr(" \t", p[-1])) p--;
    return (size_t)(p - buf);
}

/* Offer the line with name + suffix in place of the token at buf + at. */
static void add_match(linenoise_completions_t *lc, const char *buf, size_t at,
                      const char *name, const char *suffix) {
    const char *tok = buf + at;
    if (strncmp(name, tok, strlen(tok))) return;
    char out[PATH_MAX + 64];
    if (snprintf(out, sizeof out, "%.*s%s%s", (int)at, buf, name, suffix) < (int)sizeof out)
        linenoise_add_completion(lc, out);
}

static void complete_path(const char *buf, size_t at, linenoise_completions_t *lc) {
    const char *arg = buf + at, *slash = strrchr(arg, '/');
    size_t dlen = slash ? (size_t)(slash - arg) + 1 : 0; /* the directory, keeping its slash */
    char dir[PATH_MAX];
    if (dlen >= sizeof dir) return;
    memcpy(dir, arg, dlen);
    dir[dlen] = 0;
    DIR *d = opendir(dlen ? dir : ".");
    if (!d) return;
    const char *base = arg + dlen;
    size_t blen = strlen(base);
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (*base != '.' && *e->d_name == '.') continue; /* hidden ones only when asked for */
        if (strncmp(e->d_name, base, blen)) continue;
        char path[PATH_MAX];
        struct stat st;
        if (snprintf(path, sizeof path, "%s%s", dir, e->d_name) >= (int)sizeof path) continue;
        add_match(lc, buf, at, path, !stat(path, &st) && S_ISDIR(st.st_mode) ? "/" : "");
    }
    closedir(d);
}

static void complete(const char *buf, linenoise_completions_t *lc) {
    const char *arg;
    if (*buf == '/' && !strpbrk(buf, " \t")) { /* a command name */
        for (size_t i = 0; i < NCOMMANDS; i++) add_match(lc, buf, 0, COMMANDS[i].name, "");
    } else if ((arg = myra_command(buf, "/model"))) { /* a model id */
        if (!completion_models_tried) { /* costs a request, so only once and only if asked */
            completion_models_tried = 1;
            completion_models = myra_model_ids(completing);
        }
        for (char **m = completion_models; m && *m; m++)
            add_match(lc, buf, (size_t)(arg - buf), *m, "");
    } else {
        complete_path(buf, last_token(buf), lc);
    }
}

static void repl(myra_agent *a) {
    myra_note(MYRA_BOLD, "myra %s\n", MYRA_VERSION);
    myra_note(MYRA_DIM, "%s %s\n", a->prov->name, a->model);
    char *line = NULL;
    size_t cap = 0;
    int tty = isatty(STDIN_FILENO); /* no prompt or editing when input is piped */
    char *hist = tty ? myra_state_path("history", 1) : NULL;
    linenoise_context_t *ctx = tty ? linenoise_context_create() : NULL;
    if (ctx) {
        completing = a;
        linenoise_set_completion_callback(ctx, complete);
        linenoise_history_set_max_len(ctx, 1000);
        if (hist) linenoise_history_load(ctx, hist);
    }
    for (;;) {
        int interrupted;
        /* An interrupted turn leaves the flag set; cleared here, before completion can fetch. */
        myra_interrupted = 0;
        if (!next_line(ctx, &line, &cap, &interrupted)) {
            if (!interrupted) { /* EOF or read error */
                if (tty) fputc('\n', stderr); /* leave the prompt's line */
                break;
            }
            clearerr(stdin); /* Ctrl-C at the prompt: drop the line */
            if (!ctx) fputc('\n', stderr); /* linenoise already ended the line */
            continue;
        }
        if (!*line) continue;
        const char *arg;
        if (!strcmp(line, "/exit")) break;
        if (!strcmp(line, "/help")) {
            repl_help(stderr);
            continue;
        }
        if (!strcmp(line, "/clear")) {
            myra_clear(a);
            myra_note(MYRA_DIM, "history cleared\n");
            continue;
        }
        if ((arg = myra_command(line, "/models"))) {
            myra_list_models(a, arg);
            continue;
        }
        if ((arg = myra_command(line, "/model"))) {
            if (*arg) myra_set_model(a, arg); /* history is kept */
            myra_note(MYRA_DIM, "%s %s\n", a->prov->name, a->model);
            continue;
        }
        myra_ask(a, line);
    }
    free(line);
    myra_free_model_ids(completion_models);
    if (ctx) {
        /* prompts may hold secrets */
        if (hist && linenoise_history_save(ctx, hist) == 0) chmod(hist, 0600);
        linenoise_context_destroy(ctx);
    }
    free(hist);
}

int main(int argc, char **argv) {
    const char *prompt = NULL, *pname = NULL, *model = NULL;
    static const char *const modes[] = {[MYRA_AUTO] = "auto", [MYRA_ASK] = "ask",
                                        [MYRA_ALL] = "all", [MYRA_READ_ONLY] = "read-only"};
    static const struct option longopts[] = {{"permissions", required_argument, NULL, 'W'},
                                             {"verbose", no_argument, NULL, 'v'},
                                             {"no-color", no_argument, NULL, 'C'},
                                             {"help", no_argument, NULL, 'h'},
                                             {NULL, 0, NULL, 0}};
    myra_permissions perms = MYRA_AUTO;
    int opt, verbose = 0, color = 1;
    const char *no_color = getenv("NO_COLOR"); /* https://no-color.org */
    if (no_color && *no_color) color = 0;
    while ((opt = getopt_long(argc, argv, "p:P:m:hV", longopts, NULL)) != -1) {
        if (opt == 'V') {
            printf("myra %s\n", MYRA_VERSION);
            return 0;
        }
        if (opt == 'v') verbose = 1;
        else if (opt == 'C') color = 0;
        else if (opt == 'W') {
            size_t i = 0;
            while (i < sizeof modes / sizeof *modes && strcmp(optarg, modes[i])) i++;
            if (i == sizeof modes / sizeof *modes) {
                myra_note(MYRA_ERROR, "error: unknown permissions mode %s\n", optarg);
                return usage(argv[0], 2);
            }
            perms = (myra_permissions)i;
        }
        else if (opt == 'p') prompt = optarg;
        else if (opt == 'P') pname = optarg;
        else if (opt == 'm') model = optarg;
        else return usage(argv[0], opt == 'h' ? 0 : 2);
    }
    if (optind < argc) return usage(argv[0], 2);
    myra_color = color && isatty(STDERR_FILENO);
    if (prompt && myra_provider_named(prompt)) { /* -p and -P differ only by case */
        myra_note(MYRA_ERROR, "error: -p takes a prompt; did you mean -P %s?\n", prompt);
        return 2;
    }

    const myra_provider *p;
    if (pname) {
        if (!(p = myra_provider_named(pname))) {
            myra_note(MYRA_ERROR, "error: unknown provider %s\n", pname);
            return usage(argv[0], 2);
        }
    } else if (!(p = myra_provider_default())) {
        char keys[256] = "";
        for (size_t i = 0; i < MYRA_NPROVIDERS; i++)
            if (!MYRA_PROVIDERS[i].key_optional)
                snprintf(keys + strlen(keys), sizeof keys - strlen(keys), " %s",
                         MYRA_PROVIDERS[i].key_env);
        myra_note(MYRA_ERROR, "error: no provider: set%s or pass -P local\n", keys);
        return 1;
    }

    myra_agent a;
    if (myra_init(&a, p, model, perms) < 0) return 1;
    a.verbose = verbose;
    a.save_provider = pname != NULL;
    /* No SA_RESTART: blocking reads return EINTR, so Ctrl-C takes effect at once. */
    struct sigaction sa = {0};
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    int rc = 0;
    if (prompt) {
        rc = myra_ask(&a, prompt) < 0;
        if (myra_interrupted) rc = 130; /* 128 + SIGINT, as a shell would report */
    } else {
        repl(&a);
        myra_report_session(&a);
    }
    myra_free(&a);
    return rc;
}
