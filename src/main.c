/* main.c - the agent CLI: options, provider choice, headless mode and the REPL. */
#include "agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int usage(const char *argv0, int rc) {
    fprintf(rc ? stderr : stdout,
            "usage: %s [-y] [-P provider] [-m model] [-p prompt]\n"
            "  -P  openrouter or local; remembered. Default: the remembered one if usable,\n"
            "      else the first cloud provider whose key is set\n"
            "  -m  model id; remembered per provider\n"
            "  -p  run one prompt headless and exit (default: REPL)\n"
            "  -y  run write/edit/shell without asking\n"
            "REPL commands:\n"
            "  /model [id]        show or switch the model\n"
            "  /models [filter]   list the provider's models whose id contains filter\n"
            "  /exit              quit\n", argv0);
    return rc;
}

static void repl(agent *a) {
    fprintf(stderr, "%s %s\n", a->prov->name, a->model);
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int tty = isatty(STDIN_FILENO); /* no prompt when input is piped */
    for (;;) {
        if (tty) fputs("> ", stderr);
        if ((n = getline(&line, &cap, stdin)) < 0) break;
        while (n && strchr(" \t\r\n", line[n - 1])) line[--n] = 0;
        if (!n) continue;
        const char *arg;
        if (!strcmp(line, "/exit")) break;
        if ((arg = agent_command(line, "/models"))) {
            agent_list_models(a, arg);
            continue;
        }
        if ((arg = agent_command(line, "/model"))) {
            if (*arg) agent_set_model(a, arg); /* history is kept */
            fprintf(stderr, "%s %s\n", a->prov->name, a->model);
            continue;
        }
        agent_ask(a, line);
    }
    free(line);
}

int main(int argc, char **argv) {
    const char *prompt = NULL, *pname = NULL, *model = NULL;
    int opt, auto_yes = 0;
    while ((opt = getopt(argc, argv, "yp:P:m:h")) != -1) {
        if (opt == 'y') auto_yes = 1;
        else if (opt == 'p') prompt = optarg;
        else if (opt == 'P') pname = optarg;
        else if (opt == 'm') model = optarg;
        else return usage(argv[0], opt == 'h' ? 0 : 2);
    }
    if (optind < argc) return usage(argv[0], 2);

    const agent_provider *p;
    if (pname) {
        if (!(p = agent_provider_named(pname))) {
            fprintf(stderr, "error: unknown provider %s\n", pname);
            return usage(argv[0], 2);
        }
    } else if (!(p = agent_provider_default())) {
        fprintf(stderr, "error: no provider: set");
        for (size_t i = 0; i < AGENT_NPROVIDERS; i++)
            if (!AGENT_PROVIDERS[i].key_optional) fprintf(stderr, " %s", AGENT_PROVIDERS[i].key_env);
        fprintf(stderr, " or pass -P local\n");
        return 1;
    }

    agent a;
    if (agent_init(&a, p, model, auto_yes) < 0) return 1;
    a.save_provider = pname != NULL;
    int rc = 0;
    if (prompt) rc = agent_ask(&a, prompt) < 0;
    else repl(&a);
    agent_free(&a);
    return rc;
}
