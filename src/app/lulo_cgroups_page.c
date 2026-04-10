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
} CgroupsWidgetLayout;

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

static int cgroups_tree_selection_active(const LuloCgroupsState *state)
{
    return state && state->tree_selected >= 0 && state->selected_tree_path[0];
}

static int cgroups_file_selection_active(const LuloCgroupsState *state)
{
    return state && state->file_selected >= 0 && state->selected_file_path[0];
}

static int cgroups_config_selection_active(const LuloCgroupsState *state)
{
    return state && state->config_selected >= 0 && state->selected_config[0];
}

static int cgroups_preview_open(const LuloCgroupsState *state)
{
    if (!state) return 0;
    switch (state->view) {
    case LULO_CGROUPS_VIEW_FILES:
        return cgroups_file_selection_active(state);
    case LULO_CGROUPS_VIEW_CONFIG:
        return cgroups_config_selection_active(state);
    case LULO_CGROUPS_VIEW_TREE:
    default:
        return cgroups_tree_selection_active(state);
    }
}

static void build_cgroups_widget_layout(const Ui *ui, const LuloCgroupsState *state,
                                        CgroupsWidgetLayout *layout)
{
    unsigned rows = 0;
    unsigned cols = 0;
    int inner_w;
    int content_h;
    int info_h;

    memset(layout, 0, sizeof(*layout));
    if (!ui->cgroups) return;
    ncplane_dim_yx(ui->cgroups, &rows, &cols);
    if (rows < 8 || cols < 28) return;
    layout->tabs_y = 1;
    layout->tabs_x = 2;
    inner_w = (int)cols - 2;
    content_h = (int)rows - 4;
    if (inner_w < 20 || content_h < 4) return;

    info_h = 5;
    layout->show_info = content_h >= info_h + 4;
    if (!cgroups_preview_open(state)) {
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

int cgroups_list_rows_visible(const Ui *ui, const LuloCgroupsState *state)
{
    CgroupsWidgetLayout layout;

    if (!ui->cgroups || !state) return 1;
    build_cgroups_widget_layout(ui, state, &layout);
    return clamp_int(rect_inner_rows(&layout.list) - 1, 1, 4096);
}

int cgroups_preview_rows_visible(const Ui *ui, const LuloCgroupsState *state)
{
    CgroupsWidgetLayout layout;

    if (!ui->cgroups || !state) return 1;
    build_cgroups_widget_layout(ui, state, &layout);
    return clamp_int(rect_inner_rows(&layout.preview), 1, 4096);
}

static const LuloCgroupTreeRow *active_tree_row(const LuloCgroupsSnapshot *snap, const LuloCgroupsState *state)
{
    if (!snap || !state) return NULL;
    if (state->tree_selected >= 0 && state->tree_selected < snap->tree_count) {
        return &snap->tree_rows[state->tree_selected];
    }
    if (state->tree_cursor >= 0 && state->tree_cursor < snap->tree_count) {
        return &snap->tree_rows[state->tree_cursor];
    }
    return NULL;
}

static const LuloCgroupFileRow *active_file_row(const LuloCgroupsSnapshot *snap, const LuloCgroupsState *state)
{
    if (!snap || !state) return NULL;
    if (state->file_selected >= 0 && state->file_selected < snap->file_count) {
        return &snap->file_rows[state->file_selected];
    }
    if (state->file_cursor >= 0 && state->file_cursor < snap->file_count) {
        return &snap->file_rows[state->file_cursor];
    }
    return NULL;
}

static const LuloCgroupConfigRow *active_config_row(const LuloCgroupsSnapshot *snap, const LuloCgroupsState *state)
{
    if (!snap || !state) return NULL;
    if (state->config_selected >= 0 && state->config_selected < snap->config_count) {
        return &snap->configs[state->config_selected];
    }
    if (state->config_cursor >= 0 && state->config_cursor < snap->config_count) {
        return &snap->configs[state->config_cursor];
    }
    return NULL;
}

const char *active_cgroups_edit_path(const LuloCgroupsSnapshot *snap, const LuloCgroupsState *state)
{
    const LuloCgroupFileRow *file_row;
    const LuloCgroupConfigRow *config_row;

    if (!snap || !state) return NULL;
    if (state->view == LULO_CGROUPS_VIEW_FILES) {
        file_row = active_file_row(snap, state);
        return file_row && file_row->path[0] ? file_row->path : NULL;
    }
    if (state->view == LULO_CGROUPS_VIEW_CONFIG) {
        config_row = active_config_row(snap, state);
        return config_row && config_row->path[0] ? config_row->path : NULL;
    }
    return NULL;
}

static void render_cgroups_view_tabs(struct ncplane *p, const Theme *theme,
                                     const CgroupsWidgetLayout *layout,
                                     const LuloCgroupsState *state)
{
    unsigned cols = 0;
    int x = layout->tabs_x;

    ncplane_dim_yx(p, NULL, &cols);
    plane_fill(p, layout->tabs_y, 1, (int)cols - 2, theme->bg, theme->bg);
    for (int i = 0; i < LULO_CGROUPS_VIEW_COUNT; i++) {
        char label[24];
        int active = state && state->view == (LuloCgroupsView)i;
        int width;
        Rgb fg = active ? theme->bg : theme->white;
        Rgb bg = active ? theme->border_header : theme->bg;

        snprintf(label, sizeof(label), " %s ", lulo_cgroups_view_name((LuloCgroupsView)i));
        width = (int)strlen(label);
        plane_putn(p, layout->tabs_y, x, fg, bg, label, width);
        x += width + 1;
    }
}

static void render_cgroups_tree_list(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                     const LuloCgroupsSnapshot *snap, const LuloCgroupsState *state)
{
    char meta[48];
    int visible_rows;
    int start;
    int type_w = rect->width >= 70 ? 8 : 6;
    int count_w = rect->width >= 70 ? 4 : 3;
    int name_w;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Tree ", theme->white);
    visible_rows = clamp_int(rect_inner_rows(rect) - 1, 1, 4096);
    start = state ? clamp_int(state->tree_list_scroll, 0,
                              snap && snap->tree_count > visible_rows ? snap->tree_count - visible_rows : 0) : 0;
    if (snap && snap->tree_count > 0) {
        snprintf(meta, sizeof(meta), "%d-%d/%d",
                 start + 1, clamp_int(start + visible_rows, 1, snap->tree_count), snap->tree_count);
        draw_inner_meta(p, theme, rect, meta, state && !state->focus_preview ? theme->green : theme->cyan);
    }
    plane_putn(p, rect->row + 1, rect->col + 1, theme->dim, theme->bg, "typ", type_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + type_w + 1, theme->dim, theme->bg, "p", count_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + type_w + 1 + count_w + 1, theme->dim, theme->bg, "t", count_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + type_w + 1 + count_w + 1 + count_w + 1,
               theme->dim, theme->bg, "sub", count_w);
    name_w = rect_inner_cols(rect) - type_w - count_w - count_w - count_w - 4;
    if (name_w < 8) name_w = 8;
    plane_putn(p, rect->row + 1, rect->col + 1 + type_w + 1 + count_w + 1 + count_w + 1 + count_w + 1,
               theme->dim, theme->bg, "name", name_w);

    if (!snap || snap->tree_count <= 0) {
        plane_putn(p, rect->row + 2, rect->col + 2, theme->white, theme->bg, "no cgroups", rect->width - 4);
        return;
    }

    for (int i = 0; i < visible_rows; i++) {
        int idx = start + i;
        int y = rect->row + 2 + i;
        int x = rect->col + 1;
        int selected = state && idx == state->tree_cursor;
        Rgb row_bg = selected ? theme->select_bg : theme->bg;
        const LuloCgroupTreeRow *row;
        char countbuf[16];

        plane_fill(p, y, rect->col + 1, rect_inner_cols(rect), row_bg, row_bg);
        if (idx >= snap->tree_count) continue;
        row = &snap->tree_rows[idx];
        plane_putn(p, y, x, selected ? theme->select_fg : (row->is_parent ? theme->orange : theme->cyan),
                   row_bg, row->type, type_w);
        x += type_w + 1;
        snprintf(countbuf, sizeof(countbuf), "%d", row->process_count >= 0 ? row->process_count : 0);
        plane_putn(p, y, x, selected ? theme->select_fg : theme->yellow, row_bg, countbuf, count_w);
        x += count_w + 1;
        snprintf(countbuf, sizeof(countbuf), "%d", row->thread_count >= 0 ? row->thread_count : 0);
        plane_putn(p, y, x, selected ? theme->select_fg : theme->orange, row_bg, countbuf, count_w);
        x += count_w + 1;
        snprintf(countbuf, sizeof(countbuf), "%d", row->subgroup_count >= 0 ? row->subgroup_count : 0);
        plane_putn(p, y, x, selected ? theme->select_fg : theme->green, row_bg, countbuf, count_w);
        x += count_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg : theme->white, row_bg, row->name, name_w);
    }
}

static void render_cgroups_files_list(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                      const LuloCgroupsSnapshot *snap, const LuloCgroupsState *state)
{
    char meta[48];
    int visible_rows;
    int start;
    int rw_w = 2;
    int name_w;
    int value_w;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Files ", theme->white);
    visible_rows = clamp_int(rect_inner_rows(rect) - 1, 1, 4096);
    start = state ? clamp_int(state->file_list_scroll, 0,
                              snap && snap->file_count > visible_rows ? snap->file_count - visible_rows : 0) : 0;
    if (snap && snap->file_count > 0) {
        snprintf(meta, sizeof(meta), "%d-%d/%d",
                 start + 1, clamp_int(start + visible_rows, 1, snap->file_count), snap->file_count);
        draw_inner_meta(p, theme, rect, meta, state && !state->focus_preview ? theme->green : theme->cyan);
    }
    name_w = rect_inner_cols(rect) / 2 - 2;
    if (name_w < 12) name_w = 12;
    value_w = rect_inner_cols(rect) - rw_w - name_w - 2;
    if (value_w < 12) value_w = 12;
    plane_putn(p, rect->row + 1, rect->col + 1, theme->dim, theme->bg, "rw", rw_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + rw_w + 1, theme->dim, theme->bg, "name", name_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + rw_w + 1 + name_w + 1, theme->dim, theme->bg, "value / path", value_w);

    if (!snap || snap->file_count <= 0) {
        plane_putn(p, rect->row + 2, rect->col + 2, theme->white, theme->bg, "no files", rect->width - 4);
        return;
    }

    for (int i = 0; i < visible_rows; i++) {
        int idx = start + i;
        int y = rect->row + 2 + i;
        int x = rect->col + 1;
        int selected = state && idx == state->file_cursor;
        Rgb row_bg = selected ? theme->select_bg : theme->bg;
        const LuloCgroupFileRow *row;

        plane_fill(p, y, rect->col + 1, rect_inner_cols(rect), row_bg, row_bg);
        if (idx >= snap->file_count) continue;
        row = &snap->file_rows[idx];
        plane_putn(p, y, x, selected ? theme->select_fg : (row->writable ? theme->green : theme->dim),
                   row_bg, row->writable ? "rw" : "ro", rw_w);
        x += rw_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg : theme->white, row_bg, row->name, name_w);
        x += name_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg : theme->dim, row_bg, row->value, value_w);
    }
}

static void render_cgroups_config_list(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                       const LuloCgroupsSnapshot *snap, const LuloCgroupsState *state)
{
    char meta[48];
    int visible_rows;
    int start;
    int src_w = 6;
    int kind_w = 8;
    int path_w;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Config ", theme->white);
    visible_rows = clamp_int(rect_inner_rows(rect) - 1, 1, 4096);
    start = state ? clamp_int(state->config_list_scroll, 0,
                              snap && snap->config_count > visible_rows ? snap->config_count - visible_rows : 0) : 0;
    if (snap && snap->config_count > 0) {
        snprintf(meta, sizeof(meta), "%d-%d/%d",
                 start + 1, clamp_int(start + visible_rows, 1, snap->config_count), snap->config_count);
        draw_inner_meta(p, theme, rect, meta, state && !state->focus_preview ? theme->green : theme->cyan);
    }
    path_w = rect_inner_cols(rect) - src_w - kind_w - 2;
    if (path_w < 16) path_w = 16;
    plane_putn(p, rect->row + 1, rect->col + 1, theme->dim, theme->bg, "src", src_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + src_w + 1, theme->dim, theme->bg, "kind", kind_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + src_w + 1 + kind_w + 1, theme->dim, theme->bg, "path", path_w);

    if (!snap || snap->config_count <= 0) {
        plane_putn(p, rect->row + 2, rect->col + 2, theme->white, theme->bg, "no configs", rect->width - 4);
        return;
    }

    for (int i = 0; i < visible_rows; i++) {
        int idx = start + i;
        int y = rect->row + 2 + i;
        int x = rect->col + 1;
        int selected = state && idx == state->config_cursor;
        Rgb row_bg = selected ? theme->select_bg : theme->bg;
        const LuloCgroupConfigRow *row;
        Rgb source_color;

        plane_fill(p, y, rect->col + 1, rect_inner_cols(rect), row_bg, row_bg);
        if (idx >= snap->config_count) continue;
        row = &snap->configs[idx];
        source_color = strcmp(row->source, "etc") == 0 ? theme->green : theme->cyan;
        plane_putn(p, y, x, selected ? theme->select_fg : source_color, row_bg, row->source, src_w);
        x += src_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg : theme->yellow, row_bg, row->kind, kind_w);
        x += kind_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg : theme->white, row_bg, row->name, path_w);
    }
}

static void render_cgroups_info(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                const LuloCgroupsSnapshot *snap, const LuloCgroupsState *state)
{
    int y;
    int x;
    const LuloCgroupTreeRow *tree_row;
    const LuloCgroupFileRow *file_row;
    const LuloCgroupConfigRow *config_row;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Details ", theme->white);
    y = rect->row + 1;
    x = rect->col + 2;
    plane_putn(p, y++, x, theme->white, theme->bg,
               snap && snap->detail_title[0] ? snap->detail_title : "cgroup",
               rect->width - 4);
    if (snap && snap->detail_status[0]) {
        plane_putn(p, y++, x, theme->cyan, theme->bg, snap->detail_status, rect->width - 4);
    }

    tree_row = active_tree_row(snap, state);
    file_row = active_file_row(snap, state);
    config_row = active_config_row(snap, state);
    switch (state ? state->view : LULO_CGROUPS_VIEW_TREE) {
    case LULO_CGROUPS_VIEW_FILES:
        if (file_row) {
            plane_putn(p, y, x, theme->yellow, theme->bg,
                       file_row->writable ? "access  read-write" : "access  read-only",
                       rect->width - 4);
        }
        break;
    case LULO_CGROUPS_VIEW_CONFIG:
        if (config_row) {
            char line[256];
            snprintf(line, sizeof(line), "kind  %s   src  %s", config_row->kind, config_row->source);
            plane_putn(p, y, x, theme->yellow, theme->bg, line, rect->width - 4);
        }
        break;
    case LULO_CGROUPS_VIEW_TREE:
    default:
        if (tree_row) {
            char line[256];
            snprintf(line, sizeof(line), "typ %s   p %d   t %d   sub %d",
                     tree_row->type,
                     tree_row->process_count >= 0 ? tree_row->process_count : 0,
                     tree_row->thread_count >= 0 ? tree_row->thread_count : 0,
                     tree_row->subgroup_count >= 0 ? tree_row->subgroup_count : 0);
            plane_putn(p, y, x, theme->yellow, theme->bg, line, rect->width - 4);
        }
        break;
    }
}

static void render_cgroups_preview(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                   const LuloCgroupsSnapshot *snap, const LuloCgroupsState *state)
{
    char meta[48];
    int visible_rows;
    int scroll;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Preview ", theme->white);
    visible_rows = clamp_int(rect_inner_rows(rect), 1, 4096);
    if (state) {
        switch (state->view) {
        case LULO_CGROUPS_VIEW_FILES:
            scroll = state->file_detail_scroll;
            break;
        case LULO_CGROUPS_VIEW_CONFIG:
            scroll = state->config_detail_scroll;
            break;
        case LULO_CGROUPS_VIEW_TREE:
        default:
            scroll = state->tree_detail_scroll;
            break;
        }
    } else {
        scroll = 0;
    }
    scroll = clamp_int(scroll, 0,
                       snap && snap->detail_line_count > visible_rows ? snap->detail_line_count - visible_rows : 0);
    if (snap && snap->detail_line_count > 0) {
        snprintf(meta, sizeof(meta), "%d-%d/%d",
                 scroll + 1, clamp_int(scroll + visible_rows, 1, snap->detail_line_count),
                 snap->detail_line_count);
        draw_inner_meta(p, theme, rect, meta, state && state->focus_preview ? theme->green : theme->cyan);
    }
    if (!snap || snap->detail_line_count <= 0) {
        plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg, "no preview", rect->width - 4);
        return;
    }
    for (int i = 0; i < visible_rows; i++) {
        int idx = scroll + i;

        if (idx >= snap->detail_line_count) break;
        plane_putn(p, rect->row + 1 + i, rect->col + 1, theme->white, theme->bg,
                   snap->detail_lines[idx], rect_inner_cols(rect));
    }
}

void render_cgroups_widget(Ui *ui, const LuloCgroupsSnapshot *snap, const LuloCgroupsState *state)
{
    CgroupsWidgetLayout layout;
    unsigned rows = 0;
    unsigned cols = 0;

    if (!ui->cgroups || !ui->theme || !state) return;
    ncplane_dim_yx(ui->cgroups, &rows, &cols);
    plane_clear_inner_local(ui->cgroups, ui->theme, (int)rows, (int)cols);
    build_cgroups_widget_layout(ui, state, &layout);
    render_cgroups_view_tabs(ui->cgroups, ui->theme, &layout, state);
    switch (state->view) {
    case LULO_CGROUPS_VIEW_FILES:
        render_cgroups_files_list(ui->cgroups, ui->theme, &layout.list, snap, state);
        break;
    case LULO_CGROUPS_VIEW_CONFIG:
        render_cgroups_config_list(ui->cgroups, ui->theme, &layout.list, snap, state);
        break;
    case LULO_CGROUPS_VIEW_TREE:
    default:
        render_cgroups_tree_list(ui->cgroups, ui->theme, &layout.list, snap, state);
        break;
    }
    if (layout.show_info) render_cgroups_info(ui->cgroups, ui->theme, &layout.info, snap, state);
    if (rect_valid(&layout.preview)) render_cgroups_preview(ui->cgroups, ui->theme, &layout.preview, snap, state);
}

void render_cgroups_status(Ui *ui, const LuloCgroupsSnapshot *snap, const LuloCgroupsState *state,
                           const LuloCgroupsBackendStatus *backend_status, AppState *app)
{
    char line[320];
    const char *msg = app ? cgroups_status_current(app) : NULL;
    unsigned rows = 0;
    unsigned cols = 0;
    int y;
    int msg_x;

    if (!ui || !ui->cgroups || !state) return;
    ncplane_dim_yx(ui->cgroups, &rows, &cols);
    if (rows == 0 || cols <= 2) return;
    snprintf(line, sizeof(line),
             "view %s   path %.220s%s%s",
             lulo_cgroups_view_name(state->view),
             state->browse_path[0] ? state->browse_path : (snap && snap->browse_path[0] ? snap->browse_path : "/sys/fs/cgroup"),
             backend_status && backend_status->busy ? "   loading" : "",
             backend_status && backend_status->error[0] ? "   error" : "");
    y = (int)rows - 1;
    plane_fill(ui->cgroups, y, 1, (int)cols - 2, ui->theme->bg, ui->theme->bg);
    plane_putn(ui->cgroups, y, 1,
               backend_status && backend_status->error[0] ? ui->theme->red :
               backend_status && backend_status->busy ? ui->theme->yellow :
               ui->theme->dim,
               ui->theme->bg, line, (int)cols - 2);
    if (msg && *msg) {
        msg_x = clamp_int((int)strlen(line) + 4, 1, (int)cols - 6);
        plane_putn(ui->cgroups, y, msg_x, ui->theme->green, ui->theme->bg, msg,
                   (int)cols - msg_x - 1);
    } else if (backend_status && backend_status->error[0]) {
        msg_x = clamp_int((int)strlen(line) + 4, 1, (int)cols - 6);
        plane_putn(ui->cgroups, y, msg_x, ui->theme->red, ui->theme->bg, backend_status->error,
                   (int)cols - msg_x - 1);
    }
}

static LuloCgroupsView cgroups_view_from_point(Ui *ui, const CgroupsWidgetLayout *layout,
                                               int global_y, int global_x)
{
    int x = ui->lo.top.col + 1 + layout->tabs_x;

    if (global_y != ui->lo.top.row + 1 + layout->tabs_y) return LULO_CGROUPS_VIEW_COUNT;
    for (int i = 0; i < LULO_CGROUPS_VIEW_COUNT; i++) {
        char label[24];
        int width;

        snprintf(label, sizeof(label), " %s ", lulo_cgroups_view_name((LuloCgroupsView)i));
        width = (int)strlen(label);
        if (global_x >= x && global_x < x + width) return (LuloCgroupsView)i;
        x += width + 1;
    }
    return LULO_CGROUPS_VIEW_COUNT;
}

int point_on_cgroups_view_tabs(Ui *ui, const LuloCgroupsState *state, int global_y, int global_x)
{
    CgroupsWidgetLayout layout;

    if (!ui->cgroups || !state) return 0;
    build_cgroups_widget_layout(ui, state, &layout);
    return cgroups_view_from_point(ui, &layout, global_y, global_x) != LULO_CGROUPS_VIEW_COUNT;
}

int handle_cgroups_wheel_target(Ui *ui, LuloCgroupsState *state,
                                RenderFlags *render, int global_y, int global_x)
{
    CgroupsWidgetLayout layout;

    if (!ui->cgroups || !state) return 0;
    build_cgroups_widget_layout(ui, state, &layout);
    if (point_in_rect_global_local(&ui->lo.top, &layout.list, global_y, global_x)) {
        int local_y = global_y - (ui->lo.top.row + 1 + layout.list.row);

        if (local_y < 2) return 0;
        if (state->focus_preview) {
            state->focus_preview = 0;
            if (render) render->need_cgroups = 1;
        }
        return 1;
    }
    if (point_in_rect_global_local(&ui->lo.top, &layout.preview, global_y, global_x)) {
        int local_y = global_y - (ui->lo.top.row + 1 + layout.preview.row);

        if (local_y < 1) return 0;
        if (!state->focus_preview) {
            state->focus_preview = 1;
            if (render) render->need_cgroups = 1;
        }
        return 1;
    }
    return 0;
}

int handle_cgroups_click(Ui *ui, int global_y, int global_x,
                         const LuloCgroupsSnapshot *snap, LuloCgroupsState *state,
                         RenderFlags *render)
{
    CgroupsWidgetLayout layout;
    LuloCgroupsView hit_view;
    int local_y;

    if (!ui->cgroups || !state || !snap) return 0;
    build_cgroups_widget_layout(ui, state, &layout);
    hit_view = cgroups_view_from_point(ui, &layout, global_y, global_x);
    if (hit_view != LULO_CGROUPS_VIEW_COUNT) {
        if (state->view != hit_view) {
            state->view = hit_view;
            state->focus_preview = 0;
            render->need_cgroups_refresh = 1;
        } else {
            render->need_cgroups = 1;
        }
        render->need_render = 1;
        return 1;
    }
    if (point_in_rect_global_local(&ui->lo.top, &layout.list, global_y, global_x)) {
        int list_rows = cgroups_list_rows_visible(ui, state);
        int preview_rows = cgroups_preview_rows_visible(ui, state);
        int row_index;
        int rc = 0;

        state->focus_preview = 0;
        local_y = global_y - (ui->lo.top.row + 1 + layout.list.row);
        if (local_y < 2) return 1;
        switch (state->view) {
        case LULO_CGROUPS_VIEW_FILES:
            row_index = state->file_list_scroll + (local_y - 2);
            if (row_index >= 0 && row_index < snap->file_count) {
                lulo_cgroups_set_cursor(state, snap, list_rows, preview_rows, row_index);
                rc = 1;
            }
            break;
        case LULO_CGROUPS_VIEW_CONFIG:
            row_index = state->config_list_scroll + (local_y - 2);
            if (row_index >= 0 && row_index < snap->config_count) {
                lulo_cgroups_set_cursor(state, snap, list_rows, preview_rows, row_index);
                rc = 1;
            }
            break;
        case LULO_CGROUPS_VIEW_TREE:
        default:
            row_index = state->tree_list_scroll + (local_y - 2);
            if (row_index >= 0 && row_index < snap->tree_count) {
                lulo_cgroups_set_cursor(state, snap, list_rows, preview_rows, row_index);
                rc = lulo_cgroups_open_current(state, snap, list_rows, preview_rows);
            }
            break;
        }
        if (rc == 2) render->need_cgroups_refresh_full = 1;
        else if (rc == 1) render->need_cgroups_refresh = 1;
        else render->need_cgroups = 1;
        render->need_render = 1;
        return 1;
    }
    if (point_in_rect_global_local(&ui->lo.top, &layout.preview, global_y, global_x)) {
        if (!state->focus_preview) {
            state->focus_preview = 1;
            render->need_cgroups = 1;
            render->need_render = 1;
        }
        return 1;
    }
    if (layout.show_info && point_in_rect_global_local(&ui->lo.top, &layout.info, global_y, global_x)) {
        render->need_cgroups = 1;
        render->need_render = 1;
        return 1;
    }
    return 0;
}
