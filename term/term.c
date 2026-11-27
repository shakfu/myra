/* term.c - libmyra-term: myra on a terminal (myra/term.h). Escaping, markdown, notes
   on stderr, replies on stdout and the /dev/tty permission prompt, as myralib hooks. */
#include "myra/term.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ---- control characters ---- */

static void caret(buf *b, const char *prefix, unsigned char c) {
    char s[2] = {'^', c == 0x7f ? '?' : (char)(c + '@')};
    buf_add(b, prefix, strlen(prefix));
    buf_add(b, s, 2);
}

char *myra_escape(myra_esc *e, const char *s, int nl) {
    myra_esc local = {0}, *st = e ? e : &local;
    buf b = {0};
    buf_add(&b, "", 0);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        unsigned char c = *p;
        if (st->cr) {
            st->cr = 0;
            if (c == '\n') { buf_lit(&b, "\n"); continue; } /* CRLF */
            buf_lit(&b, "^M");
        }
        if (c == '\r' && !nl) st->cr = 1;
        else if (c == 0xc2 && p[1] >= 0x80 && p[1] <= 0x9f) caret(&b, "M-", *++p - 0x80); /* C1 */
        else if ((c < 0x20 && (nl || (c != '\n' && c != '\t'))) || c == 0x7f) caret(&b, "", c);
        else buf_add(&b, (const char *)p, 1);
    }
    if (!e || !s) { /* nothing follows: flush what is held */
        if (st->cr) buf_lit(&b, "^M");
        st->cr = 0;
    }
    return b.p;
}

/* ---- markdown ---- */

/* Line kinds. A fence or rule is known only at the line's end. */
enum { MD_PLAIN, MD_UNDECIDED, MD_HEADING, MD_QUOTE, MD_BULLET, MD_FENCE, MD_CLOSE, MD_HR, MD_CODE };
/* Styles; SGR codes in md_style. */
enum { MD_H = 1, MD_B = 2, MD_I = 4, MD_C = 8, MD_D = 16 };

/* ASCII only, so the result does not depend on the locale. */
static int md_space(char c) { return !c || c == ' ' || c == '\t' || c == '\n'; }
static int md_word(char c) { return (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z'); }

static void md_style(myra_md *m, int style, FILE *out) {
    if (style == m->style) return;
    char s[24] = "\033[", *p = s + 2;
    if (m->style) p = stpcpy(p, "0;");
    if (style & (MD_H | MD_B)) p = stpcpy(p, "1;");
    if (style & MD_D) p = stpcpy(p, "2;");
    if (style & MD_I) p = stpcpy(p, "3;");
    if (style & MD_H) p = stpcpy(p, "4;");
    if (style & MD_C) p = stpcpy(p, "32;");
    p[-1] = 'm';
    fputs(s, out);
    m->style = style;
}

static void md_put(myra_md *m, char c, int style, FILE *out) {
    md_style(m, style, out);
    fputc(c, out);
    m->prev = c;
}

static void md_emit(myra_md *m, char c, FILE *out) {
    md_put(m, c, (m->block == MD_HEADING ? MD_H : 0) | m->em | (m->code ? MD_C : 0), out);
}

/* A held run of '*' or '_' opens or closes emphasis, by CommonMark's flanking rules
   less punctuation; else it is literal. next is 0 at the end of the line. */
static void md_resolve(myra_md *m, char next, FILE *out) {
    char c = m->pend;
    int n = m->pend_n;
    m->pend_n = 0;
    if (c == '\\') { md_emit(m, c, out); return; }
    int left = !md_space(next), right = !md_space(m->prev);
    if (c == '_') { /* not inside a word: snake_case stays */
        left = left && !md_word(m->prev);
        right = right && !md_word(next);
    }
    int want = (n >= 2 ? MD_B : 0) | (n != 2 ? MD_I : 0), on = m->em & want;
    if (n <= 3 && ((on == want && right) || (!on && left))) {
        m->em ^= want;
        m->prev = c;
    } else {
        while (n--) md_emit(m, c, out);
    }
}

static void md_inline(myra_md *m, char c, FILE *out) {
    if (m->pend_n) {
        if (m->pend == '\\') {
            m->pend_n = 0;
            if (c > ' ' && c < 127 && !md_word(c)) { md_emit(m, c, out); return; } /* escaped */
            md_emit(m, '\\', out);
        } else if (c == m->pend) {
            m->pend_n++;
            return;
        } else {
            md_resolve(m, c, out);
        }
    }
    if (m->code) {
        if (c == '`') m->code = 0, m->prev = c;
        else md_emit(m, c, out);
    } else if (c == '\\' || c == '*' || c == '_') {
        m->pend = c;
        m->pend_n = 1;
    } else if (c == '`') {
        m->code = 1;
        m->prev = c;
    } else {
        md_emit(m, c, out);
    }
}

/* The kind of the held line start; *skip is how many bytes of marker to drop. */
static int md_classify(const myra_md *m, int complete, size_t *skip) {
    const char *s = m->line;
    size_t n = m->n, i = 0, k;
    *skip = 0;
    while (i < n && s[i] == ' ') i++;
    if (m->fence) { /* only a closing fence ends the block */
        for (k = i; k < n && s[k] == m->fence; k++) {}
        size_t e = k;
        while (e < n && (s[e] == ' ' || s[e] == '\t')) e++;
        if (i > 3 || e < n || (e > k && k - i < m->fence_n)) return MD_CODE;
        return !complete ? MD_UNDECIDED : k - i >= m->fence_n ? MD_CLOSE : MD_CODE;
    }
    if (i == n) return complete ? MD_PLAIN : MD_UNDECIDED;
    const char *r = s + i;
    size_t rn = n - i;
    char c = r[0];
    for (k = 0; k < rn && r[k] == c; k++) {}
    int all = k == rn; /* so far the line is one run of c */
    if (c == '#' && i <= 3 && k <= 6) {
        if (all) return complete ? MD_PLAIN : MD_UNDECIDED;
        if (r[k] != ' ') return MD_PLAIN;
        *skip = i + k + 1;
        return MD_HEADING;
    }
    if ((c == '`' || c == '~') && i <= 3 && (k >= 3 || all)) {
        if (!complete) return MD_UNDECIDED;
        if (k < 3 || (c == '`' && memchr(r + k, '`', rn - k))) return MD_PLAIN; /* inline code */
        return MD_FENCE;
    }
    if (c == '>') {
        if (rn == 1 && !complete) return MD_UNDECIDED;
        *skip = i + 1 + (rn > 1 && r[1] == ' ');
        return MD_QUOTE;
    }
    if ((c == '-' || c == '*' || c == '_') && all) return !complete ? MD_UNDECIDED : k >= 3 ? MD_HR : MD_PLAIN;
    if (c == '+' && all) return complete ? MD_PLAIN : MD_UNDECIDED;
    if ((c == '-' || c == '*' || c == '+') && k == 1 && r[1] == ' ') {
        *skip = i + 2;
        return MD_BULLET;
    }
    return MD_PLAIN;
}

/* Write the held line start as kind, and render the rest of it. */
static void md_apply(myra_md *m, int kind, size_t skip, FILE *out) {
    size_t i = 0, j;
    m->mid = 1;
    m->block = kind;
    while (i < m->n && m->line[i] == ' ') i++;
    if (kind == MD_FENCE) { /* fence lines are not shown */
        m->fence = m->line[i];
        for (m->fence_n = 0; i + m->fence_n < m->n && m->line[i + m->fence_n] == m->fence; m->fence_n++) {}
    } else if (kind == MD_CLOSE) {
        m->fence = 0;
    } else if (kind == MD_HR || kind == MD_CODE) {
        for (j = 0; j < m->n; j++) md_put(m, m->line[j], kind == MD_HR ? MD_D : MD_C, out);
    } else {
        if (kind == MD_QUOTE) {
            md_put(m, '|', MD_D, out);
            md_put(m, ' ', MD_D, out);
        } else if (kind == MD_BULLET) {
            for (j = 0; j < i; j++) md_put(m, ' ', 0, out);
            md_put(m, '-', 0, out);
            md_put(m, ' ', 0, out);
        }
        m->prev = 0;
        for (j = skip; j < m->n; j++) md_inline(m, m->line[j], out);
    }
    m->n = 0;
}

static void md_eol(myra_md *m, FILE *out) {
    if (!m->mid) {
        size_t skip;
        int kind = md_classify(m, 1, &skip);
        md_apply(m, kind, skip, out);
    }
    if (m->pend_n) md_resolve(m, 0, out);
    md_style(m, 0, out);
    if (m->block != MD_FENCE && m->block != MD_CLOSE) fputc('\n', out);
    m->mid = m->block = m->em = m->code = 0;
    m->prev = 0;
}

void myra_md_feed(myra_md *m, const char *s, FILE *out) {
    for (; *s; s++) {
        if (*s == '\n') {
            md_eol(m, out);
        } else if (m->mid) {
            if (m->block == MD_CODE) md_put(m, *s, MD_C, out);
            else md_inline(m, *s, out);
        } else {
            size_t skip = 0;
            m->line[m->n++] = *s;
            int kind = m->n < sizeof m->line ? md_classify(m, 0, &skip) : m->fence ? MD_CODE : MD_PLAIN;
            if (kind != MD_UNDECIDED) md_apply(m, kind, skip, out);
        }
    }
}

void myra_md_end(myra_md *m, FILE *out) {
    if (m->mid || m->n) myra_md_feed(m, "\n", out);
    *m = (myra_md){0};
}

/* ---- notes ---- */

static myra_term *noted; /* myra_note_hook has no user data */

/* The [tool] line in s, cut to stderr's width (100 when it has none) with " ..."
   marking the cut; malloc'd, or NULL if it fits. */
static char *fit_tool_line(const char *s) {
    struct winsize ws;
    size_t width = isatty(STDERR_FILENO) && !ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) && ws.ws_col
                       ? ws.ws_col : 100;
    size_t n = strcspn(s, "\n");
    int cut = n >= 4 && !memcmp(s + n - 4, " ...", 4); /* already cut to its first line */
    if (cut) n -= 4;
    size_t room = width > 24 ? width - 4 : 20; /* leave room for " ..." */
    if (n <= room) return NULL;
    n = room;
    while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--; /* whole characters */
    buf b = {0};
    buf_add(&b, s, n);
    buf_lit(&b, " ...\n");
    return b.p;
}

static void on_note(myra_style style, const char *s) {
    static const char *const codes[] = {[MYRA_PLAIN] = "", [MYRA_TOOL] = "\033[36m",
                                        [MYRA_ERROR] = "\033[31m", [MYRA_WARN] = "\033[33m",
                                        [MYRA_DIM] = "\033[2m", [MYRA_BOLD] = "\033[1m"};
    const myra_term *t = noted;
    char *e = t->escape_err ? myra_escape(NULL, s, 0) : NULL; /* measured as shown */
    const char *text = e ? e : s;
    char *fit = style == MYRA_TOOL && !t->verbose ? fit_tool_line(text) : NULL;
    int on = t->color && style != MYRA_PLAIN;
    if (on) fputs(codes[style], stderr);
    fputs(fit ? fit : text, stderr);
    if (on) fputs("\033[0m", stderr);
    free(fit);
    free(e);
}

void myra_term_notes(myra_term *t) {
    noted = t;
    myra_note_hook = on_note;
}

/* ---- replies and permission ---- */

/* Write reply text to stdout, escaped and rendered as t says; s NULL flushes what is held. */
static void show(myra_term *t, const char *s) {
    char *v = t->escape_out ? myra_escape(&t->esc, s, 0) : NULL;
    const char *text = v ? v : s ? s : "";
    if (t->markdown) myra_md_feed(&t->md, text, stdout);
    else fputs(text, stdout);
    free(v);
}

static void on_text(void *ud, const char *s) {
    show(ud, s);
    fflush(stdout);
}

static void on_text_end(void *ud) {
    myra_term *t = ud;
    show(t, NULL);
    if (t->markdown) myra_md_end(&t->md, stdout);
    else putchar('\n');
    fflush(stdout);
    t->esc = (myra_esc){0};
}

/* Ask on the controlling terminal, so it works in headless mode and the REPL alike. */
static int on_allow(void *ud, const char *name, const char *detail, int outside) {
    (void)ud;
    FILE *tty = fopen("/dev/tty", "r+");
    if (!tty) return -1;
    char *shown = myra_escape(NULL, detail, 1); /* else \r and ESC could hide the real command */
    fprintf(tty, "allow %s%s: %s ? [y/N] ", name, outside ? " outside the working directory" : "",
            shown);
    free(shown);
    fflush(tty);
    char line[16] = {0};
    int ok = fgets(line, sizeof line, tty) && (line[0] == 'y' || line[0] == 'Y');
    fclose(tty);
    return ok;
}

void myra_term_attach(myra_term *t, myra_agent *a) {
    a->io.ud = t;
    a->io.text = on_text;
    a->io.text_end = on_text_end;
    a->io.allow = on_allow;
}

/* Case-insensitive strstr; strcasestr needs _GNU_SOURCE on glibc. */
static int contains_ci(const char *s, const char *sub) {
    size_t n = strlen(sub);
    for (; *s; s++)
        if (!strncasecmp(s, sub, n)) return 1;
    return !n;
}

void myra_term_list_models(myra_term *t, myra_agent *a, const char *filter) {
    char **ids = myra_model_ids(a);
    for (char **p = ids; p && *p; p++)
        if (contains_ci(*p, filter)) {
            char *id = t->escape_out ? myra_escape(NULL, *p, 1) : NULL; /* from the server */
            printf("%s %s\n", strcmp(*p, a->model) ? " " : "*", id ? id : *p);
            free(id);
        }
    fflush(stdout);
    myra_free_model_ids(ids);
}

const char *myra_command(const char *line, const char *name) {
    size_t n = strlen(name);
    if (strncmp(line, name, n) || (line[n] && line[n] != ' ')) return NULL;
    for (line += n; *line == ' '; line++);
    return line;
}
