/* tools.c - myralib: the coding agent's tools: read, write, edit and shell, and the shell jobs they leave. */
#include "internal.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
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
   directory holding a writable file, it writes in place. mode >= 0 replaces the mode. */
int myra__spit(const char *path, const char *s, size_t n, int mode_set) {
    char real[PATH_MAX];
    const char *target = realpath(path, real) ? real : path; /* a new file: as given */
    struct stat st;
    mode_t mode;
    if (target == path && dangling_symlink(path)) return -1; /* the rename would eat the link */
    if (mode_set >= 0) {
        mode = (mode_t)mode_set;
    } else if (stat(target, &st) == 0) {
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
        int ok = (mode_set < 0 || !fchmod(fileno(f), mode)) && fwrite(s, 1, n, f) == n;
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
        myra__sink_add(s, chunk, (size_t)n);
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
        return myra__fmt("cannot read %s", path);
    }
    sink s;
    myra__sink_init(&s, cap);
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
        myra__sink_free(&s);
        *err = 1;
        return myra__fmt(nul ? "%s is binary" : "cannot read %s", path);
    }
    char *out = myra__sink_take(&s);
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
    myra__sink_free(&s);
    return out;
}

static char *tool_write(const char *path, const char *content, int *err) {
    if (dangling_symlink(path)) { *err = 1; return myra__fmt("%s is a symlink to a missing file", path); }
    if (myra__spit(path, content, strlen(content), -1) < 0) { *err = 1; return myra__fmt("cannot write %s", path); }
    return myra__fmt("wrote %s", path);
}

/* io: the front end's file hooks, or NULL for the disk. */
static char *tool_edit(const myra_io *io, const char *path, const char *old, const char *new,
                       int *err) {
    *err = 1;
    if (!*old) return xstrdup("old_string is empty");
    buf b = {0};
    char *abs = NULL;
    if (io) {
        char *why = NULL, *text = io->read(io->ud, abs = myra__absolute(path), &why);
        if (!text) { free(abs); return myra__with_why(myra__fmt("cannot read %s", path), why); }
        buf_add(&b, text, strlen(text));
        free(text);
    } else {
        if (dangling_symlink(path)) return myra__fmt("%s is a symlink to a missing file", path);
        struct stat st;
        if (!stat(path, &st) && st.st_size > MYRA_MAX_RAW) /* edit holds the whole file */
            return myra__fmt("%s is too large to edit; use shell (sed, awk)", path);
        if (slurp(path, &b) < 0) { free(b.p); return myra__fmt("cannot read %s", path); }
    }
    /* The searches below are strstr: a NUL hides any match past it, including a second
       one, so the unique-match guarantee would not hold. read refuses these files too. */
    if (memchr(b.p, 0, b.n)) { free(b.p); free(abs); return myra__fmt("%s is binary", path); }
    char *hit = strstr(b.p, old);
    if (!hit && strstr(old, "\xEF\xBF\xBD") && !myra__valid_utf8(&b)) { /* U+FFFD from read */
        free(b.p);
        free(abs);
        return myra__fmt("old_string not found in %s: the file is not valid UTF-8, and read showed "
                   "its invalid bytes as U+FFFD, which edit cannot match. Use shell (sed, iconv).",
                   path);
    }
    if (!hit || strstr(hit + 1, old)) {
        free(b.p);
        free(abs);
        return myra__fmt(hit ? "old_string occurs more than once in %s" : "old_string not found in %s", path);
    }
    buf out = {0};
    size_t pre = hit - b.p, olen = strlen(old);
    buf_add(&out, b.p, pre);
    buf_add(&out, new, strlen(new));
    buf_add(&out, hit + olen, b.n - pre - olen);
    char *why = NULL;
    int rc = abs ? ((why = io->write(io->ud, abs, out.p)) ? -1 : 0) : myra__spit(path, out.p, out.n, -1);
    free(b.p);
    free(out.p);
    free(abs);
    if (rc < 0) return myra__with_why(myra__fmt("cannot write %s", path), why);
    *err = 0;
    return myra__fmt("edited %s", path);
}

static void reap(pid_t pid, int *st) {
    while (waitpid(pid, st, 0) < 0 && errno == EINTR) {}
}

static int group_alive(pid_t g) { return kill(-g, 0) == 0 || errno == EPERM; }

/* SIGTERM the process group, SIGKILL whatever is left after 2 s; 1 s when myra itself
   is terminating, so a parent's own SIGKILL comes after this one. The wait is for the
   whole group: a member that handles SIGTERM, such as a nested myra, gets to finish. */
static void kill_group(pid_t pid, int *st) {
    kill(-pid, SIGTERM);
    int reaped = 0; /* sh stays a group member until reaped */
    for (double end = now() + (myra_terminated ? 1 : 2); now() < end; usleep(20000)) {
        pid_t w = reaped ? 0 : waitpid(pid, st, WNOHANG);
        if (w == pid || (w < 0 && errno != EINTR)) reaped = 1;
        if (reaped && !group_alive(pid)) break;
    }
    kill(-pid, SIGKILL); /* also children that outlived sh */
    if (!reaped) reap(pid, st);
}

/* The process group of each shell call, kept so background jobs die with myra. */
static pid_t *jobs;
static size_t njobs, jobs_cap;
static pthread_mutex_t jobs_lock = PTHREAD_MUTEX_INITIALIZER; /* agents on several threads */

static void add_job(pid_t g) {
    pthread_mutex_lock(&jobs_lock);
    size_t n = 0;
    for (size_t i = 0; i < njobs; i++) /* prune empty groups, so the list stays short */
        if (group_alive(jobs[i])) jobs[n++] = jobs[i];
    njobs = n;
    if (njobs == jobs_cap) {
        pid_t *grown = realloc(jobs, (jobs_cap = jobs_cap ? 2 * jobs_cap : 16) * sizeof *jobs);
        if (!grown) { perror("realloc"); exit(1); }
        jobs = grown;
    }
    jobs[njobs++] = g;
    pthread_mutex_unlock(&jobs_lock);
}

void myra_kill_jobs(void) {
    pthread_mutex_lock(&jobs_lock);
    int left = 0;
    for (size_t i = 0; i < njobs; i++)
        if (group_alive(jobs[i])) kill(-jobs[i], SIGTERM), left = 1;
    for (double end = now() + 1; left && now() < end;) {
        usleep(50000);
        left = 0;
        for (size_t i = 0; i < njobs && !left; i++) left = group_alive(jobs[i]);
    }
    for (size_t i = 0; left && i < njobs; i++) kill(-jobs[i], SIGKILL);
    free(jobs);
    jobs = NULL;
    njobs = jobs_cap = 0;
    pthread_mutex_unlock(&jobs_lock);
}

/* sh runs in its own process group, so the terminal's Ctrl-C reaches only the agent,
   which then decides to kill the group. */
static char *tool_shell(const char *cmd, int timeout, size_t cap, int *err) {
    int fds[2];
    if (pipe(fds) < 0) { *err = 1; return myra__fmt("pipe failed: %s", strerror(errno)); }
    long max_fd = sysconf(_SC_OPEN_MAX);
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        *err = 1;
        return myra__fmt("fork failed: %s", strerror(errno));
    }
    if (pid == 0) {
        setpgid(0, 0);
        int nul = open("/dev/null", O_RDONLY);
        if (nul < 0 || dup2(nul, 0) < 0) close(0); /* never the terminal: it would read keystrokes */
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        if (nul >= 0) close(nul);
        close_from_3(max_fd);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    setpgid(pid, pid); /* also here, so the group exists before any kill */
    add_job(pid);
    close(fds[1]);

    sink s;
    myra__sink_init(&s, cap);
    char chunk[65536];
    const char *stopped = NULL;
    double deadline = now() + timeout;
    for (;;) {
        if (myra_poll) myra_poll();
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
        myra__sink_add(&s, chunk, (size_t)n);
    }
    close(fds[0]);
    int st = 0;
    while (!stopped) { /* EOF: sh may have closed its output and kept running */
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid || (w < 0 && errno != EINTR)) break;
        if (myra_poll) myra_poll();
        if (myra_interrupted) stopped = "interrupted by user";
        else if (now() >= deadline) stopped = "timed out";
        else usleep(10000);
    }
    if (stopped) kill_group(pid, &st);

    char *out = myra__sink_take(&s);
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
    myra__sink_free(&s);
    *err = stopped || code != 0;
    return r.p;
}


/* ---- the coding tools ---- */

/* read through the front end, capped like a local read. NULL if it refused, with
   *why its reason, malloc'd. */
static char *served_read(myra_agent *a, const char *path, int *err, char **why) {
    char *abs = myra__absolute(path), *text = a->io.read(a->io.ud, abs, why);
    free(abs);
    if (!text) return NULL;
    *err = 0;
    return myra__capped(a, text);
}

static char *run_read(myra_agent *a, const myra_tool *t, cJSON *input, int *err) {
    (void)t;
    const char *path = myra__str_arg(input, "path");
    char *why_not = NULL, *out = a->io.read ? served_read(a, path, err, &why_not) : NULL;
    if (out) return out;
    /* Inside the cwd a refusal likely means a binary file, which a disk read reports
       better. Outside, only --permissions all reads disk, as its shell could. */
    if (a->io.read && myra__outside_cwd(path) && a->permissions != MYRA_ALL) return myra__with_why(myra__fmt("cannot read %s", path), why_not);
    free(why_not);
    *err = 0;
    return tool_read(path, a->max_output, err);
}

static char *run_write(myra_agent *a, const myra_tool *t, cJSON *input, int *err) {
    (void)t;
    const char *path = myra__str_arg(input, "path"), *content = myra__str_arg(input, "content");
    if (a->io.read && a->io.write) { /* the front end owns the files */
        char *abs = myra__absolute(path), *failed = a->io.write(a->io.ud, abs, content);
        free(abs);
        if (failed) return myra__with_why(myra__fmt("cannot write %s", path), failed);
        *err = 0;
        return myra__fmt("wrote %s", path);
    }
    *err = 0;
    return tool_write(path, content, err);
}

static char *run_edit(myra_agent *a, const myra_tool *t, cJSON *input, int *err) {
    (void)t;
    return tool_edit(a->io.read && a->io.write ? &a->io : NULL, myra__str_arg(input, "path"),
                     myra__str_arg(input, "old_string"), myra__str_arg(input, "new_string"), err);
}

static char *run_shell(myra_agent *a, const myra_tool *t, cJSON *input, int *err) {
    (void)t;
    cJSON *timeout = cJSON_GetObjectItemCaseSensitive(input, "timeout");
    double want = cJSON_IsNumber(timeout) ? timeout->valuedouble : MYRA_SHELL_TIMEOUT;
    /* Clamp before the cast: an out-of-range double to int is undefined. */
    int secs = !(want >= 1) ? 1 : want > MYRA_SHELL_TIMEOUT_MAX ? MYRA_SHELL_TIMEOUT_MAX
                                                                : (int)want;
    return tool_shell(myra__str_arg(input, "command"), secs, a->max_output, err);
}

static const myra_tool CODING_TOOLS[] = {
    {"read", "Read a text file and return its contents.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
     "\"required\":[\"path\"],\"additionalProperties\":false}",
     "path", MYRA_TOOL_READS, run_read, NULL},
    {"write", "Create or overwrite a file with the given content.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
     "\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"],"
     "\"additionalProperties\":false}",
     "path", MYRA_TOOL_WRITES, run_write, NULL},
    {"edit", "Replace old_string with new_string in a file. old_string must occur exactly once.",
     "{\"type\":\"object\",\"properties\":{"
     "\"path\":{\"type\":\"string\"},\"old_string\":{\"type\":\"string\"},"
     "\"new_string\":{\"type\":\"string\"}},\"required\":[\"path\",\"old_string\",\"new_string\"],"
     "\"additionalProperties\":false}",
     "path", MYRA_TOOL_WRITES, run_edit, NULL},
    {"shell",
     "Run a command with /bin/sh in the working directory. "
     "Returns combined stdout and stderr and the exit status. stdin is /dev/null. "
     "The command and its children are killed after timeout seconds (default 120, max 600). "
     "Background jobs must redirect their output, or the call waits for them.",
     "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},"
     "\"timeout\":{\"type\":\"integer\"}},"
     "\"required\":[\"command\"],\"additionalProperties\":false}",
     "command", MYRA_TOOL_RUNS, run_shell, NULL},
};

void myra_add_coding_tools(myra_agent *a) {
    for (size_t i = 0; i < sizeof CODING_TOOLS / sizeof *CODING_TOOLS; i++)
        myra_add_tool(a, &CODING_TOOLS[i]);
    a->system = "You are a coding agent. The working directory is {cwd}. Use the read, write, "
                "edit and shell tools to inspect and change code. Verify changes where you "
                "can. Be concise.";
    myra_set_cwd(a);
}
