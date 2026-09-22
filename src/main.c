/* main.c - the agent CLI: options, provider choice, headless mode and the REPL. */
#include "agent.h"

#include <errno.h>
#include <getopt.h>
#include <locale.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HAVE_LIBEDIT
#include <editline/readline.h>

/* libedit retries read() after EINTR, so the first Ctrl-C would be lost. Its own
   SIGINT handler restores the terminal and passes the signal on to on_sigint, which
   jumps back here while readline() waits. A custom rl_getc_function cannot do this:
   macOS libedit truncates its result to one byte, which breaks non-ASCII input. */
static sigjmp_buf at_prompt_jmp;
static volatile sig_atomic_t at_prompt;
#endif

static const char *REPL_HELP =
    "REPL commands:\n"
    "  /model [id]        show or switch the model\n"
    "  /models [filter]   list the provider's models whose id contains filter\n"
    "  /clear             start a new conversation\n"
    "  /help              show this list\n"
    "  /exit              quit (also Ctrl-D)\n";

static int usage(const char *argv0, int rc) {
    FILE *f = rc ? stderr : stdout;
    fprintf(f,
            "usage: %s [options] [-P provider] [-m model] [-p prompt]\n"
            "  -P  openrouter or local; remembered. Default: the remembered one if usable,\n"
            "      else the first cloud provider whose key is set\n"
            "  -m  model id; remembered per provider\n"
            "  -p  run one prompt headless and exit (default: REPL)\n"
            "  -V  print the version\n"
            "  --verbose   show each tool call's full arguments and result, not one line\n"
            "  --no-color  plain stderr; also when NO_COLOR is set or stderr is not a terminal\n"
            "  --permissions  when write, edit and shell run without asking:\n"
            "      auto       always, except write/edit outside the working directory (default)\n"
            "      ask        never: ask on the terminal each time\n"
            "      all        always\n"
            "      read-only  refuse them; read still runs\n", argv0);
    fputs(REPL_HELP, f);
    return rc;
}

static void on_sigint(int sig) {
    (void)sig;
    agent_interrupted = 1;
#ifdef HAVE_LIBEDIT
    if (at_prompt) {
        at_prompt = 0;
        siglongjmp(at_prompt_jmp, 1);
    }
#endif
}

/* The next input line in *line, trailing whitespace trimmed; NULL at EOF or Ctrl-C.
   A terminal gets line editing and history when built with libedit. */
static char *next_line(int edit, char **line, size_t *cap) {
    ssize_t n;
#ifdef HAVE_LIBEDIT
    if (edit) {
        if (sigsetjmp(at_prompt_jmp, 1)) { /* Ctrl-C at the prompt */
            fputc('\n', stderr);
            return NULL;
        }
        at_prompt = 1;
        char *l = readline("> ");
        at_prompt = 0;
        if (!l) return NULL; /* EOF */
        if (*l) add_history(l);
        free(*line);
        *line = l;
        *cap = strlen(l) + 1;
        n = (ssize_t)strlen(l);
    } else
#endif
    {
        if (edit) fputs("> ", stderr);
        if ((n = getline(line, cap, stdin)) < 0) return NULL;
    }
    while (n && strchr(" \t\r\n", (*line)[n - 1])) (*line)[--n] = 0;
    return *line;
}

static void repl(agent *a) {
    agent_note(AGENT_BOLD, "ant agent %s\n", AGENT_VERSION);
    agent_note(AGENT_DIM, "%s %s\n", a->prov->name, a->model);
    char *line = NULL;
    size_t cap = 0;
    int tty = isatty(STDIN_FILENO); /* no prompt or editing when input is piped */
#ifdef HAVE_LIBEDIT
    char *hist = tty ? agent_state_path("history", 1) : NULL;
    if (tty) {
        setlocale(LC_CTYPE, ""); /* libedit decodes and encodes input with it */
        rl_outstream = stderr; /* the prompt stays off stdout, like the other chatter */
        using_history();
        stifle_history(1000);
        if (hist) read_history(hist);
    }
#endif
    for (;;) {
        if (!next_line(tty, &line, &cap)) {
            if (!agent_interrupted) { /* EOF or read error */
                if (tty) fputc('\n', stderr); /* leave the prompt's line */
                break;
            }
            agent_interrupted = 0; /* Ctrl-C at the prompt: drop the line */
            clearerr(stdin);
#ifndef HAVE_LIBEDIT
            fputc('\n', stderr);
#else
            if (!tty) fputc('\n', stderr); /* next_line already ended libedit's line */
#endif
            continue;
        }
        if (!*line) continue;
        const char *arg;
        if (!strcmp(line, "/exit")) break;
        if (!strcmp(line, "/help")) {
            fputs(REPL_HELP, stderr);
            continue;
        }
        if (!strcmp(line, "/clear")) {
            agent_clear(a);
            agent_note(AGENT_DIM, "history cleared\n");
            continue;
        }
        if ((arg = agent_command(line, "/models"))) {
            agent_list_models(a, arg);
            continue;
        }
        if ((arg = agent_command(line, "/model"))) {
            if (*arg) agent_set_model(a, arg); /* history is kept */
            agent_note(AGENT_DIM, "%s %s\n", a->prov->name, a->model);
            continue;
        }
        agent_ask(a, line);
    }
    free(line);
#ifdef HAVE_LIBEDIT
    if (hist && write_history(hist) == 0) chmod(hist, 0600); /* prompts may hold secrets */
    free(hist);
#endif
}

int main(int argc, char **argv) {
    const char *prompt = NULL, *pname = NULL, *model = NULL;
    static const char *const modes[] = {[AGENT_AUTO] = "auto", [AGENT_ASK] = "ask",
                                        [AGENT_ALL] = "all", [AGENT_READ_ONLY] = "read-only"};
    static const struct option longopts[] = {{"permissions", required_argument, NULL, 'W'},
                                             {"verbose", no_argument, NULL, 'v'},
                                             {"no-color", no_argument, NULL, 'C'},
                                             {NULL, 0, NULL, 0}};
    agent_permissions perms = AGENT_AUTO;
    int opt, verbose = 0, color = 1;
    const char *no_color = getenv("NO_COLOR"); /* https://no-color.org */
    if (no_color && *no_color) color = 0;
    while ((opt = getopt_long(argc, argv, "p:P:m:hV", longopts, NULL)) != -1) {
        if (opt == 'V') {
            printf("ant agent %s\n", AGENT_VERSION);
            return 0;
        }
        if (opt == 'v') verbose = 1;
        else if (opt == 'C') color = 0;
        else if (opt == 'W') {
            size_t i = 0;
            while (i < sizeof modes / sizeof *modes && strcmp(optarg, modes[i])) i++;
            if (i == sizeof modes / sizeof *modes) {
                agent_note(AGENT_ERROR, "error: unknown permissions mode %s\n", optarg);
                return usage(argv[0], 2);
            }
            perms = (agent_permissions)i;
        }
        else if (opt == 'p') prompt = optarg;
        else if (opt == 'P') pname = optarg;
        else if (opt == 'm') model = optarg;
        else return usage(argv[0], opt == 'h' ? 0 : 2);
    }
    if (optind < argc) return usage(argv[0], 2);
    agent_color = color && isatty(STDERR_FILENO);
    if (prompt && agent_provider_named(prompt)) { /* -p and -P differ only by case */
        agent_note(AGENT_ERROR, "error: -p takes a prompt; did you mean -P %s?\n", prompt);
        return 2;
    }

    const agent_provider *p;
    if (pname) {
        if (!(p = agent_provider_named(pname))) {
            agent_note(AGENT_ERROR, "error: unknown provider %s\n", pname);
            return usage(argv[0], 2);
        }
    } else if (!(p = agent_provider_default())) {
        char keys[256] = "";
        for (size_t i = 0; i < AGENT_NPROVIDERS; i++)
            if (!AGENT_PROVIDERS[i].key_optional)
                snprintf(keys + strlen(keys), sizeof keys - strlen(keys), " %s",
                         AGENT_PROVIDERS[i].key_env);
        agent_note(AGENT_ERROR, "error: no provider: set%s or pass -P local\n", keys);
        return 1;
    }

    agent a;
    if (agent_init(&a, p, model, perms) < 0) return 1;
    a.verbose = verbose;
    a.save_provider = pname != NULL;
    /* No SA_RESTART: blocking reads return EINTR, so Ctrl-C takes effect at once. */
    struct sigaction sa = {0};
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    int rc = 0;
    if (prompt) {
        rc = agent_ask(&a, prompt) < 0;
        if (agent_interrupted) rc = 130; /* 128 + SIGINT, as a shell would report */
    } else {
        repl(&a);
        agent_report_session(&a);
    }
    agent_free(&a);
    return rc;
}
