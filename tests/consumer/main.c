/* main.c - calls into libmyra, its bundled cJSON and libmyra-term through the installed
   headers only, and runs an agent whose one tool is its own. */
#include <myra/term.h>

#include <stdlib.h>
#include <string.h>

static char *run_upper(myra_agent *a, const myra_tool *t, cJSON *input, int *err) {
    (void)a, (void)t;
    char *s = strdup(cJSON_GetStringValue(cJSON_GetObjectItem(input, "text")));
    for (char *p = s; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p -= 32;
    *err = 0;
    return s;
}

static const myra_tool UPPER = {"upper", "Upper-case text.",
                                "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},"
                                "\"required\":[\"text\"]}",
                                "text", MYRA_TOOL_READS, run_upper, NULL};

int main(void) {
    const myra_provider *p = myra_provider_named("local");
    cJSON *j = cJSON_Parse("{\"a\":1}");
    char *u = myra_utf8("ok");
    int ok = p && !strcmp(p->name, "local") && cJSON_IsNumber(cJSON_GetObjectItem(j, "a")) &&
             !strcmp(u, "ok") && !strcmp(myra_command("/model x", "/model"), "x");
    char *e = myra_escape(NULL, "a\033b", 0);
    ok = ok && !strcmp(e, "a^[b");
    free(e);
    free(u);
    cJSON_Delete(j);

    myra_agent a;
    ok = ok && myra_init(&a, p, NULL, MYRA_AUTO) == 0 && myra_add_tool(&a, &UPPER) == 0;
    cJSON *in = cJSON_Parse("{\"text\":\"hi\"}");
    int err = 1;
    char *out = ok ? myra_run_tool(&a, "upper", in, &err) : NULL;
    char *shell = ok ? myra_run_tool(&a, "shell", in, &err) : NULL; /* not offered */
    ok = ok && out && !strcmp(out, "HI") && shell && strstr(shell, "unknown tool");
    free(out), free(shell);
    cJSON_Delete(in);
    myra_free(&a);
    return ok ? 0 : 1;
}
