#ifndef LULO_SCHED_H
#define LULO_SCHED_H

#include <stddef.h>

typedef enum {
    LULO_SCHED_VIEW_PROFILES = 0,
    LULO_SCHED_VIEW_RULES,
    LULO_SCHED_VIEW_LIVE,
    LULO_SCHED_VIEW_COUNT
} LuloSchedView;

typedef enum {
    LULO_SCHED_MATCH_COMM = 0,
    LULO_SCHED_MATCH_EXE,
    LULO_SCHED_MATCH_CMDLINE,
    LULO_SCHED_MATCH_UNIT,
    LULO_SCHED_MATCH_SLICE,
    LULO_SCHED_MATCH_CGROUP,
} LuloSchedMatchKind;

typedef struct {
    char name[64];
    char path[320];
    int enabled;
    int has_nice;
    int nice;
    int has_policy;
    int policy;
    int has_rt_priority;
    int rt_priority;
} LuloSchedProfileRow;

typedef struct {
    char name[64];
    char path[320];
    int enabled;
    int exclude;
    LuloSchedMatchKind match_kind;
    char pattern[192];
    char profile[64];
} LuloSchedRuleRow;

typedef struct {
    int pid;
    unsigned long long start_time;
    char comm[96];
    char exe[192];
    char unit[128];
    char slice[128];
    char cgroup[320];
    char profile[64];
    char rule[64];
    int policy;
    int rt_priority;
    int nice;
    int focused;
    char status[96];
} LuloSchedLiveRow;

typedef struct {
    LuloSchedProfileRow *profiles;
    int profile_count;
    LuloSchedRuleRow *rules;
    int rule_count;
    LuloSchedLiveRow *live;
    int live_count;
    char config_root[320];
    int watcher_interval_ms;
    int scan_generation;
    int focus_enabled;
    int focused_pid;
    unsigned long long focused_start_time;
    char focus_provider[32];
    char focus_profile[64];
    char background_profile[64];
    char focused_comm[96];
    char focused_exe[192];
    char focused_unit[128];
    char focused_slice[128];
    char focused_cgroup[320];
    char **detail_lines;
    int detail_line_count;
    char detail_title[320];
    char detail_status[160];
} LuloSchedSnapshot;

typedef struct {
    LuloSchedView view;
    int focus_preview;
    int profile_cursor;
    int profile_selected;
    int profile_list_scroll;
    int profile_detail_scroll;
    int rule_cursor;
    int rule_selected;
    int rule_list_scroll;
    int rule_detail_scroll;
    int live_cursor;
    int live_selected;
    int live_list_scroll;
    int live_detail_scroll;
    char selected_profile[64];
    char selected_rule[64];
    int selected_live_pid;
    unsigned long long selected_live_start_time;
} LuloSchedState;

void lulo_sched_state_init(LuloSchedState *state);
void lulo_sched_state_cleanup(LuloSchedState *state);

int lulo_sched_snapshot_clone(LuloSchedSnapshot *dst, const LuloSchedSnapshot *src);
void lulo_sched_snapshot_free(LuloSchedSnapshot *snap);
void lulo_sched_snapshot_mark_loading(LuloSchedSnapshot *snap, const LuloSchedState *state);
int lulo_sched_snapshot_refresh_active(LuloSchedSnapshot *snap, const LuloSchedState *state);

void lulo_sched_view_sync(LuloSchedState *state, const LuloSchedSnapshot *snap,
                          int list_rows, int detail_rows);
void lulo_sched_view_move(LuloSchedState *state, const LuloSchedSnapshot *snap,
                          int list_rows, int detail_rows, int delta);
void lulo_sched_view_page(LuloSchedState *state, const LuloSchedSnapshot *snap,
                          int list_rows, int detail_rows, int pages);
void lulo_sched_view_home(LuloSchedState *state, const LuloSchedSnapshot *snap,
                          int list_rows, int detail_rows);
void lulo_sched_view_end(LuloSchedState *state, const LuloSchedSnapshot *snap,
                         int list_rows, int detail_rows);
void lulo_sched_set_cursor(LuloSchedState *state, const LuloSchedSnapshot *snap,
                           int list_rows, int detail_rows, int row_index);
int lulo_sched_open_current(LuloSchedState *state, const LuloSchedSnapshot *snap,
                            int list_rows, int detail_rows);
void lulo_sched_toggle_focus(LuloSchedState *state, const LuloSchedSnapshot *snap,
                             int list_rows, int detail_rows);
void lulo_sched_next_view(LuloSchedState *state);
void lulo_sched_prev_view(LuloSchedState *state);

const char *lulo_sched_view_name(LuloSchedView view);
const char *lulo_sched_match_kind_name(LuloSchedMatchKind kind);

#endif
