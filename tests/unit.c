/* unit.c - myralib unit tests. Usage: unit NAME. Each case runs in a fresh temp dir. */
#include "myra.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            return 1;                                                             \
        }                                                                         \
    } while (0)
#define TEST(name) static int test_##name(void)

/* ---- helpers ---- */

static myra_agent A;

static int local_agent(void) { return myra_init(&A, myra_provider_named("local"), NULL, MYRA_AUTO); }

static void put(const char *path, const char *s) {
    FILE *f = fopen(path, "wb");
    fputs(s, f);
    fclose(f);
}

static char *get(const char *path) {
    static char b[4096];
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    b[fread(b, 1, sizeof b - 1, f)] = 0;
    fclose(f);
    return b;
}

/* Run tool name with JSON input; returns the result (caller frees) and sets *err. */
static char *tool(const char *name, const char *json, int *err) {
    cJSON *in = cJSON_Parse(json);
    char *out = myra_run_tool(&A, name, in, err);
    cJSON_Delete(in);
    return out;
}

static int step(const char *json) {
    cJSON *r = cJSON_Parse(json);
    int rc = myra_step(&A, r);
    cJSON_Delete(r);
    return rc;
}

static cJSON *msg(int i) { return cJSON_GetArrayItem(A.messages, i); }
static const char *field(cJSON *o, const char *k) {
    return cJSON_GetStringValue(cJSON_GetObjectItem(o, k));
}

static double secs(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* ---- tools ---- */

TEST(tool_write_then_read) {
    int err;
    char *out = tool("write", "{\"path\":\"a.txt\",\"content\":\"one\\ntwo\\n\"}", &err);
    CHECK(!err && !strcmp(out, "wrote a.txt"));
    free(out);
    out = tool("read", "{\"path\":\"a.txt\"}", &err);
    CHECK(!err && !strcmp(out, "one\ntwo\n"));
    free(out);
    return 0;
}

/* No temporary file left behind in the current directory. */
static int no_temp_files(void) {
    DIR *d = opendir(".");
    struct dirent *e;
    int found = 0;
    while (d && (e = readdir(d)))
        if (MYRA_STARTS_WITH(e->d_name, ".myra-tmp-")) found = 1;
    if (d) closedir(d);
    return !found;
}

TEST(write_keeps_mode_and_follows_symlinks) {
    int err;
    put("m.txt", "old");
    chmod("m.txt", 0754);
    char *out = tool("write", "{\"path\":\"m.txt\",\"content\":\"new\"}", &err);
    struct stat st;
    CHECK(!err && stat("m.txt", &st) == 0 && (st.st_mode & 07777) == 0754);
    CHECK(!strcmp(get("m.txt"), "new"));
    free(out);
    put("real.txt", "old");
    CHECK(symlink("real.txt", "link") == 0);
    out = tool("edit", "{\"path\":\"link\",\"old_string\":\"old\",\"new_string\":\"via link\"}", &err);
    CHECK(!err && lstat("link", &st) == 0 && S_ISLNK(st.st_mode)); /* still a link */
    CHECK(!strcmp(get("real.txt"), "via link"));
    free(out);
    umask(027);
    out = tool("write", "{\"path\":\"fresh.txt\",\"content\":\"x\"}", &err);
    CHECK(!err && stat("fresh.txt", &st) == 0 && (st.st_mode & 07777) == 0640); /* umask applies */
    free(out);
    CHECK(no_temp_files());
    return 0;
}

TEST(write_refuses_a_dangling_symlink) {
    int err;
    CHECK(symlink("nowhere.txt", "dangling") == 0);
    char *out = tool("write", "{\"path\":\"dangling\",\"content\":\"x\"}", &err);
    CHECK(err && !strcmp(out, "dangling is a symlink to a missing file"));
    free(out);
    struct stat st;
    CHECK(lstat("dangling", &st) == 0 && S_ISLNK(st.st_mode)); /* the link survives */
    CHECK(stat("nowhere.txt", &st) != 0);                      /* and nothing was created */
    out = tool("edit", "{\"path\":\"dangling\",\"old_string\":\"a\",\"new_string\":\"b\"}", &err);
    CHECK(err && !strcmp(out, "dangling is a symlink to a missing file"));
    free(out);
    CHECK(lstat("dangling", &st) == 0 && S_ISLNK(st.st_mode));
    CHECK(no_temp_files());
    return 0;
}

TEST(edit_refuses_a_file_with_nul_bytes) {
    int err;
    FILE *f = fopen("nul.bin", "wb");
    fwrite("x\0x", 1, 3, f); /* two matches for "x", the second hidden behind the NUL */
    fclose(f);
    char *out = tool("edit", "{\"path\":\"nul.bin\",\"old_string\":\"x\",\"new_string\":\"Y\"}",
                     &err);
    CHECK(err && !strcmp(out, "nul.bin is binary"));
    free(out);
    char b[4] = {0};
    f = fopen("nul.bin", "rb");
    CHECK(fread(b, 1, sizeof b, f) == 3 && !memcmp(b, "x\0x", 3));
    fclose(f);
    CHECK(no_temp_files());
    return 0;
}

TEST(failed_write_keeps_the_original) {
    signal(SIGXFSZ, SIG_IGN); /* a write past the limit then fails with EFBIG */
    struct rlimit rl = {4096, 4096};
    CHECK(setrlimit(RLIMIT_FSIZE, &rl) == 0);
    put("keep.txt", "original\n");
    char big[16384];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    cJSON *in = cJSON_CreateObject();
    cJSON_AddStringToObject(in, "path", "keep.txt");
    cJSON_AddStringToObject(in, "content", big);
    int err;
    char *out = myra_run_tool(&A, "write", in, &err);
    cJSON_Delete(in);
    CHECK(err && strstr(out, "cannot write"));
    CHECK(!strcmp(get("keep.txt"), "original\n")); /* not truncated */
    free(out);
    in = cJSON_CreateObject();
    cJSON_AddStringToObject(in, "path", "keep.txt");
    cJSON_AddStringToObject(in, "old_string", "original");
    cJSON_AddStringToObject(in, "new_string", big);
    out = myra_run_tool(&A, "edit", in, &err);
    cJSON_Delete(in);
    CHECK(err && !strcmp(get("keep.txt"), "original\n"));
    free(out);
    CHECK(no_temp_files());
    return 0;
}

TEST(write_in_place_when_directory_is_read_only) {
    CHECK(mkdir("ro", 0755) == 0);
    put("ro/f.txt", "old");
    chmod("ro", 0555);
    int err;
    char *out = tool("write", "{\"path\":\"ro/f.txt\",\"content\":\"new\"}", &err);
    chmod("ro", 0755); /* so the runner can clean up */
    CHECK(!err && !strcmp(get("ro/f.txt"), "new"));
    free(out);
    return 0;
}

TEST(tool_edit_replaces_unique_match) {
    int err;
    put("a.txt", "one\ntwo\n");
    char *out = tool("edit", "{\"path\":\"a.txt\",\"old_string\":\"two\",\"new_string\":\"2\"}", &err);
    CHECK(!err && !strcmp(out, "edited a.txt"));
    CHECK(!strcmp(get("a.txt"), "one\n2\n"));
    free(out);
    return 0;
}

TEST(tool_edit_errors_leave_file_unchanged) {
    static const char *cases[][2] = {
        {"{\"path\":\"a.txt\",\"old_string\":\"x\",\"new_string\":\"y\"}", "more than once"},
        {"{\"path\":\"a.txt\",\"old_string\":\"zz\",\"new_string\":\"y\"}", "not found"},
        {"{\"path\":\"a.txt\",\"old_string\":\"\",\"new_string\":\"y\"}", "empty"},
        {"{\"path\":\"nope\",\"old_string\":\"x\",\"new_string\":\"y\"}", "cannot read"},
        {"{\"path\":\"a.txt\"}", "missing arguments"},
    };
    put("a.txt", "x x\n");
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        int err = 0;
        char *out = tool("edit", cases[i][0], &err);
        CHECK(err && strstr(out, cases[i][1]));
        free(out);
    }
    CHECK(!strcmp(get("a.txt"), "x x\n"));
    return 0;
}

TEST(tool_read_errors) {
    int err = 0;
    FILE *f = fopen("bin", "wb");
    fwrite("a\0b", 1, 3, f);
    fclose(f);
    char *out = tool("read", "{\"path\":\"bin\"}", &err);
    CHECK(err && strstr(out, "binary"));
    free(out);
    out = tool("read", "{\"path\":\"nope\"}", &err);
    CHECK(err && strstr(out, "cannot read"));
    free(out);
    return 0;
}

TEST(tool_shell_status_and_redirects) {
    int err;
    char *out = tool("shell", "{\"command\":\"echo hi; echo err >&2; exit 3 # comment\"}", &err);
    CHECK(err && !strcmp(out, "hi\nerr\n[exit 3]"));
    free(out);
    out = tool("shell", "{\"command\":\"cat\"}", &err); /* stdin is /dev/null: no hang */
    CHECK(!err && !strcmp(out, "[exit 0]"));
    free(out);
    out = tool("shell", "{\"command\":\"printf x\"}", &err);
    CHECK(!err && !strcmp(out, "x\n[exit 0]"));
    free(out);
    return 0;
}

TEST(tool_output_keeps_start_and_end) {
    int err; /* local caps at 16384: the first 3276 bytes, the last 13108 */
    char *out = tool("shell", "{\"command\":\"seq 1 100000\"}", &err);
    CHECK(!err && MYRA_STARTS_WITH(out, "1\n2\n3\n"));
    const char *mark = strstr(out, "\n[... ");
    CHECK(mark && mark - out == 3276);
    CHECK(strstr(mark, " bytes omitted ...]\n"));
    size_t n = strlen(out);
    CHECK(n > 13108 && !strcmp(out + n - 22, "\n99999\n100000\n[exit 0]"));
    free(out);
    return 0;
}

TEST(max_output_env_override) {
    myra_free(&A);
    setenv("MYRA_MAX_OUTPUT", "2048", 1);
    CHECK(local_agent() == 0 && A.max_output == 2048);
    myra_free(&A);
    setenv("MYRA_MAX_OUTPUT", "10", 1); /* too small: ignored */
    CHECK(local_agent() == 0 && A.max_output == 16384);
    myra_free(&A);
    unsetenv("MYRA_MAX_OUTPUT");
    CHECK(myra_init(&A, myra_provider_named("local"), NULL, MYRA_AUTO) == 0 && A.max_output == 16384);
    return 0;
}

/* Peak resident memory in bytes: macOS reports bytes, Linux kilobytes. */
static size_t peak_rss(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return (size_t)ru.ru_maxrss;
#else
    return (size_t)ru.ru_maxrss * 1024;
#endif
}

TEST(shell_output_does_not_grow_memory) {
    int err;
    double t = secs();
    /* 50 MB through the pipe, with a 16 KB cap: the middle is never held. */
    char *out = tool("shell", "{\"command\":\"head -c 50000000 /dev/zero | tr '\\\\0' a\"}", &err);
    CHECK(!err && secs() - t < 30);
    CHECK(peak_rss() < 80u * 1024 * 1024); /* the whole output would be 50 MB */
    CHECK(strspn(out, "a") == 3276 && strstr(out, "\n[... 49983616 bytes omitted ...]\n"));
    CHECK(!strcmp(out + strlen(out) - 9, "\n[exit 0]"));
    free(out);
    return 0;
}

TEST(shell_stops_after_the_raw_output_limit) {
    int err;
    double t = secs();
    char *out = tool("shell", "{\"command\":\"yes hello\"}", &err); /* endless output */
    CHECK(err && secs() - t < 60);
    CHECK(strstr(out, "[stopped after 64 MB of output; killed]"));
    CHECK(peak_rss() < 80u * 1024 * 1024);
    free(out);
    return 0;
}

TEST(read_of_a_huge_file_skips_the_middle) {
    int err;
    FILE *f = fopen("big", "wb"); /* 40 MB: head of a's, tail of z's */
    char *block = malloc(1 << 20);
    memset(block, 'a', 1 << 20);
    for (int i = 0; i < 40; i++) {
        if (i == 39) memset(block, 'z', 1 << 20);
        fwrite(block, 1, 1 << 20, f);
    }
    free(block);
    fclose(f);
    double t = secs();
    char *out = tool("read", "{\"path\":\"big\"}", &err);
    CHECK(!err && secs() - t < 5); /* seeks, rather than reading 40 MB */
    CHECK(peak_rss() < 80u * 1024 * 1024);
    CHECK(strspn(out, "a") == 3276);
    CHECK(strstr(out, "\n[... 41926656 bytes omitted ...]\n"));
    CHECK(out[strlen(out) - 1] == 'z');
    free(out);
    return 0;
}

TEST(read_cap_is_clamped_to_the_raw_limit) {
    /* Above MYRA_MAX_RAW, a file under the cap was read to MYRA_MAX_RAW and cut unmarked. */
    myra_free(&A);
    setenv("MYRA_MAX_OUTPUT", "1000000000", 1);
    CHECK(local_agent() == 0 && A.max_output == MYRA_MAX_RAW);
    FILE *f = fopen("big", "wb"); /* 1 MB over the limit, ending in z */
    char *block = malloc(1 << 20);
    memset(block, 'a', 1 << 20);
    for (int i = 0; i <= MYRA_MAX_RAW >> 20; i++) {
        if (i == MYRA_MAX_RAW >> 20) block[(1 << 20) - 1] = 'z';
        fwrite(block, 1, 1 << 20, f);
    }
    free(block);
    fclose(f);
    int err;
    char *out = tool("read", "{\"path\":\"big\"}", &err);
    CHECK(!err && strstr(out, "\n[... 1048576 bytes omitted ...]\n"));
    CHECK(out[strlen(out) - 1] == 'z');
    free(out);
    return 0;
}

TEST(edit_refuses_a_huge_file) {
    int err;
    FILE *f = fopen("big2", "wb");
    CHECK(fseeko(f, 65 * 1024 * 1024, SEEK_SET) == 0 && fputc('x', f) == 'x');
    fclose(f);
    char *out = tool("edit", "{\"path\":\"big2\",\"old_string\":\"x\",\"new_string\":\"y\"}", &err);
    CHECK(err && strstr(out, "too large to edit"));
    free(out);
    return 0;
}

TEST(tool_unknown) {
    int err = 0;
    char *out = tool("bogus", "{}", &err);
    CHECK(err && strstr(out, "unknown tool"));
    free(out);
    return 0;
}

/* Wait up to 2 s for the pid written to path to disappear. */
static int gone(const char *path) {
    const char *s = get(path);
    pid_t pid = s ? (pid_t)atoi(s) : 0;
    if (pid <= 0) return 0;
    for (int i = 0; i < 20; i++) {
        if (kill(pid, 0) < 0) return 1;
        usleep(100000);
    }
    return 0;
}

static void on_alarm(int sig) {
    (void)sig;
    myra_interrupted = 1;
}

/* Simulate Ctrl-C after one second, the way main.c installs its handler. */
static void interrupt_in_1s(void) {
    struct sigaction sa = {0};
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    alarm(1);
}

TEST(shell_timeout_kills_process_group) {
    int err;
    double t = secs();
    char *out = tool("shell", "{\"command\":\"sleep 30 & echo $! > pid; echo start; wait\","
                              "\"timeout\":1}", &err);
    CHECK(secs() - t < 4);
    CHECK(err && !strcmp(out, "start\n[timed out after 1s; killed]"));
    CHECK(gone("pid")); /* the background child died with sh */
    free(out);
    return 0;
}

TEST(shell_timeout_is_clamped) {
    int err;
    double t = secs();
    char *out = tool("shell", "{\"command\":\"sleep 30\",\"timeout\":0}", &err);
    CHECK(secs() - t < 4 && err && !strcmp(out, "[timed out after 1s; killed]"));
    free(out);
    out = tool("shell", "{\"command\":\"echo ok\",\"timeout\":\"soon\"}", &err); /* default */
    CHECK(!err && !strcmp(out, "ok\n[exit 0]"));
    free(out);
    t = secs(); /* out of int range: clamped, not undefined (make asan fails on UB) */
    out = tool("shell", "{\"command\":\"sleep 30\",\"timeout\":-1e20}", &err);
    CHECK(secs() - t < 4 && !strcmp(out, "[timed out after 1s; killed]"));
    free(out);
    const char *huge[] = {"1e20", "1e999"}; /* 1e999 parses as infinity */
    for (int i = 0; i < 2; i++) {
        char cmd[64];
        snprintf(cmd, sizeof cmd, "{\"command\":\"true\",\"timeout\":%s}", huge[i]);
        out = tool("shell", cmd, &err);
        CHECK(!err && !strcmp(out, "[exit 0]"));
        free(out);
    }
    t = secs(); /* fractions are truncated */
    out = tool("shell", "{\"command\":\"sleep 30\",\"timeout\":2.9}", &err);
    CHECK(secs() - t < 5 && !strcmp(out, "[timed out after 2s; killed]"));
    free(out);
    return 0;
}

TEST(shell_background_job_with_redirect_returns) {
    int err;
    double t = secs();
    char *out = tool("shell", "{\"command\":\"sleep 30 >/dev/null 2>&1 & echo $! > pid; echo ok\"}",
                     &err);
    CHECK(secs() - t < 2 && !err && !strcmp(out, "ok\n[exit 0]"));
    free(out);
    pid_t pid = (pid_t)atoi(get("pid"));
    CHECK(pid > 0 && kill(pid, 0) == 0); /* still running: not ours to kill */
    kill(pid, SIGKILL);
    return 0;
}

TEST(shell_interrupt_kills_process_group) {
    int err;
    interrupt_in_1s();
    double t = secs();
    char *out = tool("shell", "{\"command\":\"sleep 30 & echo $! > pid; echo a; wait\"}", &err);
    CHECK(secs() - t < 4);
    CHECK(err && !strcmp(out, "a\n[interrupted by user; killed]"));
    CHECK(gone("pid"));
    free(out);
    return 0;
}

TEST(step_interrupt_skips_remaining_calls) {
    interrupt_in_1s();
    CHECK(step("{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":["
               "{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"shell\","
               "\"arguments\":\"{\\\"command\\\":\\\"sleep 30\\\"}\"}},"
               "{\"id\":\"c2\",\"type\":\"function\",\"function\":{\"name\":\"shell\","
               "\"arguments\":\"{\\\"command\\\":\\\"touch y\\\"}\"}}]},"
               "\"finish_reason\":\"tool_calls\"}]}") == 0); /* turn ends: no next request */
    CHECK(cJSON_GetArraySize(A.messages) == 4);
    CHECK(!strcmp(field(msg(2), "content"), "error: [interrupted by user; killed]"));
    CHECK(!strcmp(field(msg(3), "tool_call_id"), "c2"));
    CHECK(!strcmp(field(msg(3), "content"), "error: skipped: interrupted by user"));
    CHECK(access("y", F_OK) != 0);
    return 0;
}

#define FFFD "\xEF\xBF\xBD"

/* Run printf with fmt in the shell tool and compare the result. */
static int printf_gives(const char *fmt_, const char *want) {
    char cmd[256];
    snprintf(cmd, sizeof cmd, "{\"command\":\"printf '%s'\"}", fmt_);
    int err;
    char *out = tool("shell", cmd, &err);
    int ok = !err && !strcmp(out, want);
    if (!ok) fprintf(stderr, "printf '%s' gave [%s]\n", fmt_, out);
    free(out);
    return ok;
}

TEST(utf8_invalid_bytes_are_replaced) {
    CHECK(printf_gives("\\\\377\\\\376ok", FFFD FFFD "ok\n[exit 0]"));    /* stray bytes */
    CHECK(printf_gives("a\\\\000b", "a" FFFD "b\n[exit 0]"));               /* NUL no longer truncates */
    CHECK(printf_gives("\\\\300\\\\257", FFFD FFFD "\n[exit 0]"));         /* overlong '/' */
    CHECK(printf_gives("\\\\355\\\\240\\\\200", FFFD FFFD FFFD "\n[exit 0]")); /* surrogate */
    CHECK(printf_gives("x\\\\342\\\\202", "x" FFFD FFFD "\n[exit 0]"));    /* cut-off sequence */
    CHECK(printf_gives("\\\\364\\\\220\\\\200\\\\200", FFFD FFFD FFFD FFFD "\n[exit 0]")); /* > U+10FFFF */
    return 0;
}

TEST(utf8_valid_text_is_unchanged) {
    CHECK(printf_gives("h\xc3\xa9llo \xe2\x82\xac \xf0\x9d\x84\x9e", /* é, €, U+1D11E */
                       "h\xc3\xa9llo \xe2\x82\xac \xf0\x9d\x84\x9e\n[exit 0]"));
    return 0;
}

TEST(utf8_read_replaces_latin1) {
    put("l1.txt", "caf\xe9\n");
    int err;
    char *out = tool("read", "{\"path\":\"l1.txt\"}", &err);
    CHECK(!err && !strcmp(out, "caf" FFFD "\n"));
    free(out);
    return 0;
}

TEST(utf8_truncation_does_not_split_a_character) {
    /* Both cut points (byte 3276, and 13108 from the end) fall inside a euro sign. */
    int err;
    char *out = tool("shell", "{\"command\":\""
                              "head -c 3275 /dev/zero | tr '\\\\0' a; printf '\\\\342\\\\202\\\\254'; "
                              "head -c 20000 /dev/zero | tr '\\\\0' b; printf '\\\\342\\\\202\\\\254'; "
                              "head -c 13107 /dev/zero | tr '\\\\0' z\"}", &err);
    CHECK(!err);
    CHECK(strspn(out, "a") == 3275);
    const char *note = "\n[... 20006 bytes omitted ...]\n";
    CHECK(!strncmp(out + 3275, note, strlen(note)));
    const char *rest = out + 3275 + strlen(note);
    CHECK(strspn(rest, "z") == 13107 && !strcmp(rest + 13107, "\n[exit 0]"));
    free(out);
    return 0;
}

TEST(truncation_keeps_the_head_after_an_invalid_byte) {
    /* The head was cut at its first invalid byte, not just at a trailing partial character. */
    int err;
    char *out = tool("shell", "{\"command\":\"printf 'x\\\\377y'; "
                              "head -c 20000 /dev/zero | tr '\\\\0' b\"}", &err);
    CHECK(!err && !strncmp(out, "x" FFFD "y", 5));
    CHECK(strspn(out + 5, "b") == 3276 - 3); /* the rest of the 3276-byte head */
    CHECK(strstr(out, "\n[... 3619 bytes omitted ...]\n")); /* 20003 - 3276 - 13108 */
    free(out);
    return 0;
}

/* ---- state ---- */

TEST(state_roundtrip) {
    CHECK(!myra_load_state("x"));
    myra_store_state("x", "value");
    CHECK(!strcmp(get("state/myra/x"), "value\n"));
    char *v = myra_load_state("x");
    CHECK(v && !strcmp(v, "value"));
    free(v);
    put("state/myra/x", " \n");
    CHECK(!myra_load_state("x"));
    return 0;
}

TEST(state_falls_back_to_home) {
    char cwd[4096];
    CHECK(getcwd(cwd, sizeof cwd));
    unsetenv("XDG_STATE_HOME");
    setenv("HOME", cwd, 1);
    myra_store_state("y", "v");
    CHECK(!strcmp(get(".local/state/myra/y"), "v\n"));
    unsetenv("HOME");
    CHECK(!myra_load_state("y"));
    return 0;
}

/* ---- providers and session ---- */

TEST(provider_selection) {
    CHECK(myra_provider_named("local") && !myra_provider_named("anthropic"));
    CHECK(!myra_provider_default());
    setenv("OPENROUTER_API_KEY", "", 1);
    CHECK(!myra_provider_default());
    setenv("OPENROUTER_API_KEY", "k", 1);
    CHECK(myra_provider_default() == myra_provider_named("openrouter"));
    return 0;
}

TEST(provider_default_prefers_usable_remembered) {
    const myra_provider *local = myra_provider_named("local");
    const myra_provider *cloud = myra_provider_named("openrouter");
    myra_store_state("provider", "local");
    CHECK(myra_provider_default() == local); /* no key needed */
    setenv("OPENROUTER_API_KEY", "k", 1);
    CHECK(myra_provider_default() == local); /* remembered beats the key rule */
    myra_store_state("provider", "openrouter");
    CHECK(myra_provider_default() == cloud);
    unsetenv("OPENROUTER_API_KEY");
    CHECK(!myra_provider_default()); /* unusable, and local is never the fallback */
    myra_store_state("provider", "anthropic");
    setenv("OPENROUTER_API_KEY", "k", 1);
    CHECK(myra_provider_default() == cloud); /* unknown name: key rule */
    return 0;
}

TEST(init_requires_cloud_key) {
    CHECK(myra_init(&A, myra_provider_named("openrouter"), NULL, MYRA_AUTO) == -1);
    setenv("OPENROUTER_API_KEY", "k", 1);
    CHECK(myra_init(&A, myra_provider_named("openrouter"), NULL, MYRA_AUTO) == 0);
    CHECK(!strcmp(A.api_key, "k") && !strcmp(A.model, "anthropic/claude-opus-5"));
    myra_free(&A);
    return 0;
}

TEST(init_model_precedence) {
    const myra_provider *p = myra_provider_named("local");
    CHECK(myra_init(&A, p, NULL, MYRA_AUTO) == 0 && !strcmp(A.model, "local") && !A.save_model);
    myra_free(&A);
    myra_store_state("local.model", "qwen");
    CHECK(myra_init(&A, p, NULL, MYRA_AUTO) == 0 && !strcmp(A.model, "qwen") && !A.save_model);
    myra_free(&A);
    CHECK(myra_init(&A, p, "qwen", MYRA_AUTO) == 0 && !A.save_model); /* same: no rewrite */
    myra_free(&A);
    CHECK(myra_init(&A, p, "llama", MYRA_AUTO) == 0 && !strcmp(A.model, "llama") && A.save_model);
    myra_set_model(&A, "gemma");
    CHECK(!strcmp(A.model, "gemma") && A.save_model);
    return 0;
}

TEST(init_builds_system_message_and_tools) {
    CHECK(cJSON_GetArraySize(A.messages) == 1 && !strcmp(field(msg(0), "role"), "system"));
    CHECK(strstr(field(msg(0), "content"), "coding agent"));
    CHECK(cJSON_GetArraySize(A.tools) == 4);
    cJSON *fn = cJSON_GetObjectItem(cJSON_GetArrayItem(A.tools, 3), "function");
    CHECK(!strcmp(field(fn, "name"), "shell"));
    return 0;
}

/* ---- the loop ---- */

TEST(step_text_reply_finishes) {
    CHECK(step("{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"hi\"},"
               "\"finish_reason\":\"stop\"}]}") == 0);
    CHECK(cJSON_GetArraySize(A.messages) == 2 && !strcmp(field(msg(1), "content"), "hi"));
    return 0;
}

TEST(step_runs_tool_calls) {
    put("f", "data");
    CHECK(step("{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":["
               "{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"read\","
               "\"arguments\":\"{\\\"path\\\":\\\"f\\\"}\"}},"
               "{\"id\":\"c2\",\"type\":\"function\",\"function\":{\"name\":\"shell\","
               "\"arguments\":\"{\\\"command\\\": \"}}]},\"finish_reason\":\"tool_calls\"}]}") == 1);
    CHECK(cJSON_GetArraySize(A.messages) == 4);
    CHECK(cJSON_IsNull(cJSON_GetObjectItem(msg(1), "content")));
    CHECK(!strcmp(field(msg(2), "role"), "tool") && !strcmp(field(msg(2), "tool_call_id"), "c1"));
    CHECK(!strcmp(field(msg(2), "content"), "data"));
    CHECK(!strcmp(field(msg(3), "content"), "error: invalid JSON arguments for shell"));
    return 0;
}

TEST(step_generates_missing_call_ids) {
    put("f", "data");
    CHECK(step("{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":["
               "{\"type\":\"function\",\"function\":{\"name\":\"read\",\"arguments\":\"{\\\"path\\\":\\\"f\\\"}\"}},"
               "{\"id\":null,\"type\":\"function\",\"function\":{\"name\":\"read\",\"arguments\":\"{\\\"path\\\":\\\"f\\\"}\"}},"
               "{\"id\":\"srv\",\"type\":\"function\",\"function\":{\"name\":\"read\",\"arguments\":\"{\\\"path\\\":\\\"f\\\"}\"}}"
               "]},\"finish_reason\":\"tool_calls\"}]}") == 1);
    cJSON *calls = cJSON_GetObjectItem(msg(1), "tool_calls");
    const char *want[] = {"myra_call_0", "myra_call_1", "srv"};
    for (int i = 0; i < 3; i++) { /* each call and its result carry the same id */
        CHECK(!strcmp(field(cJSON_GetArrayItem(calls, i), "id"), want[i]));
        CHECK(!strcmp(field(msg(2 + i), "tool_call_id"), want[i]));
        CHECK(!strcmp(field(msg(2 + i), "content"), "data"));
    }
    return 0;
}

TEST(step_length_drops_tool_calls) {
    CHECK(step("{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"part\",\"tool_calls\":["
               "{\"id\":\"c\",\"type\":\"function\",\"function\":{\"name\":\"shell\","
               "\"arguments\":\"{\\\"command\\\":\\\"touch x\\\"}\"}}]},"
               "\"finish_reason\":\"length\"}]}") == 0);
    CHECK(cJSON_GetArraySize(A.messages) == 2 && !cJSON_GetObjectItem(msg(1), "tool_calls"));
    CHECK(access("x", F_OK) != 0);
    return 0;
}

TEST(step_null_content_without_calls_becomes_empty) {
    CHECK(step("{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null},"
               "\"finish_reason\":\"stop\"}]}") == 0);
    CHECK(!strcmp(field(msg(1), "content"), ""));
    return 0;
}

TEST(step_failures_leave_history_alone) {
    CHECK(step("{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
               "\"finish_reason\":\"content_filter\"}]}") == -1);
    CHECK(step("{\"error\":{\"message\":\"x\"}}") == -1);
    CHECK(step("{\"choices\":[]}") == -1);
    CHECK(cJSON_GetArraySize(A.messages) == 1);
    return 0;
}

TEST(clear_keeps_only_system_prompt) {
    put("f", "data");
    CHECK(step("{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":["
               "{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"read\","
               "\"arguments\":\"{\\\"path\\\":\\\"f\\\"}\"}}]},\"finish_reason\":\"tool_calls\"}]}") == 1);
    CHECK(cJSON_GetArraySize(A.messages) == 3);
    myra_clear(&A);
    CHECK(cJSON_GetArraySize(A.messages) == 1 && !strcmp(field(msg(0), "role"), "system"));
    myra_clear(&A); /* idempotent */
    CHECK(cJSON_GetArraySize(A.messages) == 1);
    return 0;
}

/* ---- REPL command parsing ---- */

/* A port with nothing listening, so a request fails at once. */
static int closed_port(void) {
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t len = sizeof sa;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0 || bind(s, (struct sockaddr *)&sa, sizeof sa) < 0 ||
        getsockname(s, (struct sockaddr *)&sa, &len) < 0)
        return -1;
    close(s);
    return ntohs(sa.sin_port);
}

TEST(stale_context_full_does_not_drop_tool_output) {
    /* context_full outlived the failed request that set it, so an unrelated later failure
       blanked old tool output and retried. */
    int port = closed_port();
    CHECK(port > 0);
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/v1", port);
    setenv("LOCAL_BASE_URL", url, 1);
    put("f", "data");
    CHECK(step("{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":["
               "{\"id\":\"c1\",\"type\":\"function\",\"function\":{\"name\":\"read\","
               "\"arguments\":\"{\\\"path\\\":\\\"f\\\"}\"}}]},\"finish_reason\":\"tool_calls\"}]}") == 1);
    A.context_full = 1; /* as an earlier context error left it */
    CHECK(myra_ask(&A, "next") == -1);
    CHECK(!strcmp(field(msg(2), "content"), "data"));
    return 0;
}

TEST(command_parsing) {
    CHECK(!strcmp(myra_command("/model", "/model"), ""));
    CHECK(!strcmp(myra_command("/model   qwen", "/model"), "qwen"));
    CHECK(!myra_command("/models", "/model"));
    CHECK(!myra_command("/modelling", "/model"));
    CHECK(!myra_command("model", "/model"));
    CHECK(!strcmp(myra_command("/models claude", "/models"), "claude"));
    return 0;
}

/* ---- runner ---- */

/* T(name, needs_agent): the ctest name comes from the function, so they cannot differ.
   A TEST missing here is an unused static function, which -Wall reports. */
#define T(name, needs_agent) {#name, test_##name, needs_agent}
static const struct { const char *name; int (*fn)(void); int needs_agent; } TESTS[] = {
    T(tool_write_then_read, 1),
    T(write_keeps_mode_and_follows_symlinks, 1),
    T(write_refuses_a_dangling_symlink, 1),
    T(edit_refuses_a_file_with_nul_bytes, 1),
    T(failed_write_keeps_the_original, 1),
    T(write_in_place_when_directory_is_read_only, 1),
    T(tool_edit_replaces_unique_match, 1),
    T(tool_edit_errors_leave_file_unchanged, 1),
    T(tool_read_errors, 1),
    T(tool_shell_status_and_redirects, 1),
    T(tool_output_keeps_start_and_end, 1),
    T(max_output_env_override, 1),
    T(tool_unknown, 1),
    T(shell_output_does_not_grow_memory, 1),
    T(shell_stops_after_the_raw_output_limit, 1),
    T(read_of_a_huge_file_skips_the_middle, 1),
    T(read_cap_is_clamped_to_the_raw_limit, 1),
    T(edit_refuses_a_huge_file, 1),
    T(utf8_invalid_bytes_are_replaced, 1),
    T(utf8_valid_text_is_unchanged, 1),
    T(utf8_read_replaces_latin1, 1),
    T(utf8_truncation_does_not_split_a_character, 1),
    T(truncation_keeps_the_head_after_an_invalid_byte, 1),
    T(shell_timeout_kills_process_group, 1),
    T(shell_timeout_is_clamped, 1),
    T(shell_background_job_with_redirect_returns, 1),
    T(shell_interrupt_kills_process_group, 1),
    T(step_interrupt_skips_remaining_calls, 1),
    T(state_roundtrip, 0),
    T(state_falls_back_to_home, 0),
    T(provider_selection, 0),
    T(provider_default_prefers_usable_remembered, 0),
    T(init_requires_cloud_key, 0),
    T(init_model_precedence, 0),
    T(init_builds_system_message_and_tools, 1),
    T(step_text_reply_finishes, 1),
    T(step_runs_tool_calls, 1),
    T(step_generates_missing_call_ids, 1),
    T(step_length_drops_tool_calls, 1),
    T(step_null_content_without_calls_becomes_empty, 1),
    T(step_failures_leave_history_alone, 1),
    T(clear_keeps_only_system_prompt, 1),
    T(stale_context_full_does_not_drop_tool_output, 1),
    T(command_parsing, 0),
};

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s NAME\n", argv[0]);
        return 2;
    }
    for (size_t i = 0; i < sizeof TESTS / sizeof *TESTS; i++) {
        if (strcmp(argv[1], TESTS[i].name)) continue;
        const char *tmp = getenv("TMPDIR");
        char dir[4096];
        snprintf(dir, sizeof dir, "%s/myra-unit-XXXXXX", tmp && *tmp ? tmp : "/tmp");
        if (!mkdtemp(dir) || chdir(dir) < 0) { perror(dir); return 1; }
        char state[4200];
        snprintf(state, sizeof state, "%s/state", dir);
        setenv("XDG_STATE_HOME", state, 1);
        for (size_t j = 0; j < MYRA_NPROVIDERS; j++) unsetenv(MYRA_PROVIDERS[j].key_env);
        if (TESTS[i].needs_agent && local_agent() < 0) return 1;
        int rc = TESTS[i].fn();
        myra_free(&A);
        char rm[4200];
        snprintf(rm, sizeof rm, "rm -rf '%s'", dir);
        if (system(rm) != 0) fprintf(stderr, "warning: could not remove %s\n", dir);
        return rc;
    }
    fprintf(stderr, "unknown test %s\n", argv[1]);
    return 2;
}
