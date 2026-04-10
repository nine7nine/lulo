#ifndef LULO_CGROUPS_H
#define LULO_CGROUPS_H

typedef enum {
    LULO_CGROUPS_VIEW_TREE = 0,
    LULO_CGROUPS_VIEW_FILES,
    LULO_CGROUPS_VIEW_CONFIG,
    LULO_CGROUPS_VIEW_COUNT
} LuloCgroupsView;

typedef struct {
    char path[320];
    char name[128];
    char type[32];
    char controllers[192];
    int process_count;
    int thread_count;
    int subgroup_count;
    int is_parent;
} LuloCgroupTreeRow;

typedef struct {
    char path[320];
    char name[96];
    char value[192];
    int writable;
} LuloCgroupFileRow;

typedef struct {
    char path[320];
    char name[160];
    char source[16];
    char kind[24];
} LuloCgroupConfigRow;

typedef struct {
    LuloCgroupTreeRow *tree_rows;
    int tree_count;
    LuloCgroupFileRow *file_rows;
    int file_count;
    LuloCgroupConfigRow *configs;
    int config_count;
    int configs_loaded;
    char browse_path[320];
    char **detail_lines;
    int detail_line_count;
    char detail_title[320];
    char detail_status[160];
} LuloCgroupsSnapshot;

typedef struct {
    LuloCgroupsView view;
    int focus_preview;
    int tree_cursor;
    int tree_selected;
    int tree_list_scroll;
    int tree_detail_scroll;
    int file_cursor;
    int file_selected;
    int file_list_scroll;
    int file_detail_scroll;
    int config_cursor;
    int config_selected;
    int config_list_scroll;
    int config_detail_scroll;
    char browse_path[320];
    char selected_tree_path[320];
    char selected_file_path[320];
    char selected_config[320];
} LuloCgroupsState;

void lulo_cgroups_state_init(LuloCgroupsState *state);
void lulo_cgroups_state_cleanup(LuloCgroupsState *state);

int lulo_cgroups_snapshot_clone(LuloCgroupsSnapshot *dst, const LuloCgroupsSnapshot *src);
void lulo_cgroups_snapshot_mark_loading(LuloCgroupsSnapshot *snap, const LuloCgroupsState *state);
void lulo_cgroups_snapshot_free(LuloCgroupsSnapshot *snap);

void lulo_cgroups_view_sync(LuloCgroupsState *state, const LuloCgroupsSnapshot *snap,
                            int list_rows, int detail_rows);
void lulo_cgroups_view_move(LuloCgroupsState *state, const LuloCgroupsSnapshot *snap,
                            int list_rows, int detail_rows, int delta);
void lulo_cgroups_view_page(LuloCgroupsState *state, const LuloCgroupsSnapshot *snap,
                            int list_rows, int detail_rows, int pages);
void lulo_cgroups_view_home(LuloCgroupsState *state, const LuloCgroupsSnapshot *snap,
                            int list_rows, int detail_rows);
void lulo_cgroups_view_end(LuloCgroupsState *state, const LuloCgroupsSnapshot *snap,
                           int list_rows, int detail_rows);
void lulo_cgroups_set_cursor(LuloCgroupsState *state, const LuloCgroupsSnapshot *snap,
                             int list_rows, int detail_rows, int row_index);
int lulo_cgroups_open_current(LuloCgroupsState *state, const LuloCgroupsSnapshot *snap,
                              int list_rows, int detail_rows);
void lulo_cgroups_toggle_focus(LuloCgroupsState *state, const LuloCgroupsSnapshot *snap,
                               int list_rows, int detail_rows);
void lulo_cgroups_next_view(LuloCgroupsState *state);
void lulo_cgroups_prev_view(LuloCgroupsState *state);
const char *lulo_cgroups_view_name(LuloCgroupsView view);

#endif
