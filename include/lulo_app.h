#ifndef LULO_APP_H
#define LULO_APP_H

#include <notcurses/notcurses.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <termios.h>

#include "lulo_dizk.h"
#include "lulo_model.h"
#include "lulo_proc.h"
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
    struct ncplane *header;
    struct ncplane *tabs;
    struct ncplane *top;
    struct ncplane *cpu;
    struct ncplane *mem;
    struct ncplane *proc;
    struct ncplane *disk;
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
    int proc_refresh_ms;
    LuloProcCpuMode proc_cpu_mode;
    int last_scroll_action;
    long long last_scroll_ms;
    int scroll_streak;
    char status[160];
    long long status_until_ms;
    char tune_status[160];
    long long tune_status_until_ms;
    int tune_edit_active;
    char tune_edit_path[320];
    char tune_edit_value[192];
    int tune_edit_len;
    char tune_edit_prompt[320];
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
    int need_systemd;
    int need_tune;
    int need_rebuild;
    int need_proc_refresh;
    int need_disk_refresh;
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

typedef struct NcQueuedInput {
    uint32_t id;
    ncinput ni;
    struct NcQueuedInput *next;
} NcQueuedInput;

typedef struct {
    pthread_t tid;
    pthread_mutex_t lock;
    int pipefd[2];
    int running;
    int started;
    int notified;
    struct notcurses *nc;
    DebugLog *dlog;
    NcQueuedInput *head;
    NcQueuedInput **tail;
} NcInputThread;

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

int nc_input_start(NcInputThread *ctx, struct notcurses *nc, DebugLog *dlog);
void nc_input_begin_drain(NcInputThread *ctx);
void nc_input_stop(NcInputThread *ctx);
int nc_input_pop(NcInputThread *ctx, uint32_t *id, ncinput *ni);
InputAction decode_notcurses_input(uint32_t id);

int disk_visible_rows(const Ui *ui);
int systemd_list_rows_visible(const Ui *ui, const LuloSystemdState *state);
int systemd_preview_rows_visible(const Ui *ui, const LuloSystemdState *state);
int tune_list_rows_visible(const Ui *ui, const LuloTuneState *state);
int tune_preview_rows_visible(const Ui *ui, const LuloTuneState *state);

void render_disk_widget(Ui *ui, const LuloDizkSnapshot *snap, const LuloDizkState *state);
void render_disk_status(Ui *ui, const LuloDizkSnapshot *snap, const LuloDizkState *state);
void render_systemd_widget(Ui *ui, const LuloSystemdSnapshot *snap, const LuloSystemdState *state);
void render_systemd_status(Ui *ui, const LuloSystemdSnapshot *snap, const LuloSystemdState *state,
                           const LuloSystemdBackendStatus *backend_status);
void render_tune_widget(Ui *ui, const LuloTuneSnapshot *snap, const LuloTuneState *state);
void render_tune_status(Ui *ui, const LuloTuneSnapshot *snap, const LuloTuneState *state,
                        const LuloTuneBackendStatus *backend_status);
int handle_systemd_click(Ui *ui, int global_y, int global_x,
                         const LuloSystemdSnapshot *snap, LuloSystemdState *state,
                         RenderFlags *render);
int handle_tune_click(Ui *ui, int global_y, int global_x,
                      const LuloTuneSnapshot *snap, LuloTuneState *state,
                      RenderFlags *render);
void update_systemd_render_flags(RenderFlags *render, const LuloSystemdState *state,
                                 int prev_cursor, int prev_selected,
                                 int prev_list_scroll, int prev_file_scroll,
                                 int prev_config_cursor, int prev_config_selected,
                                 int prev_config_list_scroll, int prev_config_file_scroll,
                                 int prev_focus);
void update_tune_render_flags(RenderFlags *render, const LuloTuneState *state,
                              int prev_view, const char *prev_browse_path,
                              int prev_cursor, int prev_selected,
                              int prev_list_scroll, int prev_detail_scroll,
                              int prev_snapshot_cursor, int prev_snapshot_selected,
                              int prev_snapshot_list_scroll, int prev_snapshot_detail_scroll,
                              int prev_preset_cursor, int prev_preset_selected,
                              int prev_preset_list_scroll, int prev_preset_detail_scroll,
                              int prev_focus);

#endif
