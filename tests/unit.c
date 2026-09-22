/* unit.c - agentlib unit tests. Usage: unit NAME. Each case runs in a fresh temp dir. */
#include "agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static agent A;

static int local_agent(void) { return agent_init(&A, agent_provider_named("local"), NULL, 1); }

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
    char *out = agent_run_tool(&A, name, in, err);
    cJSON_Delete(in);
    return out;
}

static int step(const char *json) {
    cJSON *r = cJSON_Parse(json);
    int rc = agent_step(&A, r);
    cJSON_Delete(r);
    return rc;
}

static cJSON *msg(int i) { return cJSON_GetArrayItem(A.messages, i); }
static const char *field(cJSON *o, const char *k) {
    return cJSON_GetStringValue(cJSON_GetObjectItem(o, k));
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

TEST(tool_output_is_capped) {
    int err;
    char *out = tool("shell", "{\"command\":\"head -c 300000 /dev/zero | tr '\\\\0' a\"}", &err);
    CHECK(!err);
    CHECK(strspn(out, "a") == 102400);
    CHECK(!strncmp(out + 102400, "\n[truncated: 300000 bytes total", 31));
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

/* ---- state ---- */

TEST(state_roundtrip) {
    CHECK(!agent_load_state("x"));
    agent_store_state("x", "value");
    CHECK(!strcmp(get("state/ant/x"), "value\n"));
    char *v = agent_load_state("x");
    CHECK(v && !strcmp(v, "value"));
    free(v);
    put("state/ant/x", " \n");
    CHECK(!agent_load_state("x"));
    return 0;
}

TEST(state_falls_back_to_home) {
    char cwd[4096];
    CHECK(getcwd(cwd, sizeof cwd));
    unsetenv("XDG_STATE_HOME");
    setenv("HOME", cwd, 1);
    agent_store_state("y", "v");
    CHECK(!strcmp(get(".local/state/ant/y"), "v\n"));
    unsetenv("HOME");
    CHECK(!agent_load_state("y"));
    return 0;
}

/* ---- providers and session ---- */

TEST(provider_selection) {
    CHECK(agent_provider_named("local") && !agent_provider_named("anthropic"));
    CHECK(!agent_provider_default());
    setenv("OPENROUTER_API_KEY", "", 1);
    CHECK(!agent_provider_default());
    setenv("OPENROUTER_API_KEY", "k", 1);
    CHECK(agent_provider_default() == agent_provider_named("openrouter"));
    return 0;
}

TEST(init_requires_cloud_key) {
    CHECK(agent_init(&A, agent_provider_named("openrouter"), NULL, 0) == -1);
    setenv("OPENROUTER_API_KEY", "k", 1);
    CHECK(agent_init(&A, agent_provider_named("openrouter"), NULL, 0) == 0);
    CHECK(!strcmp(A.api_key, "k") && !strcmp(A.model, "anthropic/claude-opus-5"));
    agent_free(&A);
    return 0;
}

TEST(init_model_precedence) {
    const agent_provider *p = agent_provider_named("local");
    CHECK(agent_init(&A, p, NULL, 0) == 0 && !strcmp(A.model, "local") && !A.save_model);
    agent_free(&A);
    agent_store_state("local.model", "qwen");
    CHECK(agent_init(&A, p, NULL, 0) == 0 && !strcmp(A.model, "qwen") && !A.save_model);
    agent_free(&A);
    CHECK(agent_init(&A, p, "qwen", 0) == 0 && !A.save_model); /* same: no rewrite */
    agent_free(&A);
    CHECK(agent_init(&A, p, "llama", 0) == 0 && !strcmp(A.model, "llama") && A.save_model);
    agent_set_model(&A, "gemma");
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

/* ---- REPL command parsing ---- */

TEST(command_parsing) {
    CHECK(!strcmp(agent_command("/model", "/model"), ""));
    CHECK(!strcmp(agent_command("/model   qwen", "/model"), "qwen"));
    CHECK(!agent_command("/models", "/model"));
    CHECK(!agent_command("/modelling", "/model"));
    CHECK(!agent_command("model", "/model"));
    CHECK(!strcmp(agent_command("/models claude", "/models"), "claude"));
    return 0;
}

/* ---- runner ---- */

static const struct { const char *name; int (*fn)(void); int needs_agent; } TESTS[] = {
    {"tool_write_then_read", test_tool_write_then_read, 1},
    {"tool_edit_replaces_unique_match", test_tool_edit_replaces_unique_match, 1},
    {"tool_edit_errors_leave_file_unchanged", test_tool_edit_errors_leave_file_unchanged, 1},
    {"tool_read_errors", test_tool_read_errors, 1},
    {"tool_shell_status_and_redirects", test_tool_shell_status_and_redirects, 1},
    {"tool_output_is_capped", test_tool_output_is_capped, 1},
    {"tool_unknown", test_tool_unknown, 1},
    {"state_roundtrip", test_state_roundtrip, 0},
    {"state_falls_back_to_home", test_state_falls_back_to_home, 0},
    {"provider_selection", test_provider_selection, 0},
    {"init_requires_cloud_key", test_init_requires_cloud_key, 0},
    {"init_model_precedence", test_init_model_precedence, 0},
    {"init_builds_system_message_and_tools", test_init_builds_system_message_and_tools, 1},
    {"step_text_reply_finishes", test_step_text_reply_finishes, 1},
    {"step_runs_tool_calls", test_step_runs_tool_calls, 1},
    {"step_length_drops_tool_calls", test_step_length_drops_tool_calls, 1},
    {"step_null_content_without_calls_becomes_empty",
     test_step_null_content_without_calls_becomes_empty, 1},
    {"step_failures_leave_history_alone", test_step_failures_leave_history_alone, 1},
    {"command_parsing", test_command_parsing, 0},
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
        snprintf(dir, sizeof dir, "%s/agent-unit-XXXXXX", tmp && *tmp ? tmp : "/tmp");
        if (!mkdtemp(dir) || chdir(dir) < 0) { perror(dir); return 1; }
        char state[4200];
        snprintf(state, sizeof state, "%s/state", dir);
        setenv("XDG_STATE_HOME", state, 1);
        for (size_t j = 0; j < AGENT_NPROVIDERS; j++) unsetenv(AGENT_PROVIDERS[j].key_env);
        if (TESTS[i].needs_agent && local_agent() < 0) return 1;
        int rc = TESTS[i].fn();
        agent_free(&A);
        char rm[4200];
        snprintf(rm, sizeof rm, "rm -rf '%s'", dir);
        if (system(rm) != 0) fprintf(stderr, "warning: could not remove %s\n", dir);
        return rc;
    }
    fprintf(stderr, "unknown test %s\n", argv[1]);
    return 2;
}
