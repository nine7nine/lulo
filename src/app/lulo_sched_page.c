#define _GNU_SOURCE

#include "lulo_app.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    int tabs_y;
    int tabs_x;
    LuloRect list;
    LuloRect info;
    LuloRect preview;
    int show_info;
} SchedWidgetLayout;

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

static int sched_profile_selection_active(const LuloSchedState *state)
{
    return state && state->profile_selected >= 0 && state->selected_profile[0];
}

static int sched_rule_selection_active(const LuloSchedState *state)
{
    return state && state->rule_selected >= 0 && state->selected_rule[0];
}

static int sched_live_selection_active(const LuloSchedState *state)
{
    return state && state->live_selected >= 0 && state->selected_live_pid > 0;
}

static const LuloSchedRuleRow *active_sched_rule_row(const LuloSchedSnapshot *snap, const LuloSchedState *state)
{
    if (!snap || !state) return NULL;
    if (state->rule_selected >= 0 && state->rule_selected < snap->rule_count) {
        return &snap->rules[state->rule_selected];
    }
    if (state->rule_cursor >= 0 && state->rule_cursor < snap->rule_count) {
        return &snap->rules[state->rule_cursor];
    }
    return NULL;
}

static const LuloSchedProfileRow *active_sched_profile_row(const LuloSchedSnapshot *snap, const LuloSchedState *state)
{
    if (!snap || !state) return NULL;
    if (state->profile_selected >= 0 && state->profile_selected < snap->profile_count) {
        return &snap->profiles[state->profile_selected];
    }
    if (state->profile_cursor >= 0 && state->profile_cursor < snap->profile_count) {
        return &snap->profiles[state->profile_cursor];
    }
    return NULL;
}

const char *active_sched_edit_path(const LuloSchedSnapshot *snap, const LuloSchedState *state)
{
    if (!snap || !state) return NULL;
    switch (state->view) {
    case LULO_SCHED_VIEW_PROFILES:
        if (state->profile_selected >= 0 && state->profile_selected < snap->profile_count &&
            snap->profiles[state->profile_selected].path[0]) {
            return snap->profiles[state->profile_selected].path;
        }
        if (state->profile_cursor >= 0 && state->profile_cursor < snap->profile_count &&
            snap->profiles[state->profile_cursor].path[0]) {
            return snap->profiles[state->profile_cursor].path;
        }
        break;
    case LULO_SCHED_VIEW_RULES:
        if (state->rule_selected >= 0 && state->rule_selected < snap->rule_count &&
            snap->rules[state->rule_selected].path[0]) {
            return snap->rules[state->rule_selected].path;
        }
        if (state->rule_cursor >= 0 && state->rule_cursor < snap->rule_count &&
            snap->rules[state->rule_cursor].path[0]) {
            return snap->rules[state->rule_cursor].path;
        }
        break;
    case LULO_SCHED_VIEW_LIVE:
    default:
        break;
    }
    return NULL;
}

const char *active_sched_delete_path(const LuloSchedSnapshot *snap, const LuloSchedState *state)
{
    const LuloSchedRuleRow *rule;
    const LuloSchedProfileRow *profile;

    if (!snap || !state) return NULL;
    switch (state->view) {
    case LULO_SCHED_VIEW_PROFILES:
        profile = active_sched_profile_row(snap, state);
        return profile && profile->path[0] ? profile->path : NULL;
    case LULO_SCHED_VIEW_RULES:
        rule = active_sched_rule_row(snap, state);
        if (!rule || !rule->path[0] || rule->match_kind == LULO_SCHED_MATCH_DYNAMIC) return NULL;
        return rule->path;
    case LULO_SCHED_VIEW_LIVE:
    default:
        return NULL;
    }
}

int active_sched_is_builtin_rule(const LuloSchedSnapshot *snap, const LuloSchedState *state)
{
    const LuloSchedRuleRow *rule = active_sched_rule_row(snap, state);

    return rule && rule->match_kind == LULO_SCHED_MATCH_DYNAMIC;
}

static int next_sched_index_path(const char *dir, const char *prefix, char *path, size_t path_len,
                                 char *stem, size_t stem_len)
{
    for (int i = 1; i < 1000; i++) {
        char candidate_stem[64];
        char candidate_path[PATH_MAX];

        snprintf(candidate_stem, sizeof(candidate_stem), "%s-%02d", prefix, i);
        snprintf(candidate_path, sizeof(candidate_path), "%s/%s.conf", dir, candidate_stem);
        if (access(candidate_path, F_OK) == 0) continue;
        if (snprintf(path, path_len, "%s", candidate_path) >= (int)path_len) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (snprintf(stem, stem_len, "%s", candidate_stem) >= (int)stem_len) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }
    errno = EEXIST;
    return -1;
}

int sched_prepare_new_entry(const LuloSchedSnapshot *snap, const LuloSchedState *state,
                            char *path, size_t path_len, char **content_out,
                            char *err, size_t errlen)
{
    char dir[PATH_MAX];
    char stem[64];
    char *content = NULL;

    if (err && errlen > 0) err[0] = '\0';
    if (content_out) *content_out = NULL;
    if (!snap || !state || !path || path_len == 0 || !content_out) {
        if (err && errlen > 0) snprintf(err, errlen, "invalid scheduler entry request");
        errno = EINVAL;
        return -1;
    }
    if (!snap->config_root[0]) {
        if (err && errlen > 0) snprintf(err, errlen, "scheduler config root unavailable");
        errno = EINVAL;
        return -1;
    }

    switch (state->view) {
    case LULO_SCHED_VIEW_PROFILES:
        if (snprintf(dir, sizeof(dir), "%s/profiles.d", snap->config_root) >= (int)sizeof(dir) ||
            next_sched_index_path(dir, "90-custom-profile", path, path_len, stem, sizeof(stem)) < 0) {
            if (err && errlen > 0) snprintf(err, errlen, "failed to allocate new profile path");
            return -1;
        }
        if (asprintf(&content,
                     "name=%s\n"
                     "enabled=true\n"
                     "nice=0\n"
                     "# policy=other\n"
                     "# rt_priority=1\n",
                     stem) < 0) {
            if (err && errlen > 0) snprintf(err, errlen, "out of memory");
            return -1;
        }
        break;
    case LULO_SCHED_VIEW_RULES:
        if (snprintf(dir, sizeof(dir), "%s/rules.d", snap->config_root) >= (int)sizeof(dir) ||
            next_sched_index_path(dir, "90-custom-rule", path, path_len, stem, sizeof(stem)) < 0) {
            if (err && errlen > 0) snprintf(err, errlen, "failed to allocate new rule path");
            return -1;
        }
        if (asprintf(&content,
                     "name=%s\n"
                     "enabled=true\n"
                     "match=comm\n"
                     "pattern=example*\n"
                     "profile=%s\n"
                     "# exclude=true\n",
                     stem,
                     snap->background_profile[0] ? snap->background_profile : "background") < 0) {
            if (err && errlen > 0) snprintf(err, errlen, "out of memory");
            return -1;
        }
        break;
    case LULO_SCHED_VIEW_LIVE:
    default:
        if (err && errlen > 0) snprintf(err, errlen, "create entries from Profiles or Rules");
        errno = EINVAL;
        return -1;
    }

    *content_out = content;
    return 0;
}

static int sched_list_scroll(const LuloSchedState *state)
{
    if (!state) return 0;
    switch (state->view) {
    case LULO_SCHED_VIEW_RULES:
        return state->rule_list_scroll;
    case LULO_SCHED_VIEW_LIVE:
        return state->live_list_scroll;
    case LULO_SCHED_VIEW_PROFILES:
    default:
        return state->profile_list_scroll;
    }
}

static int sched_active_count(const LuloSchedSnapshot *snap, const LuloSchedState *state)
{
    if (!snap || !state) return 0;
    switch (state->view) {
    case LULO_SCHED_VIEW_RULES:
        return snap->rule_count;
    case LULO_SCHED_VIEW_LIVE:
        return snap->live_count;
    case LULO_SCHED_VIEW_PROFILES:
    default:
        return snap->profile_count;
    }
}

static int sched_preview_open(const LuloSchedState *state)
{
    if (!state) return 0;
    switch (state->view) {
    case LULO_SCHED_VIEW_RULES:
        return sched_rule_selection_active(state);
    case LULO_SCHED_VIEW_LIVE:
        return sched_live_selection_active(state);
    case LULO_SCHED_VIEW_PROFILES:
    default:
        return sched_profile_selection_active(state);
    }
}

static void build_sched_widget_layout(const Ui *ui, const LuloSchedState *state, SchedWidgetLayout *layout)
{
    unsigned rows = 0;
    unsigned cols = 0;
    int inner_w;
    int content_h;
    int info_h;

    memset(layout, 0, sizeof(*layout));
    if (!ui->sched) return;
    ncplane_dim_yx(ui->sched, &rows, &cols);
    if (rows < 8 || cols < 28) return;
    layout->tabs_y = 1;
    layout->tabs_x = 2;
    inner_w = (int)cols - 2;
    content_h = (int)rows - 4;
    if (inner_w < 20 || content_h < 4) return;

    info_h = 5;
    layout->show_info = content_h >= info_h + 4;
    if (!sched_preview_open(state)) {
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

int sched_list_rows_visible(const Ui *ui, const LuloSchedState *state)
{
    SchedWidgetLayout layout;

    if (!ui->sched || !state) return 1;
    build_sched_widget_layout(ui, state, &layout);
    return clamp_int(rect_inner_rows(&layout.list) - 1, 1, 4096);
}

int sched_preview_rows_visible(const Ui *ui, const LuloSchedState *state)
{
    SchedWidgetLayout layout;

    if (!ui->sched || !state) return 1;
    build_sched_widget_layout(ui, state, &layout);
    return clamp_int(rect_inner_rows(&layout.preview), 1, 4096);
}

static void render_sched_view_tabs(struct ncplane *p, const Theme *theme,
                                   const SchedWidgetLayout *layout,
                                   const LuloSchedState *state)
{
    unsigned cols = 0;
    int x = layout->tabs_x;

    ncplane_dim_yx(p, NULL, &cols);
    plane_fill(p, layout->tabs_y, 1, (int)cols - 2, theme->bg, theme->bg);
    for (int i = 0; i < LULO_SCHED_VIEW_COUNT; i++) {
        char label[24];
        int active = state && state->view == (LuloSchedView)i;
        int width;
        Rgb fg = active ? theme->bg : theme->white;
        Rgb bg = active ? theme->border_header : theme->bg;

        snprintf(label, sizeof(label), " %s ", lulo_sched_view_name((LuloSchedView)i));
        width = (int)strlen(label);
        plane_putn(p, layout->tabs_y, x, fg, bg, label, width);
        x += width + 1;
    }
}

static Rgb sched_policy_color(const Theme *theme, int policy)
{
    switch (policy) {
    case 1:
    case 2:
        return theme->orange;
    case 3:
        return theme->yellow;
    case 5:
        return theme->dim;
    default:
        return theme->white;
    }
}

static Rgb sched_nice_color(const Theme *theme, int nice_value)
{
    if (nice_value < 0) return theme->cyan;
    if (nice_value > 0) return theme->yellow;
    return theme->white;
}

static Rgb sched_profile_color(const Theme *theme, const char *profile)
{
    if (!profile || !*profile) return theme->dim;
    if (!strcmp(profile, "audio-rt")) return theme->orange;
    if (!strcmp(profile, "multimedia")) return theme->yellow;
    if (!strcmp(profile, "desktop-fg")) return theme->cyan;
    if (!strcmp(profile, "desktop")) return theme->blue;
    if (!strcmp(profile, "desktop-bg")) return theme->green;
    if (!strcmp(profile, "focused")) return theme->red;
    if (!strcmp(profile, "background")) return theme->green;
    if (!strcmp(profile, "idle")) return theme->dim;
    return theme->white;
}

static Rgb sched_status_color(const Theme *theme, const char *status)
{
    if (!status || !*status) return theme->dim;
    if (!strcmp(status, "ok")) return theme->green;
    if (!strcmp(status, "applied")) return theme->cyan;
    return theme->red;
}

static void render_sched_profiles_list(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                       const LuloSchedSnapshot *snap, const LuloSchedState *state)
{
    char meta[48];
    int visible_rows;
    int start;
    int flag_w = 2;
    int nice_w = 5;
    int pol_w = 5;
    int rt_w = 4;
    int name_w;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Profiles ", theme->white);
    visible_rows = clamp_int(rect_inner_rows(rect) - 1, 1, 4096);
    start = state ? clamp_int(state->profile_list_scroll, 0,
                              snap && snap->profile_count > visible_rows ? snap->profile_count - visible_rows : 0) : 0;
    if (snap && snap->profile_count > 0) {
        snprintf(meta, sizeof(meta), "%d-%d/%d",
                 start + 1, clamp_int(start + visible_rows, 1, snap->profile_count), snap->profile_count);
        draw_inner_meta(p, theme, rect, meta, state && !state->focus_preview ? theme->green : theme->cyan);
    }
    if (!snap || snap->profile_count <= 0) {
        plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg, "no profiles", rect->width - 4);
        return;
    }

    name_w = rect_inner_cols(rect) - flag_w - nice_w - pol_w - rt_w - 4;
    if (name_w < 12) name_w = 12;
    plane_putn(p, rect->row + 1, rect->col + 1, theme->dim, theme->bg, "on", flag_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + flag_w + 1, theme->dim, theme->bg, "profile", name_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + flag_w + 1 + name_w + 1, theme->dim, theme->bg, "nice", nice_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + flag_w + 1 + name_w + 1 + nice_w + 1, theme->dim, theme->bg, "pol", pol_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + flag_w + 1 + name_w + 1 + nice_w + 1 + pol_w + 1,
               theme->dim, theme->bg, "rt", rt_w);

    for (int i = 0; i < visible_rows; i++) {
        int idx = start + i;
        int y = rect->row + 2 + i;
        int x = rect->col + 1;
        int selected = state && idx == state->profile_cursor;
        Rgb row_bg = selected ? theme->select_bg : theme->bg;
        const LuloSchedProfileRow *row;
        char buf[96];
        char policy[16];

        plane_fill(p, y, rect->col + 1, rect_inner_cols(rect), row_bg, row_bg);
        if (idx >= snap->profile_count) continue;
        row = &snap->profiles[idx];
        plane_putn(p, y, x, selected ? theme->select_fg : (row->enabled ? theme->green : theme->dim),
                   row_bg, row->enabled ? "on" : "--", flag_w);
        x += flag_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg : sched_profile_color(theme, row->name),
                   row_bg, row->name, name_w);
        x += name_w + 1;
        snprintf(buf, sizeof(buf), "%*s", nice_w, row->has_nice ? "" : "-");
        if (row->has_nice) snprintf(buf, sizeof(buf), "%*d", nice_w, row->nice);
        plane_putn(p, y, x, selected ? theme->select_fg :
                   (row->has_nice ? sched_nice_color(theme, row->nice) : theme->dim),
                   row_bg, buf, nice_w);
        x += nice_w + 1;
        if (row->has_policy) lulo_format_proc_policy(policy, sizeof(policy), row->policy);
        else snprintf(policy, sizeof(policy), "-");
        snprintf(buf, sizeof(buf), "%-*s", pol_w, policy);
        plane_putn(p, y, x, selected ? theme->select_fg :
                   (row->has_policy ? sched_policy_color(theme, row->policy) : theme->dim),
                   row_bg, buf, pol_w);
        x += pol_w + 1;
        if (row->has_rt_priority) snprintf(buf, sizeof(buf), "%*d", rt_w, row->rt_priority);
        else snprintf(buf, sizeof(buf), "%*s", rt_w, "-");
        plane_putn(p, y, x, selected ? theme->select_fg :
                   (row->has_rt_priority ? theme->orange : theme->dim),
                   row_bg, buf, rt_w);
    }
}

static void render_sched_rules_list(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                    const LuloSchedSnapshot *snap, const LuloSchedState *state)
{
    char meta[48];
    int visible_rows;
    int start;
    int flag_w = 2;
    int match_w = 5;
    int target_w = clamp_int(rect_inner_cols(rect) / 4, 10, 18);
    int pattern_w;

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
    if (!snap || snap->rule_count <= 0) {
        plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg, "no rules", rect->width - 4);
        return;
    }

    pattern_w = rect_inner_cols(rect) - flag_w - match_w - target_w - 3;
    if (pattern_w < 12) pattern_w = 12;
    plane_putn(p, rect->row + 1, rect->col + 1, theme->dim, theme->bg, "on", flag_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + flag_w + 1, theme->dim, theme->bg, "match", match_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + flag_w + 1 + match_w + 1, theme->dim, theme->bg, "pattern", pattern_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + flag_w + 1 + match_w + 1 + pattern_w + 1,
               theme->dim, theme->bg, "target", target_w);

    for (int i = 0; i < visible_rows; i++) {
        int idx = start + i;
        int y = rect->row + 2 + i;
        int x = rect->col + 1;
        int selected = state && idx == state->rule_cursor;
        Rgb row_bg = selected ? theme->select_bg : theme->bg;
        const LuloSchedRuleRow *row;
        char target[96];

        plane_fill(p, y, rect->col + 1, rect_inner_cols(rect), row_bg, row_bg);
        if (idx >= snap->rule_count) continue;
        row = &snap->rules[idx];
        snprintf(target, sizeof(target), "%s", row->exclude ? "exclude" : row->profile);
        plane_putn(p, y, x, selected ? theme->select_fg : (row->enabled ? theme->green : theme->dim),
                   row_bg, row->enabled ? "on" : "--", flag_w);
        x += flag_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg : theme->cyan, row_bg,
                   lulo_sched_match_kind_name(row->match_kind), match_w);
        x += match_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg : theme->white, row_bg, row->pattern, pattern_w);
        x += pattern_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg :
                   (row->exclude ? theme->red : sched_profile_color(theme, row->profile)),
                   row_bg, target, target_w);
    }
}

static void render_sched_live_list(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                   const LuloSchedSnapshot *snap, const LuloSchedState *state)
{
    char meta[48];
    int visible_rows;
    int start;
    int pid_w = 6;
    int policy_w = 4;
    int nice_w = 4;
    int rt_w = 3;
    int profile_w = clamp_int(rect_inner_cols(rect) / 5, 10, 18);
    int comm_w;
    int status_w;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Live ", theme->white);
    visible_rows = clamp_int(rect_inner_rows(rect) - 1, 1, 4096);
    start = state ? clamp_int(state->live_list_scroll, 0,
                              snap && snap->live_count > visible_rows ? snap->live_count - visible_rows : 0) : 0;
    if (snap && snap->live_count > 0) {
        snprintf(meta, sizeof(meta), "%d-%d/%d",
                 start + 1, clamp_int(start + visible_rows, 1, snap->live_count), snap->live_count);
        draw_inner_meta(p, theme, rect, meta, state && !state->focus_preview ? theme->green : theme->cyan);
    }
    if (!snap || snap->live_count <= 0) {
        plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg,
                   "no matched processes", rect->width - 4);
        return;
    }

    status_w = clamp_int(rect_inner_cols(rect) / 5, 10, 16);
    comm_w = rect_inner_cols(rect) - pid_w - profile_w - policy_w - nice_w - rt_w - status_w - 5;
    if (comm_w < 12) comm_w = 12;
    plane_putn(p, rect->row + 1, rect->col + 1, theme->dim, theme->bg, "pid", pid_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + pid_w + 1, theme->dim, theme->bg, "comm", comm_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + pid_w + 1 + comm_w + 1, theme->dim, theme->bg, "profile", profile_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + pid_w + 1 + comm_w + 1 + profile_w + 1, theme->dim, theme->bg, "pol", policy_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + pid_w + 1 + comm_w + 1 + profile_w + 1 + policy_w + 1,
               theme->dim, theme->bg, "ni", nice_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + pid_w + 1 + comm_w + 1 + profile_w + 1 + policy_w + 1 + nice_w + 1,
               theme->dim, theme->bg, "rt", rt_w);
    plane_putn(p, rect->row + 1, rect->col + 1 + pid_w + 1 + comm_w + 1 + profile_w + 1 + policy_w + 1 + nice_w + 1 + rt_w + 1,
               theme->dim, theme->bg, "status", status_w);

    for (int i = 0; i < visible_rows; i++) {
        int idx = start + i;
        int y = rect->row + 2 + i;
        int x = rect->col + 1;
        int selected = state && idx == state->live_cursor;
        Rgb row_bg = selected ? theme->select_bg : theme->bg;
        const LuloSchedLiveRow *row;
        char buf[64];
        char policy[16];

        plane_fill(p, y, rect->col + 1, rect_inner_cols(rect), row_bg, row_bg);
        if (idx >= snap->live_count) continue;
        row = &snap->live[idx];
        snprintf(buf, sizeof(buf), "%*d", pid_w, row->pid);
        plane_putn(p, y, x, selected ? theme->select_fg : theme->cyan, row_bg, buf, pid_w);
        x += pid_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg : theme->white, row_bg, row->comm, comm_w);
        x += comm_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg : sched_profile_color(theme, row->profile),
                   row_bg, row->profile, profile_w);
        x += profile_w + 1;
        lulo_format_proc_policy(policy, sizeof(policy), row->policy);
        snprintf(buf, sizeof(buf), "%-*s", policy_w, policy);
        plane_putn(p, y, x, selected ? theme->select_fg : sched_policy_color(theme, row->policy),
                   row_bg, buf, policy_w);
        x += policy_w + 1;
        snprintf(buf, sizeof(buf), "%*d", nice_w, row->nice);
        plane_putn(p, y, x, selected ? theme->select_fg : sched_nice_color(theme, row->nice),
                   row_bg, buf, nice_w);
        x += nice_w + 1;
        if (row->rt_priority > 0) snprintf(buf, sizeof(buf), "%*d", rt_w, row->rt_priority);
        else snprintf(buf, sizeof(buf), "%*s", rt_w, "-");
        plane_putn(p, y, x, selected ? theme->select_fg :
                   (row->rt_priority > 0 ? theme->orange : theme->dim),
                   row_bg, buf, rt_w);
        x += rt_w + 1;
        plane_putn(p, y, x, selected ? theme->select_fg : sched_status_color(theme, row->status),
                   row_bg, row->status, status_w);
    }
}

static void render_sched_info(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                              const LuloSchedSnapshot *snap, const LuloSchedState *state)
{
    char buf[256];

    if (!rect_valid(rect) || !snap || !state) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Details ", theme->white);
    switch (state->view) {
    case LULO_SCHED_VIEW_RULES:
        if (state->rule_selected >= 0 && state->rule_selected < snap->rule_count) {
            const LuloSchedRuleRow *row = &snap->rules[state->rule_selected];

            plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg, row->name, rect->width - 4);
            snprintf(buf, sizeof(buf), "%s  %s", row->enabled ? "enabled" : "disabled",
                     row->exclude ? "exclude" : row->profile);
            plane_putn(p, rect->row + 2, rect->col + 2,
                       row->exclude ? theme->red : sched_profile_color(theme, row->profile),
                       theme->bg, buf, rect->width - 4);
            snprintf(buf, sizeof(buf), "%s  %s", lulo_sched_match_kind_name(row->match_kind), row->pattern);
            plane_putn(p, rect->row + 3, rect->col + 2, theme->cyan, theme->bg, buf, rect->width - 4);
            plane_putn(p, rect->row + 4, rect->col + 2, theme->dim, theme->bg, row->path, rect->width - 4);
        }
        break;
    case LULO_SCHED_VIEW_LIVE:
        if (state->live_selected >= 0 && state->live_selected < snap->live_count) {
            const LuloSchedLiveRow *row = &snap->live[state->live_selected];
            char policy[16];

            lulo_format_proc_policy(policy, sizeof(policy), row->policy);
            snprintf(buf, sizeof(buf), "%s (%d)", row->comm, row->pid);
            plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg, buf, rect->width - 4);
            snprintf(buf, sizeof(buf), "%s  %s", row->profile, row->rule);
            plane_putn(p, rect->row + 2, rect->col + 2, sched_profile_color(theme, row->profile),
                       theme->bg, buf, rect->width - 4);
            snprintf(buf, sizeof(buf), "pol %s  nice %d  rt %d", policy, row->nice, row->rt_priority);
            plane_putn(p, rect->row + 3, rect->col + 2, theme->yellow, theme->bg, buf, rect->width - 4);
            plane_putn(p, rect->row + 4, rect->col + 2, sched_status_color(theme, row->status), theme->bg,
                       row->status, rect->width - 4);
        }
        break;
    case LULO_SCHED_VIEW_PROFILES:
    default:
        if (state->profile_selected >= 0 && state->profile_selected < snap->profile_count) {
            const LuloSchedProfileRow *row = &snap->profiles[state->profile_selected];
            char policy[16];

            plane_putn(p, rect->row + 1, rect->col + 2, sched_profile_color(theme, row->name),
                       theme->bg, row->name, rect->width - 4);
            plane_putn(p, rect->row + 2, rect->col + 2, row->enabled ? theme->green : theme->dim, theme->bg,
                       row->enabled ? "enabled" : "disabled", rect->width - 4);
            if (row->has_policy) lulo_format_proc_policy(policy, sizeof(policy), row->policy);
            else snprintf(policy, sizeof(policy), "unchanged");
            snprintf(buf, sizeof(buf), "nice %s  policy %s",
                     row->has_nice ? "" : "unchanged",
                     policy);
            if (row->has_nice) snprintf(buf, sizeof(buf), "nice %d  policy %s", row->nice, policy);
            plane_putn(p, rect->row + 3, rect->col + 2, theme->cyan, theme->bg, buf, rect->width - 4);
            snprintf(buf, sizeof(buf), "rt %s", row->has_rt_priority ? "" : "unchanged");
            if (row->has_rt_priority) snprintf(buf, sizeof(buf), "rt %d", row->rt_priority);
            plane_putn(p, rect->row + 4, rect->col + 2, theme->orange, theme->bg, buf, rect->width - 4);
        }
        break;
    }
}

static void render_sched_preview(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                 const LuloSchedSnapshot *snap, const LuloSchedState *state)
{
    char meta[48];
    int visible_rows;
    int start;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Preview ", theme->white);
    visible_rows = clamp_int(rect_inner_rows(rect), 1, 4096);
    start = 0;
    if (state) {
        switch (state->view) {
        case LULO_SCHED_VIEW_RULES:
            start = clamp_int(state->rule_detail_scroll, 0,
                              snap && snap->detail_line_count > visible_rows ? snap->detail_line_count - visible_rows : 0);
            break;
        case LULO_SCHED_VIEW_LIVE:
            start = clamp_int(state->live_detail_scroll, 0,
                              snap && snap->detail_line_count > visible_rows ? snap->detail_line_count - visible_rows : 0);
            break;
        case LULO_SCHED_VIEW_PROFILES:
        default:
            start = clamp_int(state->profile_detail_scroll, 0,
                              snap && snap->detail_line_count > visible_rows ? snap->detail_line_count - visible_rows : 0);
            break;
        }
    }
    if (snap && snap->detail_line_count > 0) {
        snprintf(meta, sizeof(meta), "%d-%d/%d",
                 start + 1, clamp_int(start + visible_rows, 1, snap->detail_line_count), snap->detail_line_count);
        draw_inner_meta(p, theme, rect, meta, state && state->focus_preview ? theme->green : theme->cyan);
    }
    if (!snap || snap->detail_line_count <= 0) {
        plane_putn(p, rect->row + 1, rect->col + 2, theme->dim, theme->bg, "no details", rect->width - 4);
        return;
    }

    for (int i = 0; i < visible_rows; i++) {
        int idx = start + i;
        int y = rect->row + 1 + i;

        plane_fill(p, y, rect->col + 1, rect_inner_cols(rect), theme->bg, theme->bg);
        if (idx >= snap->detail_line_count) continue;
        plane_putn(p, y, rect->col + 2, theme->white, theme->bg,
                   snap->detail_lines[idx], rect->width - 4);
    }
}

void render_sched_widget(Ui *ui, const LuloSchedSnapshot *snap, const LuloSchedState *state)
{
    SchedWidgetLayout layout;
    unsigned rows = 0;
    unsigned cols = 0;

    if (!ui->sched || !state) return;
    ncplane_dim_yx(ui->sched, &rows, &cols);
    plane_clear_inner_local(ui->sched, ui->theme, (int)rows, (int)cols);
    build_sched_widget_layout(ui, state, &layout);
    render_sched_view_tabs(ui->sched, ui->theme, &layout, state);
    switch (state->view) {
    case LULO_SCHED_VIEW_RULES:
        render_sched_rules_list(ui->sched, ui->theme, &layout.list, snap, state);
        break;
    case LULO_SCHED_VIEW_LIVE:
        render_sched_live_list(ui->sched, ui->theme, &layout.list, snap, state);
        break;
    case LULO_SCHED_VIEW_PROFILES:
    default:
        render_sched_profiles_list(ui->sched, ui->theme, &layout.list, snap, state);
        break;
    }
    if (layout.show_info) render_sched_info(ui->sched, ui->theme, &layout.info, snap, state);
    render_sched_preview(ui->sched, ui->theme, &layout.preview, snap, state);
}

void render_sched_status(Ui *ui, const LuloSchedSnapshot *snap, const LuloSchedState *state,
                         const LuloSchedBackendStatus *backend_status, AppState *app)
{
    char buf[512];
    char focusbuf[128];

    if (!ui->load || !state) return;
    plane_reset(ui->load, ui->theme);
    if (backend_status && !backend_status->have_snapshot && backend_status->busy) {
        snprintf(buf, sizeof(buf), "view %s  loading scheduler...",
                 lulo_sched_view_name(state->view));
        plane_putn(ui->load, 0, 0, ui->theme->white, ui->theme->bg, buf, ui->lo.load.width - 2);
        return;
    }
    if (backend_status && backend_status->error[0]) {
        plane_putn(ui->load, 0, 0, ui->theme->red, ui->theme->bg,
                   backend_status->error, ui->lo.load.width - 2);
        return;
    }
    if (app && sched_status_current(app)) {
        plane_putn(ui->load, 0, 0, ui->theme->green, ui->theme->bg,
                   app->sched_status, ui->lo.load.width - 2);
        return;
    }
    if (snap && snap->focused_pid > 0) {
        if (snap->focused_comm[0]) {
            char commbuf[64];

            snprintf(commbuf, sizeof(commbuf), "%.44s", snap->focused_comm);
            snprintf(focusbuf, sizeof(focusbuf), "%s:%s(%d)",
                     snap->focus_provider[0] ? snap->focus_provider : "focus",
                     commbuf, snap->focused_pid);
        } else {
            snprintf(focusbuf, sizeof(focusbuf), "%s:%d",
                     snap->focus_provider[0] ? snap->focus_provider : "focus",
                     snap->focused_pid);
        }
    } else if (snap && snap->focus_enabled) {
        snprintf(focusbuf, sizeof(focusbuf), "%s:none",
                 snap->focus_provider[0] ? snap->focus_provider : "focus");
    } else {
        snprintf(focusbuf, sizeof(focusbuf), "off");
    }
    snprintf(buf, sizeof(buf), "view %s  pane %s  focused %s  watcher %dms",
             lulo_sched_view_name(state->view),
             state->focus_preview ? "preview" : "list",
             focusbuf,
             snap ? snap->watcher_interval_ms : 0);
    if (backend_status) {
        if (backend_status->reloading) strncat(buf, "  reloading", sizeof(buf) - strlen(buf) - 1);
        else if (backend_status->loading_full) strncat(buf, "  refreshing", sizeof(buf) - strlen(buf) - 1);
        else if (backend_status->loading_active) strncat(buf, "  loading", sizeof(buf) - strlen(buf) - 1);
    }
    plane_putn(ui->load, 0, 0, ui->theme->white, ui->theme->bg, buf, ui->lo.load.width - 2);
    if (snap && snap->config_root[0]) {
        int used = (int)strlen(buf);

        if (used + 2 < ui->lo.load.width - 2) {
            plane_putn(ui->load, 0, used, ui->theme->dim, ui->theme->bg,
                       snap->config_root, ui->lo.load.width - used - 2);
        }
    }
}

static LuloSchedView sched_view_from_point(Ui *ui, const SchedWidgetLayout *layout,
                                           int global_y, int global_x)
{
    int row = ui->lo.top.row + 1 + layout->tabs_y;
    int x = ui->lo.top.col + 1 + layout->tabs_x;

    if (global_y != row) return LULO_SCHED_VIEW_COUNT;
    for (int i = 0; i < LULO_SCHED_VIEW_COUNT; i++) {
        char label[24];
        int width;

        snprintf(label, sizeof(label), " %s ", lulo_sched_view_name((LuloSchedView)i));
        width = (int)strlen(label);
        if (global_x >= x && global_x < x + width) return (LuloSchedView)i;
        x += width + 1;
    }
    return LULO_SCHED_VIEW_COUNT;
}

int point_on_sched_view_tabs(Ui *ui, const LuloSchedState *state, int global_y, int global_x)
{
    SchedWidgetLayout layout;

    if (!ui->sched || !state) return 0;
    build_sched_widget_layout(ui, state, &layout);
    return sched_view_from_point(ui, &layout, global_y, global_x) != LULO_SCHED_VIEW_COUNT;
}

int handle_sched_wheel_target(Ui *ui, LuloSchedState *state,
                              RenderFlags *render, int global_y, int global_x)
{
    SchedWidgetLayout layout;

    if (!ui->sched || !state) return 0;
    build_sched_widget_layout(ui, state, &layout);
    if (point_in_rect_global_local(&ui->lo.top, &layout.list, global_y, global_x)) {
        int local_y = global_y - (ui->lo.top.row + 1 + layout.list.row);

        if (local_y < 2) return 0;
        if (state->focus_preview) {
            state->focus_preview = 0;
            if (render) render->need_sched = 1;
        }
        return 1;
    }
    if (point_in_rect_global_local(&ui->lo.top, &layout.preview, global_y, global_x)) {
        int local_y = global_y - (ui->lo.top.row + 1 + layout.preview.row);

        if (local_y < 1) return 0;
        if (!state->focus_preview) {
            state->focus_preview = 1;
            if (render) render->need_sched = 1;
        }
        return 1;
    }
    return 0;
}

int handle_sched_click(Ui *ui, int global_y, int global_x,
                       const LuloSchedSnapshot *snap, LuloSchedState *state,
                       RenderFlags *render)
{
    SchedWidgetLayout layout;
    LuloSchedView hit_view;
    int local_y;

    if (!ui->sched || !state || !snap) return 0;
    build_sched_widget_layout(ui, state, &layout);
    hit_view = sched_view_from_point(ui, &layout, global_y, global_x);
    if (hit_view != LULO_SCHED_VIEW_COUNT) {
        if (state->view != hit_view) {
            state->view = hit_view;
            state->focus_preview = 0;
            render->need_sched_refresh = 1;
        } else {
            render->need_sched = 1;
        }
        render->need_render = 1;
        return 1;
    }
    if (point_in_rect_global_local(&ui->lo.top, &layout.list, global_y, global_x)) {
        int list_rows = sched_list_rows_visible(ui, state);
        int preview_rows = sched_preview_rows_visible(ui, state);

        state->focus_preview = 0;
        local_y = global_y - (ui->lo.top.row + 1 + layout.list.row);
        if (local_y >= 2) {
            int row_index = sched_list_scroll(state) + (local_y - 2);
            int count = sched_active_count(snap, state);

            if (row_index >= 0 && row_index < count) {
                lulo_sched_set_cursor(state, snap, list_rows, preview_rows, row_index);
                if (lulo_sched_open_current(state, snap, list_rows, preview_rows)) {
                    render->need_sched_refresh = 1;
                } else {
                    render->need_sched = 1;
                }
            }
        } else {
            render->need_sched = 1;
        }
        render->need_render = 1;
        return 1;
    }
    if (point_in_rect_global_local(&ui->lo.top, &layout.preview, global_y, global_x)) {
        if (!state->focus_preview) {
            state->focus_preview = 1;
            render->need_sched = 1;
            render->need_render = 1;
        }
        return 1;
    }
    if (layout.show_info && point_in_rect_global_local(&ui->lo.top, &layout.info, global_y, global_x)) {
        render->need_sched = 1;
        render->need_render = 1;
        return 1;
    }
    return 0;
}
