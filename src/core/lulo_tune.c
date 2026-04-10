#define _GNU_SOURCE

#include "lulo_tune.h"

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

static int active_list_count(const LuloTuneState *state, const LuloTuneSnapshot *snap)
{
    if (!state || !snap) return 0;
    switch (state->view) {
    case LULO_TUNE_VIEW_SNAPSHOTS:
        return snap->snapshot_count;
    case LULO_TUNE_VIEW_PRESETS:
        return snap->preset_count;
    case LULO_TUNE_VIEW_EXPLORE:
    default:
        return snap->count;
    }
}

static int active_preview_count(const LuloTuneState *state, const LuloTuneSnapshot *snap)
{
    if (!state || !snap) return 0;
    return snap->detail_line_count;
}

static int sync_tune_selection(LuloTuneState *state, const LuloTuneSnapshot *snap)
{
    int selected = -1;

    if (!state || !snap || snap->count <= 0) {
        if (state) {
            state->selected = -1;
            state->list_scroll = 0;
            state->detail_scroll = 0;
            state->selected_path[0] = '\0';
        }
        return -1;
    }
    if (state->selected_path[0]) {
        for (int i = 0; i < snap->count; i++) {
            if (strcmp(snap->rows[i].path, state->selected_path) == 0) {
                selected = i;
                break;
            }
        }
    }
    if (selected < 0 && state->selected >= 0) selected = clamp_int(state->selected, 0, snap->count - 1);
    state->selected = selected;
    if (selected >= 0) snprintf(state->selected_path, sizeof(state->selected_path), "%s", snap->rows[selected].path);
    else state->selected_path[0] = '\0';
    return selected;
}

static int sync_snapshot_selection(LuloTuneState *state, const LuloTuneSnapshot *snap)
{
    int selected = -1;

    if (!state || !snap || snap->snapshot_count <= 0) {
        if (state) {
            state->snapshot_selected = -1;
            state->snapshot_list_scroll = 0;
            state->snapshot_detail_scroll = 0;
            state->selected_snapshot_id[0] = '\0';
        }
        return -1;
    }
    if (state->selected_snapshot_id[0]) {
        for (int i = 0; i < snap->snapshot_count; i++) {
            if (strcmp(snap->snapshots[i].id, state->selected_snapshot_id) == 0) {
                selected = i;
                break;
            }
        }
    }
    if (selected < 0 && state->snapshot_selected >= 0) {
        selected = clamp_int(state->snapshot_selected, 0, snap->snapshot_count - 1);
    }
    state->snapshot_selected = selected;
    if (selected >= 0) {
        snprintf(state->selected_snapshot_id, sizeof(state->selected_snapshot_id), "%s", snap->snapshots[selected].id);
    } else {
        state->selected_snapshot_id[0] = '\0';
    }
    return selected;
}

static int sync_preset_selection(LuloTuneState *state, const LuloTuneSnapshot *snap)
{
    int selected = -1;

    if (!state || !snap || snap->preset_count <= 0) {
        if (state) {
            state->preset_selected = -1;
            state->preset_list_scroll = 0;
            state->preset_detail_scroll = 0;
            state->selected_preset_id[0] = '\0';
        }
        return -1;
    }
    if (state->selected_preset_id[0]) {
        for (int i = 0; i < snap->preset_count; i++) {
            if (strcmp(snap->presets[i].id, state->selected_preset_id) == 0) {
                selected = i;
                break;
            }
        }
    }
    if (selected < 0 && state->preset_selected >= 0) {
        selected = clamp_int(state->preset_selected, 0, snap->preset_count - 1);
    }
    state->preset_selected = selected;
    if (selected >= 0) {
        snprintf(state->selected_preset_id, sizeof(state->selected_preset_id), "%s", snap->presets[selected].id);
    } else {
        state->selected_preset_id[0] = '\0';
    }
    return selected;
}

static int sync_active_cursor(LuloTuneState *state, const LuloTuneSnapshot *snap)
{
    int count;
    int *cursor = NULL;
    int *scroll = NULL;

    if (!state || !snap) return -1;
    count = active_list_count(state, snap);
    switch (state->view) {
    case LULO_TUNE_VIEW_SNAPSHOTS:
        cursor = &state->snapshot_cursor;
        scroll = &state->snapshot_list_scroll;
        break;
    case LULO_TUNE_VIEW_PRESETS:
        cursor = &state->preset_cursor;
        scroll = &state->preset_list_scroll;
        break;
    case LULO_TUNE_VIEW_EXPLORE:
    default:
        cursor = &state->cursor;
        scroll = &state->list_scroll;
        break;
    }
    if (count <= 0) {
        *cursor = -1;
        *scroll = 0;
        return -1;
    }
    if (*cursor < 0) *cursor = 0;
    *cursor = clamp_int(*cursor, 0, count - 1);
    return *cursor;
}

void lulo_tune_state_init(LuloTuneState *state)
{
    memset(state, 0, sizeof(*state));
    state->view = LULO_TUNE_VIEW_EXPLORE;
    state->cursor = -1;
    state->selected = -1;
    state->snapshot_cursor = -1;
    state->snapshot_selected = -1;
    state->preset_cursor = -1;
    state->preset_selected = -1;
}

void lulo_tune_state_cleanup(LuloTuneState *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
}

int lulo_tune_snapshot_clone(LuloTuneSnapshot *dst, const LuloTuneSnapshot *src)
{
    if (!dst || !src) return -1;
    memset(dst, 0, sizeof(*dst));
    snprintf(dst->detail_title, sizeof(dst->detail_title), "%s", src->detail_title);
    snprintf(dst->detail_status, sizeof(dst->detail_status), "%s", src->detail_status);

    if (src->count > 0) {
        dst->rows = malloc((size_t)src->count * sizeof(*dst->rows));
        if (!dst->rows) goto fail;
        memcpy(dst->rows, src->rows, (size_t)src->count * sizeof(*dst->rows));
        dst->count = src->count;
    }
    if (src->snapshot_count > 0) {
        dst->snapshots = malloc((size_t)src->snapshot_count * sizeof(*dst->snapshots));
        if (!dst->snapshots) goto fail;
        memcpy(dst->snapshots, src->snapshots, (size_t)src->snapshot_count * sizeof(*dst->snapshots));
        dst->snapshot_count = src->snapshot_count;
    }
    if (src->preset_count > 0) {
        dst->presets = malloc((size_t)src->preset_count * sizeof(*dst->presets));
        if (!dst->presets) goto fail;
        memcpy(dst->presets, src->presets, (size_t)src->preset_count * sizeof(*dst->presets));
        dst->preset_count = src->preset_count;
    }
    for (int i = 0; i < src->detail_line_count; i++) {
        if (append_snapshot_line(&dst->detail_lines, &dst->detail_line_count, src->detail_lines[i]) < 0) goto fail;
    }
    return 0;

fail:
    lulo_tune_snapshot_free(dst);
    return -1;
}

void lulo_tune_snapshot_mark_loading(LuloTuneSnapshot *snap, const LuloTuneState *state)
{
    if (!snap || !state) return;
    clear_lines(&snap->detail_lines, &snap->detail_line_count);
    switch (state->view) {
    case LULO_TUNE_VIEW_SNAPSHOTS:
        snprintf(snap->detail_title, sizeof(snap->detail_title), "snapshot");
        break;
    case LULO_TUNE_VIEW_PRESETS:
        snprintf(snap->detail_title, sizeof(snap->detail_title), "preset");
        break;
    case LULO_TUNE_VIEW_EXPLORE:
    default:
        snprintf(snap->detail_title, sizeof(snap->detail_title), "tunable");
        break;
    }
    snprintf(snap->detail_status, sizeof(snap->detail_status), "loading...");
    append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, "loading...");
}

void lulo_tune_snapshot_free(LuloTuneSnapshot *snap)
{
    if (!snap) return;
    clear_lines(&snap->detail_lines, &snap->detail_line_count);
    free(snap->rows);
    free(snap->snapshots);
    free(snap->presets);
    memset(snap, 0, sizeof(*snap));
}

void lulo_tune_view_sync(LuloTuneState *state, const LuloTuneSnapshot *snap,
                         int list_rows, int detail_rows)
{
    int count;
    int preview_count;
    int *cursor;
    int *list_scroll;
    int *detail_scroll;
    int max_scroll;
    int max_preview_scroll;

    if (!state) return;
    sync_tune_selection(state, snap);
    sync_snapshot_selection(state, snap);
    sync_preset_selection(state, snap);
    sync_active_cursor(state, snap);

    switch (state->view) {
    case LULO_TUNE_VIEW_SNAPSHOTS:
        cursor = &state->snapshot_cursor;
        list_scroll = &state->snapshot_list_scroll;
        detail_scroll = &state->snapshot_detail_scroll;
        break;
    case LULO_TUNE_VIEW_PRESETS:
        cursor = &state->preset_cursor;
        list_scroll = &state->preset_list_scroll;
        detail_scroll = &state->preset_detail_scroll;
        break;
    case LULO_TUNE_VIEW_EXPLORE:
    default:
        cursor = &state->cursor;
        list_scroll = &state->list_scroll;
        detail_scroll = &state->detail_scroll;
        break;
    }

    count = active_list_count(state, snap);
    preview_count = active_preview_count(state, snap);
    if (list_rows <= 0) list_rows = 1;
    if (detail_rows <= 0) detail_rows = 1;

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

    max_preview_scroll = preview_count > detail_rows ? preview_count - detail_rows : 0;
    *detail_scroll = clamp_int(*detail_scroll, 0, max_preview_scroll);
    if (preview_count <= 0) state->focus_preview = 0;
}

void lulo_tune_view_move(LuloTuneState *state, const LuloTuneSnapshot *snap,
                         int list_rows, int detail_rows, int delta)
{
    int count;
    int *cursor;
    int *detail_scroll;

    if (!state) return;
    lulo_tune_view_sync(state, snap, list_rows, detail_rows);
    switch (state->view) {
    case LULO_TUNE_VIEW_SNAPSHOTS:
        cursor = &state->snapshot_cursor;
        detail_scroll = &state->snapshot_detail_scroll;
        break;
    case LULO_TUNE_VIEW_PRESETS:
        cursor = &state->preset_cursor;
        detail_scroll = &state->preset_detail_scroll;
        break;
    case LULO_TUNE_VIEW_EXPLORE:
    default:
        cursor = &state->cursor;
        detail_scroll = &state->detail_scroll;
        break;
    }
    if (state->focus_preview) {
        *detail_scroll += delta;
        lulo_tune_view_sync(state, snap, list_rows, detail_rows);
        return;
    }
    count = active_list_count(state, snap);
    if (count <= 0) return;
    if (*cursor < 0) {
        *cursor = delta < 0 ? count - 1 : 0;
        *detail_scroll = 0;
        lulo_tune_view_sync(state, snap, list_rows, detail_rows);
        return;
    }
    *cursor = clamp_int(*cursor + delta, 0, count - 1);
    *detail_scroll = 0;
    lulo_tune_view_sync(state, snap, list_rows, detail_rows);
}

void lulo_tune_view_page(LuloTuneState *state, const LuloTuneSnapshot *snap,
                         int list_rows, int detail_rows, int pages)
{
    int delta;

    if (!state) return;
    delta = (state->focus_preview ? detail_rows : list_rows) * pages;
    if (delta == 0) delta = pages;
    lulo_tune_view_move(state, snap, list_rows, detail_rows, delta);
}

void lulo_tune_view_home(LuloTuneState *state, const LuloTuneSnapshot *snap,
                         int list_rows, int detail_rows)
{
    if (!state) return;
    if (state->focus_preview) {
        switch (state->view) {
        case LULO_TUNE_VIEW_SNAPSHOTS: state->snapshot_detail_scroll = 0; break;
        case LULO_TUNE_VIEW_PRESETS: state->preset_detail_scroll = 0; break;
        case LULO_TUNE_VIEW_EXPLORE:
        default: state->detail_scroll = 0; break;
        }
    } else {
        switch (state->view) {
        case LULO_TUNE_VIEW_SNAPSHOTS:
            state->snapshot_cursor = 0;
            state->snapshot_detail_scroll = 0;
            break;
        case LULO_TUNE_VIEW_PRESETS:
            state->preset_cursor = 0;
            state->preset_detail_scroll = 0;
            break;
        case LULO_TUNE_VIEW_EXPLORE:
        default:
            state->cursor = 0;
            state->detail_scroll = 0;
            break;
        }
    }
    lulo_tune_view_sync(state, snap, list_rows, detail_rows);
}

void lulo_tune_view_end(LuloTuneState *state, const LuloTuneSnapshot *snap,
                        int list_rows, int detail_rows)
{
    int count;
    int preview_count;

    if (!state) return;
    count = active_list_count(state, snap);
    preview_count = active_preview_count(state, snap);
    if (state->focus_preview) {
        int max_preview = preview_count > detail_rows ? preview_count - detail_rows : 0;
        switch (state->view) {
        case LULO_TUNE_VIEW_SNAPSHOTS: state->snapshot_detail_scroll = max_preview; break;
        case LULO_TUNE_VIEW_PRESETS: state->preset_detail_scroll = max_preview; break;
        case LULO_TUNE_VIEW_EXPLORE:
        default: state->detail_scroll = max_preview; break;
        }
    } else if (count > 0) {
        switch (state->view) {
        case LULO_TUNE_VIEW_SNAPSHOTS:
            state->snapshot_cursor = count - 1;
            state->snapshot_detail_scroll = 0;
            break;
        case LULO_TUNE_VIEW_PRESETS:
            state->preset_cursor = count - 1;
            state->preset_detail_scroll = 0;
            break;
        case LULO_TUNE_VIEW_EXPLORE:
        default:
            state->cursor = count - 1;
            state->detail_scroll = 0;
            break;
        }
    }
    lulo_tune_view_sync(state, snap, list_rows, detail_rows);
}

void lulo_tune_set_cursor(LuloTuneState *state, const LuloTuneSnapshot *snap,
                          int list_rows, int detail_rows, int row_index)
{
    int count;

    if (!state) return;
    count = active_list_count(state, snap);
    if (count <= 0) return;
    row_index = clamp_int(row_index, 0, count - 1);
    switch (state->view) {
    case LULO_TUNE_VIEW_SNAPSHOTS: state->snapshot_cursor = row_index; break;
    case LULO_TUNE_VIEW_PRESETS: state->preset_cursor = row_index; break;
    case LULO_TUNE_VIEW_EXPLORE:
    default: state->cursor = row_index; break;
    }
    lulo_tune_view_sync(state, snap, list_rows, detail_rows);
}

int lulo_tune_open_current(LuloTuneState *state, const LuloTuneSnapshot *snap,
                           int list_rows, int detail_rows)
{
    int changed = 0;

    if (!state || !snap) return 0;
    lulo_tune_view_sync(state, snap, list_rows, detail_rows);
    switch (state->view) {
    case LULO_TUNE_VIEW_SNAPSHOTS: {
        int cursor = state->snapshot_cursor;

        if (cursor < 0 || cursor >= snap->snapshot_count) return 0;
        if (state->snapshot_selected == cursor &&
            strcmp(state->selected_snapshot_id, snap->snapshots[cursor].id) == 0) {
            state->snapshot_selected = -1;
            state->selected_snapshot_id[0] = '\0';
            state->snapshot_detail_scroll = 0;
            state->focus_preview = 0;
            lulo_tune_view_sync(state, snap, list_rows, detail_rows);
            return 1;
        }
        if (state->snapshot_selected != cursor ||
            strcmp(state->selected_snapshot_id, snap->snapshots[cursor].id) != 0) {
            state->snapshot_selected = cursor;
            snprintf(state->selected_snapshot_id, sizeof(state->selected_snapshot_id), "%s", snap->snapshots[cursor].id);
            state->snapshot_detail_scroll = 0;
            changed = 1;
        }
        break;
    }
    case LULO_TUNE_VIEW_PRESETS: {
        int cursor = state->preset_cursor;

        if (cursor < 0 || cursor >= snap->preset_count) return 0;
        if (state->preset_selected == cursor &&
            strcmp(state->selected_preset_id, snap->presets[cursor].id) == 0) {
            state->preset_selected = -1;
            state->selected_preset_id[0] = '\0';
            state->preset_detail_scroll = 0;
            state->focus_preview = 0;
            lulo_tune_view_sync(state, snap, list_rows, detail_rows);
            return 1;
        }
        if (state->preset_selected != cursor ||
            strcmp(state->selected_preset_id, snap->presets[cursor].id) != 0) {
            state->preset_selected = cursor;
            snprintf(state->selected_preset_id, sizeof(state->selected_preset_id), "%s", snap->presets[cursor].id);
            state->preset_detail_scroll = 0;
            changed = 1;
        }
        break;
    }
    case LULO_TUNE_VIEW_EXPLORE:
    default: {
        int cursor = state->cursor;

        if (cursor < 0 || cursor >= snap->count) return 0;
        if (snap->rows[cursor].is_dir) {
            state->cursor = -1;
            state->selected = -1;
            state->list_scroll = 0;
            state->detail_scroll = 0;
            state->focus_preview = 0;
            state->selected_path[0] = '\0';
            snprintf(state->browse_path, sizeof(state->browse_path), "%s", snap->rows[cursor].path);
            return 2;
        }
        if (state->selected == cursor &&
            strcmp(state->selected_path, snap->rows[cursor].path) == 0) {
            state->selected = -1;
            state->selected_path[0] = '\0';
            state->detail_scroll = 0;
            state->focus_preview = 0;
            lulo_tune_view_sync(state, snap, list_rows, detail_rows);
            return 1;
        }
        if (state->selected != cursor ||
            strcmp(state->selected_path, snap->rows[cursor].path) != 0) {
            state->selected = cursor;
            snprintf(state->selected_path, sizeof(state->selected_path), "%s", snap->rows[cursor].path);
            state->detail_scroll = 0;
            changed = 1;
        }
        break;
    }
    }
    lulo_tune_view_sync(state, snap, list_rows, detail_rows);
    return changed;
}

void lulo_tune_toggle_focus(LuloTuneState *state, const LuloTuneSnapshot *snap,
                            int list_rows, int detail_rows)
{
    if (!state) return;
    if (active_preview_count(state, snap) <= 0) {
        state->focus_preview = 0;
        lulo_tune_view_sync(state, snap, list_rows, detail_rows);
        return;
    }
    state->focus_preview = !state->focus_preview;
    lulo_tune_view_sync(state, snap, list_rows, detail_rows);
}

void lulo_tune_next_view(LuloTuneState *state)
{
    if (!state) return;
    state->view = (LuloTuneView)((state->view + 1) % LULO_TUNE_VIEW_COUNT);
    state->focus_preview = 0;
}

void lulo_tune_prev_view(LuloTuneState *state)
{
    if (!state) return;
    state->view = (LuloTuneView)((state->view + LULO_TUNE_VIEW_COUNT - 1) % LULO_TUNE_VIEW_COUNT);
    state->focus_preview = 0;
}

const char *lulo_tune_view_name(LuloTuneView view)
{
    switch (view) {
    case LULO_TUNE_VIEW_SNAPSHOTS:
        return "Snapshots";
    case LULO_TUNE_VIEW_PRESETS:
        return "Presets";
    case LULO_TUNE_VIEW_EXPLORE:
    default:
        return "Explore";
    }
}

const char *lulo_tune_source_name(LuloTuneSource source)
{
    switch (source) {
    case LULO_TUNE_SOURCE_SYS:
        return "sys";
    case LULO_TUNE_SOURCE_CGROUP:
        return "cg";
    case LULO_TUNE_SOURCE_PROC:
    default:
        return "proc";
    }
}
