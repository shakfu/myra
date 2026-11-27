/* app.c - libmyra-ui's entry point: run the window on an agent the caller set up.
   Threads: the UI thread runs the frame (view.c); one worker at a time runs a turn or
   a model fetch (worker.c). */
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ui_run(app *s) {
    tr_init(&s->tr);
    cui_log_init(&s->log, s->log_lines, sizeof s->log_lines / sizeof *s->log_lines, s->log_text,
                 sizeof s->log_text);
    s->started_at = tr_now();
    s->show_files = s->show_log = true;
    s->model_index = -1;
    char *theme = myra_load_state("ui.theme"), *layout = myra_load_state("ui.layout");
    s->dark = theme && !strcmp(theme, "dark");
    if (layout) snprintf(s->layout, sizeof s->layout, "%s", layout);
    free(theme), free(layout);
    worker_install(s);
    if (s->draft[0]) s->act = ACT_SEND, view_act(s);

    int rc = cui_app_run(&(cui_app_desc){.title = "myra", .width = 1280, .height = 820,
                                         .theme = s->dark ? CUI_THEME_DARK : CUI_THEME_LIGHT},
                         view_frame, s);
    worker_shutdown(s);
    myra_note_hook = NULL;
    myra_free_model_ids(s->models);
    myra_free(&s->agent);
    tr_free(&s->tr);
    return rc;
}
