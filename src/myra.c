/* myra.c - myralib: providers, the four tools, and the chat-completions tool loop. */
#include "myra.h"

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
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RETRIES 4

/* OpenAI chat-completions tool definitions. */
static const char *TOOLS_JSON =
    "[{\"type\":\"function\",\"function\":{\"name\":\"read\","
    "\"description\":\"Read a text file and return its contents.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"],\"additionalProperties\":false}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"write\","
    "\"description\":\"Create or overwrite a file with the given content.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
    "\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"],"
    "\"additionalProperties\":false}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"edit\","
    "\"description\":\"Replace old_string with new_string in a file. "
    "old_string must occur exactly once.\",\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\"},\"old_string\":{\"type\":\"string\"},"
    "\"new_string\":{\"type\":\"string\"}},\"required\":[\"path\",\"old_string\",\"new_string\"],"
    "\"additionalProperties\":false}}},"
    "{\"type\":\"function\",\"function\":{\"name\":\"shell\","
    "\"description\":\"Run a command with /bin/sh in the working directory. "
    "Returns combined stdout and stderr and the exit status. stdin is /dev/null. "
    "The command and its children are killed after timeout seconds (default 120, max 600). "
    "Background jobs must redirect their output, or the call waits for them.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},"
    "\"timeout\":{\"type\":\"integer\"}},"
    "\"required\":[\"command\"],\"additionalProperties\":false}}}]";

volatile sig_atomic_t myra_interrupted;
int myra_color;

void myra_note(myra_style style, const char *fmt, ...) {
    static const char *const codes[] = {[MYRA_PLAIN] = "", [MYRA_TOOL] = "\033[36m",
                                        [MYRA_ERROR] = "\033[31m", [MYRA_WARN] = "\033[33m",
                                        [MYRA_DIM] = "\033[2m", [MYRA_BOLD] = "\033[1m"};
    int on = myra_color && style != MYRA_PLAIN;
    va_list ap;
    va_start(ap, fmt);
    if (on) fputs(codes[style], stderr);
    vfprintf(stderr, fmt, ap);
    if (on) fputs("\033[0m", stderr);
    va_end(ap);
}

const myra_provider MYRA_PROVIDERS[] = {
    {"openrouter", "OPENROUTER_API_KEY", "OPENROUTER_BASE_URL", "https://openrouter.ai/api/v1",
     "anthropic/claude-opus-5", 0, 1, 100 * 1024},
    /* Any OpenAI-compatible server; the default URL is llama-server's. */
    /* 16 KB is ~4-5K tokens: a quarter of llama-server's usual 16K context. */
    {"local", "LOCAL_API_KEY", "LOCAL_BASE_URL", "http://localhost:8080/v1", "local", 1, 0,
     16 * 1024},
};
const size_t MYRA_NPROVIDERS = sizeof MYRA_PROVIDERS / sizeof *MYRA_PROVIDERS;

/* ---- growable buffer ---- */

typedef struct { char *p; size_t n, cap; } buf;

static void buf_add(buf *b, const char *s, size_t n) {
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

static char *xstrdup(const char *s) { buf b = {0}; buf_add(&b, s, strlen(s)); return b.p; }

static char *fmt(const char *f, const char *a) {
    size_t n = strlen(f) + strlen(a) + 1;
    char *s = malloc(n);
    if (!s) { perror("malloc"); exit(1); }
    snprintf(s, n, f, a);
    return s;
}

static const char *get_str(cJSON *o, const char *key) {
    return cJSON_GetStringValue(cJSON_GetObjectItem(o, key));
}

static double get_num(cJSON *o, const char *key) { /* 0 when absent */
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

static int valid_utf8(const buf *b) {
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

static int is_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

/* Collects output while keeping only what will be shown: the first fifth of cap and,
   in a ring, the last four fifths. Memory therefore stays at cap however much arrives. */
typedef struct {
    size_t cap, head_max, total;
    buf head;
    char *tail;
    size_t tail_cap, tail_len, tail_at; /* ring: tail_len bytes starting at tail_at */
} sink;

static void sink_init(sink *s, size_t cap) {
    memset(s, 0, sizeof *s);
    s->cap = cap;
    s->head_max = cap / 5;
    s->tail_cap = cap - s->head_max;
    s->tail = malloc(s->tail_cap);
    if (!s->tail) { perror("malloc"); exit(1); }
}

static void sink_free(sink *s) {
    free(s->head.p);
    free(s->tail);
}

static void sink_add(sink *s, const char *p, size_t n) {
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
static char *sink_take(sink *s) {
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
        memmove(tail.p, tail.p + skip, tail.n - skip);
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

static int slurp(const char *path, buf *b) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_add(b, chunk, n);
    int err = ferror(f);
    fclose(f);
    if (!b->p) buf_lit(b, "");
    return err ? -1 : 0;
}

static int write_all(int fd, const char *s, size_t n) {
    while (n) {
        ssize_t k = write(fd, s, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        s += k, n -= (size_t)k;
    }
    return 0;
}

/* A symlink whose target does not exist. Writing would replace the link itself, since
   realpath fails on it and the path then looks like a new file. */
static int dangling_symlink(const char *path) {
    struct stat st;
    return !lstat(path, &st) && S_ISLNK(st.st_mode) && stat(path, &st) != 0;
}

/* Replace path's contents atomically: a failed write leaves the old file intact.
   Writes a temporary file beside the target (symlinks followed, mode kept) and renames
   it over; a hard-linked file thus gets its own copy. A dangling symlink is refused,
   not replaced by a regular file. Where no temporary file can be made, e.g. a read-only
   directory holding a writable file, it writes in place. */
static int spit(const char *path, const char *s, size_t n) {
    char real[PATH_MAX];
    const char *target = realpath(path, real) ? real : path; /* a new file: as given */
    struct stat st;
    mode_t mode;
    if (target == path && dangling_symlink(path)) return -1; /* the rename would eat the link */
    if (stat(target, &st) == 0) {
        mode = st.st_mode & 07777;
    } else {
        mode_t mask = umask(0);
        umask(mask);
        mode = 0666 & ~mask;
    }
    const char *slash = strrchr(target, '/');
    buf tmp = {0};
    if (slash) buf_add(&tmp, target, (size_t)(slash - target) + 1);
    buf_lit(&tmp, ".myra-tmp-XXXXXX");
    int fd = mkstemp(tmp.p);
    if (fd < 0) { /* in place, as a last resort */
        free(tmp.p);
        FILE *f = fopen(target, "wb");
        if (!f) return -1;
        int ok = fwrite(s, 1, n, f) == n;
        return (fclose(f) == 0 && ok) ? 0 : -1;
    }
    /* The directory is not fsync'ed: the rename survives a crash, not necessarily power loss. */
    int ok = !write_all(fd, s, n) && !fchmod(fd, mode) && !fsync(fd);
    ok = !close(fd) && ok && !rename(tmp.p, target);
    if (!ok) unlink(tmp.p);
    free(tmp.p);
    return ok ? 0 : -1;
}

/* ---- tools: each returns a malloc'd result and sets *err on failure ---- */

/* Reads at most limit bytes into the sink; sets *nul when any byte read is NUL. */
static int fill_sink(int fd, sink *s, size_t limit, int *nul) {
    char chunk[65536];
    for (size_t got = 0; got < limit;) {
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (!n) break;
        if (memchr(chunk, 0, (size_t)n)) *nul = 1;
        sink_add(s, chunk, (size_t)n);
        got += (size_t)n;
    }
    return 0;
}

static char *tool_read(const char *path, size_t cap, int *err) {
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) {
        if (fd >= 0) close(fd);
        *err = 1;
        return fmt("cannot read %s", path);
    }
    sink s;
    sink_init(&s, cap);
    int nul = 0, rc = 0;
    if (S_ISREG(st.st_mode) && (size_t)st.st_size > cap) { /* skip the middle, don't read it */
        rc = fill_sink(fd, &s, s.head_max, &nul);
        if (!rc && lseek(fd, (off_t)((size_t)st.st_size - s.tail_cap), SEEK_SET) < 0) rc = -1;
        if (!rc) rc = fill_sink(fd, &s, s.tail_cap, &nul);
        s.total = (size_t)st.st_size;
    } else {
        rc = fill_sink(fd, &s, MYRA_MAX_RAW, &nul);
    }
    close(fd);
    if (rc < 0 || nul) {
        sink_free(&s);
        *err = 1;
        return fmt(nul ? "%s is binary" : "cannot read %s", path);
    }
    char *out = sink_take(&s);
    /* A pipe or device can outlast the limit; a regular file here is under the cap. */
    if (!S_ISREG(st.st_mode) && s.total >= MYRA_MAX_RAW) {
        size_t len = strlen(out);
        char note[64];
        snprintf(note, sizeof note, "%s[stopped after %d MB; the rest was not read]",
                 len && out[len - 1] != '\n' ? "\n" : "", MYRA_MAX_RAW / (1024 * 1024));
        buf b = {0};
        buf_add(&b, out, len);
        buf_add(&b, note, strlen(note));
        free(out);
        out = b.p;
    }
    sink_free(&s);
    return out;
}

static char *tool_write(const char *path, const char *content, int *err) {
    if (dangling_symlink(path)) { *err = 1; return fmt("%s is a symlink to a missing file", path); }
    if (spit(path, content, strlen(content)) < 0) { *err = 1; return fmt("cannot write %s", path); }
    return fmt("wrote %s", path);
}

static char *tool_edit(const char *path, const char *old, const char *new, int *err) {
    *err = 1;
    if (!*old) return xstrdup("old_string is empty");
    if (dangling_symlink(path)) return fmt("%s is a symlink to a missing file", path);
    struct stat st;
    if (!stat(path, &st) && st.st_size > MYRA_MAX_RAW) /* edit holds the whole file */
        return fmt("%s is too large to edit; use shell (sed, awk)", path);
    buf b = {0};
    if (slurp(path, &b) < 0) { free(b.p); return fmt("cannot read %s", path); }
    /* The searches below are strstr: a NUL hides any match past it, including a second
       one, so the unique-match guarantee would not hold. read refuses these files too. */
    if (memchr(b.p, 0, b.n)) { free(b.p); return fmt("%s is binary", path); }
    char *hit = strstr(b.p, old);
    if (!hit && strstr(old, "\xEF\xBF\xBD") && !valid_utf8(&b)) { /* U+FFFD from read */
        free(b.p);
        return fmt("old_string not found in %s: the file is not valid UTF-8, and read showed "
                   "its invalid bytes as U+FFFD, which edit cannot match. Use shell (sed, iconv).",
                   path);
    }
    if (!hit) { free(b.p); return fmt("old_string not found in %s", path); }
    if (strstr(hit + 1, old)) { free(b.p); return fmt("old_string occurs more than once in %s", path); }
    buf out = {0};
    size_t pre = hit - b.p, olen = strlen(old);
    buf_add(&out, b.p, pre);
    buf_add(&out, new, strlen(new));
    buf_add(&out, hit + olen, b.n - pre - olen);
    int rc = spit(path, out.p, out.n);
    free(b.p);
    free(out.p);
    if (rc < 0) return fmt("cannot write %s", path);
    *err = 0;
    return fmt("edited %s", path);
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void reap(pid_t pid, int *st) {
    while (waitpid(pid, st, 0) < 0 && errno == EINTR) {}
}

/* SIGTERM the process group, SIGKILL whatever is left after 2 s. */
static void kill_group(pid_t pid, int *st) {
    kill(-pid, SIGTERM);
    for (int i = 0; i < 20 && waitpid(pid, st, WNOHANG) == 0; i++) usleep(100000);
    kill(-pid, SIGKILL); /* also children that outlived sh */
    if (waitpid(pid, st, WNOHANG) == 0) reap(pid, st);
}

/* sh runs in its own process group, so the terminal's Ctrl-C reaches only the agent,
   which then decides to kill the group. */
static char *tool_shell(const char *cmd, int timeout, size_t cap, int *err) {
    int fds[2];
    if (pipe(fds) < 0) { *err = 1; return fmt("pipe failed: %s", strerror(errno)); }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        *err = 1;
        return fmt("fork failed: %s", strerror(errno));
    }
    if (pid == 0) {
        setpgid(0, 0);
        int nul = open("/dev/null", O_RDONLY);
        dup2(nul, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        close(nul);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid); /* also here, so the group exists before any kill */
    close(fds[1]);

    sink s;
    sink_init(&s, cap);
    char chunk[65536];
    const char *stopped = NULL;
    double deadline = now() + timeout;
    for (;;) {
        if (myra_interrupted) { stopped = "interrupted by user"; break; }
        if (s.total > MYRA_MAX_RAW) { stopped = "output limit"; break; }
        double left = deadline - now();
        if (left <= 0) { stopped = "timed out"; break; }
        struct pollfd pfd = {fds[0], POLLIN, 0};
        int r = poll(&pfd, 1, left < 0.2 ? (int)(left * 1000) + 1 : 200);
        if (r <= 0) continue; /* timeout tick or EINTR: recheck both conditions */
        ssize_t n = read(fds[0], chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break; /* EOF: every writer has exited */
        sink_add(&s, chunk, (size_t)n);
    }
    close(fds[0]);
    int st = 0;
    while (!stopped) { /* EOF: sh may have closed its output and kept running */
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid || (w < 0 && errno != EINTR)) break;
        if (myra_interrupted) stopped = "interrupted by user";
        else if (now() >= deadline) stopped = "timed out";
        else usleep(10000);
    }
    if (stopped) kill_group(pid, &st);

    char *out = sink_take(&s);
    size_t len = strlen(out);
    char tail[80];
    const char *nl = len && out[len - 1] != '\n' ? "\n" : "";
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    if (!stopped)
        snprintf(tail, sizeof tail, "%s[exit %d]", nl, code);
    else if (!strcmp(stopped, "timed out"))
        snprintf(tail, sizeof tail, "%s[timed out after %ds; killed]", nl, timeout);
    else if (!strcmp(stopped, "output limit"))
        snprintf(tail, sizeof tail, "%s[stopped after %d MB of output; killed]", nl,
                 MYRA_MAX_RAW / (1024 * 1024));
    else
        snprintf(tail, sizeof tail, "%s[interrupted by user; killed]", nl);
    buf r = {0};
    buf_add(&r, out, len);
    buf_add(&r, tail, strlen(tail));
    free(out);
    sink_free(&s);
    *err = stopped || code != 0;
    return r.p;
}

/* Whether path, with symlinks resolved, lies outside the working directory. A path
   whose directory does not exist counts as inside: writing there fails anyway. */
static int outside_cwd(const char *path) {
    char cwd[PATH_MAX], real[PATH_MAX];
    if (!realpath(".", cwd)) return 1;
    if (!realpath(path, real)) { /* a new file: resolve its directory */
        char *dir = xstrdup(path), *slash = strrchr(dir, '/');
        const char *d = slash ? (slash == dir ? "/" : (*slash = 0, dir)) : ".";
        int ok = realpath(d, real) != NULL;
        free(dir);
        if (!ok) return 0;
    }
    size_t n = strlen(cwd);
    if (n == 1) return 0; /* cwd is / */
    return strncmp(real, cwd, n) || (real[n] && real[n] != '/');
}

/* Ask on the controlling terminal so it works in headless mode and the REPL alike.
   Returns NULL if allowed, else why not, for the model to relay. */
static const char *denied(myra_permissions mode, int outside, const char *name,
                          const char *detail) {
    if (mode == MYRA_READ_ONLY) return "denied: the agent runs with --permissions read-only";
    if (mode == MYRA_ALL || (mode == MYRA_AUTO && !outside)) return NULL;
    FILE *tty = fopen("/dev/tty", "r+");
    if (!tty)
        return mode == MYRA_AUTO ? "denied: outside the working directory; --permissions all allows it"
                                  : "denied: no terminal to confirm; --permissions auto skips asking";
    fprintf(tty, "allow %s%s: %s ? [y/N] ", name, outside ? " outside the working directory" : "",
            detail);
    fflush(tty);
    char line[16] = {0};
    int ok = fgets(line, sizeof line, tty) && (line[0] == 'y' || line[0] == 'Y');
    fclose(tty);
    return ok ? NULL : "denied by user";
}

static const char *str_arg(cJSON *input, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(input, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* One line per call, cut to the terminal width, unless verbose. */
static void tool_log(myra_agent *a, const char *name, const char *detail) {
    if (a->verbose) {
        myra_note(MYRA_TOOL, "[tool] %s %s\n", name, detail);
        return;
    }
    struct winsize ws;
    size_t width = isatty(STDERR_FILENO) && !ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) && ws.ws_col
                       ? ws.ws_col : 100;
    size_t used = strlen("[tool] ") + strlen(name) + strlen(" ") + strlen(" ...");
    size_t n = strcspn(detail, "\n");
    size_t room = width > used + 10 ? width - used : 10;
    int cut = detail[n] != 0; /* more lines follow */
    if (n > room) {
        n = room;
        while (n && ((unsigned char)detail[n] & 0xC0) == 0x80) n--; /* whole characters */
        cut = 1;
    }
    myra_note(MYRA_TOOL, "[tool] %s %.*s%s\n", name, (int)n, detail, cut ? " ..." : "");
}

char *myra_run_tool(myra_agent *a, const char *name, cJSON *input, int *err) {
    const char *path = str_arg(input, "path");
    const char *content = str_arg(input, "content");
    const char *old = str_arg(input, "old_string"), *new = str_arg(input, "new_string");
    const char *cmd = str_arg(input, "command"), *why;
    *err = 1;
    if (!strcmp(name, "read") && path) {
        tool_log(a, "read", path);
        *err = 0;
        return tool_read(path, a->max_output, err);
    }
    if (!strcmp(name, "write") && path && content) {
        tool_log(a, "write", path);
        if ((why = denied(a->permissions, outside_cwd(path), "write", path))) return xstrdup(why);
        *err = 0;
        return tool_write(path, content, err);
    }
    if (!strcmp(name, "edit") && path && old && new) {
        tool_log(a, "edit", path);
        if ((why = denied(a->permissions, outside_cwd(path), "edit", path))) return xstrdup(why);
        return tool_edit(path, old, new, err);
    }
    if (!strcmp(name, "shell") && cmd) {
        tool_log(a, "shell", cmd);
        if ((why = denied(a->permissions, 0, "shell", cmd))) return xstrdup(why); /* unconfinable */
        cJSON *t = cJSON_GetObjectItemCaseSensitive(input, "timeout");
        double want = cJSON_IsNumber(t) ? t->valuedouble : MYRA_SHELL_TIMEOUT;
        /* Clamp before the cast: an out-of-range double to int is undefined. */
        int secs = !(want >= 1) ? 1 : want > MYRA_SHELL_TIMEOUT_MAX ? MYRA_SHELL_TIMEOUT_MAX
                                                                    : (int)want;
        return tool_shell(cmd, secs, a->max_output, err);
    }
    return fmt("unknown tool or missing arguments: %s", name);
}

/* ---- remembered state ---- */

char *myra_state_path(const char *name, int make_dir) {
    const char *xdg = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    buf b = {0};
    if (xdg && *xdg) buf_add(&b, xdg, strlen(xdg));
    else if (home && *home) { buf_add(&b, home, strlen(home)); buf_lit(&b, "/.local/state"); }
    else return NULL;
    buf_lit(&b, "/myra/");
    buf_add(&b, name, strlen(name));
    for (char *p = b.p + 1; make_dir && *p; p++) /* mkdir -p the parent */
        if (*p == '/') { *p = 0; mkdir(b.p, 0755); *p = '/'; }
    return b.p;
}

char *myra_load_state(const char *name) {
    char *path = myra_state_path(name, 0);
    buf b = {0};
    int rc = path ? slurp(path, &b) : -1;
    free(path);
    if (rc < 0) { free(b.p); return NULL; }
    while (b.n && strchr(" \t\r\n", b.p[b.n - 1])) b.p[--b.n] = 0;
    if (!b.n) { free(b.p); return NULL; }
    return b.p;
}

void myra_store_state(const char *name, const char *value) {
    char *path = myra_state_path(name, 1);
    if (!path) return;
    char *line = fmt("%s\n", value);
    if (spit(path, line, strlen(line)) < 0) myra_note(MYRA_WARN, "warning: cannot save %s\n", path);
    free(line);
    free(path);
}

/* ---- providers ---- */

const myra_provider *myra_provider_named(const char *name) {
    for (size_t i = 0; i < MYRA_NPROVIDERS; i++)
        if (!strcmp(name, MYRA_PROVIDERS[i].name)) return &MYRA_PROVIDERS[i];
    return NULL;
}

static int has_key(const myra_provider *p) {
    const char *k = getenv(p->key_env);
    return k && *k;
}

const myra_provider *myra_provider_default(void) {
    char *last = myra_load_state("provider");
    const myra_provider *p = last ? myra_provider_named(last) : NULL;
    free(last);
    if (p && (p->key_optional || has_key(p))) return p;
    for (size_t i = 0; i < MYRA_NPROVIDERS; i++) /* local is never picked here */
        if (!MYRA_PROVIDERS[i].key_optional && has_key(&MYRA_PROVIDERS[i]))
            return &MYRA_PROVIDERS[i];
    return NULL;
}

/* ---- API ---- */

/* curl calls this about once a second, and sooner on activity; nonzero aborts. */
static int on_progress(void *ud, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
    (void)ud, (void)dt, (void)dn, (void)ut, (void)un;
    return myra_interrupted;
}


/* Case-insensitive strstr; strcasestr needs _GNU_SOURCE on glibc. */
static int contains_ci(const char *s, const char *sub) {
    size_t n = strlen(sub);
    for (; *s; s++)
        if (!strncasecmp(s, sub, n)) return 1;
    return !n;
}

/* OpenAI's code and llama-server's type first; wording, which varies by server, after. */
static int is_context_error(const char *body) {
    cJSON *j = cJSON_Parse(body), *e = cJSON_GetObjectItem(j, "error");
    if (!cJSON_IsObject(e)) e = j; /* a mid-stream error is the error object itself */
    const char *code = get_str(e, "code"), *type = get_str(e, "type");
    int hit = (code && !strcmp(code, "context_length_exceeded")) ||
              (type && !strcmp(type, "exceed_context_size_error"));
    cJSON_Delete(j);
    if (hit) return 1;
    static const char *const hints[] = {"context size", "context length", "context window",
                                        "maximum context", "prompt is too long", "too many tokens"};
    for (size_t i = 0; i < sizeof hints / sizeof *hints; i++)
        if (contains_ci(body, hints[i])) return 1;
    return 0;
}

/* ---- streaming (server-sent events) ---- */

typedef struct {
    char key[24];
    buf val;
} text_field;

/* One streamed reply, rebuilt into the shape of a non-streamed one. */
typedef struct {
    CURL *c;
    buf line, raw; /* raw: every byte, for error bodies and servers that ignore "stream" */
    buf content;
    text_field extra[4]; /* other text deltas: reasoning, reasoning_content, refusal */
    cJSON *calls;        /* tool calls; arguments are kept in args */
    buf *args;
    int ncalls;
    cJSON *details; /* reasoning_details, merged by index */
    cJSON *usage, *error;
    char finish[32];
    int events, printed, done; /* done: the server sent [DONE] */
    size_t received;
    int oversize; /* received passed MYRA_MAX_RESPONSE; the transfer was aborted */
} stream;

/* Count len more bytes; 0 once over the cap, which curl takes as an abort. */
static int within_cap(stream *st, size_t len) {
    st->received += len;
    if (st->received > MYRA_MAX_RESPONSE) st->oversize = 1;
    return !st->oversize;
}

static size_t on_body(char *p, size_t sz, size_t n, void *ud) {
    stream *st = ud;
    if (!within_cap(st, sz * n)) return 0;
    buf_add(&st->raw, p, sz * n);
    return sz * n;
}

static void stream_free(stream *st) {
    free(st->line.p);
    free(st->raw.p);
    free(st->content.p);
    for (size_t i = 0; i < sizeof st->extra / sizeof *st->extra; i++) free(st->extra[i].val.p);
    for (int i = 0; i < st->ncalls; i++) free(st->args[i].p);
    free(st->args);
    cJSON_Delete(st->calls);
    cJSON_Delete(st->details);
    cJSON_Delete(st->usage);
    cJSON_Delete(st->error);
}

static void add_text(stream *st, const char *key, const char *s) {
    size_t i, n = sizeof st->extra / sizeof *st->extra;
    for (i = 0; i < n && st->extra[i].key[0] && strcmp(st->extra[i].key, key); i++) {}
    if (i == n || strlen(key) >= sizeof st->extra[i].key) return; /* unknown extras: dropped */
    snprintf(st->extra[i].key, sizeof st->extra[i].key, "%s", key);
    buf_add(&st->extra[i].val, s, strlen(s));
}

/* Tool calls arrive in pieces keyed by index: arguments are appended, the rest is set. */
static void merge_calls(stream *st, cJSON *deltas) {
    cJSON *d;
    if (!st->calls) st->calls = cJSON_CreateArray();
    cJSON_ArrayForEach(d, deltas) {
        int idx = (int)get_num(d, "index");
        if (idx < 0 || idx > 255) continue;
        while (st->ncalls <= idx) {
            cJSON *c = cJSON_CreateObject();
            cJSON_AddStringToObject(c, "id", "");
            cJSON_AddStringToObject(c, "type", "function");
            cJSON_AddItemToObject(c, "function", cJSON_CreateObject());
            cJSON_AddStringToObject(cJSON_GetObjectItem(c, "function"), "name", "");
            cJSON_AddItemToArray(st->calls, c);
            st->args = realloc(st->args, (st->ncalls + 1) * sizeof *st->args);
            st->args[st->ncalls++] = (buf){0};
        }
        cJSON *c = cJSON_GetArrayItem(st->calls, idx), *fn = cJSON_GetObjectItem(d, "function");
        const char *id = get_str(d, "id"), *name = get_str(fn, "name"), *args = get_str(fn, "arguments");
        if (id && *id) cJSON_ReplaceItemInObject(c, "id", cJSON_CreateString(id));
        if (name && *name)
            cJSON_ReplaceItemInObject(cJSON_GetObjectItem(c, "function"), "name", cJSON_CreateString(name));
        if (args) buf_add(&st->args[idx], args, strlen(args));
    }
}

/* reasoning_details pieces: text fields are appended, others set, merged by index. */
static void merge_details(stream *st, cJSON *deltas) {
    cJSON *d;
    if (!st->details) st->details = cJSON_CreateArray();
    cJSON_ArrayForEach(d, deltas) {
        cJSON *idx = cJSON_GetObjectItem(d, "index"), *into = NULL, *e;
        cJSON_ArrayForEach(e, st->details)
            if (cJSON_IsNumber(idx) && cJSON_Compare(cJSON_GetObjectItem(e, "index"), idx, 1)) into = e;
        if (!into) {
            cJSON_AddItemToArray(st->details, cJSON_Duplicate(d, 1));
            continue;
        }
        cJSON *f;
        cJSON_ArrayForEach(f, d) {
            cJSON *old = cJSON_GetObjectItem(into, f->string);
            if (cJSON_IsString(f) && cJSON_IsString(old) && strcmp(f->string, "type")) {
                buf j = {0};
                buf_add(&j, old->valuestring, strlen(old->valuestring));
                buf_add(&j, f->valuestring, strlen(f->valuestring));
                cJSON_ReplaceItemInObject(into, f->string, cJSON_CreateString(j.p));
                free(j.p);
            } else if (old) {
                cJSON_ReplaceItemInObject(into, f->string, cJSON_Duplicate(f, 1));
            } else {
                cJSON_AddItemToObject(into, f->string, cJSON_Duplicate(f, 1));
            }
        }
    }
}

static void stream_event(stream *st, const char *data) {
    cJSON *chunk = cJSON_Parse(data), *f;
    if (!chunk) return;
    cJSON *err = cJSON_DetachItemFromObject(chunk, "error");
    if (err) { /* mid-stream failure */
        cJSON_Delete(st->error);
        st->error = err;
    }
    st->events++;
    cJSON *usage = cJSON_GetObjectItem(chunk, "usage");
    if (cJSON_IsObject(usage)) {
        cJSON_Delete(st->usage);
        st->usage = cJSON_Duplicate(usage, 1);
    }
    cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItem(chunk, "choices"), 0);
    const char *finish = get_str(choice, "finish_reason");
    if (finish) snprintf(st->finish, sizeof st->finish, "%s", finish);
    cJSON_ArrayForEach(f, cJSON_GetObjectItem(choice, "delta")) {
        if (!strcmp(f->string, "content") && cJSON_IsString(f) && *f->valuestring) {
            fputs(f->valuestring, stdout);
            fflush(stdout);
            st->printed = 1;
            buf_add(&st->content, f->valuestring, strlen(f->valuestring));
        } else if (!strcmp(f->string, "tool_calls") && cJSON_IsArray(f)) {
            merge_calls(st, f);
        } else if (!strcmp(f->string, "reasoning_details") && cJSON_IsArray(f)) {
            merge_details(st, f);
        } else if (strcmp(f->string, "role") && cJSON_IsString(f) && *f->valuestring) {
            add_text(st, f->string, f->valuestring);
        }
    }
    cJSON_Delete(chunk);
}

static void stream_line(stream *st, char *line) {
    if (!MYRA_STARTS_WITH(line, "data:")) return; /* comments (":") and other fields are ignored */
    const char *data = line + 5 + (line[5] == ' ');
    if (!strcmp(data, "[DONE]")) st->done = 1;
    else stream_event(st, data);
}

static size_t on_stream(char *p, size_t sz, size_t n, void *ud) {
    stream *st = ud;
    size_t len = sz * n;
    if (!within_cap(st, len)) return 0;
    /* raw serves error bodies and servers that ignore "stream"; an event stream needs none */
    if (!st->events) buf_add(&st->raw, p, len);
    long status = 0;
    curl_easy_getinfo(st->c, CURLINFO_RESPONSE_CODE, &status);
    if (status != 200) return len; /* an error body: plain JSON, kept in raw */
    size_t old = st->line.n;
    buf_add(&st->line, p, len);
    char *start = st->line.p, *from = start + old, *nl; /* earlier bytes hold no '\n' */
    while ((nl = memchr(from, '\n', st->line.n - (size_t)(from - st->line.p)))) {
        *nl = 0;
        if (nl > start && nl[-1] == '\r') nl[-1] = 0;
        stream_line(st, start);
        start = from = nl + 1;
    }
    st->line.n -= (size_t)(start - st->line.p);
    memmove(st->line.p, start, st->line.n);
    st->line.p[st->line.n] = 0;
    return len;
}

/* The streamed reply as a non-streamed response object. */
static cJSON *stream_result(stream *st) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "assistant");
    if (st->content.n) cJSON_AddStringToObject(msg, "content", st->content.p);
    else if (st->ncalls) cJSON_AddNullToObject(msg, "content");
    else cJSON_AddStringToObject(msg, "content", "");
    for (size_t i = 0; i < sizeof st->extra / sizeof *st->extra && st->extra[i].key[0]; i++)
        cJSON_AddStringToObject(msg, st->extra[i].key, st->extra[i].val.p);
    if (cJSON_GetArraySize(st->details)) {
        cJSON_AddItemToObject(msg, "reasoning_details", st->details);
        st->details = NULL;
    }
    for (int i = 0; i < st->ncalls; i++)
        cJSON_AddStringToObject(cJSON_GetObjectItem(cJSON_GetArrayItem(st->calls, i), "function"),
                                "arguments", st->args[i].p ? st->args[i].p : "");
    if (st->ncalls) {
        cJSON_AddItemToObject(msg, "tool_calls", st->calls);
        st->calls = NULL;
    }
    cJSON *choice = cJSON_CreateObject();
    cJSON_AddItemToObject(choice, "message", msg);
    if (st->finish[0]) cJSON_AddStringToObject(choice, "finish_reason", st->finish);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddItemToObject(resp, "choices", cJSON_CreateArray());
    cJSON_AddItemToArray(cJSON_GetObjectItem(resp, "choices"), choice);
    if (st->usage) {
        cJSON_AddItemToObject(resp, "usage", st->usage);
        st->usage = NULL;
    }
    return resp;
}

/* POST body to path (streamed when streaming is set), or GET path when body is NULL.
   Retries 429, 5xx and network errors, unless streamed text was already printed. */
static cJSON *request(myra_agent *a, const char *path, const char *body, int streaming) {
    const char *base = getenv(a->prov->base_env);
    buf url = {0};
    base = base && *base ? base : a->prov->base;
    buf_add(&url, base, strlen(base));
    buf_add(&url, path, strlen(path));

    struct curl_slist *h = curl_slist_append(NULL, "content-type: application/json");
    char *auth = a->api_key && *a->api_key ? fmt("authorization: Bearer %s", a->api_key) : NULL;
    if (auth) h = curl_slist_append(h, auth);

    const char *d = getenv("MYRA_RETRY_DELAY_MS"); /* first back-off; doubles each retry */
    long delay_ms = d ? strtol(d, NULL, 10) : 0;
    if (delay_ms < 1) delay_ms = 1000;
    if (delay_ms > 60000) delay_ms = 60000; /* a minute of back-off is plenty */
    cJSON *resp = NULL;
    a->streamed = a->context_full = 0;
    for (int attempt = 0; attempt <= RETRIES; attempt++) {
        if (attempt) { /* a signal ends the wait early */
            long ms = delay_ms << (attempt - 1);
            struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
            nanosleep(&ts, NULL);
        }
        if (myra_interrupted) break;
        stream st = {0};
        CURL *c = st.c = curl_easy_init();
        curl_easy_setopt(c, CURLOPT_URL, url.p);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        if (body) curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, streaming ? on_stream : on_body);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &st);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 600L);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
        CURLcode rc = curl_easy_perform(c);
        if (streaming && st.line.n) stream_line(&st, st.line.p); /* a last event without "\n" */
        long status = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(c);
        if (st.printed) putchar('\n'), fflush(stdout); /* end the streamed line */
        a->streamed = st.printed;
        const char *out = st.raw.p ? st.raw.p : "";
        /* No finish_reason and no [DONE]: the stream was cut off, so the reply is partial. */
        int truncated = st.events && !st.finish[0] && !st.done;
        if (rc == CURLE_OK && status == 200 && !st.error && !truncated) {
            resp = st.events ? stream_result(&st) : cJSON_Parse(out); /* "stream" ignored */
            if (!resp) myra_note(MYRA_ERROR, "error: response is not JSON\n");
            stream_free(&st);
            break;
        }
        if (st.oversize) { /* a retry would receive the same */
            myra_note(MYRA_ERROR, "error: response exceeds %d MB\n", MYRA_MAX_RESPONSE / (1024 * 1024));
            stream_free(&st);
            break;
        }
        if (rc == CURLE_ABORTED_BY_CALLBACK) {
            myra_note(MYRA_WARN, "interrupted\n");
            stream_free(&st);
            break;
        }
        if (rc == CURLE_COULDNT_CONNECT) { /* nothing listening: retrying cannot help */
            myra_note(MYRA_ERROR, "error: cannot connect to %s; is the server running?\n", url.p);
            stream_free(&st);
            break;
        }
        char *err = st.error ? cJSON_PrintUnformatted(st.error) : NULL;
        if (err) out = err;
        else if (truncated) out = "the stream ended early";
        a->context_full = (status == 400 || st.error) && is_context_error(out);
        int retryable = (rc != CURLE_OK || status == 429 || status >= 500 || st.error || truncated) &&
                        !st.printed && !a->context_full; /* retrying cannot shrink the context */
        const char *again = retryable && attempt < RETRIES ? ", retrying" : "";
        if (st.error || truncated) myra_note(MYRA_ERROR, "stream error%s: %s\n", again, out);
        else myra_note(MYRA_ERROR, "api error (%s, http %ld)%s: %s\n", curl_easy_strerror(rc),
                        status, again, out);
        free(err);
        stream_free(&st);
        if (!retryable) break;
    }
    curl_slist_free_all(h);
    free(auth);
    free(url.p);
    return resp;
}

static cJSON *call_api(myra_agent *a) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", a->model);
    cJSON_AddItemReferenceToObject(req, "tools", a->tools);
    cJSON_AddItemReferenceToObject(req, "messages", a->messages);
    cJSON_AddTrueToObject(req, "stream");
    cJSON_AddItemToObject(req, "stream_options", cJSON_Parse("{\"include_usage\":true}"));
    /* Turns on prompt caching for Anthropic models, where it is opt-in. Providers
       that cache automatically or lack support ignore unknown fields. */
    if (a->prov->cache_control)
        cJSON_AddItemToObject(req, "cache_control", cJSON_Parse("{\"type\":\"ephemeral\"}"));
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    cJSON *resp = request(a, "/chat/completions", body, 1);
    free(body);
    /* Saved only once the server accepts them, so a mistyped choice is not kept. */
    if (resp && a->save_provider) {
        myra_store_state("provider", a->prov->name);
        a->save_provider = 0;
    }
    if (resp && a->save_model) {
        char *key = fmt("%s.model", a->prov->name);
        myra_store_state(key, a->model);
        free(key);
        a->save_model = 0;
    }
    return resp;
}

char **myra_model_ids(myra_agent *a) {
    cJSON *resp = request(a, "/models", NULL, 0), *m;
    cJSON *data = cJSON_GetObjectItem(resp, "data");
    if (resp && !cJSON_IsArray(data)) myra_note(MYRA_ERROR, "error: no model list in response\n");
    char **ids = NULL;
    size_t n = 0, cap = 0;
    cJSON_ArrayForEach(m, data) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(m, "id"));
        if (!id) continue;
        if (n + 2 > cap) {
            cap = cap ? cap * 2 : 32;
            ids = realloc(ids, cap * sizeof *ids);
            if (!ids) { perror("realloc"); exit(1); }
        }
        ids[n++] = xstrdup(id);
    }
    if (ids) ids[n] = NULL;
    cJSON_Delete(resp);
    return ids;
}

void myra_free_model_ids(char **ids) {
    for (char **p = ids; p && *p; p++) free(*p);
    free(ids);
}

void myra_list_models(myra_agent *a, const char *filter) {
    char **ids = myra_model_ids(a);
    for (char **p = ids; p && *p; p++)
        if (contains_ci(*p, filter)) printf("%s %s\n", strcmp(*p, a->model) ? " " : "*", *p);
    fflush(stdout);
    myra_free_model_ids(ids);
}

/* ---- the loop ---- */

static void print_text(const char *s) {
    if (!s || !*s) return;
    printf("%s\n", s);
    fflush(stdout);
}

int myra_step(myra_agent *a, cJSON *resp) {
    cJSON *choice = cJSON_GetArrayItem(cJSON_GetObjectItem(resp, "choices"), 0);
    cJSON *msg = cJSON_DetachItemFromObject(choice, "message");
    if (!cJSON_IsObject(msg)) {
        char *s = cJSON_PrintUnformatted(resp);
        myra_note(MYRA_ERROR, "error: no message in response: %s\n", s);
        free(s);
        cJSON_Delete(msg);
        return -1;
    }
    const char *finish = get_str(choice, "finish_reason");
    finish = finish ? finish : "";
    if (!strcmp(finish, "content_filter")) {
        myra_note(MYRA_ERROR, "refused: content_filter\n");
        cJSON_Delete(msg);
        return -1;
    }
    /* Calls cut off by length may be truncated JSON, and dropping them keeps the
       history valid: every tool call must be followed by its result. */
    if (!strcmp(finish, "length")) {
        myra_note(MYRA_WARN, "warning: reply hit the length limit\n");
        cJSON_DeleteItemFromObject(msg, "tool_calls");
    }
    if (!a->streamed) print_text(get_str(msg, "content")); /* else already shown */
    cJSON *calls = cJSON_GetObjectItem(msg, "tool_calls");
    int ncalls = cJSON_GetArraySize(calls);
    if (!ncalls && !get_str(msg, "content")) { /* null content is only valid with tool_calls */
        cJSON_DeleteItemFromObject(msg, "tool_calls");
        cJSON_DeleteItemFromObject(msg, "content");
        cJSON_AddStringToObject(msg, "content", "");
    }
    cJSON_AddItemToArray(a->messages, msg);
    int budget = MYRA_MAX_TOOL_CALLS - a->turn_calls, skipped = 0; /* the cap holds within a batch */
    a->turn_calls += ncalls;
    for (int i = 0; i < ncalls; i++) {
        cJSON *call = cJSON_GetArrayItem(calls, i), *fn = cJSON_GetObjectItem(call, "function");
        const char *id = get_str(call, "id");
        if (!id || !*id) { /* some servers omit it; the result must still name its call */
            char gen[32];
            snprintf(gen, sizeof gen, "myra_call_%d", i);
            if (cJSON_GetObjectItem(call, "id"))
                cJSON_ReplaceItemInObject(call, "id", cJSON_CreateString(gen));
            else
                cJSON_AddStringToObject(call, "id", gen);
        }
        const char *name = get_str(fn, "name"), *args = get_str(fn, "arguments");
        cJSON *input = cJSON_Parse(args ? args : "");
        int err = 1;
        /* Every call still gets a result, so the history stays valid. */
        char *out;
        if (myra_interrupted) out = xstrdup("skipped: interrupted by user");
        else if (i >= budget) out = (skipped++, xstrdup("skipped: the turn reached its tool call limit"));
        else if (cJSON_IsObject(input)) out = myra_run_tool(a, name ? name : "", input, &err);
        else out = fmt("invalid JSON arguments for %s", name ? name : "?");
        cJSON_Delete(input);
        /* Chat completions has no is_error field, so mark failures in the text. */
        char *content = err ? fmt("error: %s", out) : fmt("%s", out);
        if (a->verbose) myra_note(MYRA_DIM, "%s\n", content);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "role", "tool");
        cJSON_AddStringToObject(r, "tool_call_id", get_str(call, "id"));
        cJSON_AddStringToObject(r, "content", content);
        cJSON_AddItemToArray(a->messages, r);
        free(content);
        free(out);
    }
    if (myra_interrupted) {
        myra_note(MYRA_WARN, "interrupted\n");
        return 0; /* end the turn without asking the model again */
    }
    if (skipped) {
        myra_note(MYRA_WARN, "stopped: %d tool calls in one turn; skipped %d\n", MYRA_MAX_TOOL_CALLS,
                  skipped);
        return 0;
    }
    return ncalls > 0;
}

static int is_tool(cJSON *m) {
    const char *role = get_str(m, "role");
    return role && !strcmp(role, "tool");
}

/* Blank every tool result except the trailing batch, which the next reply needs.
   Returns how many were blanked. */
static int drop_old_tool_output(myra_agent *a) {
    static const char *const dropped = "[output dropped to fit the context]";
    int n = cJSON_GetArraySize(a->messages), last = n, count = 0;
    while (last > 0 && is_tool(cJSON_GetArrayItem(a->messages, last - 1))) last--;
    for (int i = 0; i < last; i++) {
        cJSON *m = cJSON_GetArrayItem(a->messages, i);
        const char *content = get_str(m, "content");
        if (!is_tool(m) || !content || !strcmp(content, dropped)) continue;
        cJSON_ReplaceItemInObject(m, "content", cJSON_CreateString(dropped));
        count++;
    }
    return count;
}

/* Add one response's usage; OpenRouter's cost is in credits, which are dollars. */
static void usage_add(myra_usage *u, cJSON *usage) {
    if (!cJSON_IsObject(usage)) return;
    u->requests++;
    u->in += get_num(usage, "prompt_tokens");
    u->out += get_num(usage, "completion_tokens");
    u->cached += get_num(cJSON_GetObjectItem(usage, "prompt_tokens_details"), "cached_tokens");
    cJSON *cost = cJSON_GetObjectItem(usage, "cost");
    if (cJSON_IsNumber(cost)) {
        u->cost += cost->valuedouble;
        u->costed = 1;
    }
}

static void usage_print(const char *label, const myra_usage *u) {
    if (!u->requests) return;
    char cost[32] = "";
    if (u->costed) snprintf(cost, sizeof cost, ", $%.4f", u->cost);
    myra_note(MYRA_DIM, "%s %d request%s: %.0f in (%.0f cached), %.0f out%s\n", label,
               u->requests, u->requests == 1 ? "" : "s", u->in, u->cached, u->out, cost);
}

void myra_report_session(myra_agent *a) { usage_print("[session]", &a->session); }

int myra_ask(myra_agent *a, const char *text) {
    myra_interrupted = 0;
    a->turn_calls = 0;
    int before = cJSON_GetArraySize(a->messages);
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", text);
    cJSON_AddItemToArray(a->messages, m);
    int rc;
    myra_usage turn = {0};
    cJSON *undropped = NULL; /* history before the first drop, restored if the turn fails */
    for (;;) { /* call the model and run tools until it stops asking for them */
        cJSON *resp = call_api(a);
        usage_add(&turn, cJSON_GetObjectItem(resp, "usage"));
        usage_add(&a->session, cJSON_GetObjectItem(resp, "usage"));
        if (!resp && a->context_full) {
            if (!undropped) undropped = cJSON_Duplicate(a->messages, 1);
            int n = drop_old_tool_output(a);
            if (n) {
                myra_note(MYRA_WARN, "context full: dropped %d old tool output%s, retrying\n",
                           n, n == 1 ? "" : "s");
                continue;
            }
            myra_note(MYRA_WARN, "hint: the conversation no longer fits the model's context; "
                                   "/clear starts a new one\n");
        }
        if (!resp) { rc = -1; break; }
        rc = myra_step(a, resp);
        cJSON_Delete(resp);
        if (rc != 1) break;
        if (a->turn_calls >= MYRA_MAX_TOOL_CALLS) { /* a model looping on tools */
            myra_note(MYRA_WARN, "stopped: %d tool calls in one turn\n", a->turn_calls);
            rc = 0;
            break;
        }
    }
    usage_print("[usage]", &turn);
    if (rc < 0 && undropped) { /* a misread error must not blank the conversation for good */
        cJSON_Delete(a->messages);
        a->messages = undropped;
        undropped = NULL;
    }
    cJSON_Delete(undropped);
    if (rc < 0)
        while (cJSON_GetArraySize(a->messages) > before)
            cJSON_DeleteItemFromArray(a->messages, cJSON_GetArraySize(a->messages) - 1);
    return rc;
}

/* ---- session ---- */

int myra_init(myra_agent *a, const myra_provider *p, const char *model, myra_permissions perms) {
    memset(a, 0, sizeof *a);
    a->prov = p;
    a->permissions = perms;
    const char *cap = getenv("MYRA_MAX_OUTPUT");
    a->max_output = cap && atol(cap) >= 1024 ? (size_t)atol(cap) : p->max_output;
    /* read takes the whole-file path for a file up to the cap, and that path stops at MYRA_MAX_RAW */
    if (a->max_output > MYRA_MAX_RAW) a->max_output = MYRA_MAX_RAW;
    a->api_key = getenv(p->key_env);
    if ((!a->api_key || !*a->api_key) && !p->key_optional) {
        myra_note(MYRA_ERROR, "error: %s is not set\n", p->key_env);
        return -1;
    }
    char *key = fmt("%s.model", p->name);
    char *remembered = myra_load_state(key);
    free(key);
    if (model) a->save_model = !remembered || strcmp(model, remembered);
    a->model = xstrdup(model ? model : remembered ? remembered : p->model);
    free(remembered);

    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, ".");
    char *prompt = fmt("You are a coding agent. The working directory is %s. Use the read, write, "
                       "edit and shell tools to inspect and change code. Verify changes where you "
                       "can. Be concise.", cwd);
    a->tools = cJSON_Parse(TOOLS_JSON);
    a->messages = cJSON_CreateArray();
    cJSON *sys = cJSON_CreateObject(); /* rollback in myra_ask never reaches index 0 */
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", prompt);
    cJSON_AddItemToArray(a->messages, sys);
    free(prompt);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    return 0;
}

void myra_free(myra_agent *a) {
    if (a->messages) curl_global_cleanup(); /* paired with a successful myra_init */
    cJSON_Delete(a->messages);
    cJSON_Delete(a->tools);
    free(a->model);
    memset(a, 0, sizeof *a);
}

void myra_clear(myra_agent *a) {
    while (cJSON_GetArraySize(a->messages) > 1)
        cJSON_DeleteItemFromArray(a->messages, cJSON_GetArraySize(a->messages) - 1);
}

void myra_set_model(myra_agent *a, const char *model) {
    free(a->model);
    a->model = xstrdup(model);
    a->save_model = 1;
}

const char *myra_command(const char *line, const char *name) {
    size_t n = strlen(name);
    if (strncmp(line, name, n) || (line[n] && line[n] != ' ')) return NULL;
    for (line += n; *line == ' '; line++);
    return line;
}
