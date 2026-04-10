#define _GNU_SOURCE

#include "lulo_app.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    int tabs_y;
    int tabs_x;
    LuloRect list;
    LuloRect info;
    LuloRect preview;
    int show_info;
} TuneWidgetLayout;

static void plane_clear_inner_local(struct ncplane *p, const Theme *theme, int rows, int cols)
{
    for (int y = 1; y < rows - 1; y++) {
        plane_fill(p, y, 1, cols - 2, theme->bg, theme->bg);
    }
}

static int point_in_rect_global_local(const LuloRect *origin, const LuloRect *rect, int y, int x)
{
    int top = origin->row + 1 + rect->row;
    int left = origin->col + 1 + rect->col;

    return y >= top && y < top + rect->height && x >= left && x < left + rect->width;
}

static int tune_explore_selection_active(const LuloTuneState *state)
{
    return state && state->selected >= 0 && state->selected_path[0];
}

static int tune_snapshot_selection_active(const LuloTuneState *state)
{
    return state && state->snapshot_selected >= 0 && state->selected_snapshot_id[0];
}

static int tune_preset_selection_active(const LuloTuneState *state)
{
    return state && state->preset_selected >= 0 && state->selected_preset_id[0];
}

static int tune_preview_open(const LuloTuneState *state)
{
    if (!state) return 0;
    switch (state->view) {
    case LULO_TUNE_VIEW_SNAPSHOTS:
        return tune_snapshot_selection_active(state);
    case LULO_TUNE_VIEW_PRESETS:
        return tune_preset_selection_active(state);
    case LULO_TUNE_VIEW_EXPLORE:
    default:
        return tune_explore_selection_active(state);
    }
}

static void build_tune_widget_layout(const Ui *ui, const LuloTuneState *state, TuneWidgetLayout *layout)
{
    unsigned rows = 0;
    unsigned cols = 0;
    int inner_w;
    int content_h;
    int info_h;

    memset(layout, 0, sizeof(*layout));
    if (!ui->tune) return;
    ncplane_dim_yx(ui->tune, &rows, &cols);
    if (rows < 8 || cols < 28) return;
    layout->tabs_y = 1;
    layout->tabs_x = 2;
    inner_w = (int)cols - 2;
    content_h = (int)rows - 4;
    if (inner_w < 20 || content_h < 4) return;

    info_h = 5;
    layout->show_info = content_h >= info_h + 4;
    if (!tune_preview_open(state)) {
        layout->list.row = 2;
        layout->list.col = 1;
        layout->list.width = inner_w;
        layout->list.height = content_h;
        layout->show_info = 0;
        return;
    }

    if (inner_w >= 104) {
        int list_w = clamp_int(inner_w * 11 / 20, 42, inner_w - 34);
        int right_w = inner_w - list_w - 1;

        layout->list.row = 2;
        layout->list.col = 1;
        layout->list.width = list_w;
        layout->list.height = content_h;
        layout->info.row = 2;
        layout->info.col = list_w + 2;
        layout->info.width = right_w;
        layout->info.height = layout->show_info ? info_h : 0;
        layout->preview.col = list_w + 2;
        layout->preview.width = right_w;
        if (layout->show_info) {
            layout->preview.row = 2 + info_h + 1;
            layout->preview.height = content_h - info_h - 1;
        } else {
            layout->preview.row = 2;
            layout->preview.height = content_h;
        }
        return;
    }

    {
        int list_h = clamp_int(content_h / 3 + 1, 7, content_h - 4);

        layout->list.row = 2;
        layout->list.col = 1;
        layout->list.width = inner_w;
        layout->list.height = list_h;
        layout->info.row = 2 + list_h + 1;
        layout->info.col = 1;
        layout->info.width = inner_w;
        layout->info.height = layout->show_info ? info_h : 0;
        layout->preview.col = 1;
        layout->preview.width = inner_w;
        if (layout->show_info) {
            layout->preview.row = layout->info.row + info_h + 1;
            layout->preview.height = content_h - list_h - info_h - 2;
        } else {
            layout->preview.row = 2 + list_h + 1;
            layout->preview.height = content_h - list_h - 1;
        }
    }
}

int tune_list_rows_visible(const Ui *ui, const LuloTuneState *state)
{
    TuneWidgetLayout layout;

    if (!ui->tune || !state) return 1;
    build_tune_widget_layout(ui, state, &layout);
    return clamp_int(rect_inner_rows(&layout.list) - 1, 1, 4096);
}

int tune_preview_rows_visible(const Ui *ui, const LuloTuneState *state)
{
    TuneWidgetLayout layout;

    if (!ui->tune || !state) return 1;
    build_tune_widget_layout(ui, state, &layout);
    return clamp_int(rect_inner_rows(&layout.preview), 1, 4096);
}

static const LuloTuneRow *selected_tune_row(const LuloTuneSnapshot *snap, const LuloTuneState *state)
{
    if (!snap || !state || snap->count <= 0 || !tune_explore_selection_active(state)) return NULL;
    if (state->selected >= 0 && state->selected < snap->count) return &snap->rows[state->selected];
    return NULL;
}

const LuloTuneRow *active_tune_explore_row(const LuloTuneSnapshot *snap, const LuloTuneState *state)
{
    const LuloTuneRow *row = selected_tune_row(snap, state);

    if (row) return row;
    if (!snap || !state || snap->count <= 0) return NULL;
    if (state->cursor >= 0 && state->cursor < snap->count) return &snap->rows[state->cursor];
    return NULL;
}

int start_tune_edit(AppState *app, const LuloTuneSnapshot *snap, const LuloTuneState *state)
{
    const LuloTuneRow *row;

    if (!app || !snap || !state) return 0;
    if (state->view != LULO_TUNE_VIEW_EXPLORE) {
        tune_status_set(app, "edit is available from Explore");
        return 0;
    }
    row = active_tune_explore_row(snap, state);
    if (!row) {
        tune_status_set(app, "no tunable selected");
        return 0;
    }
    if (row->is_dir) {
        tune_status_set(app, "select a file to edit");
        return 0;
    }
    app->tune_edit_active = 1;
    snprintf(app->tune_edit_path, sizeof(app->tune_edit_path), "%s", row->path);
    if (state->staged_path[0] && strcmp(state->staged_path, row->path) == 0) {
        snprintf(app->tune_edit_value, sizeof(app->tune_edit_value), "%s", state->staged_value);
    } else {
        snprintf(app->tune_edit_value, sizeof(app->tune_edit_value), "%s", row->value);
    }
    app->tune_edit_len = (int)strlen(app->tune_edit_value);
    tune_edit_prompt_refresh(app);
    return 1;
}

int active_tune_row_is_staged(const LuloTuneSnapshot *snap, const LuloTuneState *state)
{
    const LuloTuneRow *row = active_tune_explore_row(snap, state);

    return row && state && state->staged_path[0] && strcmp(state->staged_path, row->path) == 0;
}

int handle_tune_edit_input(AppState *app, const DecodedInput *in,
                           LuloTuneState *tune_state, RenderFlags *render)
{
    if (!app || !in || !tune_state || !app->tune_edit_active) return 0;

    if (in->cancel) {
        app->tune_edit_active = 0;
        app->tune_edit_path[0] = '\0';
        app->tune_edit_value[0] = '\0';
        app->tune_edit_len = 0;
        tune_status_set(app, "edit cancelled");
        render->need_tune = 1;
        render->need_render = 1;
        return 1;
    }
    if (in->submit) {
        app->tune_edit_active = 0;
        snprintf(tune_state->staged_path, sizeof(tune_state->staged_path), "%s", app->tune_edit_path);
        snprintf(tune_state->staged_value, sizeof(tune_state->staged_value), "%s", app->tune_edit_value);
        tune_status_set(app, "staged value for %s",
                        tune_state->selected_path[0] ? tune_state->selected_path : app->tune_edit_path);
        app->tune_edit_path[0] = '\0';
        app->tune_edit_value[0] = '\0';
        app->tune_edit_len = 0;
        render->need_tune = 1;
        render->need_render = 1;
        return 1;
    }
    if (in->backspace) {
        if (app->tune_edit_len > 0) {
            app->tune_edit_value[--app->tune_edit_len] = '\0';
            tune_edit_prompt_refresh(app);
            render->need_tune = 1;
            render->need_render = 1;
        }
        return 1;
    }
    if (in->text_len > 0) {
        int avail = (int)sizeof(app->tune_edit_value) - 1 - app->tune_edit_len;

        if (avail > 0) {
            int take = in->text_len < avail ? in->text_len : avail;

            memcpy(app->tune_edit_value + app->tune_edit_len, in->text, (size_t)take);
            app->tune_edit_len += take;
            app->tune_edit_value[app->tune_edit_len] = '\0';
            tune_edit_prompt_refresh(app);
            render->need_tune = 1;
            render->need_render = 1;
        }
        return 1;
    }
    return 1;
}

static const LuloTuneBundleMeta *selected_tune_bundle(const LuloTuneSnapshot *snap, const LuloTuneState *state, int preset)
{
    if (!snap || !state) return NULL;
    if (preset) {
        if (!tune_preset_selection_active(state) || snap->preset_count <= 0) return NULL;
        if (state->preset_selected >= 0 && state->preset_selected < snap->preset_count) {
            return &snap->presets[state->preset_selected];
        }
    } else {
        if (!tune_snapshot_selection_active(state) || snap->snapshot_count <= 0) return NULL;
        if (state->snapshot_selected >= 0 && state->snapshot_selected < snap->snapshot_count) {
            return &snap->snapshots[state->snapshot_selected];
        }
    }
    return NULL;
}

static Rgb tune_source_color(const Theme *theme, LuloTuneSource source)
{
    switch (source) {
    case LULO_TUNE_SOURCE_SYS:
        return theme->green;
    case LULO_TUNE_SOURCE_CGROUP:
        return theme->orange;
    case LULO_TUNE_SOURCE_PROC:
    default:
        return theme->cyan;
    }
}

static void render_tune_view_tabs(struct ncplane *p, const Theme *theme, const TuneWidgetLayout *layout,
                                  const LuloTuneState *state)
{
    unsigned cols = 0;
    int x = layout->tabs_x;

    ncplane_dim_yx(p, NULL, &cols);
    plane_fill(p, layout->tabs_y, 1, (int)cols - 2, theme->bg, theme->bg);
    for (int i = 0; i < LULO_TUNE_VIEW_COUNT; i++) {
        char label[32];
        int active = state && state->view == (LuloTuneView)i;
        int width;
        Rgb fg = active ? theme->bg : theme->white;
        Rgb bg = active ? theme->border_header : theme->bg;

        snprintf(label, sizeof(label), " %s ", lulo_tune_view_name((LuloTuneView)i));
        width = (int)strlen(label);
        plane_putn(p, layout->tabs_y, x, fg, bg, label, width);
        x += width + 1;
    }
}

static void render_tune_explore_list(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                     const LuloTuneSnapshot *snap, const LuloTuneState *state,
                                     const AppState *app)
{
    int visible_rows;
    int start;
    int src_w = 4;
    int rw_w = 2;
    int type_w = 3;
    int name_w;
    int value_w;
    const char *path_meta;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Explorer ", theme->white);
    path_meta = state && state->browse_path[0] ? state->browse_path : "roots";
    draw_inner_meta(p, theme, rect, path_meta, state && !state->focus_preview ? theme->green : theme->cyan);
    visible_rows = clamp_int(rect_inner_rows(rect) - 1, 1, 4096);
    start = state ? clamp_int(state->list_scroll, 0, snap && snap->count > visible_rows ? snap->count - visible_rows : 0) : 0;
    plane_putn(p, rect->row + 1, rect->col + 1, theme->dim, theme->bg, "src", src_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + src_w + 1, theme->dim, theme->bg, "rw", rw_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + src_w + 1 + rw_w + 1, theme->dim, theme->bg, "typ", type_w);
    name_w = rect->width >= 78 ? 22 : rect->width >= 64 ? 18 : 14;
    value_w = rect_inner_cols(rect) - src_w - rw_w - type_w - name_w - 4;
    if (value_w < 10) value_w = 10;
    plane_putn(p, rect->row + 1, rect->col + 1 + src_w + 1 + rw_w + 1 + type_w + 1, theme->dim, theme->bg, "name", name_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + src_w + 1 + rw_w + 1 + type_w + 1 + name_w + 1,
               theme->dim, theme->bg, "value / path", value_w);
    if (!snap || snap->count <= 0) {
        plane_putn(p, rect->row + 2, rect->col + 2, theme->white, theme->bg, "empty directory", rect->width - 4);
        return;
    }
    for (int i = 0; i < visible_rows; i++) {
        int idx = start + i;
        int y = rect->row + 2 + i;
        int x = rect->col + 1;
        int selected = state && idx == state->cursor;
        int editing = 0;
        Rgb row_bg = selected ? theme->select_bg : theme->bg;
        const LuloTuneRow *row;
        char edit_buf[224];
        const char *type_text;
        const char *value_text;
        Rgb type_fg;
        Rgb value_fg;

        plane_fill(p, y, rect->col + 1, rect_inner_cols(rect), row_bg, row_bg);
        if (idx >= snap->count) continue;
        row = &snap->rows[idx];
        editing = app && app->tune_edit_active && strcmp(app->tune_edit_path, row->path) == 0;
        type_text = row->is_dir ? "dir" :
                    (editing ? "edt" :
                     (state && state->staged_path[0] && strcmp(state->staged_path, row->path) == 0 ? "stg" : "val"));
        if (editing && !row->is_dir) {
            snprintf(edit_buf, sizeof(edit_buf), "%s|", app->tune_edit_value);
            value_text = edit_buf;
        } else {
            value_text = row->is_dir ? row->path :
                         (state && state->staged_path[0] && strcmp(state->staged_path, row->path) == 0 ? state->staged_value : row->value);
        }
        type_fg = selected ? theme->select_fg :
                  (row->is_dir ? theme->orange :
                   (editing ? theme->yellow :
                    (state && state->staged_path[0] && strcmp(state->staged_path, row->path) == 0 ? theme->green : theme->white)));
        value_fg = selected ? theme->select_fg :
                   (editing ? theme->yellow :
                    (state && state->staged_path[0] && strcmp(state->staged_path, row->path) == 0 ? theme->green : theme->dim));
        plane_putn(p, y, x, selected ? theme->select_fg : tune_source_color(theme, row->source),
                   row_bg, lulo_tune_source_name(row->source), src_w);
        x += src_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg : (row->writable ? theme->green : theme->dim),
                   row_bg, row->is_dir ? "--" : (row->writable ? "rw" : "ro"), rw_w);
        x += rw_w + 1;
        plane_putn(p, y, x, type_fg, row_bg, type_text, type_w);
        x += type_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg : (row->is_dir ? theme->yellow : theme->white),
                   row_bg, row->name, name_w);
        x += name_w + 1;
        plane_putn(p, y, x, value_fg, row_bg, value_text, value_w);
    }
}

static void render_tune_bundle_list(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                    const LuloTuneSnapshot *snap, const LuloTuneState *state, int preset)
{
    char meta[48];
    int visible_rows;
    int start;
    int created_w = 19;
    int items_w = 5;
    int name_w;
    const LuloTuneBundleMeta *items = preset ? snap->presets : snap->snapshots;
    int count = preset ? snap->preset_count : snap->snapshot_count;
    int cursor = preset ? state->preset_cursor : state->snapshot_cursor;
    int scroll = preset ? state->preset_list_scroll : state->snapshot_list_scroll;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, preset ? " Presets " : " Snapshots ", theme->white);
    visible_rows = clamp_int(rect_inner_rows(rect) - 1, 1, 4096);
    start = clamp_int(scroll, 0, count > visible_rows ? count - visible_rows : 0);
    if (count > 0) {
        snprintf(meta, sizeof(meta), "%d-%d/%d",
                 start + 1, clamp_int(start + visible_rows, 1, count), count);
        draw_inner_meta(p, theme, rect, meta, state && !state->focus_preview ? theme->green : theme->cyan);
    }
    plane_putn(p, rect->row + 1, rect->col + 1, theme->dim, theme->bg, "created", created_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + created_w + 1, theme->dim, theme->bg, "itms", items_w);
    name_w = rect_inner_cols(rect) - created_w - items_w - 2;
    if (name_w < 12) name_w = 12;
    plane_putn(p, rect->row + 1, rect->col + 1 + created_w + 1 + items_w + 1, theme->dim, theme->bg, "name", name_w);
    if (count <= 0) {
        plane_putn(p, rect->row + 2, rect->col + 2, theme->white, theme->bg, "no saved configs", rect->width - 4);
        return;
    }
    for (int i = 0; i < visible_rows; i++) {
        int idx = start + i;
        int y = rect->row + 2 + i;
        int x = rect->col + 1;
        int selected = idx == cursor;
        Rgb row_bg = selected ? theme->select_bg : theme->bg;
        char items_buf[8];

        plane_fill(p, y, rect->col + 1, rect_inner_cols(rect), row_bg, row_bg);
        if (idx >= count) continue;
        snprintf(items_buf, sizeof(items_buf), "%d", items[idx].item_count);
        plane_putn(p, y, x, selected ? theme->select_fg : theme->cyan, row_bg, items[idx].created, created_w);
        x += created_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg : theme->green, row_bg, items_buf, items_w);
        x += items_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg : theme->white, row_bg, items[idx].name, name_w);
    }
}

static void render_tune_info(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                             const LuloTuneSnapshot *snap, const LuloTuneState *state,
                             const AppState *app)
{
    char buf[512];

    if (!rect_valid(rect) || !state) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Details ", theme->white);
    if (state->view == LULO_TUNE_VIEW_EXPLORE) {
        const LuloTuneRow *row = selected_tune_row(snap, state);

        if (!row) {
            plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg, "no file selected", rect->width - 4);
            return;
        }
        plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg, row->name, rect->width - 4);
        plane_putn(p, rect->row + 2, rect->col + 2, theme->dim, theme->bg, row->path, rect->width - 4);
        snprintf(buf, sizeof(buf), "%s  %s  %s",
                 lulo_tune_source_name(row->source), row->group,
                 row->is_dir ? "directory" : (row->writable ? "writable" : "read-only"));
        plane_putn(p, rect->row + 3, rect->col + 2,
                   row->is_dir ? theme->orange : tune_source_color(theme, row->source),
                   theme->bg, buf, rect->width - 4);
        if (!row->is_dir && app && app->tune_edit_active && strcmp(app->tune_edit_path, row->path) == 0) {
            snprintf(buf, sizeof(buf), "editing: %s|", app->tune_edit_value);
            plane_putn(p, rect->row + 4, rect->col + 2, theme->yellow, theme->bg, buf, rect->width - 4);
        } else if (!row->is_dir && state->staged_path[0] && strcmp(state->staged_path, row->path) == 0) {
            snprintf(buf, sizeof(buf), "staged: %s", state->staged_value[0] ? state->staged_value : "(empty)");
            plane_putn(p, rect->row + 4, rect->col + 2, theme->green, theme->bg, buf, rect->width - 4);
        } else {
            plane_putn(p, rect->row + 4, rect->col + 2,
                       row->is_dir ? theme->dim : theme->green, theme->bg,
                       row->is_dir ? "<directory>" : row->value, rect->width - 4);
        }
        return;
    }

    {
        const LuloTuneBundleMeta *bundle = selected_tune_bundle(snap, state, state->view == LULO_TUNE_VIEW_PRESETS);

        if (!bundle) {
            plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg, "no saved config selected", rect->width - 4);
            return;
        }
        plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg, bundle->name, rect->width - 4);
        plane_putn(p, rect->row + 2, rect->col + 2, theme->dim, theme->bg, bundle->id, rect->width - 4);
        snprintf(buf, sizeof(buf), "created %s", bundle->created);
        plane_putn(p, rect->row + 3, rect->col + 2, theme->cyan, theme->bg, buf, rect->width - 4);
        snprintf(buf, sizeof(buf), "items %d", bundle->item_count);
        plane_putn(p, rect->row + 4, rect->col + 2, theme->green, theme->bg, buf, rect->width - 4);
    }
}

static Rgb tune_preview_line_color(const Theme *theme, const char *line)
{
    if (!line || !*line) return theme->dim;
    if (!strncmp(line, "path:", 5) || !strncmp(line, "id:", 3) || !strncmp(line, "created:", 8)) return theme->cyan;
    if (!strncmp(line, "note:", 5)) return theme->yellow;
    if (!strncmp(line, "current value", 13) || !strncmp(line, "raw file", 8)) return theme->white;
    if (line[0] == '[') return theme->green;
    if (strchr(line, '=')) return theme->green;
    return theme->white;
}

static void render_tune_preview(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                const LuloTuneSnapshot *snap, const LuloTuneState *state)
{
    char meta[48];
    const char *title = " Preview ";
    int visible_rows;
    int start;
    int scroll = 0;

    if (!rect_valid(rect) || !state) return;
    if (state->view == LULO_TUNE_VIEW_SNAPSHOTS) title = " Snapshot ";
    else if (state->view == LULO_TUNE_VIEW_PRESETS) title = " Preset ";
    draw_inner_box(p, theme, rect, theme->border_panel, title, theme->white);
    visible_rows = rect_inner_rows(rect);
    switch (state->view) {
    case LULO_TUNE_VIEW_SNAPSHOTS: scroll = state->snapshot_detail_scroll; break;
    case LULO_TUNE_VIEW_PRESETS: scroll = state->preset_detail_scroll; break;
    case LULO_TUNE_VIEW_EXPLORE:
    default: scroll = state->detail_scroll; break;
    }
    start = clamp_int(scroll, 0, snap->detail_line_count > visible_rows ? snap->detail_line_count - visible_rows : 0);
    if (snap->detail_line_count > 0) {
        snprintf(meta, sizeof(meta), "%d-%d/%d",
                 start + 1, clamp_int(start + visible_rows, 1, snap->detail_line_count), snap->detail_line_count);
        draw_inner_meta(p, theme, rect, meta, state->focus_preview ? theme->green : theme->cyan);
    }
    if (!snap->detail_lines || snap->detail_line_count <= 0) {
        plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg, snap->detail_status, rect->width - 4);
        return;
    }
    for (int i = 0; i < visible_rows; i++) {
        int idx = start + i;
        int y = rect->row + 1 + i;
        const char *line;

        if (idx >= snap->detail_line_count) break;
        line = snap->detail_lines[idx];
        if (!line) line = "";
        plane_putn(p, y, rect->col + 1, tune_preview_line_color(theme, line), theme->bg,
                   line, rect_inner_cols(rect));
    }
}

void render_tune_widget(Ui *ui, const LuloTuneSnapshot *snap, const LuloTuneState *state,
                        const AppState *app)
{
    TuneWidgetLayout layout;
    unsigned rows = 0;
    unsigned cols = 0;

    if (!ui->tune || !state) return;
    ncplane_dim_yx(ui->tune, &rows, &cols);
    plane_clear_inner_local(ui->tune, ui->theme, (int)rows, (int)cols);
    build_tune_widget_layout(ui, state, &layout);
    render_tune_view_tabs(ui->tune, ui->theme, &layout, state);
    if (state->view == LULO_TUNE_VIEW_EXPLORE) render_tune_explore_list(ui->tune, ui->theme, &layout.list, snap, state, app);
    else render_tune_bundle_list(ui->tune, ui->theme, &layout.list, snap, state, state->view == LULO_TUNE_VIEW_PRESETS);
    if (layout.show_info) render_tune_info(ui->tune, ui->theme, &layout.info, snap, state, app);
    render_tune_preview(ui->tune, ui->theme, &layout.preview, snap, state);
}

void render_tune_status(Ui *ui, const LuloTuneSnapshot *snap, const LuloTuneState *state,
                        const LuloTuneBackendStatus *backend_status, AppState *app)
{
    char buf[512];
    char prompt[320];

    if (!ui->load || !state) return;
    plane_reset(ui->load, ui->theme);
    if (backend_status && !backend_status->have_snapshot && backend_status->busy) {
        plane_putn(ui->load, 0, 0, ui->theme->white, ui->theme->bg,
                   "view Tune  loading explorer...", ui->lo.load.width - 2);
        return;
    }
    if (backend_status && backend_status->error[0]) {
        plane_putn(ui->load, 0, 0, ui->theme->red, ui->theme->bg,
                   backend_status->error, ui->lo.load.width - 2);
        return;
    }
    if (app && app->tune_edit_active) {
        tune_edit_prompt_format(app, prompt, sizeof(prompt));
        snprintf(buf, sizeof(buf), "editing  %s  Enter stage  Esc cancel",
                 prompt[0] ? prompt : "value");
        plane_putn(ui->load, 0, 0, ui->theme->yellow, ui->theme->bg, buf, ui->lo.load.width - 2);
        return;
    }
    if (app && tune_status_current(app)) {
        plane_putn(ui->load, 0, 0, ui->theme->green, ui->theme->bg,
                   app->tune_status, ui->lo.load.width - 2);
        return;
    }
    snprintf(buf, sizeof(buf), "view %s  path %s  items %d  snapshots %d  presets %d  focus %s",
             lulo_tune_view_name(state->view),
             state->browse_path[0] ? state->browse_path : "roots",
             snap ? snap->count : 0,
             snap ? snap->snapshot_count : 0,
             snap ? snap->preset_count : 0,
             state->focus_preview ? "preview" : "list");
    if (backend_status) {
        if (backend_status->saving_snapshot) strncat(buf, "  saving snapshot", sizeof(buf) - strlen(buf) - 1);
        else if (backend_status->saving_preset) strncat(buf, "  saving preset", sizeof(buf) - strlen(buf) - 1);
        else if (backend_status->applying_selected) strncat(buf, "  applying", sizeof(buf) - strlen(buf) - 1);
        else if (backend_status->loading_active) strncat(buf, "  loading", sizeof(buf) - strlen(buf) - 1);
        else if (backend_status->loading_full) strncat(buf, "  refreshing", sizeof(buf) - strlen(buf) - 1);
    }
    if (state->staged_path[0]) strncat(buf, "  staged", sizeof(buf) - strlen(buf) - 1);
    plane_putn(ui->load, 0, 0, ui->theme->white, ui->theme->bg, buf, ui->lo.load.width - 2);
}

static LuloTuneView tune_view_from_point(Ui *ui, const TuneWidgetLayout *layout,
                                         int global_y, int global_x)
{
    int row = ui->lo.top.row + 1 + layout->tabs_y;
    int x = ui->lo.top.col + 1 + layout->tabs_x;

    if (global_y != row) return LULO_TUNE_VIEW_COUNT;
    for (int i = 0; i < LULO_TUNE_VIEW_COUNT; i++) {
        char label[32];
        int width;

        snprintf(label, sizeof(label), " %s ", lulo_tune_view_name((LuloTuneView)i));
        width = (int)strlen(label);
        if (global_x >= x && global_x < x + width) return (LuloTuneView)i;
        x += width + 1;
    }
    return LULO_TUNE_VIEW_COUNT;
}

int point_on_tune_view_tabs(Ui *ui, const LuloTuneState *state, int global_y, int global_x)
{
    TuneWidgetLayout layout;

    if (!ui->tune || !state) return 0;
    build_tune_widget_layout(ui, state, &layout);
    return tune_view_from_point(ui, &layout, global_y, global_x) != LULO_TUNE_VIEW_COUNT;
}

int handle_tune_wheel_target(Ui *ui, LuloTuneState *state,
                             RenderFlags *render, int global_y, int global_x)
{
    TuneWidgetLayout layout;

    if (!ui->tune || !state) return 0;
    build_tune_widget_layout(ui, state, &layout);
    if (point_in_rect_global_local(&ui->lo.top, &layout.list, global_y, global_x)) {
        int local_y = global_y - (ui->lo.top.row + 1 + layout.list.row);

        if (local_y < 2) return 0;
        if (state->focus_preview) {
            state->focus_preview = 0;
            if (render) render->need_tune = 1;
        }
        return 1;
    }
    if (point_in_rect_global_local(&ui->lo.top, &layout.preview, global_y, global_x)) {
        int local_y = global_y - (ui->lo.top.row + 1 + layout.preview.row);

        if (local_y < 1) return 0;
        if (!state->focus_preview) {
            state->focus_preview = 1;
            if (render) render->need_tune = 1;
        }
        return 1;
    }
    return 0;
}

int handle_tune_click(Ui *ui, int global_y, int global_x,
                      const LuloTuneSnapshot *snap, LuloTuneState *state,
                      RenderFlags *render)
{
    TuneWidgetLayout layout;
    LuloTuneView hit_view;
    int local_y;

    if (!ui->tune || !state || !snap) return 0;
    build_tune_widget_layout(ui, state, &layout);
    hit_view = tune_view_from_point(ui, &layout, global_y, global_x);
    if (hit_view != LULO_TUNE_VIEW_COUNT) {
        if (state->view != hit_view) {
            state->view = hit_view;
            state->focus_preview = 0;
            render->need_tune_refresh = 1;
        } else {
            render->need_tune = 1;
        }
        render->need_render = 1;
        return 1;
    }
    if (point_in_rect_global_local(&ui->lo.top, &layout.list, global_y, global_x)) {
        int list_rows = tune_list_rows_visible(ui, state);
        int preview_rows = tune_preview_rows_visible(ui, state);
        int rc = 0;

        state->focus_preview = 0;
        local_y = global_y - (ui->lo.top.row + 1 + layout.list.row);
        if (local_y >= 2) {
            if (state->view == LULO_TUNE_VIEW_EXPLORE) {
                int row_index = state->list_scroll + (local_y - 2);

                if (row_index >= 0 && row_index < snap->count) {
                    lulo_tune_set_cursor(state, snap, list_rows, preview_rows, row_index);
                    rc = lulo_tune_open_current(state, snap, list_rows, preview_rows);
                }
            } else if (state->view == LULO_TUNE_VIEW_SNAPSHOTS) {
                int row_index = state->snapshot_list_scroll + (local_y - 2);

                if (row_index >= 0 && row_index < snap->snapshot_count) {
                    lulo_tune_set_cursor(state, snap, list_rows, preview_rows, row_index);
                    rc = lulo_tune_open_current(state, snap, list_rows, preview_rows);
                }
            } else {
                int row_index = state->preset_list_scroll + (local_y - 2);

                if (row_index >= 0 && row_index < snap->preset_count) {
                    lulo_tune_set_cursor(state, snap, list_rows, preview_rows, row_index);
                    rc = lulo_tune_open_current(state, snap, list_rows, preview_rows);
                }
            }
        }
        if (rc == 2) render->need_tune_refresh_full = 1;
        else if (rc == 1) render->need_tune_refresh = 1;
        else render->need_tune = 1;
        render->need_render = 1;
        return 1;
    }
    if (point_in_rect_global_local(&ui->lo.top, &layout.preview, global_y, global_x)) {
        if (!state->focus_preview) {
            state->focus_preview = 1;
            render->need_tune = 1;
            render->need_render = 1;
        }
        return 1;
    }
    if (layout.show_info && point_in_rect_global_local(&ui->lo.top, &layout.info, global_y, global_x)) {
        render->need_tune = 1;
        render->need_render = 1;
        return 1;
    }
    return 0;
}
