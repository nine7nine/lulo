#define _GNU_SOURCE

#include "lulo_sched.h"

#include "lulo_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int clamp_int_local(int value, int lo, int hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static int append_detail_line(char ***lines, int *count, const char *text)
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

static int append_prefixed_detail_line(char ***lines, int *count,
                                       const char *prefix, const char *value)
{
    char *buf;
    int rc;
    size_t prefix_len = strlen(prefix ? prefix : "");
    size_t value_len = strlen(value ? value : "");

    buf = malloc(prefix_len + value_len + 1);
    if (!buf) return -1;
    memcpy(buf, prefix ? prefix : "", prefix_len);
    memcpy(buf + prefix_len, value ? value : "", value_len);
    buf[prefix_len + value_len] = '\0';
    rc = append_detail_line(lines, count, buf);
    free(buf);
    return rc;
}

static void clear_detail_lines(char ***lines, int *count)
{
    if (!lines || !count) return;
    for (int i = 0; i < *count; i++) free((*lines)[i]);
    free(*lines);
    *lines = NULL;
    *count = 0;
}

static int profile_count(const LuloSchedSnapshot *snap)
{
    return snap ? snap->profile_count : 0;
}

static int rule_count(const LuloSchedSnapshot *snap)
{
    return snap ? snap->rule_count : 0;
}

static int live_count(const LuloSchedSnapshot *snap)
{
    return snap ? snap->live_count : 0;
}

static void format_sched_policy(char *buf, size_t len, int policy)
{
    const char *label = "TS";

    switch (policy) {
    case 1:
        label = "FF";
        break;
    case 2:
        label = "RR";
        break;
    case 3:
        label = "B";
        break;
    case 5:
        label = "IDL";
        break;
    case 6:
        label = "DL";
        break;
    default:
        label = "TS";
        break;
    }
    snprintf(buf, len, "%s", label);
}

const char *lulo_sched_io_class_name(int io_class)
{
    switch (io_class) {
    case 1:
        return "realtime";
    case 2:
        return "best-effort";
    case 3:
        return "idle";
    case 0:
    default:
        return "none";
    }
}

void lulo_sched_format_io(char *buf, size_t len, int io_class, int io_priority)
{
    if (!buf || len == 0) return;
    switch (io_class) {
    case 1:
        snprintf(buf, len, "RT%d", io_priority);
        break;
    case 2:
        snprintf(buf, len, "BE%d", io_priority);
        break;
    case 3:
        snprintf(buf, len, "IDL");
        break;
    case 0:
    default:
        snprintf(buf, len, "-");
        break;
    }
}

static int *active_cursor(LuloSchedState *state)
{
    if (!state) return NULL;
    switch (state->view) {
    case LULO_SCHED_VIEW_RULES:
        return &state->rule_cursor;
    case LULO_SCHED_VIEW_LIVE:
        return &state->live_cursor;
    case LULO_SCHED_VIEW_PROFILES:
    default:
        return &state->profile_cursor;
    }
}

static int *active_selected(LuloSchedState *state)
{
    if (!state) return NULL;
    switch (state->view) {
    case LULO_SCHED_VIEW_RULES:
        return &state->rule_selected;
    case LULO_SCHED_VIEW_LIVE:
        return &state->live_selected;
    case LULO_SCHED_VIEW_PROFILES:
    default:
        return &state->profile_selected;
    }
}

static int *active_list_scroll(LuloSchedState *state)
{
    if (!state) return NULL;
    switch (state->view) {
    case LULO_SCHED_VIEW_RULES:
        return &state->rule_list_scroll;
    case LULO_SCHED_VIEW_LIVE:
        return &state->live_list_scroll;
    case LULO_SCHED_VIEW_PROFILES:
    default:
        return &state->profile_list_scroll;
    }
}

static int *active_detail_scroll(LuloSchedState *state)
{
    if (!state) return NULL;
    switch (state->view) {
    case LULO_SCHED_VIEW_RULES:
        return &state->rule_detail_scroll;
    case LULO_SCHED_VIEW_LIVE:
        return &state->live_detail_scroll;
    case LULO_SCHED_VIEW_PROFILES:
    default:
        return &state->profile_detail_scroll;
    }
}

static int active_list_count(const LuloSchedState *state, const LuloSchedSnapshot *snap)
{
    if (!state || !snap) return 0;
    switch (state->view) {
    case LULO_SCHED_VIEW_RULES:
        return rule_count(snap);
    case LULO_SCHED_VIEW_LIVE:
        return live_count(snap);
    case LULO_SCHED_VIEW_PROFILES:
    default:
        return profile_count(snap);
    }
}

static int sync_profile_selection(LuloSchedState *state, const LuloSchedSnapshot *snap)
{
    int selected = -1;

    if (!state || !snap || snap->profile_count <= 0) {
        if (state) {
            state->profile_selected = -1;
            state->profile_list_scroll = 0;
            state->profile_detail_scroll = 0;
            state->selected_profile[0] = '\0';
        }
        return -1;
    }
    if (state->selected_profile[0]) {
        for (int i = 0; i < snap->profile_count; i++) {
            if (strcmp(snap->profiles[i].name, state->selected_profile) == 0) {
                selected = i;
                break;
            }
        }
    }
    if (selected < 0 && state->profile_selected >= 0) {
        selected = clamp_int_local(state->profile_selected, 0, snap->profile_count - 1);
    }
    state->profile_selected = selected;
    if (selected >= 0) snprintf(state->selected_profile, sizeof(state->selected_profile), "%s",
                                snap->profiles[selected].name);
    else state->selected_profile[0] = '\0';
    return selected;
}

static int sync_rule_selection(LuloSchedState *state, const LuloSchedSnapshot *snap)
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
            if (strcmp(snap->rules[i].name, state->selected_rule) == 0) {
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
                                snap->rules[selected].name);
    else state->selected_rule[0] = '\0';
    return selected;
}

static int sync_live_selection(LuloSchedState *state, const LuloSchedSnapshot *snap)
{
    int selected = -1;

    if (!state || !snap || snap->live_count <= 0) {
        if (state) {
            state->live_selected = -1;
            state->live_list_scroll = 0;
            state->live_detail_scroll = 0;
            state->selected_live_pid = -1;
            state->selected_live_start_time = 0;
        }
        return -1;
    }
    if (state->selected_live_pid > 0) {
        for (int i = 0; i < snap->live_count; i++) {
            if (snap->live[i].pid == state->selected_live_pid &&
                snap->live[i].start_time == state->selected_live_start_time) {
                selected = i;
                break;
            }
        }
    }
    if (selected < 0 && state->live_selected >= 0) {
        selected = clamp_int_local(state->live_selected, 0, snap->live_count - 1);
    }
    state->live_selected = selected;
    if (selected >= 0) {
        state->selected_live_pid = snap->live[selected].pid;
        state->selected_live_start_time = snap->live[selected].start_time;
    } else {
        state->selected_live_pid = -1;
        state->selected_live_start_time = 0;
    }
    return selected;
}

static int sync_active_cursor(LuloSchedState *state, const LuloSchedSnapshot *snap)
{
    int count;
    int *cursor;
    int *scroll;

    if (!state || !snap) return -1;
    count = active_list_count(state, snap);
    cursor = active_cursor(state);
    scroll = active_list_scroll(state);
    if (!cursor || !scroll) return -1;
    if (count <= 0) {
        *cursor = -1;
        *scroll = 0;
        return -1;
    }
    if (*cursor < 0) *cursor = 0;
    *cursor = clamp_int_local(*cursor, 0, count - 1);
    return *cursor;
}

static void adjust_scroll(int *scroll, int cursor, int visible_rows, int count)
{
    int max_scroll;

    if (!scroll) return;
    if (count <= 0) {
        *scroll = 0;
        return;
    }
    if (visible_rows < 1) visible_rows = 1;
    max_scroll = count > visible_rows ? count - visible_rows : 0;
    if (cursor < *scroll) *scroll = cursor;
    if (cursor >= *scroll + visible_rows) *scroll = cursor - visible_rows + 1;
    *scroll = clamp_int_local(*scroll, 0, max_scroll);
}

static void clear_detail_view(LuloSchedSnapshot *snap)
{
    if (!snap) return;
    clear_detail_lines(&snap->detail_lines, &snap->detail_line_count);
    snap->detail_title[0] = '\0';
    snap->detail_status[0] = '\0';
}

static int format_profile_detail(LuloSchedSnapshot *snap, const LuloSchedProfileRow *row)
{
    char buf[256];

    if (!snap || !row) return -1;
    snprintf(snap->detail_title, sizeof(snap->detail_title), "%s", row->name);
    snprintf(snap->detail_status, sizeof(snap->detail_status), "%s",
             row->enabled ? "profile enabled" : "profile disabled");
    if (append_prefixed_detail_line(&snap->detail_lines, &snap->detail_line_count,
                                    "path: ", row->path[0] ? row->path : "(builtin)") < 0) return -1;
    snprintf(buf, sizeof(buf), "enabled: %s", row->enabled ? "yes" : "no");
    if (append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf) < 0) return -1;
    if (row->has_nice) snprintf(buf, sizeof(buf), "nice: %d", row->nice);
    else snprintf(buf, sizeof(buf), "nice: unchanged");
    if (append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf) < 0) return -1;
    if (row->has_policy) {
        char policy[16];
        format_sched_policy(policy, sizeof(policy), row->policy);
        snprintf(buf, sizeof(buf), "policy: %s", policy);
    } else {
        snprintf(buf, sizeof(buf), "policy: unchanged");
    }
    if (append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf) < 0) return -1;
    if (row->has_rt_priority) snprintf(buf, sizeof(buf), "rt priority: %d", row->rt_priority);
    else snprintf(buf, sizeof(buf), "rt priority: unchanged");
    if (append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf) < 0) return -1;
    if (row->has_io_class) {
        snprintf(buf, sizeof(buf), "io class: %s", lulo_sched_io_class_name(row->io_class));
    } else {
        snprintf(buf, sizeof(buf), "io class: unchanged");
    }
    if (append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf) < 0) return -1;
    if (row->has_io_priority) snprintf(buf, sizeof(buf), "io priority: %d", row->io_priority);
    else if (row->has_io_class && (row->io_class == 1 || row->io_class == 2)) {
        snprintf(buf, sizeof(buf), "io priority: default (4)");
    } else {
        snprintf(buf, sizeof(buf), "io priority: unchanged");
    }
    return append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf);
}

static int format_rule_detail(LuloSchedSnapshot *snap, const LuloSchedRuleRow *row)
{
    char buf[320];

    if (!snap || !row) return -1;
    snprintf(snap->detail_title, sizeof(snap->detail_title), "%s", row->name);
    snprintf(snap->detail_status, sizeof(snap->detail_status), "%s",
             row->enabled ? "rule enabled" : "rule disabled");
    if (append_prefixed_detail_line(&snap->detail_lines, &snap->detail_line_count,
                                    "path: ", row->path[0] ? row->path : "(builtin)") < 0) return -1;
    snprintf(buf, sizeof(buf), "enabled: %s", row->enabled ? "yes" : "no");
    if (append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf) < 0) return -1;
    snprintf(buf, sizeof(buf), "action: %s", row->exclude ? "exclude" : row->profile);
    if (append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf) < 0) return -1;
    if (row->match_kind == LULO_SCHED_MATCH_DYNAMIC) snprintf(buf, sizeof(buf), "match: dynamic");
    else snprintf(buf, sizeof(buf), "match: %s", lulo_sched_match_kind_name(row->match_kind));
    if (append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf) < 0) return -1;
    snprintf(buf, sizeof(buf), "pattern: %s", row->pattern);
    if (append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf) < 0) return -1;
    if (!row->exclude) {
        snprintf(buf, sizeof(buf), "profile: %s", row->profile);
        if (append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf) < 0) return -1;
    }
    return 0;
}

static int format_live_detail(LuloSchedSnapshot *snap, const LuloSchedLiveRow *row)
{
    char buf[320];
    char policy[16];
    char io_buf[16];

    if (!snap || !row) return -1;
    format_sched_policy(policy, sizeof(policy), row->policy);
    snprintf(snap->detail_title, sizeof(snap->detail_title), "%s (%d)", row->comm, row->pid);
    snprintf(snap->detail_status, sizeof(snap->detail_status), "%s", row->status);
    snprintf(buf, sizeof(buf), "profile: %s", row->profile);
    if (append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf) < 0) return -1;
    snprintf(buf, sizeof(buf), "rule: %s", row->rule);
    if (append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf) < 0) return -1;
    snprintf(buf, sizeof(buf), "pid: %d", row->pid);
    if (append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf) < 0) return -1;
    snprintf(buf, sizeof(buf), "exe: %s", row->exe[0] ? row->exe : "(unknown)");
    if (append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf) < 0) return -1;
    snprintf(buf, sizeof(buf), "unit: %s", row->unit[0] ? row->unit : "-");
    if (append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf) < 0) return -1;
    snprintf(buf, sizeof(buf), "slice: %s", row->slice[0] ? row->slice : "-");
    if (append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf) < 0) return -1;
    if (append_prefixed_detail_line(&snap->detail_lines, &snap->detail_line_count,
                                    "cgroup: ", row->cgroup[0] ? row->cgroup : "-") < 0) return -1;
    snprintf(buf, sizeof(buf), "focused: %s", row->focused ? "yes" : "no");
    if (append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf) < 0) return -1;
    snprintf(buf, sizeof(buf), "policy: %s", policy);
    if (append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf) < 0) return -1;
    snprintf(buf, sizeof(buf), "nice: %d", row->nice);
    if (append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf) < 0) return -1;
    snprintf(buf, sizeof(buf), "rt priority: %d", row->rt_priority);
    if (append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf) < 0) return -1;
    lulo_sched_format_io(io_buf, sizeof(io_buf), row->io_class, row->io_priority);
    snprintf(buf, sizeof(buf), "io: %s (%s)", io_buf, lulo_sched_io_class_name(row->io_class));
    return append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf);
}

void lulo_sched_state_init(LuloSchedState *state)
{
    memset(state, 0, sizeof(*state));
    state->view = LULO_SCHED_VIEW_PROFILES;
    state->profile_cursor = -1;
    state->profile_selected = -1;
    state->rule_cursor = -1;
    state->rule_selected = -1;
    state->live_cursor = -1;
    state->live_selected = -1;
    state->selected_live_pid = -1;
}

void lulo_sched_state_cleanup(LuloSchedState *state)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
}

int lulo_sched_snapshot_clone(LuloSchedSnapshot *dst, const LuloSchedSnapshot *src)
{
    if (!dst || !src) return -1;
    memset(dst, 0, sizeof(*dst));
    snprintf(dst->config_root, sizeof(dst->config_root), "%s", src->config_root);
    dst->watcher_interval_ms = src->watcher_interval_ms;
    dst->scan_generation = src->scan_generation;
    dst->focus_enabled = src->focus_enabled;
    dst->focused_pid = src->focused_pid;
    dst->focused_start_time = src->focused_start_time;
    snprintf(dst->focus_provider, sizeof(dst->focus_provider), "%s", src->focus_provider);
    snprintf(dst->focus_profile, sizeof(dst->focus_profile), "%s", src->focus_profile);
    dst->background_enabled = src->background_enabled;
    snprintf(dst->background_profile, sizeof(dst->background_profile), "%s", src->background_profile);
    dst->background_match_app_slice = src->background_match_app_slice;
    dst->background_match_background_slice = src->background_match_background_slice;
    dst->background_match_app_unit_prefix = src->background_match_app_unit_prefix;
    snprintf(dst->focused_comm, sizeof(dst->focused_comm), "%s", src->focused_comm);
    snprintf(dst->focused_exe, sizeof(dst->focused_exe), "%s", src->focused_exe);
    snprintf(dst->focused_unit, sizeof(dst->focused_unit), "%s", src->focused_unit);
    snprintf(dst->focused_slice, sizeof(dst->focused_slice), "%s", src->focused_slice);
    snprintf(dst->focused_cgroup, sizeof(dst->focused_cgroup), "%s", src->focused_cgroup);
    snprintf(dst->detail_title, sizeof(dst->detail_title), "%s", src->detail_title);
    snprintf(dst->detail_status, sizeof(dst->detail_status), "%s", src->detail_status);
    if (src->profile_count > 0) {
        dst->profiles = malloc((size_t)src->profile_count * sizeof(*dst->profiles));
        if (!dst->profiles) goto fail;
        memcpy(dst->profiles, src->profiles, (size_t)src->profile_count * sizeof(*dst->profiles));
        dst->profile_count = src->profile_count;
    }
    if (src->rule_count > 0) {
        dst->rules = malloc((size_t)src->rule_count * sizeof(*dst->rules));
        if (!dst->rules) goto fail;
        memcpy(dst->rules, src->rules, (size_t)src->rule_count * sizeof(*dst->rules));
        dst->rule_count = src->rule_count;
    }
    if (src->live_count > 0) {
        dst->live = malloc((size_t)src->live_count * sizeof(*dst->live));
        if (!dst->live) goto fail;
        memcpy(dst->live, src->live, (size_t)src->live_count * sizeof(*dst->live));
        dst->live_count = src->live_count;
    }
    for (int i = 0; i < src->detail_line_count; i++) {
        if (append_detail_line(&dst->detail_lines, &dst->detail_line_count, src->detail_lines[i]) < 0) goto fail;
    }
    return 0;

fail:
    lulo_sched_snapshot_free(dst);
    return -1;
}

void lulo_sched_snapshot_free(LuloSchedSnapshot *snap)
{
    if (!snap) return;
    free(snap->profiles);
    free(snap->rules);
    free(snap->live);
    clear_detail_lines(&snap->detail_lines, &snap->detail_line_count);
    memset(snap, 0, sizeof(*snap));
}

void lulo_sched_snapshot_mark_loading(LuloSchedSnapshot *snap, const LuloSchedState *state)
{
    if (!snap || !state) return;
    clear_detail_view(snap);
    switch (state->view) {
    case LULO_SCHED_VIEW_RULES:
        snprintf(snap->detail_title, sizeof(snap->detail_title), "rule");
        break;
    case LULO_SCHED_VIEW_LIVE:
        snprintf(snap->detail_title, sizeof(snap->detail_title), "assignment");
        break;
    case LULO_SCHED_VIEW_PROFILES:
    default:
        snprintf(snap->detail_title, sizeof(snap->detail_title), "profile");
        break;
    }
    snprintf(snap->detail_status, sizeof(snap->detail_status), "loading...");
    append_detail_line(&snap->detail_lines, &snap->detail_line_count, "loading...");
    if (snap->focus_enabled && snap->focus_profile[0]) {
        char buf[256];

        snprintf(buf, sizeof(buf), "focus profile: %s", snap->focus_profile);
        append_detail_line(&snap->detail_lines, &snap->detail_line_count, buf);
    }
}

int lulo_sched_snapshot_refresh_active(LuloSchedSnapshot *snap, const LuloSchedState *state)
{
    if (!snap || !state) return -1;
    clear_detail_view(snap);
    switch (state->view) {
    case LULO_SCHED_VIEW_RULES:
        if (state->rule_selected >= 0 && state->rule_selected < snap->rule_count) {
            return format_rule_detail(snap, &snap->rules[state->rule_selected]);
        }
        break;
    case LULO_SCHED_VIEW_LIVE:
        if (state->live_selected >= 0 && state->live_selected < snap->live_count) {
            return format_live_detail(snap, &snap->live[state->live_selected]);
        }
        break;
    case LULO_SCHED_VIEW_PROFILES:
    default:
        if (state->profile_selected >= 0 && state->profile_selected < snap->profile_count) {
            return format_profile_detail(snap, &snap->profiles[state->profile_selected]);
        }
        break;
    }
    return 0;
}

void lulo_sched_view_sync(LuloSchedState *state, const LuloSchedSnapshot *snap,
                          int list_rows, int detail_rows)
{
    int *cursor;
    int *scroll;
    int *detail_scroll;
    int count;
    int preview_rows;

    if (!state || !snap) return;
    sync_profile_selection(state, snap);
    sync_rule_selection(state, snap);
    sync_live_selection(state, snap);
    sync_active_cursor(state, snap);
    cursor = active_cursor(state);
    scroll = active_list_scroll(state);
    detail_scroll = active_detail_scroll(state);
    count = active_list_count(state, snap);
    preview_rows = detail_rows > 0 ? detail_rows : 1;
    if (cursor && scroll) adjust_scroll(scroll, *cursor, list_rows, count);
    if (detail_scroll) {
        int max_scroll = snap->detail_line_count > preview_rows ? snap->detail_line_count - preview_rows : 0;
        *detail_scroll = clamp_int_local(*detail_scroll, 0, max_scroll);
    }
}

void lulo_sched_view_move(LuloSchedState *state, const LuloSchedSnapshot *snap,
                          int list_rows, int detail_rows, int delta)
{
    int count;
    int *cursor;
    int *scroll;

    (void)detail_rows;
    if (!state || !snap) return;
    if (state->focus_preview) {
        int *detail_scroll = active_detail_scroll(state);
        int max_scroll = snap->detail_line_count > detail_rows ? snap->detail_line_count - detail_rows : 0;
        if (detail_scroll) *detail_scroll = clamp_int_local(*detail_scroll + delta, 0, max_scroll);
        return;
    }
    count = active_list_count(state, snap);
    cursor = active_cursor(state);
    scroll = active_list_scroll(state);
    if (!cursor || !scroll || count <= 0) return;
    if (*cursor < 0) *cursor = 0;
    *cursor = clamp_int_local(*cursor + delta, 0, count - 1);
    adjust_scroll(scroll, *cursor, list_rows, count);
}

void lulo_sched_view_page(LuloSchedState *state, const LuloSchedSnapshot *snap,
                          int list_rows, int detail_rows, int pages)
{
    if (!state || !snap) return;
    if (state->focus_preview) {
        lulo_sched_view_move(state, snap, list_rows, detail_rows, pages * detail_rows);
    } else {
        lulo_sched_view_move(state, snap, list_rows, detail_rows, pages * list_rows);
    }
}

void lulo_sched_view_home(LuloSchedState *state, const LuloSchedSnapshot *snap,
                          int list_rows, int detail_rows)
{
    int *cursor;
    int *scroll;
    int *detail_scroll;

    (void)snap;
    (void)detail_rows;
    if (!state) return;
    if (state->focus_preview) {
        detail_scroll = active_detail_scroll(state);
        if (detail_scroll) *detail_scroll = 0;
        return;
    }
    cursor = active_cursor(state);
    scroll = active_list_scroll(state);
    if (cursor) *cursor = 0;
    if (scroll) *scroll = 0;
    (void)list_rows;
}

void lulo_sched_view_end(LuloSchedState *state, const LuloSchedSnapshot *snap,
                         int list_rows, int detail_rows)
{
    int count;
    int *cursor;
    int *scroll;
    int *detail_scroll;
    int max_scroll;

    if (!state || !snap) return;
    if (state->focus_preview) {
        detail_scroll = active_detail_scroll(state);
        if (detail_scroll) {
            max_scroll = snap->detail_line_count > detail_rows ? snap->detail_line_count - detail_rows : 0;
            *detail_scroll = max_scroll;
        }
        return;
    }
    count = active_list_count(state, snap);
    cursor = active_cursor(state);
    scroll = active_list_scroll(state);
    if (!cursor || !scroll || count <= 0) return;
    *cursor = count - 1;
    max_scroll = count > list_rows ? count - list_rows : 0;
    *scroll = max_scroll;
}

void lulo_sched_set_cursor(LuloSchedState *state, const LuloSchedSnapshot *snap,
                           int list_rows, int detail_rows, int row_index)
{
    int count;
    int *cursor;
    int *scroll;

    (void)detail_rows;
    if (!state || !snap) return;
    if (state->focus_preview) return;
    count = active_list_count(state, snap);
    cursor = active_cursor(state);
    scroll = active_list_scroll(state);
    if (!cursor || !scroll || count <= 0) return;
    *cursor = clamp_int_local(row_index, 0, count - 1);
    adjust_scroll(scroll, *cursor, list_rows, count);
}

int lulo_sched_open_current(LuloSchedState *state, const LuloSchedSnapshot *snap,
                            int list_rows, int detail_rows)
{
    int *cursor;
    int *selected;
    int current;

    (void)list_rows;
    (void)detail_rows;
    if (!state || !snap) return 0;
    cursor = active_cursor(state);
    selected = active_selected(state);
    if (!cursor || !selected || *cursor < 0) return 0;
    current = *cursor;
    switch (state->view) {
    case LULO_SCHED_VIEW_RULES:
        if (current == state->rule_selected) {
            state->rule_selected = -1;
            state->selected_rule[0] = '\0';
            state->focus_preview = 0;
        } else if (current < snap->rule_count) {
            state->rule_selected = current;
            snprintf(state->selected_rule, sizeof(state->selected_rule), "%s", snap->rules[current].name);
        }
        break;
    case LULO_SCHED_VIEW_LIVE:
        if (current == state->live_selected) {
            state->live_selected = -1;
            state->selected_live_pid = -1;
            state->selected_live_start_time = 0;
            state->focus_preview = 0;
        } else if (current < snap->live_count) {
            state->live_selected = current;
            state->selected_live_pid = snap->live[current].pid;
            state->selected_live_start_time = snap->live[current].start_time;
        }
        break;
    case LULO_SCHED_VIEW_PROFILES:
    default:
        if (current == state->profile_selected) {
            state->profile_selected = -1;
            state->selected_profile[0] = '\0';
            state->focus_preview = 0;
        } else if (current < snap->profile_count) {
            state->profile_selected = current;
            snprintf(state->selected_profile, sizeof(state->selected_profile), "%s", snap->profiles[current].name);
        }
        break;
    }
    return 1;
}

void lulo_sched_toggle_focus(LuloSchedState *state, const LuloSchedSnapshot *snap,
                             int list_rows, int detail_rows)
{
    (void)snap;
    (void)list_rows;
    (void)detail_rows;
    if (!state) return;
    state->focus_preview = !state->focus_preview;
}

void lulo_sched_next_view(LuloSchedState *state)
{
    if (!state) return;
    state->view = (LuloSchedView)((state->view + 1) % LULO_SCHED_VIEW_COUNT);
    state->focus_preview = 0;
}

void lulo_sched_prev_view(LuloSchedState *state)
{
    if (!state) return;
    state->view = (LuloSchedView)((state->view + LULO_SCHED_VIEW_COUNT - 1) % LULO_SCHED_VIEW_COUNT);
    state->focus_preview = 0;
}

const char *lulo_sched_view_name(LuloSchedView view)
{
    switch (view) {
    case LULO_SCHED_VIEW_RULES:
        return "Rules";
    case LULO_SCHED_VIEW_LIVE:
        return "Live";
    case LULO_SCHED_VIEW_PROFILES:
    default:
        return "Profiles";
    }
}

const char *lulo_sched_match_kind_name(LuloSchedMatchKind kind)
{
    switch (kind) {
    case LULO_SCHED_MATCH_DYNAMIC:
        return "dyn";
    case LULO_SCHED_MATCH_EXE:
        return "exe";
    case LULO_SCHED_MATCH_CMDLINE:
        return "cmd";
    case LULO_SCHED_MATCH_UNIT:
        return "unit";
    case LULO_SCHED_MATCH_SLICE:
        return "slice";
    case LULO_SCHED_MATCH_CGROUP:
        return "cgroup";
    case LULO_SCHED_MATCH_COMM:
    default:
        return "comm";
    }
}
