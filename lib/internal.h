/* internal.h - declarations shared by myralib's sources; not installed. Shared
   functions are named myra__*, since a static archive exports every non-static name. */
#ifndef MYRA_INTERNAL_H
#define MYRA_INTERNAL_H

#include "myra/mcp.h"
#include "myra/myra.h"
#include "util.h"

/* text.c */
char *myra__fmt(const char *f, const char *a); /* f with %s as a, malloc'd */
char *myra__with_why(char *msg, char *why);
char *myra__absolute(const char *path);
const char *myra__get_str(cJSON *o, const char *key);
double myra__get_num(cJSON *o, const char *key);

/* Collects output while keeping only what will be shown: the first fifth of cap and,
   in a ring, the last four fifths. Memory therefore stays at cap however much arrives. */
typedef struct {
    size_t cap, head_max, total;
    buf head;
    char *tail;
    size_t tail_cap, tail_len, tail_at; /* ring: tail_len bytes starting at tail_at */
} sink;

void myra__sink_init(sink *s, size_t cap);
void myra__sink_free(sink *s);
void myra__sink_add(sink *s, const char *p, size_t n);
char *myra__sink_take(sink *s);
int myra__valid_utf8(const buf *b);

/* tools.c */
int myra__spit(const char *path, const char *s, size_t n, int mode_set);

/* agent.c */
struct myra_tool_entry {
    myra_tool tool;
    cJSON *schema; /* tool.parameters, parsed */
};
int myra__outside_cwd(const char *path);
const char *myra__str_arg(cJSON *input, const char *key);
char *myra__capped(myra_agent *a, char *text);

/* api.c */
cJSON *myra__call_api(myra_agent *a);

#endif
