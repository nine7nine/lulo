#ifndef LULO_SYSTEMD_H
#define LULO_SYSTEMD_H

typedef struct {
    int user_scope;
    char raw_unit[256];
    char unit[256];
    char object_path[320];
    char fragment_path[320];
    char load[24];
    char active[24];
    char sub[24];
    char file_state[24];
    char preset[24];
    char description[192];
} LuloSystemdServiceRow;

typedef struct {
    char path[320];
    char name[96];
} LuloSystemdConfigRow;

typedef enum {
    LULO_SYSTEMD_VIEW_SERVICES = 0,
    LULO_SYSTEMD_VIEW_DEPS,
    LULO_SYSTEMD_VIEW_CONFIG,
    LULO_SYSTEMD_VIEW_COUNT
} LuloSystemdView;

typedef struct {
    LuloSystemdServiceRow *rows;
    int count;
    int configs_loaded;
    char **file_lines;
    int file_line_count;
    char file_title[320];
    char file_status[160];
    char **dep_lines;
    int dep_line_count;
    char dep_title[320];
    char dep_status[160];
    LuloSystemdConfigRow *configs;
    int config_count;
    char **config_lines;
    int config_line_count;
    char config_title[320];
    char config_status[160];
} LuloSystemdSnapshot;

typedef struct {
    LuloSystemdView view;
    int cursor;
    int selected;
    int list_scroll;
    int file_scroll;
    int focus_preview;
    int config_cursor;
    int config_selected;
    int config_list_scroll;
    int config_file_scroll;
    int selected_user_scope;
    char selected_unit[256];
    char selected_config[320];
} LuloSystemdState;

void lulo_systemd_state_init(LuloSystemdState *state);
void lulo_systemd_state_cleanup(LuloSystemdState *state);

int lulo_systemd_snapshot_clone(LuloSystemdSnapshot *dst, const LuloSystemdSnapshot *src);
void lulo_systemd_snapshot_mark_loading(LuloSystemdSnapshot *snap, const LuloSystemdState *state);
void lulo_systemd_snapshot_free(LuloSystemdSnapshot *snap);

void lulo_systemd_view_sync(LuloSystemdState *state, const LuloSystemdSnapshot *snap,
                            int list_rows, int file_rows);
void lulo_systemd_view_move(LuloSystemdState *state, const LuloSystemdSnapshot *snap,
                            int list_rows, int file_rows, int delta);
void lulo_systemd_view_page(LuloSystemdState *state, const LuloSystemdSnapshot *snap,
                            int list_rows, int file_rows, int pages);
void lulo_systemd_view_home(LuloSystemdState *state, const LuloSystemdSnapshot *snap,
                            int list_rows, int file_rows);
void lulo_systemd_view_end(LuloSystemdState *state, const LuloSystemdSnapshot *snap,
                           int list_rows, int file_rows);
void lulo_systemd_set_cursor(LuloSystemdState *state, const LuloSystemdSnapshot *snap,
                             int list_rows, int file_rows, int row_index);
int lulo_systemd_open_current(LuloSystemdState *state, const LuloSystemdSnapshot *snap,
                              int list_rows, int file_rows);
void lulo_systemd_toggle_focus(LuloSystemdState *state, const LuloSystemdSnapshot *snap,
                               int list_rows, int file_rows);
void lulo_systemd_next_view(LuloSystemdState *state);
void lulo_systemd_prev_view(LuloSystemdState *state);
const char *lulo_systemd_view_name(LuloSystemdView view);

#endif
