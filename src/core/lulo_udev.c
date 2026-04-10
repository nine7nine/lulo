#define _GNU_SOURCE

#include "lulo_udev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int clamp_int_local(int value, int lo, int hi)
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

static int active_list_count(const LuloUdevState *state, const LuloUdevSnapshot *snap)
{
    if (!state || !snap) return 0;
    switch (state->view) {
    case LULO_UDEV_VIEW_HWDB:
        return snap->hwdb_count;
    case LULO_UDEV_VIEW_DEVICES:
        return snap->device_count;
    case LULO_UDEV_VIEW_RULES:
    default:
        return snap->rule_count;
    }
}

static int *active_cursor(LuloUdevState *state)
{
    if (!state) return NULL;
    switch (state->view) {
    case LULO_UDEV_VIEW_HWDB:
        return &state->hwdb_cursor;
    case LULO_UDEV_VIEW_DEVICES:
        return &state->device_cursor;
    case LULO_UDEV_VIEW_RULES:
    default:
        return &state->rule_cursor;
    }
}

static int *active_selected(LuloUdevState *state)
{
    if (!state) return NULL;
    switch (state->view) {
    case LULO_UDEV_VIEW_HWDB:
        return &state->hwdb_selected;
    case LULO_UDEV_VIEW_DEVICES:
        return &state->device_selected;
    case LULO_UDEV_VIEW_RULES:
    default:
        return &state->rule_selected;
    }
}

static int *active_list_scroll(LuloUdevState *state)
{
    if (!state) return NULL;
    switch (state->view) {
    case LULO_UDEV_VIEW_HWDB:
        return &state->hwdb_list_scroll;
    case LULO_UDEV_VIEW_DEVICES:
        return &state->device_list_scroll;
    case LULO_UDEV_VIEW_RULES:
    default:
        return &state->rule_list_scroll;
    }
}

static int *active_detail_scroll(LuloUdevState *state)
{
    if (!state) return NULL;
    switch (state->view) {
    case LULO_UDEV_VIEW_HWDB:
        return &state->hwdb_detail_scroll;
    case LULO_UDEV_VIEW_DEVICES:
        return &state->device_detail_scroll;
    case LULO_UDEV_VIEW_RULES:
    default:
        return &state->rule_detail_scroll;
    }
}

static char *active_selected_path(LuloUdevState *state)
{
    if (!state) return NULL;
    switch (state->view) {
    case LULO_UDEV_VIEW_HWDB:
        return state->selected_hwdb;
    case LULO_UDEV_VIEW_DEVICES:
        return state->selected_device;
    case LULO_UDEV_VIEW_RULES:
    default:
        return state->selected_rule;
    }
}

static int sync_rule_selection(LuloUdevState *state, const LuloUdevSnapshot *snap)
{
    int selected = -1;

    if (!state || !snap || snap->rule_count <= 0) {
        if (state) {
            state->rule_selected = -1;
            state->rule_list_scroll = 0;
            state->rule_detail_scroll = 0;
            state->selected_rule[0] = '\0';
        }
        return -1;
    }
    if (state->selected_rule[0]) {
        for (int i = 0; i < snap->rule_count; i++) {
            if (strcmp(snap->rules[i].path, state->selected_rule) == 0) {
                selected = i;
                break;
            }
        }
    }
    if (selected < 0 && state->rule_selected >= 0) {
        selected = clamp_int_local(state->rule_selected, 0, snap->rule_count - 1);
    }
    state->rule_selected = selected;
    if (selected >= 0) snprintf(state->selected_rule, sizeof(state->selected_rule), "%s",
                                snap->rules[selected].path);
    else state->selected_rule[0] = '\0';
    return selected;
}

static int sync_hwdb_selection(LuloUdevState *state, const LuloUdevSnapshot *snap)
{
    int selected = -1;

    if (!state || !snap || snap->hwdb_count <= 0) {
        if (state) {
            state->hwdb_selected = -1;
            state->hwdb_list_scroll = 0;
            state->hwdb_detail_scroll = 0;
            state->selected_hwdb[0] = '\0';
        }
        return -1;
    }
    if (state->selected_hwdb[0]) {
        for (int i = 0; i < snap->hwdb_count; i++) {
            if (strcmp(snap->hwdb[i].path, state->selected_hwdb) == 0) {
                selected = i;
                break;
            }
        }
    }
    if (selected < 0 && state->hwdb_selected >= 0) {
        selected = clamp_int_local(state->hwdb_selected, 0, snap->hwdb_count - 1);
    }
    state->hwdb_selected = selected;
    if (selected >= 0) snprintf(state->selected_hwdb, sizeof(state->selected_hwdb), "%s",
                                snap->hwdb[selected].path);
    else state->selected_hwdb[0] = '\0';
    return selected;
}

static int sync_device_selection(LuloUdevState *state, const LuloUdevSnapshot *snap)
{
    int selected = -1;

    if (!state || !snap || snap->device_count <= 0) {
        if (state) {
            state->device_selected = -1;
            state->device_list_scroll = 0;
            state->device_detail_scroll = 0;
            state->selected_device[0] = '\0';
        }
        return -1;
    }
    if (state->selected_device[0]) {
        for (int i = 0; i < snap->device_count; i++) {
            if (strcmp(snap->devices[i].path, state->selected_device) == 0) {
                selected = i;
                break;
            }
        }
    }
    if (selected < 0 && state->device_selected >= 0) {
        selected = clamp_int_local(state->device_selected, 0, snap->device_count - 1);
    }
    state->device_selected = selected;
    if (selected >= 0) snprintf(state->selected_device, sizeof(state->selected_device), "%s",
                                snap->devices[selected].path);
    else state->selected_device[0] = '\0';
    return selected;
}

static int sync_rule_cursor(LuloUdevState *state, const LuloUdevSnapshot *snap)
{
    if (!state || !snap || snap->rule_count <= 0) {
        if (state) {
            state->rule_cursor = -1;
            state->rule_list_scroll = 0;
        }
        return -1;
    }
    if (state->rule_cursor < 0) state->rule_cursor = 0;
    state->rule_cursor = clamp_int_local(state->rule_cursor, 0, snap->rule_count - 1);
    if (state->rule_selected < 0) {
        state->rule_selected = state->rule_cursor;
        snprintf(state->selected_rule, sizeof(state->selected_rule), "%s",
                 snap->rules[state->rule_cursor].path);
    }
    return state->rule_cursor;
}

static int sync_hwdb_cursor(LuloUdevState *state, const LuloUdevSnapshot *snap)
{
    if (!state || !snap || snap->hwdb_count <= 0) {
        if (state) {
            state->hwdb_cursor = -1;
            state->hwdb_list_scroll = 0;
        }
        return -1;
    }
    if (state->hwdb_cursor < 0) state->hwdb_cursor = 0;
    state->hwdb_cursor = clamp_int_local(state->hwdb_cursor, 0, snap->hwdb_count - 1);
    if (state->hwdb_selected < 0) {
        state->hwdb_selected = state->hwdb_cursor;
        snprintf(state->selected_hwdb, sizeof(state->selected_hwdb), "%s",
                 snap->hwdb[state->hwdb_cursor].path);
    }
    return state->hwdb_cursor;
}

static int sync_device_cursor(LuloUdevState *state, const LuloUdevSnapshot *snap)
{
    if (!state || !snap || snap->device_count <= 0) {
        if (state) {
            state->device_cursor = -1;
            state->device_list_scroll = 0;
        }
        return -1;
    }
    if (state->device_cursor < 0) state->device_cursor = 0;
    state->device_cursor = clamp_int_local(state->device_cursor, 0, snap->device_count - 1);
    if (state->device_selected < 0) {
        state->device_selected = state->device_cursor;
        snprintf(state->selected_device, sizeof(state->selected_device), "%s",
                 snap->devices[state->device_cursor].path);
    }
    return state->device_cursor;
}

static void sync_selections(LuloUdevState *state, const LuloUdevSnapshot *snap)
{
    if (!state) return;
    sync_rule_selection(state, snap);
    sync_hwdb_selection(state, snap);
    sync_device_selection(state, snap);
}

static void sync_cursors(LuloUdevState *state, const LuloUdevSnapshot *snap)
{
    if (!state) return;
    sync_rule_cursor(state, snap);
    sync_hwdb_cursor(state, snap);
    sync_device_cursor(state, snap);
}

void lulo_udev_state_init(LuloUdevState *state)
{
    memset(state, 0, sizeof(*state));
    state->view = LULO_UDEV_VIEW_RULES;
    state->rule_cursor = -1;
    state->rule_selected = -1;
    state->hwdb_cursor = -1;
    state->hwdb_selected = -1;
    state->device_cursor = -1;
    state->device_selected = -1;
}

void lulo_udev_state_cleanup(LuloUdevState *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
}

int lulo_udev_snapshot_clone(LuloUdevSnapshot *dst, const LuloUdevSnapshot *src)
{
    if (!dst || !src) return -1;
    memset(dst, 0, sizeof(*dst));
    snprintf(dst->detail_title, sizeof(dst->detail_title), "%s", src->detail_title);
    snprintf(dst->detail_status, sizeof(dst->detail_status), "%s", src->detail_status);

    if (src->rule_count > 0) {
        dst->rules = malloc((size_t)src->rule_count * sizeof(*dst->rules));
        if (!dst->rules) goto fail;
        memcpy(dst->rules, src->rules, (size_t)src->rule_count * sizeof(*dst->rules));
        dst->rule_count = src->rule_count;
    }
    if (src->hwdb_count > 0) {
        dst->hwdb = malloc((size_t)src->hwdb_count * sizeof(*dst->hwdb));
        if (!dst->hwdb) goto fail;
        memcpy(dst->hwdb, src->hwdb, (size_t)src->hwdb_count * sizeof(*dst->hwdb));
        dst->hwdb_count = src->hwdb_count;
    }
    if (src->device_count > 0) {
        dst->devices = malloc((size_t)src->device_count * sizeof(*dst->devices));
        if (!dst->devices) goto fail;
        memcpy(dst->devices, src->devices, (size_t)src->device_count * sizeof(*dst->devices));
        dst->device_count = src->device_count;
    }
    for (int i = 0; i < src->detail_line_count; i++) {
        if (append_snapshot_line(&dst->detail_lines, &dst->detail_line_count,
                                 src->detail_lines[i]) < 0) goto fail;
    }
    return 0;

fail:
    lulo_udev_snapshot_free(dst);
    return -1;
}

void lulo_udev_snapshot_mark_loading(LuloUdevSnapshot *snap, const LuloUdevState *state)
{
    const char *path = NULL;
    const char *label = "UDEV";

    if (!snap) return;
    clear_lines(&snap->detail_lines, &snap->detail_line_count);
    switch (state ? state->view : LULO_UDEV_VIEW_RULES) {
    case LULO_UDEV_VIEW_HWDB:
        label = "Hwdb";
        path = state ? state->selected_hwdb : NULL;
        break;
    case LULO_UDEV_VIEW_DEVICES:
        label = "Device";
        path = state ? state->selected_device : NULL;
        break;
    case LULO_UDEV_VIEW_RULES:
    default:
        label = "Rule";
        path = state ? state->selected_rule : NULL;
        break;
    }
    if (path && *path) {
        snprintf(snap->detail_title, sizeof(snap->detail_title), "%s", path);
        snprintf(snap->detail_status, sizeof(snap->detail_status), "loading preview...");
        append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, "loading preview...");
    } else {
        snprintf(snap->detail_title, sizeof(snap->detail_title), "%s", label);
        snprintf(snap->detail_status, sizeof(snap->detail_status), "select an item");
        append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, "select an item to preview");
    }
}

void lulo_udev_snapshot_free(LuloUdevSnapshot *snap)
{
    if (!snap) return;
    free(snap->rules);
    free(snap->hwdb);
    free(snap->devices);
    clear_lines(&snap->detail_lines, &snap->detail_line_count);
    memset(snap, 0, sizeof(*snap));
}

void lulo_udev_view_sync(LuloUdevState *state, const LuloUdevSnapshot *snap,
                         int list_rows, int detail_rows)
{
    int count;
    int *cursor;
    int *selected;
    int *list_scroll;
    int *detail_scroll;
    int max_list_scroll;
    int max_detail_scroll;

    if (!state) return;
    sync_selections(state, snap);
    sync_cursors(state, snap);

    count = active_list_count(state, snap);
    cursor = active_cursor(state);
    selected = active_selected(state);
    list_scroll = active_list_scroll(state);
    detail_scroll = active_detail_scroll(state);

    if (count <= 0) {
        if (cursor) *cursor = -1;
        if (selected) *selected = -1;
        if (list_scroll) *list_scroll = 0;
        if (detail_scroll) *detail_scroll = 0;
        state->focus_preview = 0;
        if (active_selected_path(state)) active_selected_path(state)[0] = '\0';
        return;
    }

    if (cursor && *cursor < 0) *cursor = 0;
    if (cursor) *cursor = clamp_int_local(*cursor, 0, count - 1);
    if (selected && *selected >= count) *selected = count - 1;
    if (selected && *selected < 0 && cursor) *selected = *cursor;

    max_list_scroll = count > list_rows ? count - list_rows : 0;
    if (list_scroll) {
        if (cursor) {
            if (*cursor < *list_scroll) *list_scroll = *cursor;
            if (*cursor >= *list_scroll + list_rows) *list_scroll = *cursor - list_rows + 1;
        }
        *list_scroll = clamp_int_local(*list_scroll, 0, max_list_scroll);
    }

    max_detail_scroll = snap && snap->detail_line_count > detail_rows ? snap->detail_line_count - detail_rows : 0;
    if (detail_scroll) *detail_scroll = clamp_int_local(*detail_scroll, 0, max_detail_scroll);
}

void lulo_udev_view_move(LuloUdevState *state, const LuloUdevSnapshot *snap,
                         int list_rows, int detail_rows, int delta)
{
    int count;
    int *cursor;
    int *detail_scroll;

    if (!state) return;
    if (state->focus_preview) {
        detail_scroll = active_detail_scroll(state);
        if (!detail_scroll) return;
        *detail_scroll += delta;
        lulo_udev_view_sync(state, snap, list_rows, detail_rows);
        return;
    }
    count = active_list_count(state, snap);
    cursor = active_cursor(state);
    if (!cursor || count <= 0) return;
    *cursor += delta;
    lulo_udev_view_sync(state, snap, list_rows, detail_rows);
}

void lulo_udev_view_page(LuloUdevState *state, const LuloUdevSnapshot *snap,
                         int list_rows, int detail_rows, int pages)
{
    int amount;

    if (!state) return;
    amount = state->focus_preview ? detail_rows : list_rows;
    if (amount < 1) amount = 1;
    lulo_udev_view_move(state, snap, list_rows, detail_rows, amount * pages);
}

void lulo_udev_view_home(LuloUdevState *state, const LuloUdevSnapshot *snap,
                         int list_rows, int detail_rows)
{
    int *cursor;
    int *detail_scroll;

    if (!state) return;
    if (state->focus_preview) {
        detail_scroll = active_detail_scroll(state);
        if (detail_scroll) *detail_scroll = 0;
    } else {
        cursor = active_cursor(state);
        if (cursor) *cursor = 0;
    }
    lulo_udev_view_sync(state, snap, list_rows, detail_rows);
}

void lulo_udev_view_end(LuloUdevState *state, const LuloUdevSnapshot *snap,
                        int list_rows, int detail_rows)
{
    int count;
    int *cursor;
    int *detail_scroll;

    if (!state) return;
    if (state->focus_preview) {
        detail_scroll = active_detail_scroll(state);
        if (detail_scroll) {
            int max_detail = snap && snap->detail_line_count > detail_rows ? snap->detail_line_count - detail_rows : 0;
            *detail_scroll = max_detail;
        }
    } else {
        count = active_list_count(state, snap);
        cursor = active_cursor(state);
        if (cursor) *cursor = count > 0 ? count - 1 : -1;
    }
    lulo_udev_view_sync(state, snap, list_rows, detail_rows);
}

void lulo_udev_set_cursor(LuloUdevState *state, const LuloUdevSnapshot *snap,
                          int list_rows, int detail_rows, int row_index)
{
    int count;
    int *cursor;

    if (!state) return;
    count = active_list_count(state, snap);
    cursor = active_cursor(state);
    if (!cursor || count <= 0) return;
    *cursor = clamp_int_local(row_index, 0, count - 1);
    lulo_udev_view_sync(state, snap, list_rows, detail_rows);
}

int lulo_udev_open_current(LuloUdevState *state, const LuloUdevSnapshot *snap,
                           int list_rows, int detail_rows)
{
    int count;
    int *cursor;
    int *selected;
    int *detail_scroll;
    char *selected_path;
    const char *current_path = NULL;

    if (!state || !snap) return 0;
    count = active_list_count(state, snap);
    cursor = active_cursor(state);
    selected = active_selected(state);
    detail_scroll = active_detail_scroll(state);
    selected_path = active_selected_path(state);
    if (!cursor || !selected || !selected_path || count <= 0 || *cursor < 0 || *cursor >= count) return 0;

    switch (state->view) {
    case LULO_UDEV_VIEW_HWDB:
        current_path = snap->hwdb[*cursor].path;
        break;
    case LULO_UDEV_VIEW_DEVICES:
        current_path = snap->devices[*cursor].path;
        break;
    case LULO_UDEV_VIEW_RULES:
    default:
        current_path = snap->rules[*cursor].path;
        break;
    }
    if (!current_path || !*current_path) return 0;

    if (*selected == *cursor && strcmp(selected_path, current_path) == 0) {
        *selected = -1;
        selected_path[0] = '\0';
        state->focus_preview = 0;
        if (detail_scroll) *detail_scroll = 0;
        lulo_udev_view_sync(state, snap, list_rows, detail_rows);
        return 1;
    }

    *selected = *cursor;
    snprintf(selected_path, 320, "%s", current_path);
    if (detail_scroll) *detail_scroll = 0;
    lulo_udev_view_sync(state, snap, list_rows, detail_rows);
    return 1;
}

void lulo_udev_toggle_focus(LuloUdevState *state, const LuloUdevSnapshot *snap,
                            int list_rows, int detail_rows)
{
    if (!state) return;
    if (!active_selected_path(state) || !active_selected_path(state)[0]) {
        state->focus_preview = 0;
    } else {
        state->focus_preview = !state->focus_preview;
    }
    lulo_udev_view_sync(state, snap, list_rows, detail_rows);
}

void lulo_udev_next_view(LuloUdevState *state)
{
    if (!state) return;
    state->view = (LuloUdevView)((state->view + 1) % LULO_UDEV_VIEW_COUNT);
    state->focus_preview = 0;
}

void lulo_udev_prev_view(LuloUdevState *state)
{
    if (!state) return;
    state->view = (LuloUdevView)((state->view + LULO_UDEV_VIEW_COUNT - 1) % LULO_UDEV_VIEW_COUNT);
    state->focus_preview = 0;
}

const char *lulo_udev_view_name(LuloUdevView view)
{
    switch (view) {
    case LULO_UDEV_VIEW_HWDB:
        return "Hwdb";
    case LULO_UDEV_VIEW_DEVICES:
        return "Devices";
    case LULO_UDEV_VIEW_RULES:
    default:
        return "Rules";
    }
}
