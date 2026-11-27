/* myra-ui: a CUI desktop front end for myralib. Options mirror myra's; words after
   them are sent as the first message. The window is libmyra-ui (app.h). */
#include "app.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int usage(const char *argv0, int rc) {
    fprintf(rc ? stderr : stdout,
            "usage: %s [-P provider] [-m model] [--permissions MODE] [-C DIR] [PROMPT...]\n"
            "  -P NAME            provider, as for myra (remembered)\n"
            "  -m MODEL           model id (remembered after a reply)\n"
            "  --permissions MODE ask (default), auto, all or read-only\n"
            "  -C DIR             run in DIR\n"
            "Provider and model are chosen as myra chooses them: see myra's README.\n",
            argv0);
    return rc;
}

static int parse_permissions(const char *s, myra_permissions *p) {
    static const struct { const char *name; myra_permissions p; } names[] = {
        {"ask", MYRA_ASK}, {"auto", MYRA_AUTO}, {"all", MYRA_ALL}, {"read-only", MYRA_READ_ONLY}};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (!strcmp(s, names[i].name)) return *p = names[i].p, 0;
    return -1;
}

int main(int argc, char **argv) {
    static app s;
    const char *pname = NULL, *model = NULL, *dir = NULL;
    s.permissions = MYRA_ASK; /* a window has no scrollback to review changes in afterwards */
    static const struct option longs[] = {{"permissions", required_argument, NULL, 'p'},
                                          {"cwd", required_argument, NULL, 'C'},
                                          {"help", no_argument, NULL, 'h'},
                                          {0}};
    int opt;
    while ((opt = getopt_long(argc, argv, "+P:m:C:h", longs, NULL)) != -1) {
        if (opt == 'P') pname = optarg;
        else if (opt == 'm') model = optarg;
        else if (opt == 'C') dir = optarg;
        else if (opt == 'p' && parse_permissions(optarg, &s.permissions) == 0) {}
        else if (opt == 'h') return usage(argv[0], 0);
        else return usage(argv[0], 2);
    }
    for (int i = optind; i < argc; i++) { /* the words, joined by spaces, as myra does */
        size_t n = strlen(s.draft);
        snprintf(s.draft + n, sizeof s.draft - n, "%s%s", n ? " " : "", argv[i]);
    }

    if (dir && chdir(dir) < 0) {
        fprintf(stderr, "myra-ui: cannot change to %s: %s\n", dir, strerror(errno));
        return 2;
    }
    if (!getcwd(s.cwd, sizeof s.cwd)) s.cwd[0] = 0;
    snprintf(s.browse, sizeof s.browse, "%s", s.cwd);
    const myra_provider *p = pname ? myra_provider_named(pname) : myra_provider_default();
    if (!p) {
        fprintf(stderr, pname ? "myra-ui: unknown provider %s\n"
                              : "myra-ui: no provider: set OPENROUTER_API_KEY, or pass -P local\n", pname);
        return pname ? 2 : 1;
    }
    if (myra_init(&s.agent, p, model, s.permissions) < 0) { /* no note hook yet: say why */
        fprintf(stderr, "%s\n", myra_last_error); /* "error: ..." */
        return 1;
    }
    myra_add_coding_tools(&s.agent);
    s.agent.save_provider = pname != NULL;

    return ui_run(&s);
}
