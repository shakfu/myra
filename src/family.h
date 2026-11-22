/* family.h - recursive agents: -r, its subcommands, and a member's hooks.
   Design: docs/dev/recursion.md. */
#ifndef MYRA_FAMILY_H
#define MYRA_FAMILY_H

#include "myra.h"

/* Whether argv[1] is -r and argv[2] names a subcommand. */
int myra_family_is_command(int argc, char **argv);
/* myra -r spawn|wait|agents|shutdown ...: argv[0] is the subcommand. The exit status. */
int myra_family_command(int argc, char **argv);

/* Found a family, or join as the child MYRA_ID names; sets a's family hooks. perms is
   the --permissions name. The root needs a budget of tokens, and may add one in dollars
   (0: none); a child takes neither, and must be headless. 1 for a child, 0 for the root,
   -1 with an error note. */
int myra_family_join(myra_agent *a, const char *perms, int headless, double budget,
                     double budget_cost);
/* Leave the family. answer: a child's final answer; ignored for the root, which removes
   the family's files. */
void myra_family_leave(const char *answer);

#endif
