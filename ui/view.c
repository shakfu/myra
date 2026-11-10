/* view.c - the frame: header, dockable chat, files and log panels, dialogs and the
   command palette. It reads the transcript under its lock and calls only CUI;
   anything that calls into myra is queued as s->act for view_act. */
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const PERMISSION_NAMES[] = {"Ask before changes", "Auto", "All", "Read only"};
static const myra_permissions PERMISSIONS[] = {MYRA_ASK, MYRA_AUTO, MYRA_ALL, MYRA_READ_ONLY};
#define NPERMISSIONS 4

static int permission_index(myra_permissions p) {
    for (int i = 0; i < NPERMISSIONS; i++)
        if (PERMISSIONS[i] == p) return i;
    return 1;
}

static const char *tool_icon(const char *name) {
    return !strcmp(name, "read")    ? CUI_ICON_FILE_TEXT
           : !strcmp(name, "shell") ? CUI_ICON_TERMINAL
           : !strcmp(name, "write") || !strcmp(name, "edit") ? CUI_ICON_PENCIL
                                    : CUI_ICON_LAYERS; /* an MCP tool */
}

static cui_task_status task_status(tool_status st) {
    return st == TOOL_PENDING   ? CUI_TASK_PENDING
           : st == TOOL_RUNNING ? CUI_TASK_RUNNING
           : st == TOOL_DONE    ? CUI_TASK_DONE
                                : CUI_TASK_FAILED;
}

/* Syntax colors for a path, in the names cui_diff_view and cui_code_block take. */
static const char *language(const char *path) {
    static const char *const map[][2] = {
        {".c", "c"},   {".h", "c"},        {".cc", "cpp"}, {".cpp", "cpp"}, {".hpp", "cpp"},
        {".js", "js"}, {".ts", "ts"},      {".py", "python"}, {".rs", "rust"}, {".go", "go"},
        {".json", "json"}, {".sh", "shell"},
    };
    const char *dot = path ? strrchr(path, '.') : NULL;
    for (size_t i = 0; dot && i < sizeof map / sizeof *map; i++)
        if (!strcmp(dot, map[i][0])) return map[i][1];
    return NULL;
}

/* path relative to cwd when it is inside it. */
static const char *relative(const app *s, const char *path) {
    size_t n = strlen(s->cwd);
    return !strncmp(path, s->cwd, n) && path[n] == '/' ? path + n + 1 : path;
}

/* ---- notes into the log panel ---- */

static cui_log_level level_of(int style) {
    return style == MYRA_ERROR ? CUI_LOG_ERROR
           : style == MYRA_WARN ? CUI_LOG_WARN
           : style == MYRA_DIM  ? CUI_LOG_DEBUG
                                : CUI_LOG_INFO;
}

/* Copy notes not yet seen into the log; errors also show as a toast. tr is locked. */
static void take_notes(cui_ctx *ui, app *s) {
    for (; s->notes_seen < s->tr.nnotes; s->notes_seen++) {
        const note *n = &s->tr.notes[s->notes_seen];
        cui_log_append_text(&s->log, level_of(n->style), n->time - s->started_at, n->text, -1);
        if (n->style == MYRA_ERROR) cui_toast(ui, n->text, .tone = CUI_TONE_DANGER);
    }
}

/* ---- chat ---- */

static void tool_item(cui_ctx *ui, const item *it) {
    double end = it->t1 ? it->t1 : tr_now();
    cui_tool_call(ui, "tool", it->name, .icon = tool_icon(it->name), .detail = it->detail,
                  .status = task_status(it->status), .input = it->input,
                  .output = it->t1 ? it->text : NULL,
                  .seconds = it->status == TOOL_PENDING ? 0 : (float)(end - it->t0));
    if (it->after && it->status != TOOL_FAILED)
        cui_diff_view(ui, "diff", it->before, it->after, .mode = CUI_DIFF_INLINE,
                      .language = language(it->detail), .new_title = it->detail, .height = 0);
}

static const cui_prompt_suggestion SUGGESTIONS[] = {
    {CUI_ICON_FILE_TEXT, "Summarize this project", "What it does and how it is laid out"},
    {CUI_ICON_TERMINAL, "Run the tests", "Then explain any failure"},
    {CUI_ICON_SEARCH, "Find the TODOs", "And rank them by effort"},
};

static void messages(cui_ctx *ui, app *s, bool busy) {
    const transcript *tr = &s->tr;
    if (!tr->n) {
        cui_empty_state(ui, "Ask myra about this folder", .icon = CUI_ICON_MESSAGE_SQUARE,
                        .description = "It can read, write and edit files, and run shell commands.");
        int pick = cui_prompt_suggestions(ui, "suggest", SUGGESTIONS, 3, .columns = 3);
        if (pick >= 0) snprintf(s->draft, sizeof s->draft, "%s. %s.", SUGGESTIONS[pick].title, SUGGESTIONS[pick].detail);
    }
    for (int i = 0; i < tr->n; i++) {
        const item *it = &tr->items[i];
        cui_push_id_int(ui, i);
        switch (it->kind) {
        case ITEM_USER: cui_chat_message(ui, it->text, .mine = true); break;
        case ITEM_REPLY:
            cui_chat_message(ui, it->text, .markdown = true);
            /* a turn's final reply: the next item is the user's, or the turn is over */
            if (i + 1 < tr->n ? tr->items[i + 1].kind == ITEM_USER : !busy)
                cui_copy_button(ui, "copy", it->text);
            break;
        case ITEM_REASONING: {
            double end = it->t1 ? it->t1 : tr_now();
            cui_reasoning(ui, "reasoning", .thinking = !it->t1, .seconds = (float)(end - it->t0)) {
                cui_text(ui, it->text, .size = 13.5f, .color = CUI_COLOR_TEXT_SECONDARY);
            }
            break;
        }
        case ITEM_TOOL: tool_item(ui, it); break;
        case ITEM_ERROR: cui_alert(ui, it->text, .tone = CUI_TONE_DANGER); break;
        }
        cui_pop_id(ui);
    }
    const item *last = tr->n ? &tr->items[tr->n - 1] : NULL;
    if (busy && s->job == JOB_TURN && !tr->asking && (!last || last->kind == ITEM_USER || last->t1))
        cui_typing_indicator(ui, "myra");
}

static void chat(cui_ctx *ui, app *s, bool busy) {
    if (s->tr.version != s->seen) {
        s->seen = s->tr.version;
        cui_scroll_to(ui, "chat", 1e9f); /* clamped to the bottom */
    }
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) {
        cui_scroll(ui, .id = "chat", .width = CUI_FILL, .height = CUI_FILL, .fade_edges = true) {
            cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_CENTER, .pad = 24) {
                cui_column(ui, .width = CUI_FILL, .max_width = 860, .gap = 12) { messages(ui, s, busy); }
            }
        }
        cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_CENTER, .pad_x = 24, .pad_bottom = 16) {
            int ev = cui_chat_composer(ui, "composer", s->draft, sizeof s->draft,
                                       .busy = busy && s->job == JOB_TURN,
                                       .placeholder = "Ask myra   (" CUI_ICON_COMMAND "K for commands)",
                                       .width = CUI_PX(860));
            if ((ev & CUI_COMPOSER_SEND) && !busy) s->act = ACT_SEND;
            if (ev & CUI_COMPOSER_STOP) myra_interrupted = 1;
            if (ev & CUI_COMPOSER_ATTACH) s->show_files = true;
        }
    }
}

/* ---- panels ---- */

static void files(cui_ctx *ui, app *s) {
    cui_file_browser_result r = cui_file_browser(ui, "files", s->browse, sizeof s->browse,
                                                 .height = CUI_FILL, .no_places = true);
    if (r.picked) { /* add the file to the prompt, as myra's tools take paths */
        size_t n = strlen(s->draft);
        snprintf(s->draft + n, sizeof s->draft - n, "%s%s", n && s->draft[n - 1] != ' ' ? " " : "",
                 relative(s, r.picked));
    }
}

static void header(cui_ctx *ui, app *s, bool busy) {
    cui_row(ui, .width = CUI_FILL, .pad_x = 16, .pad_y = 10, .gap = 10, .align = CUI_ALIGN_CENTER) {
        cui_text(ui, "myra", .font = CUI_FONT_MEDIUM, .size = 15);
        char label[256];
        snprintf(label, sizeof label, "%s / %s", s->agent.prov->name, s->agent.model);
        if (cui_button(ui, label, .icon = CUI_ICON_LAYERS, .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM,
                       .disabled = busy, .tooltip = "Choose a model")) {
            s->choose_model = true;
            if (!s->models) s->act = ACT_FETCH_MODELS;
        }
        int perm = permission_index(s->permissions);
        if (cui_select(ui, "permissions", PERMISSION_NAMES, NPERMISSIONS, &perm, .width = CUI_PX(190)))
            s->permissions = PERMISSIONS[perm];
        if (cui_icon_button(ui, CUI_ICON_FOLDER_OPEN, .size = CUI_SIZE_SM, .tooltip = "Open a folder", .disabled = busy))
            s->choose_folder = true;
        cui_text(ui, s->cwd, .size = 12, .color = CUI_COLOR_TEXT_MUTED, .width = CUI_FILL, .nowrap = true);
        const myra_usage *u = &s->agent.session;
        if (!busy && u->requests) {
            int k = snprintf(label, sizeof label, "%.0f in, %.0f out", u->in, u->out);
            if (u->costed && k > 0) snprintf(label + k, sizeof label - (size_t)k, ", $%.4f", u->cost);
            cui_text(ui, label, .size = 12, .color = CUI_COLOR_TEXT_MUTED, .nowrap = true);
        }
        if (cui_icon_button(ui, s->dark ? CUI_ICON_SUN : CUI_ICON_MOON, .size = CUI_SIZE_SM, .tooltip = "Theme"))
            s->dark = !s->dark;
        if (cui_icon_button(ui, CUI_ICON_COMMAND, .size = CUI_SIZE_SM, .tooltip = "Commands"))
            s->palette = true;
    }
}

/* ---- dialogs ---- */

/* The permission question for the pending call; returns 1 allow, 0 reject, -1 open. */
static int permission(cui_ctx *ui, const transcript *tr) {
    if (!tr->asking) return -1;
    const item *call = NULL;
    for (int i = tr->n - 1; i >= 0 && !call; i--)
        if (tr->items[i].kind == ITEM_TOOL && tr->items[i].status == TOOL_PENDING) call = &tr->items[i];
    bool open = true;
    int choice = -1;
    cui_dialog(ui, "Allow this tool call?", &open, .persistent = true, .width = 640,
               .description = tr->outside ? "It changes a file outside the working directory." : NULL) {
        if (call && call->after)
            cui_diff_view(ui, "ask-diff", call->before, call->after, .mode = CUI_DIFF_INLINE,
                          .language = language(call->detail), .new_title = call->detail, .height = 360);
        else if (call)
            cui_code_block(ui, "ask-detail", call->detail, .lang = strcmp(call->name, "shell") ? "text" : "shell",
                           .title = call->name, .max_height = 360);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Reject", .variant = CUI_BUTTON_OUTLINE)) choice = 0;
            if (cui_button(ui, call ? call->name : "Allow", .variant = CUI_BUTTON_PRIMARY,
                           .icon = call ? tool_icon(call->name) : NULL))
                choice = 1;
        }
    }
    if (!open) choice = 0;
    return choice;
}

static void model_dialog(cui_ctx *ui, app *s, bool busy) {
    cui_dialog(ui, "Choose a model", &s->choose_model, .width = 520,
               .description = "The provider's models. The choice is remembered after its first reply.") {
        if (busy && s->job == JOB_MODELS) cui_progress(ui, -1, .label = "Fetching models");
        else if (s->models_failed) {
            cui_alert(ui, "Could not fetch the model list.", .tone = CUI_TONE_DANGER, .description = myra_last_error);
            if (cui_button(ui, "Retry")) s->act = ACT_FETCH_MODELS;
        } else if (s->models) {
            if (cui_select(ui, "model", (const char *const *)s->models, s->nmodels, &s->model_index,
                           .searchable = true, .placeholder = s->agent.model, .width = CUI_FILL))
                s->act = ACT_SET_MODEL;
        }
    }
}

enum { CMD_NEW, CMD_MODEL, CMD_FOLDER, CMD_ASK, CMD_AUTO, CMD_ALL, CMD_READ_ONLY, CMD_THEME, CMD_FILES, CMD_LOG, CMD_STOP };

static void palette(cui_ctx *ui, app *s, bool busy) {
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_K)) s->palette = true;
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_N) && !busy) s->act = ACT_NEW_CHAT;
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_O) && !busy) s->choose_folder = true;
    static const cui_command cmds[] = {
        [CMD_NEW] = {"New chat", CUI_ICON_SQUARE_PEN, "Chat", NULL},
        [CMD_MODEL] = {"Choose a model", CUI_ICON_LAYERS, "Chat", NULL},
        [CMD_FOLDER] = {"Open a folder", CUI_ICON_FOLDER_OPEN, "Chat", NULL},
        [CMD_ASK] = {"Permissions: ask before changes", CUI_ICON_SHIELD, "Permissions", NULL},
        [CMD_AUTO] = {"Permissions: auto", CUI_ICON_SHIELD, "Permissions", NULL},
        [CMD_ALL] = {"Permissions: all", CUI_ICON_SHIELD, "Permissions", NULL},
        [CMD_READ_ONLY] = {"Permissions: read only", CUI_ICON_SHIELD, "Permissions", NULL},
        [CMD_THEME] = {"Toggle light and dark", CUI_ICON_MOON, "View", NULL},
        [CMD_FILES] = {"Show files", CUI_ICON_FOLDER, "View", NULL},
        [CMD_LOG] = {"Show log", CUI_ICON_LIST, "View", NULL},
        [CMD_STOP] = {"Stop the turn", CUI_ICON_SQUARE, "Chat", "Esc"},
    };
    switch (cui_command_palette(ui, &s->palette, cmds, sizeof cmds / sizeof *cmds)) {
    case CMD_NEW: if (!busy) s->act = ACT_NEW_CHAT; break;
    case CMD_MODEL: if (!busy) { s->choose_model = true; if (!s->models) s->act = ACT_FETCH_MODELS; } break;
    case CMD_FOLDER: if (!busy) s->choose_folder = true; break;
    case CMD_ASK: s->permissions = MYRA_ASK; break;
    case CMD_AUTO: s->permissions = MYRA_AUTO; break;
    case CMD_ALL: s->permissions = MYRA_ALL; break;
    case CMD_READ_ONLY: s->permissions = MYRA_READ_ONLY; break;
    case CMD_THEME: s->dark = !s->dark; break;
    case CMD_FILES: s->show_files = true; break;
    case CMD_LOG: s->show_log = true; break;
    case CMD_STOP: myra_interrupted = 1; break;
    default: break;
    }
}

/* Keep the dock layout and theme for the next run, a second after they settle. */
static void remember(cui_ctx *ui, app *s) {
    char now[sizeof s->layout];
    size_t n = cui_dock_save(ui, "dock", now, sizeof now);
    if (n && n < sizeof now && strcmp(now, s->layout)) {
        snprintf(s->layout, sizeof s->layout, "%s", now);
        s->layout_changed = cui_time(ui);
    }
    if (s->layout_changed && cui_time(ui) - s->layout_changed > 1) {
        myra_store_state("ui.layout", s->layout);
        s->layout_changed = 0;
    }
    if ((cui_get_theme_mode(ui) == CUI_THEME_DARK) != s->dark) {
        cui_set_theme(ui, s->dark ? CUI_THEME_DARK : CUI_THEME_LIGHT);
        myra_store_state("ui.theme", s->dark ? "dark" : "light");
    }
}

#define DEFAULT_LAYOUT "dock1 h0.22([*Files],v0.76([*Chat],[*Log]))"

void view_frame(cui_ctx *ui, void *user) {
    app *s = user;
    bool busy = atomic_load(&s->busy);

    tr_lock(&s->tr);
    take_notes(ui, s);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND) {
        header(ui, s, busy);
        cui_separator(ui);
        cui_dockspace(ui, "dock", .layout = *s->layout ? s->layout : DEFAULT_LAYOUT) {
            cui_dock_panel(ui, "Files", &s->show_files, .icon = CUI_ICON_FOLDER) { files(ui, s); }
            cui_dock_panel(ui, "Chat", NULL, .icon = CUI_ICON_MESSAGE_SQUARE) { chat(ui, s, busy); }
            cui_dock_panel(ui, "Log", &s->show_log, .icon = CUI_ICON_LIST) {
                cui_log_view(ui, "log", &s->log, .height = -1, .timestamps = true);
            }
        }
    }
    palette(ui, s, busy);
    model_dialog(ui, s, busy);
    if (cui_file_picker(ui, "folder", &s->choose_folder, s->chosen, sizeof s->chosen, .folders = true,
                        .start = s->cwd) && !busy)
        s->act = ACT_CHDIR;
    int choice = permission(ui, &s->tr);
    bool stop = busy && !s->palette && !s->choose_model && !s->choose_folder && cui_key_pressed(ui, CUI_KEY_ESCAPE);
    if (stop) myra_interrupted = 1;
    bool reject = myra_interrupted && s->tr.asking;
    tr_unlock(&s->tr);

    /* These take the lock, so they run after the frame releases it. */
    if (choice >= 0) tr_answer(&s->tr, choice);
    else if (reject) tr_answer(&s->tr, 0);
    remember(ui, s);
    view_act(s);
    if (busy || s->act != ACT_NONE) cui_request_wakeup(ui, 0.05f);
}

void view_act(app *s) {
    action act = s->act;
    s->act = ACT_NONE;
    if (act == ACT_NONE || atomic_load(&s->busy)) return;
    switch (act) {
    case ACT_SEND:
        if (!s->draft[0] || !(s->prompt = strdup(s->draft))) return;
        s->agent.permissions = s->permissions; /* the worker is idle: safe to change */
        tr_user(&s->tr, s->draft);
        s->draft[0] = 0;
        if (!worker_start(s, JOB_TURN)) {
            free(s->prompt);
            s->prompt = NULL;
            tr_error(&s->tr, "Could not start the agent thread.");
        }
        break;
    case ACT_NEW_CHAT:
        myra_clear(&s->agent);
        tr_clear(&s->tr);
        break;
    case ACT_SET_MODEL:
        if (s->models && s->model_index >= 0 && s->model_index < s->nmodels) {
            myra_set_model(&s->agent, s->models[s->model_index]);
            s->choose_model = false;
        }
        break;
    case ACT_FETCH_MODELS:
        s->models_failed = false;
        worker_start(s, JOB_MODELS);
        break;
    case ACT_CHDIR: /* a new folder gets a new conversation, as an ACP session/new does */
        if (chdir(s->chosen) < 0) {
            tr_note(&s->tr, MYRA_ERROR, "cannot open that folder\n");
            break;
        }
        if (!getcwd(s->cwd, sizeof s->cwd)) snprintf(s->cwd, sizeof s->cwd, "%s", s->chosen);
        snprintf(s->browse, sizeof s->browse, "%s", s->cwd);
        myra_set_cwd(&s->agent);
        myra_clear(&s->agent);
        tr_clear(&s->tr);
        break;
    case ACT_NONE: break;
    }
}
