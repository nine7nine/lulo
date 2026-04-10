#define _GNU_SOURCE

#include "lulo_cgroups.h"

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

static int active_list_count(const LuloCgroupsState *state, const LuloCgroupsSnapshot *snap)
{
    if (!state || !snap) return 0;
    switch (state->view) {
    case LULO_CGROUPS_VIEW_FILES:
        return snap->file_count;
    case LULO_CGROUPS_VIEW_CONFIG:
        return snap->config_count;
    case LULO_CGROUPS_VIEW_TREE:
    default:
        return snap->tree_count;
    }
}

static int *active_cursor(LuloCgroupsState *state)
{
    if (!state) return NULL;
    switch (state->view) {
    case LULO_CGROUPS_VIEW_FILES:
        return &state->file_cursor;
    case LULO_CGROUPS_VIEW_CONFIG:
        return &state->config_cursor;
    case LULO_CGROUPS_VIEW_TREE:
    default:
        return &state->tree_cursor;
    }
}

static int *active_selected(LuloCgroupsState *state)
{
    if (!state) return NULL;
    switch (state->view) {
    case LULO_CGROUPS_VIEW_FILES:
        return &state->file_selected;
    case LULO_CGROUPS_VIEW_CONFIG:
        return &state->config_selected;
    case LULO_CGROUPS_VIEW_TREE:
    default:
        return &state->tree_selected;
    }
}

static int *active_list_scroll(LuloCgroupsState *state)
{
    if (!state) return NULL;
    switch (state->view) {
    case LULO_CGROUPS_VIEW_FILES:
        return &state->file_list_scroll;
    case LULO_CGROUPS_VIEW_CONFIG:
        return &state->config_list_scroll;
    case LULO_CGROUPS_VIEW_TREE:
    default:
        return &state->tree_list_scroll;
    }
}

static int *active_detail_scroll(LuloCgroupsState *state)
{
    if (!state) return NULL;
    switch (state->view) {
    case LULO_CGROUPS_VIEW_FILES:
        return &state->file_detail_scroll;
    case LULO_CGROUPS_VIEW_CONFIG:
        return &state->config_detail_scroll;
    case LULO_CGROUPS_VIEW_TREE:
    default:
        return &state->tree_detail_scroll;
    }
}

static int sync_tree_selection(LuloCgroupsState *state, const LuloCgroupsSnapshot *snap)
{
    int selected = -1;

    if (!state || !snap || snap->tree_count <= 0) {
        if (state) {
            state->tree_selected = -1;
            state->tree_list_scroll = 0;
            state->tree_detail_scroll = 0;
            state->selected_tree_path[0] = '\0';
        }
        return -1;
    }
    if (state->selected_tree_path[0]) {
        for (int i = 0; i < snap->tree_count; i++) {
            if (strcmp(snap->tree_rows[i].path, state->selected_tree_path) == 0) {
                selected = i;
                break;
            }
        }
    }
    if (selected < 0 && state->tree_selected >= 0) {
        selected = clamp_int_local(state->tree_selected, 0, snap->tree_count - 1);
    }
    state->tree_selected = selected;
    if (selected >= 0) {
        snprintf(state->selected_tree_path, sizeof(state->selected_tree_path), "%s",
                 snap->tree_rows[selected].path);
    } else {
        state->selected_tree_path[0] = '\0';
    }
    return selected;
}

static int sync_file_selection(LuloCgroupsState *state, const LuloCgroupsSnapshot *snap)
{
    int selected = -1;

    if (!state || !snap || snap->file_count <= 0) {
        if (state) {
            state->file_selected = -1;
            state->file_list_scroll = 0;
            state->file_detail_scroll = 0;
            state->selected_file_path[0] = '\0';
        }
        return -1;
    }
    if (state->selected_file_path[0]) {
        for (int i = 0; i < snap->file_count; i++) {
            if (strcmp(snap->file_rows[i].path, state->selected_file_path) == 0) {
                selected = i;
                break;
            }
        }
    }
    if (selected < 0 && state->file_selected >= 0) {
        selected = clamp_int_local(state->file_selected, 0, snap->file_count - 1);
    }
    state->file_selected = selected;
    if (selected >= 0) {
        snprintf(state->selected_file_path, sizeof(state->selected_file_path), "%s",
                 snap->file_rows[selected].path);
    } else {
        state->selected_file_path[0] = '\0';
    }
    return selected;
}

static int sync_config_selection(LuloCgroupsState *state, const LuloCgroupsSnapshot *snap)
{
    int selected = -1;

    if (!state || !snap || snap->config_count <= 0) {
        if (state) {
            state->config_selected = -1;
            state->config_list_scroll = 0;
            state->config_detail_scroll = 0;
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
        selected = clamp_int_local(state->config_selected, 0, snap->config_count - 1);
    }
    state->config_selected = selected;
    if (selected >= 0) {
        snprintf(state->selected_config, sizeof(state->selected_config), "%s",
                 snap->configs[selected].path);
    } else {
        state->selected_config[0] = '\0';
    }
    return selected;
}

static int sync_tree_cursor(LuloCgroupsState *state, const LuloCgroupsSnapshot *snap)
{
    if (!state || !snap || snap->tree_count <= 0) {
        if (state) {
            state->tree_cursor = -1;
            state->tree_list_scroll = 0;
        }
        return -1;
    }
    if (state->tree_cursor < 0) state->tree_cursor = 0;
    state->tree_cursor = clamp_int_local(state->tree_cursor, 0, snap->tree_count - 1);
    if (state->tree_selected < 0) {
        state->tree_selected = state->tree_cursor;
        snprintf(state->selected_tree_path, sizeof(state->selected_tree_path), "%s",
                 snap->tree_rows[state->tree_cursor].path);
    }
    return state->tree_cursor;
}

static int sync_file_cursor(LuloCgroupsState *state, const LuloCgroupsSnapshot *snap)
{
    if (!state || !snap || snap->file_count <= 0) {
        if (state) {
            state->file_cursor = -1;
            state->file_list_scroll = 0;
        }
        return -1;
    }
    if (state->file_cursor < 0) state->file_cursor = 0;
    state->file_cursor = clamp_int_local(state->file_cursor, 0, snap->file_count - 1);
    if (state->file_selected < 0) {
        state->file_selected = state->file_cursor;
        snprintf(state->selected_file_path, sizeof(state->selected_file_path), "%s",
                 snap->file_rows[state->file_cursor].path);
    }
    return state->file_cursor;
}

static int sync_config_cursor(LuloCgroupsState *state, const LuloCgroupsSnapshot *snap)
{
    if (!state || !snap || snap->config_count <= 0) {
        if (state) {
            state->config_cursor = -1;
            state->config_list_scroll = 0;
        }
        return -1;
    }
    if (state->config_cursor < 0) state->config_cursor = 0;
    state->config_cursor = clamp_int_local(state->config_cursor, 0, snap->config_count - 1);
    if (state->config_selected < 0) {
        state->config_selected = state->config_cursor;
        snprintf(state->selected_config, sizeof(state->selected_config), "%s",
                 snap->configs[state->config_cursor].path);
    }
    return state->config_cursor;
}

void lulo_cgroups_state_init(LuloCgroupsState *state)
{
    memset(state, 0, sizeof(*state));
    state->view = LULO_CGROUPS_VIEW_TREE;
    state->tree_cursor = -1;
    state->tree_selected = -1;
    state->file_cursor = -1;
    state->file_selected = -1;
    state->config_cursor = -1;
    state->config_selected = -1;
}

void lulo_cgroups_state_cleanup(LuloCgroupsState *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
}

int lulo_cgroups_snapshot_clone(LuloCgroupsSnapshot *dst, const LuloCgroupsSnapshot *src)
{
    if (!dst || !src) return -1;
    memset(dst, 0, sizeof(*dst));
    dst->configs_loaded = src->configs_loaded;
    snprintf(dst->browse_path, sizeof(dst->browse_path), "%s", src->browse_path);
    snprintf(dst->detail_title, sizeof(dst->detail_title), "%s", src->detail_title);
    snprintf(dst->detail_status, sizeof(dst->detail_status), "%s", src->detail_status);

    if (src->tree_count > 0) {
        dst->tree_rows = malloc((size_t)src->tree_count * sizeof(*dst->tree_rows));
        if (!dst->tree_rows) goto fail;
        memcpy(dst->tree_rows, src->tree_rows, (size_t)src->tree_count * sizeof(*dst->tree_rows));
        dst->tree_count = src->tree_count;
    }
    if (src->file_count > 0) {
        dst->file_rows = malloc((size_t)src->file_count * sizeof(*dst->file_rows));
        if (!dst->file_rows) goto fail;
        memcpy(dst->file_rows, src->file_rows, (size_t)src->file_count * sizeof(*dst->file_rows));
        dst->file_count = src->file_count;
    }
    if (src->config_count > 0) {
        dst->configs = malloc((size_t)src->config_count * sizeof(*dst->configs));
        if (!dst->configs) goto fail;
        memcpy(dst->configs, src->configs, (size_t)src->config_count * sizeof(*dst->configs));
        dst->config_count = src->config_count;
    }
    for (int i = 0; i < src->detail_line_count; i++) {
        if (append_snapshot_line(&dst->detail_lines, &dst->detail_line_count,
                                 src->detail_lines[i]) < 0) {
            goto fail;
        }
    }
    return 0;

fail:
    lulo_cgroups_snapshot_free(dst);
    return -1;
}

void lulo_cgroups_snapshot_mark_loading(LuloCgroupsSnapshot *snap, const LuloCgroupsState *state)
{
    if (!snap || !state) return;
    clear_lines(&snap->detail_lines, &snap->detail_line_count);
    switch (state->view) {
    case LULO_CGROUPS_VIEW_FILES:
        snprintf(snap->detail_title, sizeof(snap->detail_title), "cgroup file");
        break;
    case LULO_CGROUPS_VIEW_CONFIG:
        snprintf(snap->detail_title, sizeof(snap->detail_title), "cgroup config");
        break;
    case LULO_CGROUPS_VIEW_TREE:
    default:
        snprintf(snap->detail_title, sizeof(snap->detail_title), "cgroup");
        break;
    }
    snprintf(snap->detail_status, sizeof(snap->detail_status), "loading...");
    append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, "loading...");
}

void lulo_cgroups_snapshot_free(LuloCgroupsSnapshot *snap)
{
    if (!snap) return;
    clear_lines(&snap->detail_lines, &snap->detail_line_count);
    free(snap->tree_rows);
    free(snap->file_rows);
    free(snap->configs);
    memset(snap, 0, sizeof(*snap));
}

void lulo_cgroups_view_sync(LuloCgroupsState *state, const LuloCgroupsSnapshot *snap,
                            int list_rows, int detail_rows)
{
    int count;
    int max_scroll;
    int max_detail_scroll;
    int *cursor;
    int *selected;
    int *list_scroll;
    int *detail_scroll;

    if (!state) return;
    sync_tree_selection(state, snap);
    sync_file_selection(state, snap);
    sync_config_selection(state, snap);
    sync_tree_cursor(state, snap);
    sync_file_cursor(state, snap);
    sync_config_cursor(state, snap);

    cursor = active_cursor(state);
    selected = active_selected(state);
    list_scroll = active_list_scroll(state);
    detail_scroll = active_detail_scroll(state);
    count = active_list_count(state, snap);

    if (list_rows <= 0) list_rows = 1;
    if (detail_rows <= 0) detail_rows = 1;

    if (count <= 0) {
        *cursor = -1;
        *selected = -1;
        *list_scroll = 0;
    } else {
        if (*cursor < 0) *cursor = 0;
        *cursor = clamp_int_local(*cursor, 0, count - 1);
        if (*selected < 0) *selected = *cursor;
        *selected = clamp_int_local(*selected, 0, count - 1);
        if (*cursor < *list_scroll) *list_scroll = *cursor;
        if (*cursor >= *list_scroll + list_rows) *list_scroll = *cursor - list_rows + 1;
        max_scroll = count > list_rows ? count - list_rows : 0;
        *list_scroll = clamp_int_local(*list_scroll, 0, max_scroll);
    }

    max_detail_scroll = snap && snap->detail_line_count > detail_rows ? snap->detail_line_count - detail_rows : 0;
    *detail_scroll = clamp_int_local(*detail_scroll, 0, max_detail_scroll);
    if (!snap || snap->detail_line_count <= 0) state->focus_preview = 0;
}

void lulo_cgroups_view_move(LuloCgroupsState *state, const LuloCgroupsSnapshot *snap,
                            int list_rows, int detail_rows, int delta)
{
    int count;
    int *cursor;
    int *selected;
    int *detail_scroll;

    if (!state) return;
    lulo_cgroups_view_sync(state, snap, list_rows, detail_rows);
    detail_scroll = active_detail_scroll(state);
    if (state->focus_preview) {
        *detail_scroll += delta;
        lulo_cgroups_view_sync(state, snap, list_rows, detail_rows);
        return;
    }
    count = active_list_count(state, snap);
    if (count <= 0) return;
    cursor = active_cursor(state);
    selected = active_selected(state);
    if (*cursor < 0) {
        *cursor = delta < 0 ? count - 1 : 0;
    } else {
        *cursor = clamp_int_local(*cursor + delta, 0, count - 1);
    }
    *selected = *cursor;
    *detail_scroll = 0;
    switch (state->view) {
    case LULO_CGROUPS_VIEW_FILES:
        snprintf(state->selected_file_path, sizeof(state->selected_file_path), "%s",
                 snap->file_rows[*selected].path);
        break;
    case LULO_CGROUPS_VIEW_CONFIG:
        snprintf(state->selected_config, sizeof(state->selected_config), "%s",
                 snap->configs[*selected].path);
        break;
    case LULO_CGROUPS_VIEW_TREE:
    default:
        snprintf(state->selected_tree_path, sizeof(state->selected_tree_path), "%s",
                 snap->tree_rows[*selected].path);
        break;
    }
    lulo_cgroups_view_sync(state, snap, list_rows, detail_rows);
}

void lulo_cgroups_view_page(LuloCgroupsState *state, const LuloCgroupsSnapshot *snap,
                            int list_rows, int detail_rows, int pages)
{
    int delta;

    if (!state) return;
    delta = (state->focus_preview ? detail_rows : list_rows) * pages;
    if (delta == 0) delta = pages;
    lulo_cgroups_view_move(state, snap, list_rows, detail_rows, delta);
}

void lulo_cgroups_view_home(LuloCgroupsState *state, const LuloCgroupsSnapshot *snap,
                            int list_rows, int detail_rows)
{
    int *cursor;
    int *selected;
    int *detail_scroll;

    if (!state) return;
    cursor = active_cursor(state);
    selected = active_selected(state);
    detail_scroll = active_detail_scroll(state);
    if (state->focus_preview) {
        *detail_scroll = 0;
    } else {
        *cursor = 0;
        *selected = 0;
        *detail_scroll = 0;
        switch (state->view) {
        case LULO_CGROUPS_VIEW_FILES:
            if (snap && snap->file_count > 0) {
                snprintf(state->selected_file_path, sizeof(state->selected_file_path), "%s",
                         snap->file_rows[0].path);
            }
            break;
        case LULO_CGROUPS_VIEW_CONFIG:
            if (snap && snap->config_count > 0) {
                snprintf(state->selected_config, sizeof(state->selected_config), "%s",
                         snap->configs[0].path);
            }
            break;
        case LULO_CGROUPS_VIEW_TREE:
        default:
            if (snap && snap->tree_count > 0) {
                snprintf(state->selected_tree_path, sizeof(state->selected_tree_path), "%s",
                         snap->tree_rows[0].path);
            }
            break;
        }
    }
    lulo_cgroups_view_sync(state, snap, list_rows, detail_rows);
}

void lulo_cgroups_view_end(LuloCgroupsState *state, const LuloCgroupsSnapshot *snap,
                           int list_rows, int detail_rows)
{
    int count;
    int *cursor;
    int *selected;
    int *detail_scroll;

    if (!state) return;
    cursor = active_cursor(state);
    selected = active_selected(state);
    detail_scroll = active_detail_scroll(state);
    count = active_list_count(state, snap);
    if (state->focus_preview) {
        int max_scroll = snap && snap->detail_line_count > detail_rows ? snap->detail_line_count - detail_rows : 0;
        *detail_scroll = max_scroll;
    } else if (count > 0) {
        *cursor = count - 1;
        *selected = count - 1;
        *detail_scroll = 0;
        switch (state->view) {
        case LULO_CGROUPS_VIEW_FILES:
            snprintf(state->selected_file_path, sizeof(state->selected_file_path), "%s",
                     snap->file_rows[*selected].path);
            break;
        case LULO_CGROUPS_VIEW_CONFIG:
            snprintf(state->selected_config, sizeof(state->selected_config), "%s",
                     snap->configs[*selected].path);
            break;
        case LULO_CGROUPS_VIEW_TREE:
        default:
            snprintf(state->selected_tree_path, sizeof(state->selected_tree_path), "%s",
                     snap->tree_rows[*selected].path);
            break;
        }
    }
    lulo_cgroups_view_sync(state, snap, list_rows, detail_rows);
}

void lulo_cgroups_set_cursor(LuloCgroupsState *state, const LuloCgroupsSnapshot *snap,
                             int list_rows, int detail_rows, int row_index)
{
    int count;
    int *cursor;
    int *selected;

    if (!state) return;
    lulo_cgroups_view_sync(state, snap, list_rows, detail_rows);
    count = active_list_count(state, snap);
    if (count <= 0) return;
    cursor = active_cursor(state);
    selected = active_selected(state);
    *cursor = clamp_int_local(row_index, 0, count - 1);
    *selected = *cursor;
    switch (state->view) {
    case LULO_CGROUPS_VIEW_FILES:
        snprintf(state->selected_file_path, sizeof(state->selected_file_path), "%s",
                 snap->file_rows[*selected].path);
        state->file_detail_scroll = 0;
        break;
    case LULO_CGROUPS_VIEW_CONFIG:
        snprintf(state->selected_config, sizeof(state->selected_config), "%s",
                 snap->configs[*selected].path);
        state->config_detail_scroll = 0;
        break;
    case LULO_CGROUPS_VIEW_TREE:
    default:
        snprintf(state->selected_tree_path, sizeof(state->selected_tree_path), "%s",
                 snap->tree_rows[*selected].path);
        state->tree_detail_scroll = 0;
        break;
    }
    lulo_cgroups_view_sync(state, snap, list_rows, detail_rows);
}

int lulo_cgroups_open_current(LuloCgroupsState *state, const LuloCgroupsSnapshot *snap,
                              int list_rows, int detail_rows)
{
    int idx;

    if (!state || !snap) return 0;
    lulo_cgroups_view_sync(state, snap, list_rows, detail_rows);
    if (state->view == LULO_CGROUPS_VIEW_TREE) {
        idx = state->tree_cursor;
        if (idx < 0 || idx >= snap->tree_count) return 0;
        if (strcmp(state->browse_path, snap->tree_rows[idx].path) == 0) return 1;
        snprintf(state->browse_path, sizeof(state->browse_path), "%s", snap->tree_rows[idx].path);
        snprintf(state->selected_tree_path, sizeof(state->selected_tree_path), "%s", snap->tree_rows[idx].path);
        state->tree_cursor = 0;
        state->tree_selected = -1;
        state->tree_list_scroll = 0;
        state->tree_detail_scroll = 0;
        state->file_cursor = -1;
        state->file_selected = -1;
        state->file_list_scroll = 0;
        state->file_detail_scroll = 0;
        return 2;
    }
    return 1;
}

void lulo_cgroups_toggle_focus(LuloCgroupsState *state, const LuloCgroupsSnapshot *snap,
                               int list_rows, int detail_rows)
{
    if (!state) return;
    lulo_cgroups_view_sync(state, snap, list_rows, detail_rows);
    if (!snap || snap->detail_line_count <= 0) {
        state->focus_preview = 0;
    } else {
        state->focus_preview = !state->focus_preview;
    }
    lulo_cgroups_view_sync(state, snap, list_rows, detail_rows);
}

void lulo_cgroups_next_view(LuloCgroupsState *state)
{
    if (!state) return;
    state->view = (LuloCgroupsView)((state->view + 1) % LULO_CGROUPS_VIEW_COUNT);
    state->focus_preview = 0;
}

void lulo_cgroups_prev_view(LuloCgroupsState *state)
{
    if (!state) return;
    state->view = (LuloCgroupsView)((state->view + LULO_CGROUPS_VIEW_COUNT - 1) % LULO_CGROUPS_VIEW_COUNT);
    state->focus_preview = 0;
}

const char *lulo_cgroups_view_name(LuloCgroupsView view)
{
    switch (view) {
    case LULO_CGROUPS_VIEW_FILES:
        return "Files";
    case LULO_CGROUPS_VIEW_CONFIG:
        return "Config";
    case LULO_CGROUPS_VIEW_TREE:
    default:
        return "Tree";
    }
}
