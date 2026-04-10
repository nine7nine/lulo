#define _GNU_SOURCE

#include "lulo_systemd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int clamp_int(int value, int lo, int hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static int append_snapshot_line(char ***lines, int *count, const char *text)
{
    char **next;
    char *copy;

    next = realloc(*lines, (size_t)(*count + 1) * sizeof(*next));
    if (!next) return -1;
    *lines = next;
    copy = strdup(text ? text : "");
    if (!copy) return -1;
    (*lines)[(*count)++] = copy;
    return 0;
}

static void clear_lines(char ***lines, int *count)
{
    if (!lines || !count) return;
    for (int i = 0; i < *count; i++) free((*lines)[i]);
    free(*lines);
    *lines = NULL;
    *count = 0;
}

static int active_list_count(const LuloSystemdState *state, const LuloSystemdSnapshot *snap)
{
    if (!state || !snap) return 0;
    return state->view == LULO_SYSTEMD_VIEW_CONFIG ? snap->config_count : snap->count;
}

static int active_preview_count(const LuloSystemdState *state, const LuloSystemdSnapshot *snap)
{
    if (!state || !snap) return 0;
    switch (state->view) {
    case LULO_SYSTEMD_VIEW_SERVICES:
        return snap->file_line_count;
    case LULO_SYSTEMD_VIEW_DEPS:
        return snap->dep_line_count;
    case LULO_SYSTEMD_VIEW_CONFIG:
        return snap->config_line_count;
    default:
        return 0;
    }
}

static int sync_service_selection(LuloSystemdState *state, const LuloSystemdSnapshot *snap)
{
    int selected = -1;

    if (!state || !snap || snap->count <= 0) {
        if (state) {
            state->selected = -1;
            state->list_scroll = 0;
            state->file_scroll = 0;
            state->selected_user_scope = 0;
            state->selected_unit[0] = '\0';
        }
        return -1;
    }
    if (state->selected_unit[0]) {
        for (int i = 0; i < snap->count; i++) {
            if (snap->rows[i].user_scope == state->selected_user_scope &&
                strcmp(snap->rows[i].raw_unit, state->selected_unit) == 0) {
                selected = i;
                break;
            }
        }
    }
    if (selected < 0 && state->selected >= 0) selected = clamp_int(state->selected, 0, snap->count - 1);
    state->selected = selected;
    if (selected >= 0) {
        state->selected_user_scope = snap->rows[selected].user_scope;
        snprintf(state->selected_unit, sizeof(state->selected_unit), "%s", snap->rows[selected].raw_unit);
    } else {
        state->selected_user_scope = 0;
        state->selected_unit[0] = '\0';
    }
    return selected;
}

static int sync_service_cursor(LuloSystemdState *state, const LuloSystemdSnapshot *snap)
{
    if (!state || !snap || snap->count <= 0) {
        if (state) {
            state->cursor = -1;
            state->list_scroll = 0;
        }
        return -1;
    }
    if (state->cursor < 0) state->cursor = 0;
    state->cursor = clamp_int(state->cursor, 0, snap->count - 1);
    return state->cursor;
}

static int sync_config_selection(LuloSystemdState *state, const LuloSystemdSnapshot *snap)
{
    int selected = -1;

    if (!state || !snap || snap->config_count <= 0) {
        if (state) {
            state->config_selected = -1;
            state->config_list_scroll = 0;
            state->config_file_scroll = 0;
            state->selected_config[0] = '\0';
        }
        return -1;
    }
    if (state->selected_config[0]) {
        for (int i = 0; i < snap->config_count; i++) {
            if (strcmp(snap->configs[i].path, state->selected_config) == 0) {
                selected = i;
                break;
            }
        }
    }
    if (selected < 0 && state->config_selected >= 0) {
        selected = clamp_int(state->config_selected, 0, snap->config_count - 1);
    }
    state->config_selected = selected;
    if (selected >= 0) snprintf(state->selected_config, sizeof(state->selected_config), "%s", snap->configs[selected].path);
    else state->selected_config[0] = '\0';
    return selected;
}

static int sync_config_cursor(LuloSystemdState *state, const LuloSystemdSnapshot *snap)
{
    if (!state || !snap || snap->config_count <= 0) {
        if (state) {
            state->config_cursor = -1;
            state->config_list_scroll = 0;
        }
        return -1;
    }
    if (state->config_cursor < 0) state->config_cursor = 0;
    state->config_cursor = clamp_int(state->config_cursor, 0, snap->config_count - 1);
    return state->config_cursor;
}

void lulo_systemd_state_init(LuloSystemdState *state)
{
    memset(state, 0, sizeof(*state));
    state->view = LULO_SYSTEMD_VIEW_SERVICES;
    state->cursor = -1;
    state->selected = -1;
    state->config_cursor = -1;
    state->config_selected = -1;
}

void lulo_systemd_state_cleanup(LuloSystemdState *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
}

int lulo_systemd_snapshot_clone(LuloSystemdSnapshot *dst, const LuloSystemdSnapshot *src)
{
    if (!dst || !src) return -1;
    memset(dst, 0, sizeof(*dst));
    dst->configs_loaded = src->configs_loaded;
    snprintf(dst->file_title, sizeof(dst->file_title), "%s", src->file_title);
    snprintf(dst->file_status, sizeof(dst->file_status), "%s", src->file_status);
    snprintf(dst->dep_title, sizeof(dst->dep_title), "%s", src->dep_title);
    snprintf(dst->dep_status, sizeof(dst->dep_status), "%s", src->dep_status);
    snprintf(dst->config_title, sizeof(dst->config_title), "%s", src->config_title);
    snprintf(dst->config_status, sizeof(dst->config_status), "%s", src->config_status);

    if (src->count > 0) {
        dst->rows = malloc((size_t)src->count * sizeof(*dst->rows));
        if (!dst->rows) goto fail;
        memcpy(dst->rows, src->rows, (size_t)src->count * sizeof(*dst->rows));
        dst->count = src->count;
    }
    if (src->config_count > 0) {
        dst->configs = malloc((size_t)src->config_count * sizeof(*dst->configs));
        if (!dst->configs) goto fail;
        memcpy(dst->configs, src->configs, (size_t)src->config_count * sizeof(*dst->configs));
        dst->config_count = src->config_count;
    }
    for (int i = 0; i < src->file_line_count; i++) {
        if (append_snapshot_line(&dst->file_lines, &dst->file_line_count, src->file_lines[i]) < 0) goto fail;
    }
    for (int i = 0; i < src->dep_line_count; i++) {
        if (append_snapshot_line(&dst->dep_lines, &dst->dep_line_count, src->dep_lines[i]) < 0) goto fail;
    }
    for (int i = 0; i < src->config_line_count; i++) {
        if (append_snapshot_line(&dst->config_lines, &dst->config_line_count, src->config_lines[i]) < 0) goto fail;
    }
    return 0;

fail:
    lulo_systemd_snapshot_free(dst);
    return -1;
}

void lulo_systemd_snapshot_mark_loading(LuloSystemdSnapshot *snap, const LuloSystemdState *state)
{
    if (!snap || !state) return;

    switch (state->view) {
    case LULO_SYSTEMD_VIEW_SERVICES:
        clear_lines(&snap->file_lines, &snap->file_line_count);
        snprintf(snap->file_title, sizeof(snap->file_title), "unit file");
        snprintf(snap->file_status, sizeof(snap->file_status), "loading...");
        append_snapshot_line(&snap->file_lines, &snap->file_line_count, "loading...");
        break;
    case LULO_SYSTEMD_VIEW_DEPS:
        clear_lines(&snap->dep_lines, &snap->dep_line_count);
        snprintf(snap->dep_title, sizeof(snap->dep_title), "reverse deps");
        snprintf(snap->dep_status, sizeof(snap->dep_status), "loading...");
        append_snapshot_line(&snap->dep_lines, &snap->dep_line_count, "loading...");
        break;
    case LULO_SYSTEMD_VIEW_CONFIG:
    default:
        clear_lines(&snap->config_lines, &snap->config_line_count);
        snprintf(snap->config_title, sizeof(snap->config_title), "config");
        snprintf(snap->config_status, sizeof(snap->config_status), "loading...");
        append_snapshot_line(&snap->config_lines, &snap->config_line_count, "loading...");
        break;
    }
}

void lulo_systemd_snapshot_free(LuloSystemdSnapshot *snap)
{
    if (!snap) return;
    clear_lines(&snap->file_lines, &snap->file_line_count);
    clear_lines(&snap->dep_lines, &snap->dep_line_count);
    clear_lines(&snap->config_lines, &snap->config_line_count);
    free(snap->rows);
    free(snap->configs);
    memset(snap, 0, sizeof(*snap));
}

void lulo_systemd_view_sync(LuloSystemdState *state, const LuloSystemdSnapshot *snap,
                            int list_rows, int file_rows)
{
    int count;
    int preview_count;
    int max_scroll;
    int max_preview_scroll;
    int *cursor;
    int *list_scroll;
    int *preview_scroll;

    if (!state) return;
    sync_service_selection(state, snap);
    sync_config_selection(state, snap);
    sync_service_cursor(state, snap);
    sync_config_cursor(state, snap);

    cursor = state->view == LULO_SYSTEMD_VIEW_CONFIG ? &state->config_cursor : &state->cursor;
    list_scroll = state->view == LULO_SYSTEMD_VIEW_CONFIG ? &state->config_list_scroll : &state->list_scroll;
    preview_scroll = state->view == LULO_SYSTEMD_VIEW_CONFIG ? &state->config_file_scroll : &state->file_scroll;
    count = active_list_count(state, snap);
    preview_count = active_preview_count(state, snap);

    if (list_rows <= 0) list_rows = 1;
    if (file_rows <= 0) file_rows = 1;

    if (count <= 0) {
        *cursor = -1;
        *list_scroll = 0;
    } else {
        if (*cursor < 0) *cursor = 0;
        *cursor = clamp_int(*cursor, 0, count - 1);
        if (*cursor < *list_scroll) *list_scroll = *cursor;
        if (*cursor >= *list_scroll + list_rows) *list_scroll = *cursor - list_rows + 1;
        max_scroll = count > list_rows ? count - list_rows : 0;
        *list_scroll = clamp_int(*list_scroll, 0, max_scroll);
    }

    max_preview_scroll = preview_count > file_rows ? preview_count - file_rows : 0;
    *preview_scroll = clamp_int(*preview_scroll, 0, max_preview_scroll);
    if (preview_count <= 0) state->focus_preview = 0;
}

void lulo_systemd_view_move(LuloSystemdState *state, const LuloSystemdSnapshot *snap,
                            int list_rows, int file_rows, int delta)
{
    int count;
    int *cursor;
    int *preview_scroll;

    if (!state) return;
    lulo_systemd_view_sync(state, snap, list_rows, file_rows);
    preview_scroll = state->view == LULO_SYSTEMD_VIEW_CONFIG ? &state->config_file_scroll : &state->file_scroll;
    if (state->focus_preview) {
        *preview_scroll += delta;
        lulo_systemd_view_sync(state, snap, list_rows, file_rows);
        return;
    }
    count = active_list_count(state, snap);
    if (count <= 0) return;
    cursor = state->view == LULO_SYSTEMD_VIEW_CONFIG ? &state->config_cursor : &state->cursor;
    if (*cursor < 0) {
        *cursor = delta < 0 ? count - 1 : 0;
        *preview_scroll = 0;
        lulo_systemd_view_sync(state, snap, list_rows, file_rows);
        return;
    }
    *cursor = clamp_int(*cursor + delta, 0, count - 1);
    *preview_scroll = 0;
    lulo_systemd_view_sync(state, snap, list_rows, file_rows);
}

void lulo_systemd_view_page(LuloSystemdState *state, const LuloSystemdSnapshot *snap,
                            int list_rows, int file_rows, int pages)
{
    int delta;

    if (!state) return;
    delta = (state->focus_preview ? file_rows : list_rows) * pages;
    if (delta == 0) delta = pages;
    lulo_systemd_view_move(state, snap, list_rows, file_rows, delta);
}

void lulo_systemd_view_home(LuloSystemdState *state, const LuloSystemdSnapshot *snap,
                            int list_rows, int file_rows)
{
    if (!state) return;
    if (state->focus_preview) {
        if (state->view == LULO_SYSTEMD_VIEW_CONFIG) state->config_file_scroll = 0;
        else state->file_scroll = 0;
    } else if (state->view == LULO_SYSTEMD_VIEW_CONFIG) {
        state->config_cursor = 0;
        state->config_file_scroll = 0;
    } else {
        state->cursor = 0;
        state->file_scroll = 0;
    }
    lulo_systemd_view_sync(state, snap, list_rows, file_rows);
}

void lulo_systemd_view_end(LuloSystemdState *state, const LuloSystemdSnapshot *snap,
                           int list_rows, int file_rows)
{
    int count;
    int preview_count;

    if (!state) return;
    count = active_list_count(state, snap);
    preview_count = active_preview_count(state, snap);
    if (state->focus_preview) {
        int max_preview = preview_count > file_rows ? preview_count - file_rows : 0;
        if (state->view == LULO_SYSTEMD_VIEW_CONFIG) state->config_file_scroll = max_preview;
        else state->file_scroll = max_preview;
    } else if (count > 0) {
        if (state->view == LULO_SYSTEMD_VIEW_CONFIG) {
            state->config_cursor = count - 1;
            state->config_file_scroll = 0;
        } else {
            state->cursor = count - 1;
            state->file_scroll = 0;
        }
    }
    lulo_systemd_view_sync(state, snap, list_rows, file_rows);
}

void lulo_systemd_set_cursor(LuloSystemdState *state, const LuloSystemdSnapshot *snap,
                             int list_rows, int file_rows, int row_index)
{
    int count;

    if (!state) return;
    count = active_list_count(state, snap);
    if (count <= 0) return;
    row_index = clamp_int(row_index, 0, count - 1);
    if (state->view == LULO_SYSTEMD_VIEW_CONFIG) state->config_cursor = row_index;
    else state->cursor = row_index;
    lulo_systemd_view_sync(state, snap, list_rows, file_rows);
}

int lulo_systemd_open_current(LuloSystemdState *state, const LuloSystemdSnapshot *snap,
                              int list_rows, int file_rows)
{
    int changed = 0;

    if (!state || !snap) return 0;
    lulo_systemd_view_sync(state, snap, list_rows, file_rows);
    if (state->view == LULO_SYSTEMD_VIEW_CONFIG) {
        int cursor = state->config_cursor;

        if (cursor < 0 || cursor >= snap->config_count) return 0;
        if (state->config_selected == cursor &&
            strcmp(state->selected_config, snap->configs[cursor].path) == 0) {
            state->config_selected = -1;
            state->selected_config[0] = '\0';
            state->config_file_scroll = 0;
            state->focus_preview = 0;
            lulo_systemd_view_sync(state, snap, list_rows, file_rows);
            return 1;
        }
        if (state->config_selected != cursor ||
            strcmp(state->selected_config, snap->configs[cursor].path) != 0) {
            state->config_selected = cursor;
            snprintf(state->selected_config, sizeof(state->selected_config), "%s", snap->configs[cursor].path);
            state->config_file_scroll = 0;
            changed = 1;
        }
    } else {
        int cursor = state->cursor;

        if (cursor < 0 || cursor >= snap->count) return 0;
        if (state->selected == cursor &&
            state->selected_user_scope == snap->rows[cursor].user_scope &&
            strcmp(state->selected_unit, snap->rows[cursor].raw_unit) == 0) {
            state->selected = -1;
            state->selected_user_scope = 0;
            state->selected_unit[0] = '\0';
            state->file_scroll = 0;
            state->focus_preview = 0;
            lulo_systemd_view_sync(state, snap, list_rows, file_rows);
            return 1;
        }
        if (state->selected != cursor ||
            state->selected_user_scope != snap->rows[cursor].user_scope ||
            strcmp(state->selected_unit, snap->rows[cursor].raw_unit) != 0) {
            state->selected = cursor;
            state->selected_user_scope = snap->rows[cursor].user_scope;
            snprintf(state->selected_unit, sizeof(state->selected_unit), "%s", snap->rows[cursor].raw_unit);
            state->file_scroll = 0;
            changed = 1;
        }
    }
    lulo_systemd_view_sync(state, snap, list_rows, file_rows);
    return changed;
}

void lulo_systemd_toggle_focus(LuloSystemdState *state, const LuloSystemdSnapshot *snap,
                               int list_rows, int file_rows)
{
    if (!state) return;
    if (active_preview_count(state, snap) <= 0) {
        state->focus_preview = 0;
        lulo_systemd_view_sync(state, snap, list_rows, file_rows);
        return;
    }
    state->focus_preview = !state->focus_preview;
    lulo_systemd_view_sync(state, snap, list_rows, file_rows);
}

void lulo_systemd_next_view(LuloSystemdState *state)
{
    if (!state) return;
    state->view = (LuloSystemdView)((state->view + 1) % LULO_SYSTEMD_VIEW_COUNT);
    state->focus_preview = 0;
}

void lulo_systemd_prev_view(LuloSystemdState *state)
{
    if (!state) return;
    state->view = (LuloSystemdView)((state->view + LULO_SYSTEMD_VIEW_COUNT - 1) % LULO_SYSTEMD_VIEW_COUNT);
    state->focus_preview = 0;
}

const char *lulo_systemd_view_name(LuloSystemdView view)
{
    switch (view) {
    case LULO_SYSTEMD_VIEW_SERVICES:
        return "Services";
    case LULO_SYSTEMD_VIEW_DEPS:
        return "Deps";
    case LULO_SYSTEMD_VIEW_CONFIG:
        return "Config";
    default:
        return "Systemd";
    }
}
