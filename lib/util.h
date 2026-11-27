/* util.h - helpers shared by myralib's sources. static inline: none is exported. */
#ifndef MYRA_UTIL_H
#define MYRA_UTIL_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- growable buffer ---- */

typedef struct { char *p; size_t n, cap; } buf;

static inline void buf_add(buf *b, const char *s, size_t n) {
    if (n >= SIZE_MAX / 2 - b->n) { fputs("buf_add: size overflow\n", stderr); exit(1); }
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = realloc(b->p, b->cap);
        if (!b->p) { perror("realloc"); exit(1); }
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

/* Appends a string literal; "" rejects a pointer, whose sizeof is not its length. */
#define buf_lit(b, s) buf_add((b), "" s, sizeof(s) - 1)

static inline char *xstrdup(const char *s) { buf b = {0}; buf_add(&b, s, strlen(s)); return b.p; }

/* ---- files and time ---- */

static inline int slurp(const char *path, buf *b) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char chunk[65536];
    size_t n;
    do { /* a short read means EOF or an error; reading on is undefined after an error */
        n = fread(chunk, 1, sizeof chunk, f);
        buf_add(b, chunk, n);
    } while (n == sizeof chunk);
    int err = ferror(f);
    fclose(f);
    if (!b->p) buf_lit(b, "");
    return err ? -1 : 0;
}

static inline int write_all(int fd, const char *s, size_t n) {
    while (n) {
        ssize_t k = write(fd, s, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        s += k, n -= (size_t)k;
    }
    return 0;
}

static inline double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

#endif
