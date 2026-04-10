#ifndef LULO_TUNE_H
#define LULO_TUNE_H

typedef enum {
    LULO_TUNE_SOURCE_PROC = 0,
    LULO_TUNE_SOURCE_SYS,
    LULO_TUNE_SOURCE_CGROUP,
} LuloTuneSource;

typedef struct {
    char path[320];
    char name[96];
    char group[96];
    char value[192];
    int writable;
    int is_dir;
    LuloTuneSource source;
} LuloTuneRow;

typedef struct {
    char id[96];
    char name[96];
    char created[32];
    int item_count;
} LuloTuneBundleMeta;

typedef enum {
    LULO_TUNE_VIEW_EXPLORE = 0,
    LULO_TUNE_VIEW_SNAPSHOTS,
    LULO_TUNE_VIEW_PRESETS,
    LULO_TUNE_VIEW_COUNT
} LuloTuneView;

typedef struct {
    LuloTuneRow *rows;
    int count;
    LuloTuneBundleMeta *snapshots;
    int snapshot_count;
    LuloTuneBundleMeta *presets;
    int preset_count;
    char **detail_lines;
    int detail_line_count;
    char detail_title[320];
    char detail_status[160];
} LuloTuneSnapshot;

typedef struct {
    LuloTuneView view;
    int cursor;
    int selected;
    int list_scroll;
    int detail_scroll;
    int focus_preview;
    int snapshot_cursor;
    int snapshot_selected;
    int snapshot_list_scroll;
    int snapshot_detail_scroll;
    int preset_cursor;
    int preset_selected;
    int preset_list_scroll;
    int preset_detail_scroll;
    char browse_path[320];
    char selected_path[320];
    char selected_snapshot_id[96];
    char selected_preset_id[96];
    char staged_path[320];
    char staged_value[192];
} LuloTuneState;

void lulo_tune_state_init(LuloTuneState *state);
void lulo_tune_state_cleanup(LuloTuneState *state);

int lulo_tune_snapshot_clone(LuloTuneSnapshot *dst, const LuloTuneSnapshot *src);
void lulo_tune_snapshot_mark_loading(LuloTuneSnapshot *snap, const LuloTuneState *state);
void lulo_tune_snapshot_free(LuloTuneSnapshot *snap);

void lulo_tune_view_sync(LuloTuneState *state, const LuloTuneSnapshot *snap,
                         int list_rows, int detail_rows);
void lulo_tune_view_move(LuloTuneState *state, const LuloTuneSnapshot *snap,
                         int list_rows, int detail_rows, int delta);
void lulo_tune_view_page(LuloTuneState *state, const LuloTuneSnapshot *snap,
                         int list_rows, int detail_rows, int pages);
void lulo_tune_view_home(LuloTuneState *state, const LuloTuneSnapshot *snap,
                         int list_rows, int detail_rows);
void lulo_tune_view_end(LuloTuneState *state, const LuloTuneSnapshot *snap,
                        int list_rows, int detail_rows);
void lulo_tune_set_cursor(LuloTuneState *state, const LuloTuneSnapshot *snap,
                          int list_rows, int detail_rows, int row_index);
int lulo_tune_open_current(LuloTuneState *state, const LuloTuneSnapshot *snap,
                           int list_rows, int detail_rows);
void lulo_tune_toggle_focus(LuloTuneState *state, const LuloTuneSnapshot *snap,
                            int list_rows, int detail_rows);
void lulo_tune_next_view(LuloTuneState *state);
void lulo_tune_prev_view(LuloTuneState *state);
const char *lulo_tune_view_name(LuloTuneView view);
const char *lulo_tune_source_name(LuloTuneSource source);

#endif
