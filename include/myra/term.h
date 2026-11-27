/* term.h - libmyra-term: myra on a terminal. Through myralib's hooks it renders replies
   on stdout, prints notes on stderr and asks permission on /dev/tty. The CLI uses it;
   a front end without a terminal does not. */
#ifndef MYRA_TERM_H
#define MYRA_TERM_H

#include "myra.h"

#include <stdio.h>

typedef struct {
    int cr; /* a held '\r', dropped if '\n' follows */
} myra_esc;
/* s with C0 controls, DEL and C1 as ^X, ^? and M-^X; malloc'd. nl: '\n', '\t' and '\r'
   too. With state e, a trailing '\r' is held for the next call, and s NULL flushes it;
   with e NULL, s is complete. s must hold whole UTF-8 characters. */
char *myra_escape(myra_esc *e, const char *s, int nl);

/* Streaming markdown renderer: headings, bold, italic, inline code, fenced code,
   bullets, quotes and rules. Zero it before first use. */
typedef struct {
    char line[256];      /* the start of a line, held until its block kind is known */
    size_t n;
    int mid;             /* past the start of the line */
    int block;           /* the current line's kind */
    char fence;          /* inside a code block opened by fence_n of this character */
    size_t fence_n;
    int em, code;        /* open emphasis (bold, italic) and inline code */
    char pend, prev;     /* pend: a held '*', '_' or '\\' run; prev: last character */
    int pend_n;
    int style;           /* the SGR styles set on out */
} myra_md;
/* Render s, holding back what the next characters decide. */
void myra_md_feed(myra_md *m, const char *s, FILE *out);
/* Flush what is held, end a started line, and zero m. */
void myra_md_end(myra_md *m, FILE *out);

/* One terminal's settings and reply state. Zero it, set the flags, then install it. */
typedef struct {
    int color;      /* color notes on stderr with ANSI codes */
    int markdown;   /* render replies as markdown with ANSI styles */
    /* Show control characters in caret notation on stdout and on stderr, so model and
       tool output cannot drive the terminal. The permission prompt always escapes. */
    int escape_out, escape_err;
    int verbose;    /* tool notes in full; else one line, cut to stderr's width */
    myra_esc esc;   /* the reply being printed */
    myra_md md;
} myra_term;

/* Print notes on stderr through t. One terminal per process: the note hook is global,
   so t must outlive every agent. Call it first: notes come before any agent exists. */
void myra_term_notes(myra_term *t);
/* Print a's replies on stdout and ask a's permission questions on /dev/tty, through t.
   Sets a->io's text, text_end and allow; call it after myra_init. */
void myra_term_attach(myra_term *t, myra_agent *a);
/* Print the provider's model ids that contain filter, ignoring case; the current one
   is starred. Fetches. */
void myra_term_list_models(myra_term *t, myra_agent *a, const char *filter);

/* If line is "name" or "name args", return args with leading spaces skipped; else NULL. */
const char *myra_command(const char *line, const char *name);

#endif
