#ifndef LULO_APP_H
#define LULO_APP_H

#include <notcurses/notcurses.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <termios.h>

#include "lulo_dizk.h"
#include "lulo_model.h"
#include "lulo_proc.h"
#include "lulo_cgroups.h"
#include "lulo_cgroups_backend.h"
#include "lulo_sched.h"
#include "lulo_sched_backend.h"
#include "lulo_systemd.h"
#include "lulo_systemd_backend.h"
#include "lulo_tune.h"
#include "lulo_tune_backend.h"

typedef struct {
    unsigned r;
    unsigned g;
    unsigned b;
} Rgb;

typedef struct {
    int mono;
    Rgb bg;
    Rgb fg;
    Rgb dim;
    Rgb cyan;
    Rgb white;
    Rgb blue;
    Rgb green;
    Rgb yellow;
    Rgb orange;
    Rgb red;
    Rgb border_header;
    Rgb border_frame;
    Rgb border_panel;
    Rgb branch;
    Rgb select_bg;
    Rgb select_fg;
    Rgb mem_free_bg;
    Rgb mem_fill[6];
    Rgb mem_text[6];
} Theme;

typedef enum {
    APP_PAGE_CPU = 0,
    APP_PAGE_DIZK,
    APP_PAGE_SCHED,
    APP_PAGE_CGROUPS,
    APP_PAGE_SYSTEMD,
    APP_PAGE_TUNE,
    APP_PAGE_COUNT
} AppPage;

typedef struct {
    int visible;
    int x;
    int width;
    LuloProcSortKey sort_key;
} ProcHeaderHit;

typedef struct {
    int inner_w;
    int show_user;
    int show_mem;
    int show_time;
    int pid_w;
    int user_w;
    int policy_w;
    int prio_w;
    int nice_w;
    int cpu_w;
    int mem_w;
    int time_w;
    int cmd_w;
    int body_x;
    int body_y;
    int body_rows;
    int controls_visible;
    int collapse_all_x;
    int collapse_all_w;
    int expand_all_x;
    int expand_all_w;
    ProcHeaderHit headers[LULO_PROC_SORT_COUNT];
} ProcTableLayout;

typedef struct {
    int x;
    int width;
} TabHit;

typedef struct {
    struct notcurses *nc;
    struct ncplane *std;
    struct ncplane *modal;
    struct ncplane *header;
    struct ncplane *tabs;
    struct ncplane *top;
    struct ncplane *cpu;
    struct ncplane *mem;
    struct ncplane *proc;
    struct ncplane *disk;
    struct ncplane *sched;
    struct ncplane *cgroups;
    struct ncplane *systemd;
    struct ncplane *tune;
    struct ncplane *load;
    struct ncplane *footer;
    TopLayout lo;
    int term_h;
    int term_w;
    const Theme *theme;
    ProcTableLayout proc_table;
    TabHit tab_hits[APP_PAGE_COUNT];
} Ui;

typedef struct {
    AppPage active_page;
    int help_visible;
    int proc_refresh_ms;
    LuloProcCpuMode proc_cpu_mode;
    int last_scroll_action;
    long long last_scroll_ms;
    int scroll_streak;
    char status[160];
    long long status_until_ms;
    char sched_status[160];
    long long sched_status_until_ms;
    char cgroups_status[160];
    long long cgroups_status_until_ms;
    char tune_status[160];
    long long tune_status_until_ms;
    int tune_edit_active;
    char tune_edit_path[320];
    char tune_edit_value[192];
    int tune_edit_len;
    char tune_edit_prompt[320];
    int tune_rename_active;
    char tune_rename_path[320];
    char tune_rename_value[192];
    int tune_rename_len;
} AppState;

typedef struct {
    int enabled;
    FILE *fp;
} DebugLog;

typedef struct {
    int active;
    struct termios old_tc;
    unsigned char buf[512];
    size_t len;
    long long first_ms;
} RawInput;

typedef enum {
    INPUT_NONE = 0,
    INPUT_TOGGLE_HELP,
    INPUT_QUIT,
    INPUT_SAMPLE_FASTER,
    INPUT_SAMPLE_SLOWER,
    INPUT_TOGGLE_PROC_CPU,
    INPUT_CYCLE_PROC_REFRESH,
    INPUT_TOGGLE_FOCUS,
    INPUT_SCROLL_UP,
    INPUT_SCROLL_DOWN,
    INPUT_PAGE_UP,
    INPUT_PAGE_DOWN,
    INPUT_HOME,
    INPUT_END,
    INPUT_TAB_NEXT,
    INPUT_TAB_PREV,
    INPUT_VIEW_NEXT,
    INPUT_VIEW_PREV,
    INPUT_TOGGLE_BRANCH,
    INPUT_COLLAPSE_ALL,
    INPUT_EXPAND_ALL,
    INPUT_SIGNAL_TERM,
    INPUT_SIGNAL_KILL,
    INPUT_SAVE_SNAPSHOT,
    INPUT_SAVE_PRESET,
    INPUT_EDIT_SELECTED,
    INPUT_APPLY_SELECTED,
    INPUT_RELOAD_PAGE,
    INPUT_NEW_ITEM,
    INPUT_DELETE_SELECTED,
    INPUT_RENAME_SELECTED,
    INPUT_RESIZE,
} InputAction;

typedef struct {
    InputAction action;
    int mouse_press;
    int mouse_release;
    int mouse_wheel;
    int key_repeat;
    int text_len;
    int backspace;
    int submit;
    int cancel;
    char text[16];
    int mouse_x;
    int mouse_y;
    int mouse_button;
} DecodedInput;

typedef enum {
    INPUT_BACKEND_AUTO = 0,
    INPUT_BACKEND_NOTCURSES,
    INPUT_BACKEND_RAW,
} InputBackend;

typedef struct {
    int need_render;
    int need_tabs;
    int need_footer;
    int need_load;
    int need_header;
    int need_cpu;
    int need_proc;
    int need_disk;
    int need_sched;
    int need_cgroups;
    int need_systemd;
    int need_tune;
    int need_rebuild;
    int need_proc_refresh;
    int need_disk_refresh;
    int need_sched_refresh;
    int need_sched_reload;
    int need_cgroups_refresh;
    int need_cgroups_refresh_full;
    int need_systemd_refresh;
    int need_tune_refresh;
    int need_tune_refresh_full;
    int need_tune_save_snapshot;
    int need_tune_save_preset;
    int need_tune_apply_selected;
    int need_proc_cursor_only;
    int need_proc_body_only;
    int proc_prev_selected;
    int proc_prev_scroll;
} RenderFlags;

const char *input_backend_name(InputBackend backend);
int parse_input_backend(const char *text, InputBackend *backend);
InputBackend auto_input_backend(void);

void debug_log_open(DebugLog *log);
void debug_log_close(DebugLog *log);
void debug_log_stage(DebugLog *log, const char *stage);
void debug_log_errno(DebugLog *log, const char *tag);
void debug_log_poll(DebugLog *log, const char *tag, int fd, int revents);
void debug_log_message(DebugLog *log, const char *tag, const char *value);
void debug_log_nc_event(DebugLog *log, const char *tag, uint32_t id, const ncinput *ni);
void debug_log_bytes(DebugLog *log, const char *tag, const unsigned char *buf, size_t len);
void debug_log_action(DebugLog *log, const char *tag, const DecodedInput *in);

void terminal_mouse_enable(void);
void terminal_mouse_disable(void);
int raw_input_enable(RawInput *in);
void raw_input_disable(RawInput *in);
ssize_t raw_input_fill(RawInput *in);
int raw_input_decode_one(RawInput *in, DecodedInput *out);
InputAction decode_notcurses_input(uint32_t id);

void plane_reset(struct ncplane *p, const Theme *theme);
void plane_putn(struct ncplane *p, int y, int x, Rgb fg, Rgb bg, const char *text, int width);
void plane_fill(struct ncplane *p, int y, int x, int width, Rgb fg, Rgb bg);
int clamp_int(int value, int lo, int hi);
int rect_valid(const LuloRect *rect);
int rect_inner_rows(const LuloRect *rect);
int rect_inner_cols(const LuloRect *rect);
void draw_inner_box(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                    Rgb border, const char *title, Rgb title_color);
void draw_inner_meta(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                     const char *text, Rgb fg);
Rgb disk_usage_fill(const Theme *theme, int pct);
Rgb disk_usage_text(const Theme *theme, int pct);
void render_inline_meter(struct ncplane *p, const Theme *theme, int y, int x, int width,
                         int pct, Rgb fill);
int disk_visible_rows(const Ui *ui);
void render_disk_widget(Ui *ui, const LuloDizkSnapshot *snap, const LuloDizkState *state);
void render_disk_status(Ui *ui, const LuloDizkSnapshot *snap, const LuloDizkState *state);
int sched_list_rows_visible(const Ui *ui, const LuloSchedState *state);
int sched_preview_rows_visible(const Ui *ui, const LuloSchedState *state);
const char *active_sched_edit_path(const LuloSchedSnapshot *snap, const LuloSchedState *state);
const char *active_sched_delete_path(const LuloSchedSnapshot *snap, const LuloSchedState *state);
int active_sched_is_builtin_rule(const LuloSchedSnapshot *snap, const LuloSchedState *state);
int sched_prepare_new_entry(const LuloSchedSnapshot *snap, const LuloSchedState *state,
                            char *path, size_t path_len, char **content_out,
                            char *err, size_t errlen);
void render_sched_widget(Ui *ui, const LuloSchedSnapshot *snap, const LuloSchedState *state);
void render_sched_status(Ui *ui, const LuloSchedSnapshot *snap, const LuloSchedState *state,
                         const LuloSchedBackendStatus *backend_status, AppState *app);
int point_on_sched_view_tabs(Ui *ui, const LuloSchedState *state, int global_y, int global_x);
int handle_sched_wheel_target(Ui *ui, LuloSchedState *state,
                              RenderFlags *render, int global_y, int global_x);
int handle_sched_click(Ui *ui, int global_y, int global_x,
                       const LuloSchedSnapshot *snap, LuloSchedState *state,
                       RenderFlags *render);
int cgroups_list_rows_visible(const Ui *ui, const LuloCgroupsState *state);
int cgroups_preview_rows_visible(const Ui *ui, const LuloCgroupsState *state);
const char *active_cgroups_edit_path(const LuloCgroupsSnapshot *snap, const LuloCgroupsState *state);
void render_cgroups_widget(Ui *ui, const LuloCgroupsSnapshot *snap, const LuloCgroupsState *state);
void render_cgroups_status(Ui *ui, const LuloCgroupsSnapshot *snap, const LuloCgroupsState *state,
                           const LuloCgroupsBackendStatus *backend_status, AppState *app);
int point_on_cgroups_view_tabs(Ui *ui, const LuloCgroupsState *state, int global_y, int global_x);
int handle_cgroups_click(Ui *ui, int global_y, int global_x,
                         const LuloCgroupsSnapshot *snap, LuloCgroupsState *state,
                         RenderFlags *render);
int handle_cgroups_wheel_target(Ui *ui, LuloCgroupsState *state,
                                RenderFlags *render, int global_y, int global_x);
int systemd_list_rows_visible(const Ui *ui, const LuloSystemdState *state);
int systemd_preview_rows_visible(const Ui *ui, const LuloSystemdState *state);
const char *active_systemd_edit_path(const LuloSystemdSnapshot *snap, const LuloSystemdState *state);
void render_systemd_widget(Ui *ui, const LuloSystemdSnapshot *snap, const LuloSystemdState *state);
void render_systemd_status(Ui *ui, const LuloSystemdSnapshot *snap, const LuloSystemdState *state,
                           const LuloSystemdBackendStatus *backend_status);
int point_on_systemd_view_tabs(Ui *ui, const LuloSystemdState *state, int global_y, int global_x);
int handle_systemd_click(Ui *ui, int global_y, int global_x,
                         const LuloSystemdSnapshot *snap, LuloSystemdState *state,
                         RenderFlags *render);
int handle_systemd_wheel_target(Ui *ui, LuloSystemdState *state,
                                RenderFlags *render, int global_y, int global_x);
void sched_status_set(AppState *app, const char *fmt, ...);
const char *sched_status_current(AppState *app);
void cgroups_status_set(AppState *app, const char *fmt, ...);
const char *cgroups_status_current(AppState *app);
void tune_status_set(AppState *app, const char *fmt, ...);
const char *tune_status_current(AppState *app);
void tune_edit_prompt_refresh(AppState *app);
void tune_edit_prompt_format(const AppState *app, char *buf, size_t len);
int tune_list_rows_visible(const Ui *ui, const LuloTuneState *state);
int tune_preview_rows_visible(const Ui *ui, const LuloTuneState *state);
const LuloTuneRow *active_tune_explore_row(const LuloTuneSnapshot *snap, const LuloTuneState *state);
int active_tune_bundle_edit_path(const LuloTuneSnapshot *snap, const LuloTuneState *state,
                                 char *path, size_t path_len);
int active_tune_bundle_delete_path(const LuloTuneSnapshot *snap, const LuloTuneState *state,
                                   char *path, size_t path_len);
int tune_prepare_new_bundle(const LuloTuneSnapshot *snap, const LuloTuneState *state,
                            char *path, size_t path_len, char *id_out, size_t id_len,
                            char **content_out, char *err, size_t errlen);
int start_tune_edit(AppState *app, const LuloTuneSnapshot *snap, const LuloTuneState *state);
int start_tune_bundle_rename(AppState *app, const LuloTuneSnapshot *snap, const LuloTuneState *state);
int active_tune_row_is_staged(const LuloTuneSnapshot *snap, const LuloTuneState *state);
int handle_tune_edit_input(AppState *app, const DecodedInput *in,
                           LuloTuneState *tune_state, RenderFlags *render);
int handle_tune_bundle_rename_input(AppState *app, const DecodedInput *in,
                                    RenderFlags *render);
void render_tune_widget(Ui *ui, const LuloTuneSnapshot *snap, const LuloTuneState *state,
                        const AppState *app);
void render_tune_status(Ui *ui, const LuloTuneSnapshot *snap, const LuloTuneState *state,
                        const LuloTuneBackendStatus *backend_status, AppState *app);
int point_on_tune_view_tabs(Ui *ui, const LuloTuneState *state, int global_y, int global_x);
int handle_tune_wheel_target(Ui *ui, LuloTuneState *state,
                             RenderFlags *render, int global_y, int global_x);
int handle_tune_click(Ui *ui, int global_y, int global_x,
                      const LuloTuneSnapshot *snap, LuloTuneState *state,
                      RenderFlags *render);

#endif
