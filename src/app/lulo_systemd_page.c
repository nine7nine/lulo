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
} SystemdWidgetLayout;

static void plane_clear_inner_local(struct ncplane *p, const Theme *theme, int rows, int cols)
{
    for (int y = 1; y < rows - 1; y++) {
        plane_fill(p, y, 1, cols - 2, theme->bg, theme->bg);
    }
}

static int systemd_service_selection_active(const LuloSystemdState *state)
{
    return state && state->selected >= 0 && state->selected_unit[0];
}

static int systemd_service_cursor_active(const LuloSystemdState *state)
{
    return state && state->cursor >= 0;
}

static int systemd_config_selection_active(const LuloSystemdState *state)
{
    return state && state->config_selected >= 0 && state->selected_config[0];
}

static int systemd_config_cursor_active(const LuloSystemdState *state)
{
    return state && state->config_cursor >= 0;
}

static int systemd_preview_open(const LuloSystemdState *state)
{
    if (!state) return 0;
    return state->view == LULO_SYSTEMD_VIEW_CONFIG ?
           systemd_config_selection_active(state) :
           systemd_service_selection_active(state);
}

static void build_systemd_widget_layout(const Ui *ui, const LuloSystemdState *state, SystemdWidgetLayout *layout)
{
    unsigned rows = 0;
    unsigned cols = 0;
    int inner_w;
    int content_h;
    int info_h;
    int preview_open;

    memset(layout, 0, sizeof(*layout));
    if (!ui->systemd) return;
    ncplane_dim_yx(ui->systemd, &rows, &cols);
    if (rows < 8 || cols < 28) return;
    layout->tabs_y = 1;
    layout->tabs_x = 2;
    inner_w = (int)cols - 2;
    content_h = (int)rows - 4;
    if (inner_w < 20 || content_h < 4) return;

    preview_open = systemd_preview_open(state);
    info_h = state && state->view == LULO_SYSTEMD_VIEW_CONFIG ? 4 : 5;
    layout->show_info = content_h >= info_h + 4;

    if (!preview_open) {
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

        layout->preview.row = layout->show_info ? 2 + info_h + 1 : 2;
        layout->preview.col = list_w + 2;
        layout->preview.width = right_w;
        layout->preview.height = layout->show_info ? content_h - info_h - 1 : content_h;
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
        layout->preview.row = layout->show_info ? layout->info.row + info_h + 1 : 2 + list_h + 1;
        layout->preview.height = layout->show_info ? content_h - list_h - info_h - 2 : content_h - list_h - 1;
    }
}

int systemd_list_rows_visible(const Ui *ui, const LuloSystemdState *state)
{
    SystemdWidgetLayout layout;

    if (!ui->systemd || !state) return 1;
    build_systemd_widget_layout(ui, state, &layout);
    return clamp_int(rect_inner_rows(&layout.list) - 1, 1, 4096);
}

int systemd_preview_rows_visible(const Ui *ui, const LuloSystemdState *state)
{
    SystemdWidgetLayout layout;

    if (!ui->systemd || !state) return 1;
    build_systemd_widget_layout(ui, state, &layout);
    return clamp_int(rect_inner_rows(&layout.preview), 1, 4096);
}

static const LuloSystemdServiceRow *selected_systemd_service(const LuloSystemdSnapshot *snap,
                                                             const LuloSystemdState *state)
{
    if (!snap || !state || snap->count <= 0 || !systemd_service_selection_active(state)) return NULL;
    if (state->selected >= 0 && state->selected < snap->count) return &snap->rows[state->selected];
    return NULL;
}

static const LuloSystemdConfigRow *selected_systemd_config(const LuloSystemdSnapshot *snap,
                                                           const LuloSystemdState *state)
{
    if (!snap || !state || snap->config_count <= 0 || !systemd_config_selection_active(state)) return NULL;
    if (state->config_selected >= 0 && state->config_selected < snap->config_count) {
        return &snap->configs[state->config_selected];
    }
    return NULL;
}

const char *active_systemd_edit_path(const LuloSystemdSnapshot *snap, const LuloSystemdState *state)
{
    const LuloSystemdServiceRow *service;
    const LuloSystemdConfigRow *row;

    if (!snap || !state) return NULL;
    if (state->view == LULO_SYSTEMD_VIEW_CONFIG) {
        row = selected_systemd_config(snap, state);
        if (row && row->path[0]) return row->path;
        if (state->config_cursor >= 0 && state->config_cursor < snap->config_count &&
            snap->configs[state->config_cursor].path[0]) {
            return snap->configs[state->config_cursor].path;
        }
        return NULL;
    }
    service = selected_systemd_service(snap, state);
    if (service && service->fragment_path[0]) return service->fragment_path;
    if (state->cursor >= 0 && state->cursor < snap->count &&
        snap->rows[state->cursor].fragment_path[0]) {
        return snap->rows[state->cursor].fragment_path;
    }
    return NULL;
}

static Rgb systemd_active_color(const Theme *theme, const LuloSystemdServiceRow *row)
{
    if (!strcmp(row->active, "failed")) return theme->red;
    if (!strcmp(row->active, "active") && !strcmp(row->sub, "running")) return theme->green;
    if (!strcmp(row->active, "active")) return theme->cyan;
    if (!strcmp(row->active, "activating")) return theme->orange;
    if (!strcmp(row->active, "deactivating")) return theme->yellow;
    if (!strcmp(row->active, "inactive")) return theme->dim;
    return theme->white;
}

static Rgb systemd_file_state_color(const Theme *theme, const char *state)
{
    if (!strcmp(state, "enabled")) return theme->green;
    if (!strcmp(state, "disabled")) return theme->dim;
    if (!strcmp(state, "static")) return theme->cyan;
    if (!strcmp(state, "generated")) return theme->blue;
    if (!strcmp(state, "transient")) return theme->orange;
    if (!strcmp(state, "alias")) return theme->dim;
    return theme->white;
}

static void render_systemd_view_tabs(struct ncplane *p, const Theme *theme, const SystemdWidgetLayout *layout,
                                     const LuloSystemdState *state)
{
    unsigned cols = 0;
    int x = layout->tabs_x;

    ncplane_dim_yx(p, NULL, &cols);
    plane_fill(p, layout->tabs_y, 1, (int)cols - 2, theme->bg, theme->bg);
    for (int i = 0; i < LULO_SYSTEMD_VIEW_COUNT; i++) {
        char label[24];
        int active = state && state->view == (LuloSystemdView)i;
        int width;
        Rgb fg = active ? theme->bg : theme->white;
        Rgb bg = active ? theme->border_header : theme->bg;

        snprintf(label, sizeof(label), " %s ", lulo_systemd_view_name((LuloSystemdView)i));
        width = (int)strlen(label);
        plane_putn(p, layout->tabs_y, x, fg, bg, label, width);
        x += width + 1;
    }
}

static void render_systemd_services_list(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                         const LuloSystemdSnapshot *snap, const LuloSystemdState *state)
{
    char meta[48];
    int visible_rows;
    int start;
    int scope_w = 3;
    int state_w = rect->width >= 56 ? 12 : 10;
    int file_w = rect->width >= 72 ? 10 : 8;
    int unit_w;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel,
                   state && state->view == LULO_SYSTEMD_VIEW_DEPS ? " Services / Deps " : " Services ",
                   theme->white);
    visible_rows = clamp_int(rect_inner_rows(rect) - 1, 1, 4096);
    start = state ? clamp_int(state->list_scroll, 0, snap && snap->count > visible_rows ? snap->count - visible_rows : 0) : 0;
    if (snap && snap->count > 0) {
        snprintf(meta, sizeof(meta), "%d-%d/%d",
                 start + 1, clamp_int(start + visible_rows, 1, snap->count), snap->count);
        draw_inner_meta(p, theme, rect, meta, state && !state->focus_preview ? theme->green : theme->cyan);
    }
    if (!snap || snap->count <= 0) {
        plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg, "no services", rect->width - 4);
        return;
    }

    unit_w = rect_inner_cols(rect) - scope_w - state_w - file_w - 3;
    if (unit_w < 12) unit_w = 12;
    plane_putn(p, rect->row + 1, rect->col + 1, theme->dim, theme->bg, "scp", scope_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + scope_w + 1, theme->dim, theme->bg, "state", state_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + scope_w + 1 + state_w + 1, theme->dim, theme->bg, "file", file_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + scope_w + 1 + state_w + 1 + file_w + 1,
               theme->dim, theme->bg, "unit", unit_w);

    for (int i = 0; i < visible_rows; i++) {
        int idx = start + i;
        int y = rect->row + 2 + i;
        int x = rect->col + 1;
        int selected = state && idx == state->cursor;
        Rgb row_bg = selected ? theme->select_bg : theme->bg;
        const LuloSystemdServiceRow *row;
        char statebuf[64];

        plane_fill(p, y, rect->col + 1, rect_inner_cols(rect), row_bg, row_bg);
        if (idx >= snap->count) continue;
        row = &snap->rows[idx];
        snprintf(statebuf, sizeof(statebuf), "%s/%s", row->active, row->sub);
        plane_putn(p, y, x, selected ? theme->select_fg : (row->user_scope ? theme->orange : theme->cyan),
                   row_bg, row->user_scope ? "usr" : "sys", scope_w);
        x += scope_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg : systemd_active_color(theme, row), row_bg, statebuf, state_w);
        x += state_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg : systemd_file_state_color(theme, row->file_state),
                   row_bg, row->file_state, file_w);
        x += file_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg : theme->white, row_bg, row->unit, unit_w);
    }
}

static void render_systemd_config_list(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                       const LuloSystemdSnapshot *snap, const LuloSystemdState *state)
{
    char meta[48];
    int visible_rows;
    int start;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Config Files ", theme->white);
    visible_rows = clamp_int(rect_inner_rows(rect) - 1, 1, 4096);
    start = state ? clamp_int(state->config_list_scroll, 0,
                              snap && snap->config_count > visible_rows ? snap->config_count - visible_rows : 0) : 0;
    if (snap && snap->config_count > 0) {
        snprintf(meta, sizeof(meta), "%d-%d/%d",
                 start + 1, clamp_int(start + visible_rows, 1, snap->config_count), snap->config_count);
        draw_inner_meta(p, theme, rect, meta, state && !state->focus_preview ? theme->green : theme->cyan);
    }
    plane_putn(p, rect->row + 1, rect->col + 1, theme->dim, theme->bg, "path", rect_inner_cols(rect));
    if (!snap || snap->config_count <= 0) {
        plane_putn(p, rect->row + 2, rect->col + 2, theme->white, theme->bg, "no configs", rect->width - 4);
        return;
    }

    for (int i = 0; i < visible_rows; i++) {
        int idx = start + i;
        int y = rect->row + 2 + i;
        int selected = state && idx == state->config_cursor;
        Rgb row_bg = selected ? theme->select_bg : theme->bg;

        plane_fill(p, y, rect->col + 1, rect_inner_cols(rect), row_bg, row_bg);
        if (idx >= snap->config_count) continue;
        plane_putn(p, y, rect->col + 1,
                   selected ? theme->select_fg :
                   (strstr(snap->configs[idx].path, ".pacnew") ? theme->orange : theme->cyan),
                   row_bg, snap->configs[idx].name, rect_inner_cols(rect));
    }
}

static void render_systemd_info(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                const LuloSystemdSnapshot *snap, const LuloSystemdState *state)
{
    char buf[512];

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Details ", theme->white);

    if (state && state->view == LULO_SYSTEMD_VIEW_CONFIG) {
        const LuloSystemdConfigRow *row = selected_systemd_config(snap, state);

        if (!row) {
            plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg, "no config selected", rect->width - 4);
            return;
        }
        plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg, row->name, rect->width - 4);
        plane_putn(p, rect->row + 2, rect->col + 2, theme->dim, theme->bg, row->path, rect->width - 4);
        snprintf(buf, sizeof(buf), "lines %d%s", snap ? snap->config_line_count : 0,
                 strstr(row->path, ".pacnew") ? "  pacnew" : "");
        plane_putn(p, rect->row + 3, rect->col + 2,
                   strstr(row->path, ".pacnew") ? theme->orange : theme->cyan, theme->bg,
                   buf, rect->width - 4);
        return;
    }

    {
        const LuloSystemdServiceRow *row = selected_systemd_service(snap, state);

        if (!row) {
            plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg, "no service selected", rect->width - 4);
            return;
        }
        snprintf(buf, sizeof(buf), "%s  (%s)", row->unit, row->user_scope ? "user" : "system");
        plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg, buf, rect->width - 4);
        snprintf(buf, sizeof(buf), "load %-10s active %-10s sub %-10s", row->load, row->active, row->sub);
        plane_putn(p, rect->row + 2, rect->col + 2, systemd_active_color(theme, row), theme->bg, buf, rect->width - 4);
        snprintf(buf, sizeof(buf), "file %-10s preset %-10s", row->file_state, row->preset);
        plane_putn(p, rect->row + 3, rect->col + 2, systemd_file_state_color(theme, row->file_state), theme->bg,
                   buf, rect->width - 4);
        snprintf(buf, sizeof(buf), "%s", row->description[0] ? row->description : "(no description)");
        plane_putn(p, rect->row + 4, rect->col + 2, theme->dim, theme->bg, buf, rect->width - 4);
    }
}

static Rgb systemd_preview_line_color(const Theme *theme, LuloSystemdView view, const char *line)
{
    const char *trim = line;

    while (trim && *trim == ' ') trim++;
    if (!line || !*line) return theme->dim;
    if (line[0] == '#') return theme->dim;
    if (line[0] == '[') return theme->cyan;
    if (view == LULO_SYSTEMD_VIEW_DEPS) {
        if (strstr(trim, ".target")) return theme->cyan;
        if (strstr(trim, ".service")) return theme->green;
        if (strstr(trim, ".socket")) return theme->orange;
        if (strstr(trim, ".timer")) return theme->yellow;
        return theme->white;
    }
    if (view == LULO_SYSTEMD_VIEW_CONFIG) {
        if (strchr(line, '=')) return theme->green;
        return theme->white;
    }
    if (!strncmp(line, "Exec", 4) || !strncmp(line, "WantedBy", 8) || !strncmp(line, "Alias", 5)) return theme->green;
    if (strstr(line, "failed") || strstr(line, "No files")) return theme->red;
    return theme->white;
}

static void render_systemd_preview(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                   const LuloSystemdSnapshot *snap, const LuloSystemdState *state)
{
    char meta[48];
    char title[96];
    const char *const *lines = NULL;
    int line_count = 0;
    int start;
    int visible_rows;

    if (!rect_valid(rect) || !state) return;

    switch (state->view) {
    case LULO_SYSTEMD_VIEW_SERVICES:
        snprintf(title, sizeof(title), " Unit File ");
        lines = (const char *const *)snap->file_lines;
        line_count = snap->file_line_count;
        break;
    case LULO_SYSTEMD_VIEW_DEPS:
        snprintf(title, sizeof(title), " Reverse Deps ");
        lines = (const char *const *)snap->dep_lines;
        line_count = snap->dep_line_count;
        break;
    case LULO_SYSTEMD_VIEW_CONFIG:
    default:
        snprintf(title, sizeof(title), " Config Preview ");
        lines = (const char *const *)snap->config_lines;
        line_count = snap->config_line_count;
        break;
    }

    draw_inner_box(p, theme, rect, theme->border_panel, title, theme->white);
    visible_rows = rect_inner_rows(rect);
    start = state->view == LULO_SYSTEMD_VIEW_CONFIG ? state->config_file_scroll : state->file_scroll;
    start = clamp_int(start, 0, line_count > visible_rows ? line_count - visible_rows : 0);
    if (line_count > 0) {
        snprintf(meta, sizeof(meta), "%d-%d/%d",
                 start + 1, clamp_int(start + visible_rows, 1, line_count), line_count);
        draw_inner_meta(p, theme, rect, meta, state->focus_preview ? theme->green : theme->cyan);
    }
    if (!lines || line_count <= 0) {
        plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg, "no preview", rect->width - 4);
        return;
    }

    for (int i = 0; i < visible_rows; i++) {
        int idx = start + i;
        int y = rect->row + 1 + i;
        const char *line;

        if (idx >= line_count) break;
        line = lines[idx];
        if (!line) line = "";
        plane_putn(p, y, rect->col + 1, systemd_preview_line_color(theme, state->view, line),
                   theme->bg, line, rect_inner_cols(rect));
    }
}

void render_systemd_widget(Ui *ui, const LuloSystemdSnapshot *snap, const LuloSystemdState *state)
{
    SystemdWidgetLayout layout;
    unsigned rows = 0;
    unsigned cols = 0;

    if (!ui->systemd || !state) return;
    ncplane_dim_yx(ui->systemd, &rows, &cols);
    plane_clear_inner_local(ui->systemd, ui->theme, (int)rows, (int)cols);
    build_systemd_widget_layout(ui, state, &layout);
    render_systemd_view_tabs(ui->systemd, ui->theme, &layout, state);
    if (state->view == LULO_SYSTEMD_VIEW_CONFIG) render_systemd_config_list(ui->systemd, ui->theme, &layout.list, snap, state);
    else render_systemd_services_list(ui->systemd, ui->theme, &layout.list, snap, state);
    if (layout.show_info) render_systemd_info(ui->systemd, ui->theme, &layout.info, snap, state);
    render_systemd_preview(ui->systemd, ui->theme, &layout.preview, snap, state);
}

void render_systemd_status(Ui *ui, const LuloSystemdSnapshot *snap, const LuloSystemdState *state,
                           const LuloSystemdBackendStatus *backend_status)
{
    char buf[512];

    if (!ui->load || !state) return;
    plane_reset(ui->load, ui->theme);
    if (backend_status && !backend_status->have_snapshot && backend_status->busy) {
        snprintf(buf, sizeof(buf), "view %s  loading systemd cache...",
                 lulo_systemd_view_name(state->view));
        plane_putn(ui->load, 0, 0, ui->theme->white, ui->theme->bg, buf, ui->lo.load.width - 2);
        return;
    }
    if (backend_status && backend_status->error[0]) {
        plane_putn(ui->load, 0, 0, ui->theme->red, ui->theme->bg,
                   backend_status->error, ui->lo.load.width - 2);
        return;
    }
    if (state->view == LULO_SYSTEMD_VIEW_CONFIG) {
        const LuloSystemdConfigRow *row = selected_systemd_config(snap, state);

        snprintf(buf, sizeof(buf), "view %s  configs %d  focus %s  cursor %d/%d  open %s",
                 lulo_systemd_view_name(state->view),
                 snap ? snap->config_count : 0,
                 state->focus_preview ? "preview" : "list",
                 snap && snap->config_count > 0 && systemd_config_cursor_active(state) ? state->config_cursor + 1 : 0,
                 snap ? snap->config_count : 0,
                 row ? row->name : "none");
        if (backend_status && backend_status->loading_active) {
            strncat(buf, "  loading", sizeof(buf) - strlen(buf) - 1);
        } else if (backend_status && backend_status->loading_full) {
            strncat(buf, "  refreshing", sizeof(buf) - strlen(buf) - 1);
        }
        plane_putn(ui->load, 0, 0, ui->theme->white, ui->theme->bg, buf, ui->lo.load.width - 2);
        return;
    }

    {
        const LuloSystemdServiceRow *row = selected_systemd_service(snap, state);

        snprintf(buf, sizeof(buf), "view %s  services %d  focus %s  cursor %d/%d  open %s%.160s%s",
                 lulo_systemd_view_name(state->view),
                 snap ? snap->count : 0,
                 state->focus_preview ? "preview" : "list",
                 snap && snap->count > 0 && systemd_service_cursor_active(state) ? state->cursor + 1 : 0,
                 snap ? snap->count : 0,
                 row ? (row->user_scope ? "usr " : "sys ") : "",
                 row ? row->unit : "none",
                 row && row->description[0] ? "  " : "");
        if (backend_status && backend_status->loading_active) {
            strncat(buf, "  loading", sizeof(buf) - strlen(buf) - 1);
        } else if (backend_status && backend_status->loading_full) {
            strncat(buf, "  refreshing", sizeof(buf) - strlen(buf) - 1);
        }
        plane_putn(ui->load, 0, 0, ui->theme->white, ui->theme->bg, buf, ui->lo.load.width - 2);
        if (row && row->description[0]) {
            int used = (int)strlen(buf);

            if (used + 2 < ui->lo.load.width - 2) {
                plane_putn(ui->load, 0, used, ui->theme->dim, ui->theme->bg,
                           row->description, ui->lo.load.width - used - 2);
            }
        }
    }
}

static int point_in_rect_global_local(const LuloRect *origin, const LuloRect *rect, int y, int x)
{
    int top = origin->row + 1 + rect->row;
    int left = origin->col + 1 + rect->col;

    return y >= top && y < top + rect->height && x >= left && x < left + rect->width;
}

static LuloSystemdView systemd_view_from_point(Ui *ui, const SystemdWidgetLayout *layout,
                                               int global_y, int global_x)
{
    int row = ui->lo.top.row + 1 + layout->tabs_y;
    int x = ui->lo.top.col + 1 + layout->tabs_x;

    if (global_y != row) return LULO_SYSTEMD_VIEW_COUNT;
    for (int i = 0; i < LULO_SYSTEMD_VIEW_COUNT; i++) {
        char label[24];
        int width;

        snprintf(label, sizeof(label), " %s ", lulo_systemd_view_name((LuloSystemdView)i));
        width = (int)strlen(label);
        if (global_x >= x && global_x < x + width) return (LuloSystemdView)i;
        x += width + 1;
    }
    return LULO_SYSTEMD_VIEW_COUNT;
}

int point_on_systemd_view_tabs(Ui *ui, const LuloSystemdState *state, int global_y, int global_x)
{
    SystemdWidgetLayout layout;

    if (!ui->systemd || !state) return 0;
    build_systemd_widget_layout(ui, state, &layout);
    return systemd_view_from_point(ui, &layout, global_y, global_x) != LULO_SYSTEMD_VIEW_COUNT;
}

int handle_systemd_click(Ui *ui, int global_y, int global_x,
                         const LuloSystemdSnapshot *snap, LuloSystemdState *state,
                         RenderFlags *render)
{
    SystemdWidgetLayout layout;
    LuloSystemdView hit_view;
    int local_y;

    if (!ui->systemd || !state || !snap) return 0;
    build_systemd_widget_layout(ui, state, &layout);
    hit_view = systemd_view_from_point(ui, &layout, global_y, global_x);
    if (hit_view != LULO_SYSTEMD_VIEW_COUNT) {
        if (state->view != hit_view) {
            state->view = hit_view;
            state->focus_preview = 0;
            render->need_systemd_refresh = 1;
        } else {
            render->need_systemd = 1;
        }
        render->need_render = 1;
        return 1;
    }
    if (point_in_rect_global_local(&ui->lo.top, &layout.list, global_y, global_x)) {
        int list_rows = systemd_list_rows_visible(ui, state);
        int preview_rows = systemd_preview_rows_visible(ui, state);

        state->focus_preview = 0;
        local_y = global_y - (ui->lo.top.row + 1 + layout.list.row);
        if (local_y >= 2) {
            if (state->view == LULO_SYSTEMD_VIEW_CONFIG) {
                int row_index = state->config_list_scroll + (local_y - 2);

                if (row_index >= 0 && row_index < snap->config_count) {
                    lulo_systemd_set_cursor(state, snap, list_rows, preview_rows, row_index);
                    if (lulo_systemd_open_current(state, snap, list_rows, preview_rows)) render->need_systemd_refresh = 1;
                    else render->need_systemd = 1;
                }
            } else {
                int row_index = state->list_scroll + (local_y - 2);

                if (row_index >= 0 && row_index < snap->count) {
                    lulo_systemd_set_cursor(state, snap, list_rows, preview_rows, row_index);
                    if (lulo_systemd_open_current(state, snap, list_rows, preview_rows)) render->need_systemd_refresh = 1;
                    else render->need_systemd = 1;
                }
            }
        } else {
            render->need_systemd = 1;
        }
        render->need_render = 1;
        return 1;
    }
    if (point_in_rect_global_local(&ui->lo.top, &layout.preview, global_y, global_x)) {
        if (!state->focus_preview) {
            state->focus_preview = 1;
            render->need_systemd = 1;
            render->need_render = 1;
        }
        return 1;
    }
    if (layout.show_info && point_in_rect_global_local(&ui->lo.top, &layout.info, global_y, global_x)) {
        render->need_systemd = 1;
        render->need_render = 1;
        return 1;
    }
    return 0;
}

int handle_systemd_wheel_target(Ui *ui, LuloSystemdState *state,
                                RenderFlags *render, int global_y, int global_x)
{
    SystemdWidgetLayout layout;

    if (!ui->systemd || !state) return 0;
    build_systemd_widget_layout(ui, state, &layout);
    if (point_in_rect_global_local(&ui->lo.top, &layout.list, global_y, global_x)) {
        int local_y = global_y - (ui->lo.top.row + 1 + layout.list.row);

        if (local_y < 2) return 0;
        if (state->focus_preview) {
            state->focus_preview = 0;
            if (render) render->need_systemd = 1;
        }
        return 1;
    }
    if (point_in_rect_global_local(&ui->lo.top, &layout.preview, global_y, global_x)) {
        int local_y = global_y - (ui->lo.top.row + 1 + layout.preview.row);

        if (local_y < 1) return 0;
        if (!state->focus_preview) {
            state->focus_preview = 1;
            if (render) render->need_systemd = 1;
        }
        return 1;
    }
    return 0;
}
