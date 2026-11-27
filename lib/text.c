/* text.c - myralib: strings, UTF-8 and the output sink shared by the other sources. */
#include "internal.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

char *myra__fmt(const char *f, const char *a) {
    size_t n = strlen(f) + strlen(a) + 1;
    char *s = malloc(n);
    if (!s) { perror("malloc"); exit(1); }
    snprintf(s, n, f, a);
    return s;
}

/* msg + ": " + why; frees both. */
char *myra__with_why(char *msg, char *why) {
    buf b = {0};
    buf_add(&b, msg, strlen(msg));
    if (why && *why) {
        buf_lit(&b, ": ");
        buf_add(&b, why, strlen(why));
    }
    free(msg);
    free(why);
    return b.p;
}

/* path made absolute against the working directory, malloc'd; ACP requires absolute paths. */
char *myra__absolute(const char *path) {
    char cwd[PATH_MAX];
    if (*path == '/' || !getcwd(cwd, sizeof cwd)) return xstrdup(path);
    buf b = {0};
    buf_add(&b, cwd, strlen(cwd));
    buf_lit(&b, "/");
    buf_add(&b, path, strlen(path));
    return b.p;
}

const char *myra__get_str(cJSON *o, const char *key) {
    return cJSON_GetStringValue(cJSON_GetObjectItem(o, key));
}

double myra__get_num(cJSON *o, const char *key) { /* 0 when absent */
    cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

/* Length of the valid UTF-8 sequence at p (at most n bytes), or 0 if invalid.
   NUL counts as invalid: results become C strings, where it would end them. */
static size_t utf8_len(const unsigned char *p, size_t n) {
    unsigned char c = p[0];
    if (c >= 0x01 && c <= 0x7F) return 1;
    size_t len;
    unsigned char lo = 0x80, hi = 0xBF; /* allowed range of the second byte */
    if (c >= 0xC2 && c <= 0xDF) len = 2;
    else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        if (c == 0xE0) lo = 0xA0;      /* no overlong forms */
        else if (c == 0xED) hi = 0x9F; /* no UTF-16 surrogates */
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        if (c == 0xF0) lo = 0x90;
        else if (c == 0xF4) hi = 0x8F; /* nothing above U+10FFFF */
    } else return 0;
    if (n < len || p[1] < lo || p[1] > hi) return 0;
    for (size_t i = 2; i < len; i++)
        if ((p[i] & 0xC0) != 0x80) return 0;
    return len;
}

int myra__valid_utf8(const buf *b) {
    size_t i = 0, k;
    while (i < b->n && (k = utf8_len((const unsigned char *)b->p + i, b->n - i))) i += k;
    return i == b->n;
}

/* Replace each invalid byte with U+FFFD, so the JSON request stays valid. */
static void sanitize_utf8(buf *b) {
    const unsigned char *p = (const unsigned char *)b->p;
    size_t i = 0, k;
    while (i < b->n && (k = utf8_len(p + i, b->n - i))) i += k;
    if (i == b->n) return; /* the common case: already valid */
    buf out = {0};
    buf_add(&out, b->p, i);
    while (i < b->n) {
        if ((k = utf8_len(p + i, b->n - i))) buf_add(&out, b->p + i, k), i += k;
        else buf_lit(&out, "\xEF\xBF\xBD"), i++;
    }
    free(b->p);
    *b = out;
}

char *myra_utf8(const char *s) {
    buf b = {0};
    buf_add(&b, s, strlen(s));
    sanitize_utf8(&b);
    return b.p;
}

static int is_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

void myra__sink_init(sink *s, size_t cap) {
    memset(s, 0, sizeof *s);
    s->cap = cap;
    s->head_max = cap / 5;
    s->tail_cap = cap - s->head_max;
    s->tail = malloc(s->tail_cap);
    if (!s->tail) { perror("malloc"); exit(1); }
}

void myra__sink_free(sink *s) {
    free(s->head.p);
    free(s->tail);
}

void myra__sink_add(sink *s, const char *p, size_t n) {
    s->total += n;
    if (s->head.n < s->head_max) { /* the head is kept whole */
        size_t k = s->head_max - s->head.n;
        if (k > n) k = n;
        buf_add(&s->head, p, k);
        p += k, n -= k;
    }
    if (n >= s->tail_cap) { /* only the last tail_cap bytes can survive */
        memcpy(s->tail, p + n - s->tail_cap, s->tail_cap);
        s->tail_len = s->tail_cap;
        s->tail_at = 0;
        return;
    }
    for (size_t i = 0; i < n; i++) { /* wrap into the ring */
        size_t at = (s->tail_at + s->tail_len) % s->tail_cap;
        s->tail[at] = p[i];
        if (s->tail_len < s->tail_cap) s->tail_len++;
        else s->tail_at = (s->tail_at + 1) % s->tail_cap;
    }
}

/* The collected output as one malloc'd string: head, an omitted-bytes note when
   anything was dropped, then tail. Both parts are cut on character boundaries and
   sanitized to valid UTF-8. */
char *myra__sink_take(sink *s) {
    buf tail = {0};
    for (size_t i = 0; i < s->tail_len; i++) {
        char c = s->tail[(s->tail_at + i) % s->tail_cap];
        buf_add(&tail, &c, 1);
    }
    size_t dropped = s->total - s->head.n - s->tail_len;
    buf head = s->head;
    if (dropped) { /* keep whole characters on both sides of the gap */
        size_t keep = head.n;
        for (size_t i = 1; i <= 3 && i <= head.n; i++) { /* back to the last lead byte */
            unsigned char c = (unsigned char)head.p[head.n - i];
            if (is_cont((char)c)) continue;
            size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            if (need > i) keep = head.n - i; /* its sequence runs into the gap */
            break;
        }
        head.n = keep;
        size_t skip = 0;
        while (skip < tail.n && is_cont(tail.p[skip])) skip++;
        if (skip) memmove(tail.p, tail.p + skip, tail.n - skip); /* tail.p is NULL when empty */
        tail.n -= skip;
        dropped += (s->head.n - head.n) + skip;
    }
    sanitize_utf8(&head);
    sanitize_utf8(&tail);
    buf out = {0};
    buf_add(&out, head.p ? head.p : "", head.n);
    if (dropped) {
        char note[64];
        snprintf(note, sizeof note, "\n[... %zu bytes omitted ...]\n", dropped);
        buf_add(&out, note, strlen(note));
    }
    buf_add(&out, tail.p ? tail.p : "", tail.n);
    free(tail.p);
    s->head = head; /* sink_free releases whatever sanitize_utf8 left */
    return out.p ? out.p : xstrdup("");
}

