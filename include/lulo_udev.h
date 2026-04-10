#ifndef LULO_UDEV_H
#define LULO_UDEV_H

typedef enum {
    LULO_UDEV_VIEW_RULES = 0,
    LULO_UDEV_VIEW_HWDB,
    LULO_UDEV_VIEW_DEVICES,
    LULO_UDEV_VIEW_COUNT
} LuloUdevView;

typedef struct {
    char path[320];
    char name[160];
    char source[16];
} LuloUdevConfigRow;

typedef struct {
    char path[320];
    char name[128];
    char subsystem[64];
    char devnode[160];
    char devpath[320];
} LuloUdevDeviceRow;

typedef struct {
    LuloUdevConfigRow *rules;
    int rule_count;
    LuloUdevConfigRow *hwdb;
    int hwdb_count;
    LuloUdevDeviceRow *devices;
    int device_count;
    char **detail_lines;
    int detail_line_count;
    char detail_title[320];
    char detail_status[160];
} LuloUdevSnapshot;

typedef struct {
    LuloUdevView view;
    int focus_preview;
    int rule_cursor;
    int rule_selected;
    int rule_list_scroll;
    int rule_detail_scroll;
    int hwdb_cursor;
    int hwdb_selected;
    int hwdb_list_scroll;
    int hwdb_detail_scroll;
    int device_cursor;
    int device_selected;
    int device_list_scroll;
    int device_detail_scroll;
    char selected_rule[320];
    char selected_hwdb[320];
    char selected_device[320];
} LuloUdevState;

void lulo_udev_state_init(LuloUdevState *state);
void lulo_udev_state_cleanup(LuloUdevState *state);

int lulo_udev_snapshot_clone(LuloUdevSnapshot *dst, const LuloUdevSnapshot *src);
void lulo_udev_snapshot_mark_loading(LuloUdevSnapshot *snap, const LuloUdevState *state);
void lulo_udev_snapshot_free(LuloUdevSnapshot *snap);

void lulo_udev_view_sync(LuloUdevState *state, const LuloUdevSnapshot *snap,
                         int list_rows, int detail_rows);
void lulo_udev_view_move(LuloUdevState *state, const LuloUdevSnapshot *snap,
                         int list_rows, int detail_rows, int delta);
void lulo_udev_view_page(LuloUdevState *state, const LuloUdevSnapshot *snap,
                         int list_rows, int detail_rows, int pages);
void lulo_udev_view_home(LuloUdevState *state, const LuloUdevSnapshot *snap,
                         int list_rows, int detail_rows);
void lulo_udev_view_end(LuloUdevState *state, const LuloUdevSnapshot *snap,
                        int list_rows, int detail_rows);
void lulo_udev_set_cursor(LuloUdevState *state, const LuloUdevSnapshot *snap,
                          int list_rows, int detail_rows, int row_index);
int lulo_udev_open_current(LuloUdevState *state, const LuloUdevSnapshot *snap,
                           int list_rows, int detail_rows);
void lulo_udev_toggle_focus(LuloUdevState *state, const LuloUdevSnapshot *snap,
                            int list_rows, int detail_rows);
void lulo_udev_next_view(LuloUdevState *state);
void lulo_udev_prev_view(LuloUdevState *state);
const char *lulo_udev_view_name(LuloUdevView view);

#endif
