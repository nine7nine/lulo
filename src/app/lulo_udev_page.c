#define _GNU_SOURCE

#include "lulo_app.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    int tabs_y;
    int tabs_x;
    LuloRect list;
    LuloRect preview;
    int show_preview;
} UdevWidgetLayout;

static int point_in_rect_global_local(const LuloRect *origin, const LuloRect *rect, int y, int x)
{
    int top = origin->row + 1 + rect->row;
    int left = origin->col + 1 + rect->col;

    return y >= top && y < top + rect->height && x >= left && x < left + rect->width;
}

static int udev_preview_open(const LuloUdevState *state)
{
    if (!state) return 0;
    switch (state->view) {
    case LULO_UDEV_VIEW_HWDB:
        return state->hwdb_selected >= 0 && state->selected_hwdb[0];
    case LULO_UDEV_VIEW_DEVICES:
        return state->device_selected >= 0 && state->selected_device[0];
    case LULO_UDEV_VIEW_RULES:
    default:
        return state->rule_selected >= 0 && state->selected_rule[0];
    }
}

static void build_udev_widget_layout(const Ui *ui, const LuloUdevState *state,
                                     UdevWidgetLayout *layout)
{
    unsigned rows = 0;
    unsigned cols = 0;
    int inner_w;
    int content_h;

    memset(layout, 0, sizeof(*layout));
    if (!ui->udev) return;
    ncplane_dim_yx(ui->udev, &rows, &cols);
    if (rows < 8 || cols < 28) return;
    layout->tabs_y = 1;
    layout->tabs_x = 2;
    inner_w = (int)cols - 2;
    content_h = (int)rows - 4;
    if (inner_w < 20 || content_h < 4) return;

    layout->show_preview = udev_preview_open(state);
    if (!layout->show_preview) {
        layout->list.row = 2;
        layout->list.col = 1;
        layout->list.width = inner_w;
        layout->list.height = content_h;
        return;
    }

    if (inner_w >= 96) {
        int list_w = clamp_int(inner_w * 11 / 20, 42, inner_w - 28);
        int preview_w = inner_w - list_w - 1;

        layout->list.row = 2;
        layout->list.col = 1;
        layout->list.width = list_w;
        layout->list.height = content_h;
        layout->preview.row = 2;
        layout->preview.col = list_w + 2;
        layout->preview.width = preview_w;
        layout->preview.height = content_h;
        return;
    }

    {
        int list_h = clamp_int(content_h / 3 + 1, 7, content_h - 4);

        layout->list.row = 2;
        layout->list.col = 1;
        layout->list.width = inner_w;
        layout->list.height = list_h;
        layout->preview.row = 2 + list_h + 1;
        layout->preview.col = 1;
        layout->preview.width = inner_w;
        layout->preview.height = content_h - list_h - 1;
    }
}

int udev_list_rows_visible(const Ui *ui, const LuloUdevState *state)
{
    UdevWidgetLayout layout;

    if (!ui->udev || !state) return 1;
    build_udev_widget_layout(ui, state, &layout);
    return clamp_int(rect_inner_rows(&layout.list) - 1, 1, 4096);
}

int udev_preview_rows_visible(const Ui *ui, const LuloUdevState *state)
{
    UdevWidgetLayout layout;

    if (!ui->udev || !state) return 1;
    build_udev_widget_layout(ui, state, &layout);
    return clamp_int(rect_inner_rows(&layout.preview), 1, 4096);
}

static const LuloUdevConfigRow *active_rule_row(const LuloUdevSnapshot *snap, const LuloUdevState *state)
{
    if (!snap || !state) return NULL;
    if (state->rule_selected >= 0 && state->rule_selected < snap->rule_count) return &snap->rules[state->rule_selected];
    if (state->rule_cursor >= 0 && state->rule_cursor < snap->rule_count) return &snap->rules[state->rule_cursor];
    return NULL;
}

static const LuloUdevConfigRow *active_hwdb_row(const LuloUdevSnapshot *snap, const LuloUdevState *state)
{
    if (!snap || !state) return NULL;
    if (state->hwdb_selected >= 0 && state->hwdb_selected < snap->hwdb_count) return &snap->hwdb[state->hwdb_selected];
    if (state->hwdb_cursor >= 0 && state->hwdb_cursor < snap->hwdb_count) return &snap->hwdb[state->hwdb_cursor];
    return NULL;
}

const char *active_udev_edit_path(const LuloUdevSnapshot *snap, const LuloUdevState *state)
{
    const LuloUdevConfigRow *row;

    if (!snap || !state) return NULL;
    if (state->view == LULO_UDEV_VIEW_RULES) {
        row = active_rule_row(snap, state);
        return row && row->path[0] ? row->path : NULL;
    }
    if (state->view == LULO_UDEV_VIEW_HWDB) {
        row = active_hwdb_row(snap, state);
        return row && row->path[0] ? row->path : NULL;
    }
    return NULL;
}

static void render_udev_view_tabs(struct ncplane *p, const Theme *theme,
                                  const UdevWidgetLayout *layout,
                                  const LuloUdevState *state)
{
    unsigned cols = 0;
    int x = layout->tabs_x;

    ncplane_dim_yx(p, NULL, &cols);
    plane_fill(p, layout->tabs_y, 1, (int)cols - 2, theme->bg, theme->bg);
    for (int i = 0; i < LULO_UDEV_VIEW_COUNT; i++) {
        char label[24];
        int active = state && state->view == (LuloUdevView)i;
        int width;
        Rgb fg = active ? theme->bg : theme->white;
        Rgb bg = active ? theme->border_header : theme->bg;

        snprintf(label, sizeof(label), " %s ", lulo_udev_view_name((LuloUdevView)i));
        width = (int)strlen(label);
        plane_putn(p, layout->tabs_y, x, fg, bg, label, width);
        x += width + 1;
    }
}

static void render_udev_rules_list(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                   const LuloUdevSnapshot *snap, const LuloUdevState *state)
{
    char meta[48];
    int visible_rows;
    int start;
    int src_w = 6;
    int path_w;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Rules ", theme->white);
    visible_rows = clamp_int(rect_inner_rows(rect) - 1, 1, 4096);
    start = state ? clamp_int(state->rule_list_scroll, 0,
                              snap && snap->rule_count > visible_rows ? snap->rule_count - visible_rows : 0) : 0;
    if (snap && snap->rule_count > 0) {
        snprintf(meta, sizeof(meta), "%d-%d/%d",
                 start + 1, clamp_int(start + visible_rows, 1, snap->rule_count), snap->rule_count);
        draw_inner_meta(p, theme, rect, meta, state && !state->focus_preview ? theme->green : theme->cyan);
    }
    path_w = rect_inner_cols(rect) - src_w - 1;
    if (path_w < 16) path_w = 16;
    plane_putn(p, rect->row + 1, rect->col + 1, theme->dim, theme->bg, "src", src_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + src_w + 1, theme->dim, theme->bg, "file", path_w);

    if (!snap || snap->rule_count <= 0) {
        plane_putn(p, rect->row + 2, rect->col + 2, theme->white, theme->bg, "no udev rules", rect->width - 4);
        return;
    }

    for (int i = 0; i < visible_rows; i++) {
        int idx = start + i;
        int y = rect->row + 2 + i;
        int x = rect->col + 1;
        int selected = state && idx == state->rule_cursor;
        Rgb row_bg = selected ? theme->select_bg : theme->bg;
        const LuloUdevConfigRow *row;
        Rgb source_color;

        plane_fill(p, y, rect->col + 1, rect_inner_cols(rect), row_bg, row_bg);
        if (idx >= snap->rule_count) continue;
        row = &snap->rules[idx];
        source_color = strcmp(row->source, "etc") == 0 ? theme->green : theme->cyan;
        plane_putn(p, y, x, selected ? theme->select_fg : source_color, row_bg, row->source, src_w);
        x += src_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg : theme->white, row_bg, row->name, path_w);
    }
}

static void render_udev_hwdb_list(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                  const LuloUdevSnapshot *snap, const LuloUdevState *state)
{
    char meta[48];
    int visible_rows;
    int start;
    int src_w = 6;
    int path_w;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Hwdb ", theme->white);
    visible_rows = clamp_int(rect_inner_rows(rect) - 1, 1, 4096);
    start = state ? clamp_int(state->hwdb_list_scroll, 0,
                              snap && snap->hwdb_count > visible_rows ? snap->hwdb_count - visible_rows : 0) : 0;
    if (snap && snap->hwdb_count > 0) {
        snprintf(meta, sizeof(meta), "%d-%d/%d",
                 start + 1, clamp_int(start + visible_rows, 1, snap->hwdb_count), snap->hwdb_count);
        draw_inner_meta(p, theme, rect, meta, state && !state->focus_preview ? theme->green : theme->cyan);
    }
    path_w = rect_inner_cols(rect) - src_w - 1;
    if (path_w < 16) path_w = 16;
    plane_putn(p, rect->row + 1, rect->col + 1, theme->dim, theme->bg, "src", src_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + src_w + 1, theme->dim, theme->bg, "file", path_w);

    if (!snap || snap->hwdb_count <= 0) {
        plane_putn(p, rect->row + 2, rect->col + 2, theme->white, theme->bg, "no hwdb entries", rect->width - 4);
        return;
    }

    for (int i = 0; i < visible_rows; i++) {
        int idx = start + i;
        int y = rect->row + 2 + i;
        int x = rect->col + 1;
        int selected = state && idx == state->hwdb_cursor;
        Rgb row_bg = selected ? theme->select_bg : theme->bg;
        const LuloUdevConfigRow *row;
        Rgb source_color;

        plane_fill(p, y, rect->col + 1, rect_inner_cols(rect), row_bg, row_bg);
        if (idx >= snap->hwdb_count) continue;
        row = &snap->hwdb[idx];
        source_color = strcmp(row->source, "etc") == 0 ? theme->green : theme->cyan;
        plane_putn(p, y, x, selected ? theme->select_fg : source_color, row_bg, row->source, src_w);
        x += src_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg : theme->white, row_bg, row->name, path_w);
    }
}

static void render_udev_device_list(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                    const LuloUdevSnapshot *snap, const LuloUdevState *state)
{
    char meta[48];
    int visible_rows;
    int start;
    int sub_w = rect->width >= 72 ? 16 : 12;
    int name_w = rect_inner_cols(rect) / 3;
    int node_w;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Devices ", theme->white);
    visible_rows = clamp_int(rect_inner_rows(rect) - 1, 1, 4096);
    start = state ? clamp_int(state->device_list_scroll, 0,
                              snap && snap->device_count > visible_rows ? snap->device_count - visible_rows : 0) : 0;
    if (snap && snap->device_count > 0) {
        snprintf(meta, sizeof(meta), "%d-%d/%d",
                 start + 1, clamp_int(start + visible_rows, 1, snap->device_count), snap->device_count);
        draw_inner_meta(p, theme, rect, meta, state && !state->focus_preview ? theme->green : theme->cyan);
    }
    if (name_w < 12) name_w = 12;
    node_w = rect_inner_cols(rect) - sub_w - name_w - 2;
    if (node_w < 14) node_w = 14;
    plane_putn(p, rect->row + 1, rect->col + 1, theme->dim, theme->bg, "subsystem", sub_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + sub_w + 1, theme->dim, theme->bg, "name", name_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + sub_w + 1 + name_w + 1, theme->dim, theme->bg, "devnode", node_w);

    if (!snap || snap->device_count <= 0) {
        plane_putn(p, rect->row + 2, rect->col + 2, theme->white, theme->bg, "no udev devices", rect->width - 4);
        return;
    }

    for (int i = 0; i < visible_rows; i++) {
        int idx = start + i;
        int y = rect->row + 2 + i;
        int x = rect->col + 1;
        int selected = state && idx == state->device_cursor;
        Rgb row_bg = selected ? theme->select_bg : theme->bg;
        const LuloUdevDeviceRow *row;

        plane_fill(p, y, rect->col + 1, rect_inner_cols(rect), row_bg, row_bg);
        if (idx >= snap->device_count) continue;
        row = &snap->devices[idx];
        plane_putn(p, y, x, selected ? theme->select_fg : theme->cyan, row_bg, row->subsystem, sub_w);
        x += sub_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg : theme->white, row_bg, row->name, name_w);
        x += name_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg : theme->dim, row_bg, row->devnode, node_w);
    }
}

static void render_udev_preview(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                const LuloUdevSnapshot *snap, const LuloUdevState *state)
{
    int start;
    int rows;
    char meta[48];

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Preview ", theme->white);
    rows = rect_inner_rows(rect);
    start = 0;
    switch (state ? state->view : LULO_UDEV_VIEW_RULES) {
    case LULO_UDEV_VIEW_HWDB:
        start = state ? state->hwdb_detail_scroll : 0;
        break;
    case LULO_UDEV_VIEW_DEVICES:
        start = state ? state->device_detail_scroll : 0;
        break;
    case LULO_UDEV_VIEW_RULES:
    default:
        start = state ? state->rule_detail_scroll : 0;
        break;
    }
    if (snap && snap->detail_line_count > 0) {
        snprintf(meta, sizeof(meta), "%d-%d/%d",
                 start + 1, clamp_int(start + rows, 1, snap->detail_line_count), snap->detail_line_count);
        draw_inner_meta(p, theme, rect, meta, state && state->focus_preview ? theme->green : theme->cyan);
    }
    plane_putn(p, rect->row + 1, rect->col + 2,
               theme->white, theme->bg,
               snap && snap->detail_title[0] ? snap->detail_title : "UDEV",
               rect->width - 4);
    if (snap && snap->detail_status[0]) {
        plane_putn(p, rect->row + 2, rect->col + 2, theme->cyan, theme->bg,
                   snap->detail_status, rect->width - 4);
    }

    if (!snap || snap->detail_line_count <= 0) {
        plane_putn(p, rect->row + 4, rect->col + 2, theme->dim, theme->bg,
                   "no preview", rect->width - 4);
        return;
    }

    for (int i = 0; i < rows - 3; i++) {
        int idx = start + i;
        int y = rect->row + 3 + i;

        plane_fill(p, y, rect->col + 1, rect_inner_cols(rect), theme->bg, theme->bg);
        if (idx >= snap->detail_line_count) continue;
        plane_putn(p, y, rect->col + 2, theme->white, theme->bg,
                   snap->detail_lines[idx], rect->width - 4);
    }
}

void render_udev_widget(Ui *ui, const LuloUdevSnapshot *snap, const LuloUdevState *state)
{
    UdevWidgetLayout layout;

    if (!ui || !ui->udev || !state) return;
    plane_reset(ui->udev, ui->theme);
    build_udev_widget_layout(ui, state, &layout);
    render_udev_view_tabs(ui->udev, ui->theme, &layout, state);

    switch (state->view) {
    case LULO_UDEV_VIEW_HWDB:
        render_udev_hwdb_list(ui->udev, ui->theme, &layout.list, snap, state);
        break;
    case LULO_UDEV_VIEW_DEVICES:
        render_udev_device_list(ui->udev, ui->theme, &layout.list, snap, state);
        break;
    case LULO_UDEV_VIEW_RULES:
    default:
        render_udev_rules_list(ui->udev, ui->theme, &layout.list, snap, state);
        break;
    }

    if (layout.show_preview) render_udev_preview(ui->udev, ui->theme, &layout.preview, snap, state);
}

void render_udev_status(Ui *ui, const LuloUdevSnapshot *snap, const LuloUdevState *state,
                        const LuloUdevBackendStatus *backend_status, AppState *app)
{
    char line[320];
    const char *msg = app ? udev_status_current(app) : NULL;
    unsigned rows = 0;
    unsigned cols = 0;
    int y;
    int msg_x;

    if (!ui || !ui->udev || !state) return;
    ncplane_dim_yx(ui->udev, &rows, &cols);
    if (rows == 0 || cols <= 2) return;
    snprintf(line, sizeof(line),
             "view %s   rules %d   hwdb %d   devices %d   focus %s%s%s",
             lulo_udev_view_name(state->view),
             snap ? snap->rule_count : 0,
             snap ? snap->hwdb_count : 0,
             snap ? snap->device_count : 0,
             state->focus_preview ? "preview" : "list",
             backend_status && backend_status->busy ? "   loading" : "",
             backend_status && backend_status->error[0] ? "   error" : "");
    y = (int)rows - 1;
    plane_fill(ui->udev, y, 1, (int)cols - 2, ui->theme->bg, ui->theme->bg);
    plane_putn(ui->udev, y, 1,
               backend_status && backend_status->error[0] ? ui->theme->red :
               backend_status && backend_status->busy ? ui->theme->yellow :
               ui->theme->dim,
               ui->theme->bg, line, (int)cols - 2);
    if (msg && *msg) {
        msg_x = clamp_int((int)strlen(line) + 4, 1, (int)cols - 6);
        plane_putn(ui->udev, y, msg_x, ui->theme->green, ui->theme->bg, msg,
                   (int)cols - msg_x - 1);
    } else if (backend_status && backend_status->error[0]) {
        msg_x = clamp_int((int)strlen(line) + 4, 1, (int)cols - 6);
        plane_putn(ui->udev, y, msg_x, ui->theme->red, ui->theme->bg, backend_status->error,
                   (int)cols - msg_x - 1);
    }
}

static LuloUdevView udev_view_from_point(Ui *ui, const UdevWidgetLayout *layout,
                                         int global_y, int global_x)
{
    int x = ui->lo.top.col + 1 + layout->tabs_x;

    if (global_y != ui->lo.top.row + 1 + layout->tabs_y) return LULO_UDEV_VIEW_COUNT;
    for (int i = 0; i < LULO_UDEV_VIEW_COUNT; i++) {
        char label[24];
        int width;

        snprintf(label, sizeof(label), " %s ", lulo_udev_view_name((LuloUdevView)i));
        width = (int)strlen(label);
        if (global_x >= x && global_x < x + width) return (LuloUdevView)i;
        x += width + 1;
    }
    return LULO_UDEV_VIEW_COUNT;
}

int point_on_udev_view_tabs(Ui *ui, const LuloUdevState *state, int global_y, int global_x)
{
    UdevWidgetLayout layout;

    if (!ui->udev || !state) return 0;
    build_udev_widget_layout(ui, state, &layout);
    return udev_view_from_point(ui, &layout, global_y, global_x) != LULO_UDEV_VIEW_COUNT;
}

int handle_udev_wheel_target(Ui *ui, LuloUdevState *state,
                             RenderFlags *render, int global_y, int global_x)
{
    UdevWidgetLayout layout;

    if (!ui->udev || !state) return 0;
    build_udev_widget_layout(ui, state, &layout);
    if (point_in_rect_global_local(&ui->lo.top, &layout.list, global_y, global_x)) {
        int local_y = global_y - (ui->lo.top.row + 1 + layout.list.row);

        if (local_y < 2) return 0;
        if (state->focus_preview) {
            state->focus_preview = 0;
            if (render) render->need_udev = 1;
        }
        return 1;
    }
    if (layout.show_preview && point_in_rect_global_local(&ui->lo.top, &layout.preview, global_y, global_x)) {
        int local_y = global_y - (ui->lo.top.row + 1 + layout.preview.row);

        if (local_y < 1) return 0;
        if (!state->focus_preview) {
            state->focus_preview = 1;
            if (render) render->need_udev = 1;
        }
        return 1;
    }
    return 0;
}

int handle_udev_click(Ui *ui, int global_y, int global_x,
                      const LuloUdevSnapshot *snap, LuloUdevState *state,
                      RenderFlags *render)
{
    UdevWidgetLayout layout;
    LuloUdevView hit_view;
    int local_y;

    if (!ui->udev || !state || !snap) return 0;
    build_udev_widget_layout(ui, state, &layout);
    hit_view = udev_view_from_point(ui, &layout, global_y, global_x);
    if (hit_view != LULO_UDEV_VIEW_COUNT) {
        if (state->view != hit_view) {
            state->view = hit_view;
            state->focus_preview = 0;
            render->need_udev_refresh = 1;
        } else {
            render->need_udev = 1;
        }
        render->need_render = 1;
        return 1;
    }
    if (point_in_rect_global_local(&ui->lo.top, &layout.list, global_y, global_x)) {
        int list_rows = udev_list_rows_visible(ui, state);
        int preview_rows = udev_preview_rows_visible(ui, state);

        state->focus_preview = 0;
        local_y = global_y - (ui->lo.top.row + 1 + layout.list.row);
        if (local_y >= 2) {
            int row_index;

            switch (state->view) {
            case LULO_UDEV_VIEW_HWDB:
                row_index = state->hwdb_list_scroll + (local_y - 2);
                break;
            case LULO_UDEV_VIEW_DEVICES:
                row_index = state->device_list_scroll + (local_y - 2);
                break;
            case LULO_UDEV_VIEW_RULES:
            default:
                row_index = state->rule_list_scroll + (local_y - 2);
                break;
            }
            lulo_udev_set_cursor(state, snap, list_rows, preview_rows, row_index);
            if (lulo_udev_open_current(state, snap, list_rows, preview_rows)) {
                render->need_udev_refresh = 1;
            } else {
                render->need_udev = 1;
            }
        } else {
            render->need_udev = 1;
        }
        render->need_render = 1;
        return 1;
    }
    if (layout.show_preview && point_in_rect_global_local(&ui->lo.top, &layout.preview, global_y, global_x)) {
        if (!state->focus_preview) {
            state->focus_preview = 1;
            render->need_udev = 1;
            render->need_render = 1;
        }
        return 1;
    }
    return 0;
}
