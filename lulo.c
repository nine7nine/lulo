/* lulo.c — Notcurses frontend for lulo
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -o lulo lulo.c lulo_model.c lulo_proc.c \
 *     $(pkg-config --cflags --libs notcurses) -lm
 */
#define _GNU_SOURCE

#include <locale.h>
#include <notcurses/notcurses.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "lulo_model.h"
#include "lulo_dizk.h"
#include "lulo_proc.h"

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
    INPUT_TOGGLE_HEAT,
    INPUT_TOGGLE_PROC_CPU,
    INPUT_CYCLE_PROC_REFRESH,
    INPUT_SCROLL_UP,
    INPUT_SCROLL_DOWN,
    INPUT_PAGE_UP,
    INPUT_PAGE_DOWN,
    INPUT_HOME,
    INPUT_END,
    INPUT_TAB_NEXT,
    INPUT_TAB_PREV,
    INPUT_TOGGLE_BRANCH,
    INPUT_COLLAPSE_ALL,
    INPUT_EXPAND_ALL,
    INPUT_SIGNAL_TERM,
    INPUT_SIGNAL_KILL,
    INPUT_RESIZE,
} InputAction;

typedef struct {
    InputAction action;
    int mouse_press;
    int mouse_release;
    int mouse_wheel;
    int key_repeat;
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
    int need_rebuild;
    int need_proc_refresh;
    int need_disk_refresh;
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

static const Theme theme_color = {
    .mono = 0,
    .bg = { 29, 27, 33 },
    .fg = { 230, 230, 234 },
    .dim = { 168, 166, 178 },
    .cyan = { 88, 201, 255 },
    .white = { 244, 244, 246 },
    .blue = { 70, 120, 255 },
    .green = { 84, 226, 120 },
    .yellow = { 240, 214, 72 },
    .orange = { 232, 156, 58 },
    .red = { 238, 90, 72 },
    .border_header = { 84, 196, 255 },
    .border_frame = { 50, 102, 255 },
    .border_panel = { 82, 124, 248 },
    .branch = { 82, 108, 92 },
    .select_bg = { 72, 76, 86 },
    .select_fg = { 240, 240, 244 },
    .mem_free_bg = { 62, 62, 70 },
    .mem_fill = {
        { 40, 80, 160 },
        { 30, 120, 130 },
        { 50, 130, 70 },
        { 90, 60, 140 },
        { 150, 90, 30 },
        { 100, 90, 80 },
    },
    .mem_text = {
        { 80, 140, 255 },
        { 50, 190, 200 },
        { 80, 200, 110 },
        { 150, 100, 220 },
        { 220, 150, 50 },
        { 160, 145, 130 },
    },
};

static const Theme theme_mono = {
    .mono = 1,
    .bg = { 20, 20, 20 },
    .fg = { 236, 236, 236 },
    .dim = { 188, 188, 188 },
    .cyan = { 236, 236, 236 },
    .white = { 236, 236, 236 },
    .blue = { 208, 208, 208 },
    .green = { 208, 208, 208 },
    .yellow = { 224, 224, 224 },
    .orange = { 216, 216, 216 },
    .red = { 255, 255, 255 },
    .border_header = { 236, 236, 236 },
    .border_frame = { 208, 208, 208 },
    .border_panel = { 208, 208, 208 },
    .branch = { 132, 132, 132 },
    .select_bg = { 96, 96, 96 },
    .select_fg = { 236, 236, 236 },
    .mem_free_bg = { 68, 68, 68 },
    .mem_fill = {
        { 120, 120, 120 },
        { 140, 140, 140 },
        { 156, 156, 156 },
        { 168, 168, 168 },
        { 184, 184, 184 },
        { 196, 196, 196 },
    },
    .mem_text = {
        { 210, 210, 210 },
        { 214, 214, 214 },
        { 220, 220, 220 },
        { 224, 224, 224 },
        { 228, 228, 228 },
        { 232, 232, 232 },
    },
};

static Rgb rgb_interp(Rgb a, Rgb b, int t, int span)
{
    Rgb out;
    if (span <= 0) span = 1;
    out.r = a.r + (unsigned)((int)(b.r - a.r) * t / span);
    out.g = a.g + (unsigned)((int)(b.g - a.g) * t / span);
    out.b = a.b + (unsigned)((int)(b.b - a.b) * t / span);
    return out;
}

static uint64_t rgb_channels(Rgb fg, Rgb bg)
{
    uint64_t channels = 0;
    ncchannels_set_fg_rgb8(&channels, fg.r, fg.g, fg.b);
    ncchannels_set_bg_rgb8(&channels, bg.r, bg.g, bg.b);
    return channels;
}

static void plane_color(struct ncplane *p, Rgb fg, Rgb bg)
{
    ncplane_set_fg_rgb8(p, fg.r, fg.g, fg.b);
    ncplane_set_bg_rgb8(p, bg.r, bg.g, bg.b);
}

static void plane_reset(struct ncplane *p, const Theme *theme)
{
    ncplane_set_base(p, " ", 0, rgb_channels(theme->fg, theme->bg));
    ncplane_erase(p);
}

static void plane_putn(struct ncplane *p, int y, int x, Rgb fg, Rgb bg, const char *text, int width)
{
    char buf[1024];
    int n;

    if (!text || width <= 0) return;
    n = (int)strlen(text);
    if (n > width) n = width;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    memcpy(buf, text, (size_t)n);
    buf[n] = '\0';
    plane_color(p, fg, bg);
    ncplane_putstr_yx(p, y, x, buf);
}

static void plane_fill(struct ncplane *p, int y, int x, int width, Rgb fg, Rgb bg)
{
    char spaces[128];

    if (width <= 0) return;
    while (width > 0) {
        int chunk = width < (int)sizeof(spaces) - 1 ? width : (int)sizeof(spaces) - 1;
        memset(spaces, ' ', (size_t)chunk);
        spaces[chunk] = '\0';
        plane_color(p, fg, bg);
        ncplane_putstr_yx(p, y, x, spaces);
        x += chunk;
        width -= chunk;
    }
}

static void plane_clear_inner(struct ncplane *p, const Theme *theme, int rows, int cols)
{
    for (int y = 1; y < rows - 1; y++) {
        plane_fill(p, y, 1, cols - 2, theme->bg, theme->bg);
    }
}

static int plane_segment_fit(struct ncplane *p, int y, int x, int remaining, Rgb fg, Rgb bg, const char *text)
{
    int n;

    if (remaining <= 0 || !text || !*text) return 0;
    n = (int)strlen(text);
    if (n > remaining) n = remaining;
    plane_putn(p, y, x, fg, bg, text, n);
    return n;
}

static void draw_box_title(struct ncplane *p, const Theme *theme, Rgb border, const char *title, Rgb title_color)
{
    plane_reset(p, theme);
    ncplane_perimeter_rounded(p, 0, rgb_channels(border, theme->bg), 0);
    if (title && *title) plane_putn(p, 0, 2, title_color, theme->bg, title, (int)strlen(title));
}

static long long mono_ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int ms_until_deadline(long long deadline_ms)
{
    long long diff = deadline_ms - mono_ms_now();
    if (diff <= 0) return 0;
    if (diff > 2147483647LL) return 2147483647;
    return (int)diff;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int clamp_int(int value, int lo, int hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static int digits_int(int value)
{
    int digits = 1;

    while (value >= 10) {
        value /= 10;
        digits++;
    }
    return digits;
}

static int terminal_get_size(int *rows, int *cols)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0) return -1;
    if (ws.ws_row <= 0 || ws.ws_col <= 0) return -1;
    *rows = ws.ws_row;
    *cols = ws.ws_col;
    return 0;
}

static const char *input_backend_name(InputBackend backend)
{
    switch (backend) {
    case INPUT_BACKEND_NOTCURSES:
        return "notcurses";
    case INPUT_BACKEND_RAW:
        return "raw";
    default:
        return "auto";
    }
}

static int parse_input_backend(const char *text, InputBackend *backend)
{
    if (!text || !*text) return -1;
    if (!strcmp(text, "auto")) *backend = INPUT_BACKEND_AUTO;
    else if (!strcmp(text, "nc") || !strcmp(text, "notcurses")) *backend = INPUT_BACKEND_NOTCURSES;
    else if (!strcmp(text, "raw")) *backend = INPUT_BACKEND_RAW;
    else return -1;
    return 0;
}

static InputBackend auto_input_backend(void)
{
    const char *term = getenv("TERM");

    if (!term || !*term) return INPUT_BACKEND_RAW;
    if (!strcmp(term, "vte-256color")) return INPUT_BACKEND_NOTCURSES;
    if (!strcmp(term, "gnome")) return INPUT_BACKEND_NOTCURSES;
    return INPUT_BACKEND_RAW;
}

static const char *app_page_name(AppPage page)
{
    switch (page) {
    case APP_PAGE_DIZK:
        return "Dizk";
    case APP_PAGE_CPU:
    default:
        return "CPU";
    }
}

static const char *footer_hint(AppPage page)
{
    switch (page) {
    case APP_PAGE_DIZK:
        return "q / ESC exit   tab or <- -> switch   j/k or arrows scroll filesystems   PgUp/PgDn jump";
    case APP_PAGE_CPU:
    default:
        return "q / ESC exit   tab or <- -> switch   +/- sample   t heat   p proc cpu   r proc ms   space fold   c/e all   x/X term/kill";
    }
}

static int next_proc_refresh_ms(int current)
{
    static const int steps[] = { 1000, 2000, 3000, 5000 };
    size_t count = sizeof(steps) / sizeof(steps[0]);

    for (size_t i = 0; i < count; i++) {
        if (current < steps[i]) return steps[i];
        if (current == steps[i]) return steps[(i + 1) % count];
    }
    return steps[0];
}

static int effective_proc_refresh_ms(int sample_ms, int proc_refresh_ms)
{
    return proc_refresh_ms > sample_ms ? proc_refresh_ms : sample_ms;
}

static void app_status_set(AppState *app, const char *fmt, ...)
{
    va_list ap;

    if (!app) return;
    va_start(ap, fmt);
    vsnprintf(app->status, sizeof(app->status), fmt, ap);
    va_end(ap);
    app->status_until_ms = mono_ms_now() + 2500;
}

static const char *app_status_current(AppState *app)
{
    if (!app || !app->status[0]) return NULL;
    if (mono_ms_now() > app->status_until_ms) {
        app->status[0] = '\0';
        app->status_until_ms = 0;
        return NULL;
    }
    return app->status;
}

static void reset_scroll_repeat(AppState *app)
{
    if (!app) return;
    app->last_scroll_action = INPUT_NONE;
    app->last_scroll_ms = 0;
    app->scroll_streak = 0;
}

static int scroll_units_for_input(AppState *app, InputAction action, int wheel, int repeat)
{
    long long now;

    if (!app) return 1;
    if (action != INPUT_SCROLL_UP && action != INPUT_SCROLL_DOWN) {
        reset_scroll_repeat(app);
        return 1;
    }
    if (wheel) {
        reset_scroll_repeat(app);
        return 1;
    }
    now = mono_ms_now();
    if (app->last_scroll_action == (int)action &&
        (repeat || (app->last_scroll_ms > 0 && now - app->last_scroll_ms <= 120))) {
        app->scroll_streak++;
    } else {
        app->last_scroll_action = (int)action;
        app->scroll_streak = 1;
    }
    app->last_scroll_ms = now;
    if (app->scroll_streak >= 10) return 3;
    if (app->scroll_streak >= 4) return 2;
    return 1;
}

static void debug_log_open(DebugLog *log)
{
    const char *path = getenv("LULO_DEBUG_INPUT");
    memset(log, 0, sizeof(*log));
    if (!path || !*path) return;
    log->fp = fopen(path, "a");
    if (!log->fp) return;
    log->enabled = 1;
    fprintf(log->fp, "opened\n");
    fflush(log->fp);
}

static void debug_log_close(DebugLog *log)
{
    if (log->fp) fclose(log->fp);
    log->fp = NULL;
    log->enabled = 0;
}

static void debug_log_stage(DebugLog *log, const char *stage)
{
    if (!log->enabled || !log->fp) return;
    fprintf(log->fp, "stage %s\n", stage);
    fflush(log->fp);
}

static void debug_log_errno(DebugLog *log, const char *tag)
{
    if (!log->enabled || !log->fp) return;
    fprintf(log->fp, "%s errno=%d (%s)\n", tag, errno, strerror(errno));
    fflush(log->fp);
}

static void debug_log_poll(DebugLog *log, const char *tag, int fd, int revents)
{
    if (!log->enabled || !log->fp) return;
    fprintf(log->fp, "%s fd=%d revents=0x%x\n", tag, fd, revents);
    fflush(log->fp);
}

static void debug_log_message(DebugLog *log, const char *tag, const char *value)
{
    if (!log->enabled || !log->fp) return;
    fprintf(log->fp, "%s %s\n", tag, value ? value : "");
    fflush(log->fp);
}

static void debug_log_nc_event(DebugLog *log, const char *tag, uint32_t id, const ncinput *ni)
{
    if (!log->enabled || !log->fp) return;
    fprintf(log->fp,
            "%s id=%u evtype=%d y=%d x=%d mods=%u utf8='%s' eff0=%u eff1=%u\n",
            tag, id, ni ? (int)ni->evtype : -1, ni ? ni->y : -1, ni ? ni->x : -1,
            ni ? ni->modifiers : 0, ni ? ni->utf8 : "",
            ni ? ni->eff_text[0] : 0, ni ? ni->eff_text[1] : 0);
    fflush(log->fp);
}

static void nc_input_queue_free(NcInputThread *ctx)
{
    NcQueuedInput *q = ctx->head;
    while (q) {
        NcQueuedInput *next = q->next;
        free(q);
        q = next;
    }
    ctx->head = NULL;
    ctx->tail = &ctx->head;
}

static void *nc_input_thread_main(void *opaque)
{
    NcInputThread *ctx = opaque;

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
    while (ctx->running) {
        ncinput ni;
        uint32_t id;
        int saved_errno;
        NcQueuedInput *q;
        int notify = 0;

        errno = 0;
        memset(&ni, 0, sizeof(ni));
        id = notcurses_get_blocking(ctx->nc, &ni);
        saved_errno = errno;
        if (!ctx->running) break;
        if (id == 0) continue;
        if (id == (uint32_t)-1) {
            if (saved_errno == EINTR) continue;
            if (saved_errno) debug_log_errno(ctx->dlog, "nc-thread-get-error");
            else debug_log_message(ctx->dlog, "nc-thread-get-minus1", "errno0");
            sleep_ms(10);
            continue;
        }

        q = calloc(1, sizeof(*q));
        if (!q) continue;
        q->id = id;
        q->ni = ni;

        pthread_mutex_lock(&ctx->lock);
        if (!ctx->notified) {
            ctx->notified = 1;
            notify = 1;
        }
        *ctx->tail = q;
        ctx->tail = &q->next;
        pthread_mutex_unlock(&ctx->lock);

        if (notify && write(ctx->pipefd[1], "i", 1) < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            debug_log_errno(ctx->dlog, "nc-thread-pipe-write-error");
        }
    }
    return NULL;
}

static int nc_input_start(NcInputThread *ctx, struct notcurses *nc, DebugLog *dlog)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->pipefd[0] = -1;
    ctx->pipefd[1] = -1;
    ctx->nc = nc;
    ctx->dlog = dlog;
    ctx->tail = &ctx->head;
    if (pipe2(ctx->pipefd, O_CLOEXEC | O_NONBLOCK) < 0) return -1;
    if (pthread_mutex_init(&ctx->lock, NULL) != 0) {
        close(ctx->pipefd[0]);
        close(ctx->pipefd[1]);
        ctx->pipefd[0] = ctx->pipefd[1] = -1;
        return -1;
    }
    ctx->running = 1;
    if (pthread_create(&ctx->tid, NULL, nc_input_thread_main, ctx) != 0) {
        ctx->running = 0;
        pthread_mutex_destroy(&ctx->lock);
        close(ctx->pipefd[0]);
        close(ctx->pipefd[1]);
        ctx->pipefd[0] = ctx->pipefd[1] = -1;
        return -1;
    }
    ctx->started = 1;
    return 0;
}

static void nc_input_begin_drain(NcInputThread *ctx)
{
    char drain[64];

    while (read(ctx->pipefd[0], drain, sizeof(drain)) > 0) {}
    pthread_mutex_lock(&ctx->lock);
    ctx->notified = 0;
    pthread_mutex_unlock(&ctx->lock);
}

static void nc_input_stop(NcInputThread *ctx)
{
    if (!ctx->started) return;
    ctx->running = 0;
    pthread_cancel(ctx->tid);
    pthread_join(ctx->tid, NULL);
    close(ctx->pipefd[0]);
    close(ctx->pipefd[1]);
    ctx->pipefd[0] = ctx->pipefd[1] = -1;
    pthread_mutex_destroy(&ctx->lock);
    nc_input_queue_free(ctx);
    ctx->started = 0;
}

static int nc_input_pop(NcInputThread *ctx, uint32_t *id, ncinput *ni)
{
    NcQueuedInput *q;

    pthread_mutex_lock(&ctx->lock);
    q = ctx->head;
    if (q) {
        ctx->head = q->next;
        if (!ctx->head) ctx->tail = &ctx->head;
    }
    pthread_mutex_unlock(&ctx->lock);
    if (!q) return 0;
    if (id) *id = q->id;
    if (ni) *ni = q->ni;
    free(q);
    return 1;
}

static void debug_log_bytes(DebugLog *log, const char *tag, const unsigned char *buf, size_t len)
{
    if (!log->enabled || !log->fp) return;
    fprintf(log->fp, "%s len=%zu", tag, len);
    for (size_t i = 0; i < len; i++) fprintf(log->fp, " %02x", buf[i]);
    fputc('\n', log->fp);
    fflush(log->fp);
}

static void debug_log_action(DebugLog *log, const char *tag, const DecodedInput *in)
{
    if (!log->enabled || !log->fp || !in) return;
    fprintf(log->fp,
            "%s action=%d mouse_press=%d mouse_release=%d mouse_button=%d x=%d y=%d\n",
            tag, in->action, in->mouse_press, in->mouse_release,
            in->mouse_button, in->mouse_x, in->mouse_y);
    fflush(log->fp);
}

static void terminal_write_escape(const char *seq)
{
    if (!seq || !*seq) return;
    (void)!write(STDOUT_FILENO, seq, strlen(seq));
}

static void terminal_mouse_enable(void)
{
    terminal_write_escape("\x1b[?1000h\x1b[?1002h\x1b[?1006h");
}

static void terminal_mouse_disable(void)
{
    terminal_write_escape("\x1b[?1006l\x1b[?1002l\x1b[?1000l");
}

static int raw_input_enable(RawInput *in)
{
    struct termios raw;

    memset(in, 0, sizeof(*in));
    if (tcgetattr(STDIN_FILENO, &in->old_tc) < 0) return -1;
    raw = in->old_tc;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) return -1;
    in->active = 1;
    return 0;
}

static void raw_input_disable(RawInput *in)
{
    if (!in || !in->active) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &in->old_tc);
    in->active = 0;
    in->len = 0;
}

static ssize_t raw_input_fill(RawInput *in)
{
    ssize_t total = 0;

    if (!in || in->len >= sizeof(in->buf)) return 0;
    for (;;) {
        ssize_t n = read(STDIN_FILENO, in->buf + in->len, sizeof(in->buf) - in->len);
        if (n > 0) {
            if (in->len == 0) in->first_ms = mono_ms_now();
            in->len += (size_t)n;
            total += n;
            if (in->len >= sizeof(in->buf)) break;
            continue;
        }
        if (n == 0) break;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        return total > 0 ? total : -1;
    }
    return total;
}

static void raw_input_consume(RawInput *in, size_t count)
{
    if (!in || count == 0) return;
    if (count >= in->len) {
        in->len = 0;
        in->first_ms = 0;
        return;
    }
    memmove(in->buf, in->buf + count, in->len - count);
    in->len -= count;
}

static InputAction decode_key_byte(unsigned char ch)
{
    switch (ch) {
    case 'q':
    case 'Q':
    case '\r':
    case '\n':
    case 0x1b:
        return INPUT_QUIT;
    case '+':
    case '=':
        return INPUT_SAMPLE_FASTER;
    case '-':
    case '_':
        return INPUT_SAMPLE_SLOWER;
    case 't':
    case 'T':
        return INPUT_TOGGLE_HEAT;
    case 'p':
    case 'P':
        return INPUT_TOGGLE_PROC_CPU;
    case 'r':
    case 'R':
        return INPUT_CYCLE_PROC_REFRESH;
    case '\t':
    case ']':
        return INPUT_TAB_NEXT;
    case '[':
        return INPUT_TAB_PREV;
    case ' ':
        return INPUT_TOGGLE_BRANCH;
    case 'c':
        return INPUT_COLLAPSE_ALL;
    case 'e':
    case 'E':
        return INPUT_EXPAND_ALL;
    case 'x':
        return INPUT_SIGNAL_TERM;
    case 'X':
        return INPUT_SIGNAL_KILL;
    case 'k':
    case 'K':
        return INPUT_SCROLL_UP;
    case 'j':
    case 'J':
        return INPUT_SCROLL_DOWN;
    case 'u':
    case 'U':
        return INPUT_PAGE_UP;
    case 'd':
    case 'D':
        return INPUT_PAGE_DOWN;
    case 'g':
        return INPUT_HOME;
    case 'G':
        return INPUT_END;
    default:
        return INPUT_NONE;
    }
}

static int parse_decimal_field(const unsigned char *buf, size_t len, size_t *pos, int *value)
{
    int out = 0;
    int digits = 0;

    while (*pos < len && isdigit(buf[*pos])) {
        out = out * 10 + (buf[*pos] - '0');
        (*pos)++;
        digits = 1;
    }
    if (!digits) return 0;
    *value = out;
    return 1;
}

static int raw_input_decode_csi(RawInput *in, DecodedInput *out)
{
    size_t pos;
    int first = 0;

    if (in->len < 3) return 0;
    if (in->buf[2] == '<') {
        int cb = 0;
        int cx = 0;
        int cy = 0;
        char final;

        pos = 3;
        if (!parse_decimal_field(in->buf, in->len, &pos, &cb)) return 0;
        if (pos >= in->len || in->buf[pos++] != ';') return 0;
        if (!parse_decimal_field(in->buf, in->len, &pos, &cx)) return 0;
        if (pos >= in->len || in->buf[pos++] != ';') return 0;
        if (!parse_decimal_field(in->buf, in->len, &pos, &cy)) return 0;
        if (pos >= in->len) return 0;
        final = (char)in->buf[pos++];
        if (final != 'M' && final != 'm') {
            raw_input_consume(in, pos);
            return 1;
        }
        raw_input_consume(in, pos);
        out->mouse_x = cx;
        out->mouse_y = cy;
        out->mouse_button = (cb & 3) + 1;
        if (cb & 64) {
            out->mouse_wheel = 1;
            out->action = (cb & 1) ? INPUT_SCROLL_DOWN : INPUT_SCROLL_UP;
        } else if (final == 'M' && (cb & 3) == 0) {
            out->mouse_press = 1;
        } else if (final == 'm' && (cb & 3) == 0) {
            out->mouse_release = 1;
        }
        return 1;
    }

    if (in->buf[2] == 'A' || in->buf[2] == 'B' || in->buf[2] == 'C' ||
        in->buf[2] == 'D' || in->buf[2] == 'H' || in->buf[2] == 'F') {
        switch (in->buf[2]) {
        case 'A': out->action = INPUT_SCROLL_UP; break;
        case 'B': out->action = INPUT_SCROLL_DOWN; break;
        case 'C': out->action = INPUT_TAB_NEXT; break;
        case 'D': out->action = INPUT_TAB_PREV; break;
        case 'H': out->action = INPUT_HOME; break;
        case 'F': out->action = INPUT_END; break;
        }
        raw_input_consume(in, 3);
        return 1;
    }

    pos = 2;
    if (!parse_decimal_field(in->buf, in->len, &pos, &first)) return 0;
    while (pos < in->len && in->buf[pos] == ';') {
        pos++;
        while (pos < in->len && isdigit(in->buf[pos])) pos++;
    }
    if (pos >= in->len) return 0;
    if (in->buf[pos] == '~') {
        switch (first) {
        case 1:
        case 7:
            out->action = INPUT_HOME;
            break;
        case 4:
        case 8:
            out->action = INPUT_END;
            break;
        case 5:
            out->action = INPUT_PAGE_UP;
            break;
        case 6:
            out->action = INPUT_PAGE_DOWN;
            break;
        }
        raw_input_consume(in, pos + 1);
        return 1;
    }
    if (in->buf[pos] == 'A' || in->buf[pos] == 'B' || in->buf[pos] == 'C' ||
        in->buf[pos] == 'D' || in->buf[pos] == 'H' || in->buf[pos] == 'F') {
        switch (in->buf[pos]) {
        case 'A': out->action = INPUT_SCROLL_UP; break;
        case 'B': out->action = INPUT_SCROLL_DOWN; break;
        case 'C': out->action = INPUT_TAB_NEXT; break;
        case 'D': out->action = INPUT_TAB_PREV; break;
        case 'H': out->action = INPUT_HOME; break;
        case 'F': out->action = INPUT_END; break;
        }
        raw_input_consume(in, pos + 1);
        return 1;
    }

    raw_input_consume(in, pos + 1);
    return 1;
}

static int raw_input_decode_one(RawInput *in, DecodedInput *out)
{
    memset(out, 0, sizeof(*out));
    if (!in || in->len == 0) return 0;
    if (in->buf[0] != 0x1b) {
        out->action = decode_key_byte(in->buf[0]);
        raw_input_consume(in, 1);
        return 1;
    }
    if (in->len == 1) {
        if (mono_ms_now() - in->first_ms < 25) return 0;
        out->action = INPUT_QUIT;
        raw_input_consume(in, 1);
        return 1;
    }
    if (in->buf[1] == '[') return raw_input_decode_csi(in, out);
    if (in->buf[1] == 'O') {
        if (in->len < 3) return 0;
        switch (in->buf[2]) {
        case 'A': out->action = INPUT_SCROLL_UP; break;
        case 'B': out->action = INPUT_SCROLL_DOWN; break;
        case 'C': out->action = INPUT_TAB_NEXT; break;
        case 'D': out->action = INPUT_TAB_PREV; break;
        case 'H': out->action = INPUT_HOME; break;
        case 'F': out->action = INPUT_END; break;
        default: break;
        }
        raw_input_consume(in, 3);
        return 1;
    }
    out->action = INPUT_QUIT;
    raw_input_consume(in, 1);
    return 1;
}

static Rgb heat_mode_accent(const Theme *theme, HeatMode mode)
{
    return mode == HEAT_MODE_TASK ? theme->green : theme->cyan;
}

static Rgb heat_cell_color(const Theme *theme, HeatMode mode, int pct)
{
    typedef struct {
        int p;
        Rgb c;
    } Stop;
    static const Stop raw_stops[] = {
        {  1, { 25,  25, 100 } },
        {  8, {  0,  60, 190 } },
        { 15, {  0, 140, 210 } },
        { 25, {  0, 200, 150 } },
        { 35, { 40, 210,  40 } },
        { 50, {110, 170,   0 } },
        { 70, {160, 150,   0 } },
        { 85, {185, 110,   0 } },
        { 95, {200,  45,   0 } },
        {100, {210,  15,   0 } },
    };
    static const Stop task_stops[] = {
        {  1, { 20,  48,  30 } },
        {  8, { 28,  86,  42 } },
        { 15, { 32, 122,  54 } },
        { 25, { 22, 158,  88 } },
        { 35, { 42, 192, 112 } },
        { 50, {120, 205,  70 } },
        { 70, {178, 188,  46 } },
        { 85, {210, 150,  38 } },
        { 95, {225, 102,  28 } },
        {100, {235,  55,  25 } },
    };
    static const Stop mono_stops[] = {
        {  1, { 86,  86,  86 } },
        { 10, {110, 110, 110 } },
        { 25, {140, 140, 140 } },
        { 50, {180, 180, 180 } },
        {100, {235, 235, 235 } },
    };
    const Stop *stops;
    int count;
    int lo = 0;

    if (pct <= 0) return theme->bg;
    if (pct > 100) pct = 100;
    if (theme->mono) {
        stops = mono_stops;
        count = (int)(sizeof(mono_stops) / sizeof(mono_stops[0]));
    } else if (mode == HEAT_MODE_TASK) {
        stops = task_stops;
        count = (int)(sizeof(task_stops) / sizeof(task_stops[0]));
    } else {
        stops = raw_stops;
        count = (int)(sizeof(raw_stops) / sizeof(raw_stops[0]));
    }
    for (int i = 0; i < count - 1; i++) {
        if (pct >= stops[i].p && pct <= stops[i + 1].p) {
            lo = i;
            break;
        }
    }
    return rgb_interp(stops[lo].c, stops[lo + 1].c, pct - stops[lo].p, stops[lo + 1].p - stops[lo].p);
}

static Rgb cpu_pct_color(const Theme *theme, int pct)
{
    typedef struct {
        int p;
        Rgb c;
    } Stop;
    static const Stop stops[] = {
        {  0, { 80, 100, 200 } },
        { 10, { 70, 140, 230 } },
        { 20, { 60, 190, 230 } },
        { 35, { 80, 220, 140 } },
        { 50, {140, 220,  60 } },
        { 70, {200, 200,  50 } },
        { 85, {230, 160,  50 } },
        { 95, {240, 100,  40 } },
        {100, {240,  60,  40 } },
    };
    int lo = 0;

    if (theme->mono) {
        if (pct >= 90) return theme->white;
        if (pct >= 60) return theme->fg;
        if (pct >= 25) return theme->blue;
        return theme->dim;
    }
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    for (int i = 0; i < (int)(sizeof(stops) / sizeof(stops[0])) - 1; i++) {
        if (pct >= stops[i].p && pct <= stops[i + 1].p) {
            lo = i;
            break;
        }
    }
    return rgb_interp(stops[lo].c, stops[lo + 1].c, pct - stops[lo].p, stops[lo + 1].p - stops[lo].p);
}

static Rgb temp_color(const Theme *theme, double temp)
{
    if (temp >= 75.0) return theme->red;
    if (temp >= 65.0) return theme->orange;
    if (temp >= 55.0) return theme->green;
    return theme->blue;
}

static Rgb proc_state_color(const Theme *theme, char state)
{
    switch (state) {
    case 'R': return theme->green;
    case 'D': return theme->orange;
    case 'Z': return theme->red;
    case 'T': return theme->yellow;
    case 'I': return theme->blue;
    case 'S': return theme->white;
    default: return theme->dim;
    }
}

static Rgb proc_pid_color(const Theme *theme, const LuloProcRow *row)
{
    if (row->state == 'Z') return theme->red;
    if (row->is_kernel) return theme->cyan;
    if (row->is_thread) return theme->green;
    return proc_state_color(theme, row->state);
}

static Rgb proc_user_color(const Theme *theme, const LuloProcRow *row)
{
    if (row->is_kernel) return theme->dim;
    if (!strcmp(row->user, "root")) return theme->red;
    return row->is_thread ? theme->green : theme->cyan;
}

static Rgb proc_cpu_color(const Theme *theme, int tenths)
{
    if (tenths >= 500) return theme->red;
    if (tenths >= 200) return theme->orange;
    if (tenths >= 50) return theme->yellow;
    if (tenths > 0) return theme->green;
    return theme->dim;
}

static Rgb proc_mem_color(const Theme *theme, int tenths)
{
    if (tenths >= 100) return theme->red;
    if (tenths >= 50) return theme->orange;
    if (tenths >= 20) return theme->yellow;
    if (tenths > 0) return theme->blue;
    return theme->dim;
}

static Rgb proc_prio_color(const Theme *theme, const LuloProcRow *row)
{
    if (row->policy == 6) return theme->red;
    if (row->rt_priority > 0 || row->priority < 0) return theme->orange;
    if (row->priority <= 19) return theme->green;
    if (row->priority <= 39) return theme->white;
    return theme->dim;
}

static Rgb proc_policy_color(const Theme *theme, int policy)
{
    switch (policy) {
    case 1:
    case 2:
        return theme->orange;
    case 6:
        return theme->red;
    case 7:
        return theme->cyan;
    case 3:
        return theme->yellow;
    case 5:
        return theme->dim;
    default:
        return theme->white;
    }
}

static Rgb proc_nice_color(const Theme *theme, int nice_value)
{
    if (nice_value < 0) return theme->cyan;
    if (nice_value > 0) return theme->yellow;
    return theme->white;
}

static Rgb proc_time_color(const Theme *theme, unsigned long long time_cs)
{
    if (time_cs >= 6000) return theme->orange;
    if (time_cs > 0) return theme->green;
    return theme->dim;
}

static Rgb proc_command_color(const Theme *theme, const LuloProcRow *row)
{
    if (row->state == 'Z') return theme->red;
    if (row->is_kernel) return theme->cyan;
    if (row->is_thread) return theme->green;
    return proc_state_color(theme, row->state);
}

static struct ncplane *create_plane(struct ncplane *parent, int row, int col, int height, int width, const char *name)
{
    ncplane_options opts;

    if (height <= 0 || width <= 0) return NULL;
    memset(&opts, 0, sizeof(opts));
    opts.y = row - 1;
    opts.x = col - 1;
    opts.rows = (unsigned)height;
    opts.cols = (unsigned)width;
    opts.name = name;
    return ncplane_create(parent, &opts);
}

static void destroy_plane(struct ncplane **p)
{
    if (*p) {
        ncplane_destroy(*p);
        *p = NULL;
    }
}

static void ui_destroy_planes(Ui *ui)
{
    destroy_plane(&ui->footer);
    destroy_plane(&ui->load);
    destroy_plane(&ui->disk);
    destroy_plane(&ui->proc);
    destroy_plane(&ui->mem);
    destroy_plane(&ui->cpu);
    destroy_plane(&ui->top);
    destroy_plane(&ui->tabs);
    destroy_plane(&ui->header);
}

static int ui_rebuild_planes(Ui *ui, const TopLayout *lo, AppPage page)
{
    unsigned rows;
    unsigned cols;

    ui_destroy_planes(ui);
    ui->lo = *lo;
    ncplane_dim_yx(ui->std, &rows, &cols);
    ui->term_h = (int)rows;
    ui->term_w = (int)cols;
    plane_reset(ui->std, ui->theme);
    memset(&ui->proc_table, 0, sizeof(ui->proc_table));
    memset(ui->tab_hits, 0, sizeof(ui->tab_hits));

    ui->header = create_plane(ui->std, lo->header.row, lo->header.col, lo->header.height, lo->header.width, "header");
    ui->tabs = create_plane(ui->std, lo->header.row + lo->header.height, 1, 1, ui->term_w, "tabs");
    ui->top = create_plane(ui->std, lo->top.row, lo->top.col, lo->top.height, lo->top.width, "top");
    ui->load = create_plane(ui->std, lo->load.row, lo->load.col, lo->load.height, lo->load.width, "load");
    ui->footer = create_plane(ui->std, lo->hint_row, 1, 1, ui->term_w, "footer");
    if (page == APP_PAGE_CPU) {
        ui->cpu = create_plane(ui->std, lo->top.row + 1, lo->cpu.col, lo->cpu_rows + 1, lo->cpu.width, "cpu");
        if (lo->show_mem && lo->mem.height > 0) {
            ui->mem = create_plane(ui->std, lo->mem.row, lo->mem.col, lo->mem.height, lo->mem.width, "mem");
        }
        if (lo->show_proc && lo->proc.height > 0) {
            ui->proc = create_plane(ui->std, lo->proc.row, lo->proc.col, lo->proc.height, lo->proc.width, "proc");
        }
    } else {
        int disk_height = lo->top.height - 3;
        if (disk_height > 0) {
            ui->disk = create_plane(ui->std, lo->top.row + 1, lo->top.col + 1, disk_height, lo->top.width - 2, "disk");
        }
    }

    if (!ui->header || !ui->tabs || !ui->top || !ui->load || !ui->footer) {
        return -1;
    }
    if (page == APP_PAGE_CPU && (!ui->cpu || (lo->show_proc && !ui->proc))) {
        return -1;
    }
    if (page == APP_PAGE_DIZK && !ui->disk) {
        return -1;
    }
    if (page == APP_PAGE_CPU) {
        if (ui->mem) draw_box_title(ui->mem, ui->theme, ui->theme->border_panel, " Memory ", ui->theme->white);
        if (ui->proc) draw_box_title(ui->proc, ui->theme, ui->theme->border_panel, " Process Tree ", ui->theme->white);
    } else if (ui->disk) {
        draw_box_title(ui->disk, ui->theme, ui->theme->border_panel, " Dizk ", ui->theme->white);
    }
    return 0;
}

static void render_top_frame(Ui *ui)
{
    draw_box_title(ui->top, ui->theme, ui->theme->border_frame, NULL, ui->theme->fg);
}

static void render_header_widget(Ui *ui, const DashboardState *dash, const AppState *app)
{
    char tb[32];
    char sample_buf[24];
    char heat_buf[24];
    char proc_buf[32];
    char proc_cpu_buf[24];
    int row;
    int x;
    int remaining;
    Rgb accent = heat_mode_accent(ui->theme, dash->heat_mode);

    draw_box_title(ui->header, ui->theme, ui->theme->border_header, NULL, ui->theme->fg);
    if (ui->lo.header.height < 3) return;

    strftime(tb, sizeof(tb), "%a %d %b %Y  %H:%M:%S", localtime(&(time_t){ time(NULL) }));
    snprintf(sample_buf, sizeof(sample_buf), "%dms", dash->sample_ms);
    snprintf(heat_buf, sizeof(heat_buf), "heat %s", lulo_heat_mode_name(dash->heat_mode));
    snprintf(proc_buf, sizeof(proc_buf), "proc %dms",
             app ? effective_proc_refresh_ms(dash->sample_ms, app->proc_refresh_ms) : 1000);
    snprintf(proc_cpu_buf, sizeof(proc_cpu_buf), "cpu %s",
             app ? lulo_proc_cpu_mode_name(app->proc_cpu_mode) : lulo_proc_cpu_mode_name(LULO_PROC_CPU_PER_CORE));

    row = 1;
    x = 2;
    remaining = ui->lo.header.width - 4;
    x += plane_segment_fit(ui->header, row, x, remaining, accent, ui->theme->bg, "LULO");
    remaining = ui->lo.header.width - 4 - (x - 2);
    x += plane_segment_fit(ui->header, row, x, remaining, ui->theme->dim, ui->theme->bg, "  ");
    remaining = ui->lo.header.width - 4 - (x - 2);
    x += plane_segment_fit(ui->header, row, x, remaining, ui->theme->white, ui->theme->bg,
                           dash->model_short[0] ? dash->model_short : "CPU");
    remaining = ui->lo.header.width - 4 - (x - 2);
    x += plane_segment_fit(ui->header, row, x, remaining, ui->theme->dim, ui->theme->bg, "  ·  ");
    remaining = ui->lo.header.width - 4 - (x - 2);
    x += plane_segment_fit(ui->header, row, x, remaining, ui->theme->white, ui->theme->bg, dash->hostname);
    remaining = ui->lo.header.width - 4 - (x - 2);
    x += plane_segment_fit(ui->header, row, x, remaining, ui->theme->dim, ui->theme->bg, "  ·  ");
    remaining = ui->lo.header.width - 4 - (x - 2);
    x += plane_segment_fit(ui->header, row, x, remaining, ui->theme->cyan, ui->theme->bg, sample_buf);
    remaining = ui->lo.header.width - 4 - (x - 2);
    x += plane_segment_fit(ui->header, row, x, remaining, ui->theme->dim, ui->theme->bg, "  ·  ");
    remaining = ui->lo.header.width - 4 - (x - 2);
    x += plane_segment_fit(ui->header, row, x, remaining, accent, ui->theme->bg, heat_buf);
    remaining = ui->lo.header.width - 4 - (x - 2);
    x += plane_segment_fit(ui->header, row, x, remaining, ui->theme->dim, ui->theme->bg, "  ·  ");
    remaining = ui->lo.header.width - 4 - (x - 2);
    x += plane_segment_fit(ui->header, row, x, remaining, ui->theme->green, ui->theme->bg, proc_cpu_buf);
    remaining = ui->lo.header.width - 4 - (x - 2);
    x += plane_segment_fit(ui->header, row, x, remaining, ui->theme->dim, ui->theme->bg, "  ·  ");
    remaining = ui->lo.header.width - 4 - (x - 2);
    x += plane_segment_fit(ui->header, row, x, remaining, ui->theme->cyan, ui->theme->bg, proc_buf);
    remaining = ui->lo.header.width - 4 - (x - 2);
    x += plane_segment_fit(ui->header, row, x, remaining, ui->theme->dim, ui->theme->bg, "  ·  ");
    remaining = ui->lo.header.width - 4 - (x - 2);
    plane_segment_fit(ui->header, row, x, remaining, ui->theme->white, ui->theme->bg, tb);
}

static void render_cpu_headers(Ui *ui, const DashboardState *dash)
{
    char buf[64];
    int x = 0;
    Rgb accent = heat_mode_accent(ui->theme, dash->heat_mode);

    plane_fill(ui->cpu, 0, 0, ui->lo.cpu.width, ui->theme->bg, ui->theme->bg);
    snprintf(buf, sizeof(buf), "%-*s", ui->lo.cpu_label_w, "CPU");
    plane_putn(ui->cpu, 0, x, ui->theme->white, ui->theme->bg, buf, ui->lo.cpu_label_w);
    x += ui->lo.cpu_label_w + 2;
    snprintf(buf, sizeof(buf), "%-*s", ui->lo.cpu_hist_w + 4, "HISTORY");
    plane_putn(ui->cpu, 0, x, accent, ui->theme->bg, buf, ui->lo.cpu_hist_w + 4);
    x += ui->lo.cpu_hist_w + 5;
    plane_putn(ui->cpu, 0, x, ui->theme->white, ui->theme->bg, "MHz", 6);
    x += 8;
    if (ui->lo.show_governor) {
        snprintf(buf, sizeof(buf), "%-*s", ui->lo.cpu_gov_w, "GOV");
        plane_putn(ui->cpu, 0, x, ui->theme->white, ui->theme->bg, buf, ui->lo.cpu_gov_w);
        x += ui->lo.cpu_gov_w + 2;
    }
    if (ui->lo.show_temp) {
        plane_putn(ui->cpu, 0, x, ui->theme->white, ui->theme->bg, "TEMP", 6);
    }
}

static void render_heatmap(Ui *ui, int y, int x, const unsigned char *hist, int width, HeatMode mode)
{
    for (int i = 0; i < width; i++) {
        if (hist[i] <= 0) continue;
        plane_color(ui->cpu, heat_cell_color(ui->theme, mode, hist[i]), ui->theme->bg);
        ncplane_putstr_yx(ui->cpu, y, x + i, "█");
    }
}

static void render_cpu_widget(Ui *ui, const CpuInfo *ci, DashboardState *dash,
                              const CpuStat *sa, const CpuStat *sb,
                              const CpuFreq *freqs, int nfreq,
                              const double *cpu_temps, int ncpu_temp,
                              int append_sample)
{
    render_cpu_headers(ui, dash);
    for (int i = 0; i < ui->lo.cpu_rows; i++) {
        char label[32];
        char buf[64];
        int y = i + 1;
        int x = 0;
        int pct = lulo_cpu_pct(&sa->cpu[i + 1], &sb->cpu[i + 1]);
        int heat_raw = lulo_cpu_heat_pct(&sa->cpu[i + 1], &sb->cpu[i + 1], HEAT_MODE_RAW);
        int heat_task = lulo_cpu_heat_pct(&sa->cpu[i + 1], &sb->cpu[i + 1], HEAT_MODE_TASK);
        long mhz = i < nfreq ? freqs[i].cur_khz / 1000 : 0;
        const unsigned char *hist;

        if (append_sample) lulo_dashboard_append_heat(dash, i, heat_raw, heat_task);
        hist = lulo_dashboard_history(dash, i, ui->lo.cpu_hist_w);

        plane_fill(ui->cpu, y, 0, ui->lo.cpu.width, ui->theme->bg, ui->theme->bg);
        snprintf(label, sizeof(label), "cpu%d", i);
        snprintf(buf, sizeof(buf), "%-*s", ui->lo.cpu_label_w, label);
        plane_putn(ui->cpu, y, x, ui->theme->white, ui->theme->bg, buf, ui->lo.cpu_label_w);
        x += ui->lo.cpu_label_w + 2;
        render_heatmap(ui, y, x, hist, ui->lo.cpu_hist_w, dash->heat_mode);
        x += ui->lo.cpu_hist_w + 1;

        snprintf(buf, sizeof(buf), "%3d%%", pct);
        plane_putn(ui->cpu, y, x, cpu_pct_color(ui->theme, pct), ui->theme->bg, buf, 4);
        x += 6;

        if (mhz > 0) snprintf(label, sizeof(label), "%ld", mhz);
        else snprintf(label, sizeof(label), "-");
        snprintf(buf, sizeof(buf), "%-6s", label);
        plane_putn(ui->cpu, y, x, cpu_pct_color(ui->theme, pct), ui->theme->bg, buf, 6);
        x += 8;

        if (ui->lo.show_governor) {
            snprintf(buf, sizeof(buf), "%-*.*s", ui->lo.cpu_gov_w, ui->lo.cpu_gov_w,
                     i < nfreq ? freqs[i].governor : "-");
            plane_putn(ui->cpu, y, x, ui->theme->green, ui->theme->bg, buf, ui->lo.cpu_gov_w);
            x += ui->lo.cpu_gov_w + 2;
        }
        if (ui->lo.show_temp) {
            double temp = lulo_cpu_temp_for_row(ci, cpu_temps, ncpu_temp, i, sb->ncpu);
            snprintf(buf, sizeof(buf), "%4.1f°C", temp);
            plane_putn(ui->cpu, y, x, temp_color(ui->theme, temp), ui->theme->bg, buf, 6);
        }
    }
}

static void render_mem_metric(struct ncplane *p, const Theme *theme, int y, const char *label,
                              int pct, const char *value, int bar_w, Rgb fill_bg, Rgb value_fg)
{
    char pctbuf[16];
    int used;
    int free_w;

    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    used = pct * bar_w / 100;
    free_w = bar_w - used;
    plane_putn(p, y, 2, theme->white, theme->bg, label, 5);
    if (used > 0) plane_fill(p, y, 8, used, theme->bg, fill_bg);
    if (free_w > 0) plane_fill(p, y, 8 + used, free_w, theme->bg, theme->mem_free_bg);
    snprintf(pctbuf, sizeof(pctbuf), "%3d%%", pct);
    plane_putn(p, y, 9 + bar_w, value_fg, theme->bg, pctbuf, 4);
    plane_putn(p, y, 15 + bar_w, theme->white, theme->bg, value, (int)strlen(value));
}

static void render_memory_widget(Ui *ui, const MemInfo *mi)
{
    char buf[32];
    unsigned long long mem_used;
    unsigned long long swap_used;
    unsigned rows = 0;
    unsigned cols = 0;

    if (!ui->mem || !ui->lo.show_mem || ui->lo.mem_rows <= 0) return;
    ncplane_dim_yx(ui->mem, &rows, &cols);
    plane_clear_inner(ui->mem, ui->theme, (int)rows, (int)cols);
    mem_used = mi->total > mi->available ? mi->total - mi->available : 0;
    swap_used = mi->swap_total > mi->swap_free ? mi->swap_total - mi->swap_free : 0;

    lulo_format_size(buf, sizeof(buf), mem_used);
    render_mem_metric(ui->mem, ui->theme, 1, "used ", mi->total ? (int)(mem_used * 100ULL / mi->total) : 0,
                      buf, ui->lo.mem_bar_w, ui->theme->mem_fill[0], ui->theme->mem_text[0]);
    lulo_format_size(buf, sizeof(buf), mi->buffers);
    render_mem_metric(ui->mem, ui->theme, 2, "buff ", mi->total ? (int)(mi->buffers * 100ULL / mi->total) : 0,
                      buf, ui->lo.mem_bar_w, ui->theme->mem_fill[1], ui->theme->mem_text[1]);
    lulo_format_size(buf, sizeof(buf), mi->cached);
    render_mem_metric(ui->mem, ui->theme, 3, "cache", mi->total ? (int)(mi->cached * 100ULL / mi->total) : 0,
                      buf, ui->lo.mem_bar_w, ui->theme->mem_fill[2], ui->theme->mem_text[2]);
    render_mem_metric(ui->mem, ui->theme, 4, "slab ", mi->total ? (int)(mi->slab * 100ULL / mi->total) : 0,
                      (lulo_format_size(buf, sizeof(buf), mi->slab), buf), ui->lo.mem_bar_w,
                      ui->theme->mem_fill[3], ui->theme->mem_text[3]);
    render_mem_metric(ui->mem, ui->theme, 5, "swap ", mi->swap_total ? (int)(swap_used * 100ULL / mi->swap_total) : 0,
                      (lulo_format_size(buf, sizeof(buf), swap_used), buf), ui->lo.mem_bar_w,
                      ui->theme->mem_fill[4], ui->theme->mem_text[4]);
    if (ui->lo.mem_rows >= 6) {
        render_mem_metric(ui->mem, ui->theme, 6, "dirty", mi->total ? (int)(mi->dirty * 100ULL / mi->total) : 0,
                          (lulo_format_size(buf, sizeof(buf), mi->dirty), buf), ui->lo.mem_bar_w,
                          ui->theme->mem_fill[5], ui->theme->mem_text[5]);
    }
    if (ui->lo.mem_rows >= 7 && ui->lo.mem.height >= 9) {
        snprintf(buf, sizeof(buf), "hp %llu x %llu kB (%llu free)",
                 mi->hugepages_total, mi->hugepage_size_kb, mi->hugepages_free);
        plane_putn(ui->mem, 7, 2, ui->theme->dim, ui->theme->bg, buf, ui->lo.mem.width - 4);
    }
}

static void render_load_widget(Ui *ui, const LoadInfo *li, int logical_cpus)
{
    char buf[128];
    int x = 0;
    int remaining = ui->lo.load.width - 2;
    int ncpu = logical_cpus > 0 ? logical_cpus : 1;
    Rgb load_color;

    if (!ui->load) return;
    plane_reset(ui->load, ui->theme);
    if (remaining <= 0) return;

    if (li->load1 > ncpu * 0.9) load_color = ui->theme->red;
    else if (li->load1 > ncpu * 0.7) load_color = ui->theme->orange;
    else if (li->load1 > ncpu * 0.5) load_color = ui->theme->yellow;
    else load_color = ui->theme->green;

    x += plane_segment_fit(ui->load, 0, x, remaining, ui->theme->white, ui->theme->bg, "load ");
    remaining = ui->lo.load.width - 2 - x;
    snprintf(buf, sizeof(buf), "%.2f %.2f %.2f", li->load1, li->load5, li->load15);
    x += plane_segment_fit(ui->load, 0, x, remaining, load_color, ui->theme->bg, buf);
    remaining = ui->lo.load.width - 2 - x;
    x += plane_segment_fit(ui->load, 0, x, remaining, ui->theme->white, ui->theme->bg, "  run ");
    remaining = ui->lo.load.width - 2 - x;
    snprintf(buf, sizeof(buf), "%d/%d", li->running, li->total);
    x += plane_segment_fit(ui->load, 0, x, remaining, ui->theme->cyan, ui->theme->bg, buf);
    remaining = ui->lo.load.width - 2 - x;
    x += plane_segment_fit(ui->load, 0, x, remaining, ui->theme->white, ui->theme->bg, "  blk ");
    remaining = ui->lo.load.width - 2 - x;
    snprintf(buf, sizeof(buf), "%d", li->procs_blocked);
    x += plane_segment_fit(ui->load, 0, x, remaining,
                           li->procs_blocked > 0 ? ui->theme->orange : ui->theme->white,
                           ui->theme->bg, buf);
    remaining = ui->lo.load.width - 2 - x;
    x += plane_segment_fit(ui->load, 0, x, remaining, ui->theme->white, ui->theme->bg, "  ctxsw ");
    remaining = ui->lo.load.width - 2 - x;
    snprintf(buf, sizeof(buf), "%ld", li->ctxt);
    x += plane_segment_fit(ui->load, 0, x, remaining, ui->theme->white, ui->theme->bg, buf);
    remaining = ui->lo.load.width - 2 - x;
    if (ui->lo.hidden_cpus > 0) {
        snprintf(buf, sizeof(buf), "  +%d cpu%s hidden", ui->lo.hidden_cpus, ui->lo.hidden_cpus == 1 ? "" : "s");
        plane_segment_fit(ui->load, 0, x, remaining, ui->theme->dim, ui->theme->bg, buf);
    }
}

static void render_tabs(Ui *ui, AppPage page)
{
    int x = 2;

    plane_reset(ui->tabs, ui->theme);
    for (int i = 0; i < APP_PAGE_COUNT; i++) {
        char label[32];
        int active = i == (int)page;
        Rgb fg = active ? ui->theme->bg : ui->theme->white;
        Rgb bg = active ? ui->theme->border_header : ui->theme->bg;
        int width;

        snprintf(label, sizeof(label), " %s ", app_page_name((AppPage)i));
        width = (int)strlen(label);
        ui->tab_hits[i].x = x;
        ui->tab_hits[i].width = width;
        plane_putn(ui->tabs, 0, x, fg, bg, label, width);
        x += width + 1;
    }
}

static void render_footer(Ui *ui, AppPage page, const char *status)
{
    const char *hint = footer_hint(page);

    plane_reset(ui->footer, ui->theme);
    plane_putn(ui->footer, 0, 1, ui->theme->white, ui->theme->bg, hint, ui->term_w - 2);
    if (status && *status) {
        int width = (int)strlen(status);
        int x = ui->term_w - width - 2;
        if (x < 2) x = 2;
        plane_putn(ui->footer, 0, x, ui->theme->cyan, ui->theme->bg, status, ui->term_w - x - 1);
    }
}

static void build_proc_table_layout(Ui *ui)
{
    ProcTableLayout *pt = &ui->proc_table;
    int inner_w;
    int cmd_w;

    memset(pt, 0, sizeof(*pt));
    if (!ui->proc || !ui->lo.show_proc || ui->lo.proc_rows <= 0) return;

    inner_w = ui->lo.proc.width - 2;
    pt->inner_w = inner_w;
    pt->pid_w = 5;
    pt->policy_w = 4;
    pt->prio_w = 4;
    pt->nice_w = 3;
    pt->cpu_w = 5;
    pt->show_user = inner_w >= 58;
    pt->show_mem = inner_w >= 72;
    pt->show_time = inner_w >= 86;
    pt->user_w = pt->show_user ? (inner_w >= 68 ? 8 : 6) : 0;
    pt->mem_w = pt->show_mem ? 5 : 0;
    pt->time_w = pt->show_time ? 8 : 0;
    cmd_w = inner_w - pt->pid_w - pt->policy_w - pt->prio_w - pt->nice_w - pt->cpu_w - 6;
    if (pt->show_user) cmd_w -= pt->user_w + 1;
    if (pt->show_mem) cmd_w -= pt->mem_w + 1;
    if (pt->show_time) cmd_w -= pt->time_w + 1;
    if (cmd_w < 10 && pt->show_time) {
        pt->show_time = 0;
        cmd_w += pt->time_w + 1;
        pt->time_w = 0;
    }
    if (cmd_w < 10 && pt->show_mem) {
        pt->show_mem = 0;
        cmd_w += pt->mem_w + 1;
        pt->mem_w = 0;
    }
    if (cmd_w < 10 && pt->show_user) {
        pt->show_user = 0;
        cmd_w += pt->user_w + 1;
        pt->user_w = 0;
    }
    if (cmd_w < 8) cmd_w = 8;
    pt->cmd_w = cmd_w;
    pt->body_x = 1;
    pt->body_y = 2;
    pt->body_rows = ui->lo.proc_rows;
    pt->controls_visible = inner_w >= 74;
}

static void set_proc_header_hit(ProcTableLayout *pt, LuloProcSortKey key, int x, int width, int visible)
{
    pt->headers[key].visible = visible;
    pt->headers[key].sort_key = key;
    pt->headers[key].x = x;
    pt->headers[key].width = width;
}

static void format_sort_header(char *buf, size_t len, const char *label,
                               const LuloProcState *state, LuloProcSortKey key, int width, int left_align)
{
    char core[32];
    const char *arrow = "";

    snprintf(core, sizeof(core), "%s", label);
    if (state && state->sort_key == key) arrow = state->sort_desc ? "v" : "^";
    if (width <= 0) {
        if (len > 0) buf[0] = '\0';
        return;
    }
    if (left_align) snprintf(buf, len, "%-*.*s%s", width - (int)strlen(arrow), width - (int)strlen(arrow), core, arrow);
    else snprintf(buf, len, "%*.*s%s", width - (int)strlen(arrow), width - (int)strlen(arrow), core, arrow);
}

static void render_process_count(Ui *ui, const LuloProcSnapshot *snap, const LuloProcState *state)
{
    int count_digits;
    int field_w;
    int rows_x;
    char rows_buf[32];

    if (!ui->proc || !snap || snap->count <= 0 || !state) return;
    count_digits = clamp_int(digits_int(snap->count), 1, 9);
    field_w = count_digits * 2 + 1;
    rows_x = ui->lo.proc.width - field_w - 2;
    if (rows_x < 2) return;
    snprintf(rows_buf, sizeof(rows_buf), "%*d/%d", count_digits, state->selected + 1, snap->count);
    plane_putn(ui->proc, 0, rows_x, ui->theme->dim, ui->theme->bg, rows_buf, field_w);
}

static void render_process_title(Ui *ui, const LuloProcSnapshot *snap, const LuloProcState *state)
{
    ProcTableLayout *pt = &ui->proc_table;

    if (!ui->proc) return;
    if (snap && snap->count > 0 && state) {
        int rows_x;
        int count_digits = digits_int(snap->count);
        int field_w = count_digits * 2 + 1;

        rows_x = ui->lo.proc.width - field_w - 2;
        if (pt->controls_visible) {
            const char *collapse_label = " [-] all ";
            const char *expand_label = " [+] all ";

            pt->expand_all_w = (int)strlen(expand_label);
            pt->collapse_all_w = (int)strlen(collapse_label);
            pt->expand_all_x = rows_x - pt->expand_all_w - 2;
            pt->collapse_all_x = pt->expand_all_x - pt->collapse_all_w - 1;
            if (pt->collapse_all_x > 18) {
                plane_putn(ui->proc, 0, pt->collapse_all_x, ui->theme->cyan, ui->theme->bg,
                           collapse_label, pt->collapse_all_w);
                plane_putn(ui->proc, 0, pt->expand_all_x, ui->theme->green, ui->theme->bg,
                           expand_label, pt->expand_all_w);
            } else {
                pt->controls_visible = 0;
            }
        }
        render_process_count(ui, snap, state);
    }
}

static void render_process_columns(Ui *ui, const LuloProcState *state)
{
    ProcTableLayout *pt = &ui->proc_table;

    {
        int x = 1;
        char buf[64];

        format_sort_header(buf, sizeof(buf), "PID", state, LULO_PROC_SORT_PID, pt->pid_w, 0);
        plane_putn(ui->proc, 1, x, ui->theme->dim, ui->theme->bg, buf, pt->pid_w);
        set_proc_header_hit(pt, LULO_PROC_SORT_PID, x, pt->pid_w, 1);
        x += pt->pid_w + 1;
        if (pt->show_user) {
            format_sort_header(buf, sizeof(buf), "USER", state, LULO_PROC_SORT_USER, pt->user_w, 1);
            plane_putn(ui->proc, 1, x, ui->theme->orange, ui->theme->bg, buf, pt->user_w);
            set_proc_header_hit(pt, LULO_PROC_SORT_USER, x, pt->user_w, 1);
            x += pt->user_w + 1;
        }
        format_sort_header(buf, sizeof(buf), "POL", state, LULO_PROC_SORT_POLICY, pt->policy_w, 1);
        plane_putn(ui->proc, 1, x, ui->theme->yellow, ui->theme->bg, buf, pt->policy_w);
        set_proc_header_hit(pt, LULO_PROC_SORT_POLICY, x, pt->policy_w, 1);
        x += pt->policy_w + 1;
        format_sort_header(buf, sizeof(buf), "PRI", state, LULO_PROC_SORT_PRIORITY, pt->prio_w, 0);
        plane_putn(ui->proc, 1, x, ui->theme->orange, ui->theme->bg, buf, pt->prio_w);
        set_proc_header_hit(pt, LULO_PROC_SORT_PRIORITY, x, pt->prio_w, 1);
        x += pt->prio_w + 1;
        format_sort_header(buf, sizeof(buf), "NI", state, LULO_PROC_SORT_NICE, pt->nice_w, 0);
        plane_putn(ui->proc, 1, x, ui->theme->cyan, ui->theme->bg, buf, pt->nice_w);
        set_proc_header_hit(pt, LULO_PROC_SORT_NICE, x, pt->nice_w, 1);
        x += pt->nice_w + 1;
        format_sort_header(buf, sizeof(buf), "CPU%", state, LULO_PROC_SORT_CPU, pt->cpu_w, 0);
        plane_putn(ui->proc, 1, x, ui->theme->green, ui->theme->bg, buf, pt->cpu_w);
        set_proc_header_hit(pt, LULO_PROC_SORT_CPU, x, pt->cpu_w, 1);
        x += pt->cpu_w + 1;
        if (pt->show_mem) {
            format_sort_header(buf, sizeof(buf), "MEM%", state, LULO_PROC_SORT_MEM, pt->mem_w, 0);
            plane_putn(ui->proc, 1, x, ui->theme->blue, ui->theme->bg, buf, pt->mem_w);
            set_proc_header_hit(pt, LULO_PROC_SORT_MEM, x, pt->mem_w, 1);
            x += pt->mem_w + 1;
        }
        if (pt->show_time) {
            format_sort_header(buf, sizeof(buf), "TIME+", state, LULO_PROC_SORT_TIME, pt->time_w, 1);
            plane_putn(ui->proc, 1, x, ui->theme->cyan, ui->theme->bg, buf, pt->time_w);
            set_proc_header_hit(pt, LULO_PROC_SORT_TIME, x, pt->time_w, 1);
            x += pt->time_w + 1;
        }
        format_sort_header(buf, sizeof(buf), "COMMAND", state, LULO_PROC_SORT_COMMAND, pt->cmd_w, 1);
        plane_putn(ui->proc, 1, x, ui->theme->white, ui->theme->bg, buf, pt->cmd_w);
        set_proc_header_hit(pt, LULO_PROC_SORT_COMMAND, x, pt->cmd_w, 1);
    }
}

static void render_process_row(Ui *ui, const LuloProcSnapshot *snap, const LuloProcState *state, int row_slot)
{
    ProcTableLayout *pt = &ui->proc_table;
    int start = state ? state->scroll : 0;
    int row_index = start + row_slot;
    int y = row_slot + pt->body_y;
    int x = 1;
    int selected = state && row_index == state->selected;
    Rgb row_bg = selected ? ui->theme->select_bg : ui->theme->bg;
    char cpu_buf[16];
    char mem_buf[16];
    char time_buf[16];
    char policy_buf[8];
    char prio_buf[16];
    char out[1024];

    plane_fill(ui->proc, y, 1, pt->inner_w, row_bg, row_bg);
    if (!snap || row_index < 0 || row_index >= snap->count) return;

    const LuloProcRow *row = &snap->rows[row_index];
    lulo_format_proc_pct(cpu_buf, sizeof(cpu_buf), row->cpu_tenths);
    lulo_format_proc_pct(mem_buf, sizeof(mem_buf), row->mem_tenths);
    lulo_format_proc_time(time_buf, sizeof(time_buf), row->time_cs);
    lulo_format_proc_policy(policy_buf, sizeof(policy_buf), row->policy);
    lulo_format_proc_priority(prio_buf, sizeof(prio_buf), row->rt_priority, row->priority);

    snprintf(out, sizeof(out), "%*d", pt->pid_w, row->pid);
    plane_putn(ui->proc, y, x, selected ? ui->theme->select_fg : proc_pid_color(ui->theme, row),
               row_bg, out, pt->pid_w);
    x += pt->pid_w + 1;

    if (pt->show_user) {
        snprintf(out, sizeof(out), "%-*.*s", pt->user_w, pt->user_w, row->user);
        plane_putn(ui->proc, y, x, selected ? ui->theme->select_fg : proc_user_color(ui->theme, row),
                   row_bg, out, pt->user_w);
        x += pt->user_w + 1;
    }

    snprintf(out, sizeof(out), "%-*s", pt->policy_w, policy_buf);
    plane_putn(ui->proc, y, x, selected ? ui->theme->select_fg : proc_policy_color(ui->theme, row->policy),
               row_bg, out, pt->policy_w);
    x += pt->policy_w + 1;

    snprintf(out, sizeof(out), "%*s", pt->prio_w, prio_buf);
    plane_putn(ui->proc, y, x, selected ? ui->theme->select_fg : proc_prio_color(ui->theme, row),
               row_bg, out, pt->prio_w);
    x += pt->prio_w + 1;

    snprintf(out, sizeof(out), "%*d", pt->nice_w, row->nice);
    plane_putn(ui->proc, y, x, selected ? ui->theme->select_fg : proc_nice_color(ui->theme, row->nice),
               row_bg, out, pt->nice_w);
    x += pt->nice_w + 1;

    snprintf(out, sizeof(out), "%*s", pt->cpu_w, cpu_buf);
    plane_putn(ui->proc, y, x, selected ? ui->theme->select_fg : proc_cpu_color(ui->theme, row->cpu_tenths),
               row_bg, out, pt->cpu_w);
    x += pt->cpu_w + 1;

    if (pt->show_mem) {
        snprintf(out, sizeof(out), "%*s", pt->mem_w, mem_buf);
        plane_putn(ui->proc, y, x, selected ? ui->theme->select_fg : proc_mem_color(ui->theme, row->mem_tenths),
                   row_bg, out, pt->mem_w);
        x += pt->mem_w + 1;
    }

    if (pt->show_time) {
        snprintf(out, sizeof(out), "%-*s", pt->time_w, time_buf);
        plane_putn(ui->proc, y, x, selected ? ui->theme->select_fg : proc_time_color(ui->theme, row->time_cs),
                   row_bg, out, pt->time_w);
        x += pt->time_w + 1;
    }

    if (row->label_prefix_len > 0 &&
        row->label_prefix_len < (int)sizeof(row->label) &&
        row->label_prefix_cols < pt->cmd_w) {
        int cmd_only_w = pt->cmd_w - row->label_prefix_cols;
        snprintf(out, sizeof(out), "%.*s", row->label_prefix_len, row->label);
        plane_putn(ui->proc, y, x, selected ? ui->theme->select_fg : ui->theme->branch,
                   row_bg, out, row->label_prefix_cols);
        x += row->label_prefix_cols;
        snprintf(out, sizeof(out), "%-*.*s", cmd_only_w, cmd_only_w, row->label + row->label_prefix_len);
        plane_putn(ui->proc, y, x, selected ? ui->theme->select_fg : proc_command_color(ui->theme, row),
                   row_bg, out, cmd_only_w);
    } else {
        snprintf(out, sizeof(out), "%-*.*s", pt->cmd_w, pt->cmd_w, row->label);
        plane_putn(ui->proc, y, x, selected ? ui->theme->select_fg : proc_command_color(ui->theme, row),
                   row_bg, out, pt->cmd_w);
    }
}

static void render_process_rows(Ui *ui, const LuloProcSnapshot *snap, const LuloProcState *state,
                                int first_slot, int count)
{
    ProcTableLayout *pt = &ui->proc_table;
    int end;

    if (!ui->proc || !ui->lo.show_proc || ui->lo.proc_rows <= 0) return;
    if (first_slot < 0) first_slot = 0;
    if (count < 0) count = 0;
    end = first_slot + count;
    if (end > pt->body_rows) end = pt->body_rows;
    for (int i = first_slot; i < end; i++) render_process_row(ui, snap, state, i);
}

static void render_process_widget(Ui *ui, const LuloProcSnapshot *snap, const LuloProcState *state)
{
    unsigned rows = 0;
    unsigned cols = 0;

    if (!ui->proc || !ui->lo.show_proc || ui->lo.proc_rows <= 0) return;
    build_proc_table_layout(ui);
    ncplane_dim_yx(ui->proc, &rows, &cols);
    plane_fill(ui->proc, 1, 1, ui->proc_table.inner_w, ui->theme->bg, ui->theme->bg);
    render_process_title(ui, snap, state);
    render_process_columns(ui, state);
    render_process_rows(ui, snap, state, 0, ui->proc_table.body_rows);
}

static void render_process_body_only(Ui *ui, const LuloProcSnapshot *snap, const LuloProcState *state)
{
    if (!ui->proc || !ui->lo.show_proc || ui->lo.proc_rows <= 0) return;
    build_proc_table_layout(ui);
    render_process_count(ui, snap, state);
    render_process_rows(ui, snap, state, 0, ui->proc_table.body_rows);
}

static void render_process_cursor_only(Ui *ui, const LuloProcSnapshot *snap, const LuloProcState *state,
                                       int prev_selected, int prev_scroll)
{
    int old_slot;
    int new_slot;

    if (!ui->proc || !snap || !state) return;
    build_proc_table_layout(ui);
    render_process_count(ui, snap, state);
    old_slot = prev_selected - prev_scroll;
    new_slot = state->selected - state->scroll;
    if (old_slot >= 0 && old_slot < ui->proc_table.body_rows) {
        render_process_row(ui, snap, state, old_slot);
    }
    if (new_slot >= 0 && new_slot < ui->proc_table.body_rows && new_slot != old_slot) {
        render_process_row(ui, snap, state, new_slot);
    }
}

typedef struct {
    LuloRect fs;
    LuloRect dev;
    LuloRect io;
    LuloRect queue;
    int show_dev;
    int show_io;
    int show_queue;
} DiskWidgetLayout;

static int rect_valid(const LuloRect *rect)
{
    return rect && rect->height >= 3 && rect->width >= 12;
}

static int rect_inner_rows(const LuloRect *rect)
{
    if (!rect_valid(rect)) return 0;
    return rect->height - 2;
}

static int rect_inner_cols(const LuloRect *rect)
{
    if (!rect_valid(rect)) return 0;
    return rect->width - 2;
}

static void draw_inner_box(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                           Rgb border, const char *title, Rgb title_color)
{
    if (!rect_valid(rect)) return;
    ncplane_cursor_move_yx(p, rect->row, rect->col);
    ncplane_rounded_box_sized(p, 0, rgb_channels(border, theme->bg),
                              (unsigned)rect->height, (unsigned)rect->width, 0);
    if (title && *title) {
        int max_title = rect->width - 6;
        if (max_title > 0) {
            plane_putn(p, rect->row, rect->col + 2, title_color, theme->bg, title, max_title);
        }
    }
}

static void draw_inner_meta(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                            const char *text, Rgb fg)
{
    int len;
    int x;

    if (!rect_valid(rect) || !text || !*text) return;
    len = (int)strlen(text);
    x = rect->col + rect->width - len - 2;
    if (x <= rect->col + 2) return;
    plane_putn(p, rect->row, x, fg, theme->bg, text, len);
}

static Rgb disk_usage_fill(const Theme *theme, int pct)
{
    if (pct >= 90) return theme->red;
    if (pct >= 75) return theme->mem_fill[4];
    if (pct >= 50) return theme->mem_fill[2];
    if (pct >= 25) return theme->mem_fill[1];
    return theme->mem_fill[0];
}

static Rgb disk_usage_text(const Theme *theme, int pct)
{
    if (pct >= 90) return theme->red;
    if (pct >= 75) return theme->orange;
    if (pct >= 50) return theme->yellow;
    if (pct >= 25) return theme->green;
    return theme->cyan;
}

static void render_inline_meter(struct ncplane *p, const Theme *theme, int y, int x, int width,
                                int pct, Rgb fill_bg)
{
    int used;
    int free_w;

    if (width <= 0) return;
    pct = clamp_int(pct, 0, 100);
    used = pct * width / 100;
    free_w = width - used;
    if (used > 0) plane_fill(p, y, x, used, theme->bg, fill_bg);
    if (free_w > 0) plane_fill(p, y, x + used, free_w, theme->bg, theme->mem_free_bg);
}

static void build_disk_widget_layout(const Ui *ui, DiskWidgetLayout *layout)
{
    unsigned rows = 0;
    unsigned cols = 0;
    int inner_h;
    int inner_w;
    int reserve_bottom;
    int lower_row;
    int lower_h;

    memset(layout, 0, sizeof(*layout));
    if (!ui->disk) return;
    ncplane_dim_yx(ui->disk, &rows, &cols);
    if (rows < 5 || cols < 24) return;

    inner_h = (int)rows - 2;
    inner_w = (int)cols - 2;
    layout->fs.row = 1;
    layout->fs.col = 1;
    layout->fs.width = inner_w;
    layout->fs.height = inner_h;

    if (inner_h < 10) return;

    reserve_bottom = inner_w >= 96 ? 9 : inner_w >= 72 ? 8 : 6;
    layout->fs.height = clamp_int(inner_h - reserve_bottom - 1, 6, 10);
    if (layout->fs.height >= inner_h - 3) return;

    lower_row = layout->fs.row + layout->fs.height + 1;
    lower_h = (int)rows - 1 - lower_row;
    if (lower_h < 4) {
        layout->fs.height = inner_h;
        return;
    }

    if (inner_w >= 96) {
        int left_w = clamp_int(inner_w * 11 / 20, 32, inner_w - 34);
        int right_w = inner_w - left_w - 1;

        layout->dev.row = lower_row;
        layout->dev.col = 1;
        layout->dev.width = left_w;
        layout->dev.height = lower_h;
        layout->show_dev = rect_valid(&layout->dev);

        layout->io.col = left_w + 2;
        layout->io.width = right_w;
        if (lower_h >= 10) {
            int io_h = clamp_int(lower_h / 2, 4, lower_h - 5);
            int queue_h = lower_h - io_h - 1;

            layout->io.row = lower_row;
            layout->io.height = io_h;
            layout->queue.row = lower_row + io_h + 1;
            layout->queue.col = layout->io.col;
            layout->queue.width = right_w;
            layout->queue.height = queue_h;
            layout->show_queue = rect_valid(&layout->queue);
        } else {
            layout->io.row = lower_row;
            layout->io.height = lower_h;
        }
        layout->show_io = rect_valid(&layout->io);
        return;
    }

    if (lower_h >= 12) {
        int dev_h = clamp_int(lower_h / 3 + 1, 4, lower_h - 8);
        int io_h = clamp_int((lower_h - dev_h - 1) / 2, 4, lower_h - dev_h - 5);
        int queue_h = lower_h - dev_h - io_h - 2;

        layout->dev.row = lower_row;
        layout->dev.col = 1;
        layout->dev.width = inner_w;
        layout->dev.height = dev_h;
        layout->show_dev = rect_valid(&layout->dev);

        layout->io.row = lower_row + dev_h + 1;
        layout->io.col = 1;
        layout->io.width = inner_w;
        layout->io.height = io_h;
        layout->show_io = rect_valid(&layout->io);

        layout->queue.row = layout->io.row + io_h + 1;
        layout->queue.col = 1;
        layout->queue.width = inner_w;
        layout->queue.height = queue_h;
        layout->show_queue = rect_valid(&layout->queue);
        return;
    }

    layout->dev.row = lower_row;
    layout->dev.col = 1;
    layout->dev.width = inner_w;
    layout->dev.height = clamp_int(lower_h / 2, 4, lower_h);
    layout->show_dev = rect_valid(&layout->dev);

    layout->io.row = lower_row + layout->dev.height + 1;
    layout->io.col = 1;
    layout->io.width = inner_w;
    layout->io.height = lower_h - layout->dev.height - 1;
    layout->show_io = rect_valid(&layout->io);
}

static int disk_visible_rows(const Ui *ui)
{
    DiskWidgetLayout layout;

    if (!ui->disk) return 1;
    build_disk_widget_layout(ui, &layout);
    return clamp_int(rect_inner_rows(&layout.fs), 1, 1024);
}

static void render_disk_filesystems(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                    const LuloDizkSnapshot *snap, const LuloDizkState *state)
{
    char rows_buf[48];
    int visible_rows;
    int start;
    int dev_w;
    int mount_w;
    int value_w;
    int bar_w;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Filesystems ", theme->white);

    visible_rows = rect_inner_rows(rect);
    start = state ? clamp_int(state->scroll, 0, snap && snap->fs_count > visible_rows ? snap->fs_count - visible_rows : 0) : 0;
    if (snap && snap->fs_count > 0) {
        snprintf(rows_buf, sizeof(rows_buf), "%d-%d/%d",
                 start + 1,
                 clamp_int(start + visible_rows, 1, snap->fs_count),
                 snap->fs_count);
        draw_inner_meta(p, theme, rect, rows_buf, theme->cyan);
    }

    if (!snap || snap->fs_count <= 0) {
        plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg,
                   "no mounted filesystems", rect->width - 4);
        return;
    }

    dev_w = rect->width >= 84 ? 12 : 8;
    mount_w = rect->width >= 84 ? 16 : 12;
    value_w = rect->width >= 96 ? 18 : rect->width >= 72 ? 14 : 11;
    bar_w = rect_inner_cols(rect) - dev_w - mount_w - value_w - 8;
    if (bar_w < 8) {
        mount_w = clamp_int(mount_w - (8 - bar_w), 8, mount_w);
        bar_w = rect_inner_cols(rect) - dev_w - mount_w - value_w - 8;
    }
    if (bar_w < 8) bar_w = 8;

    for (int i = 0; i < visible_rows; i++) {
        int idx = start + i;
        int y = rect->row + 1 + i;
        int x = rect->col + 1;
        char pctbuf[8];
        char value[32];

        if (idx >= snap->fs_count) break;
        plane_putn(p, y, x, theme->cyan, theme->bg, snap->filesystems[idx].device, dev_w);
        x += dev_w + 1;
        plane_putn(p, y, x, theme->white, theme->bg, snap->filesystems[idx].mount, mount_w);
        x += mount_w + 1;
        render_inline_meter(p, theme, y, x, bar_w, snap->filesystems[idx].pct,
                            disk_usage_fill(theme, snap->filesystems[idx].pct));
        x += bar_w + 1;
        snprintf(pctbuf, sizeof(pctbuf), "%3d%%", snap->filesystems[idx].pct);
        plane_putn(p, y, x, disk_usage_text(theme, snap->filesystems[idx].pct), theme->bg, pctbuf, 4);
        x += 5;
        if (value_w >= 14) snprintf(value, sizeof(value), "%s/%s",
                                    snap->filesystems[idx].used, snap->filesystems[idx].total);
        else snprintf(value, sizeof(value), "%s", snap->filesystems[idx].used);
        plane_putn(p, y, x, theme->white, theme->bg, value, value_w);
    }
}

static void render_disk_devices(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                                const LuloDizkSnapshot *snap)
{
    int visible_rows;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Devices ", theme->white);
    if (snap && snap->blockdev_count > 0) {
        char meta[16];
        snprintf(meta, sizeof(meta), "%d", snap->blockdev_count);
        draw_inner_meta(p, theme, rect, meta, theme->green);
    }

    visible_rows = rect_inner_rows(rect);
    if (!snap || snap->blockdev_count <= 0) {
        plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg,
                   "no block devices", rect->width - 4);
        return;
    }

    for (int i = 0; i < visible_rows; i++) {
        int y = rect->row + 1 + i;
        int x = rect->col + 1;
        int right_w;
        const LuloDizkBlockRow *row;
        char kind[48];

        if (i >= snap->blockdev_count) break;
        row = &snap->blockdevs[i];
        snprintf(kind, sizeof(kind), "%s %s", row->type, row->transport);
        plane_putn(p, y, x, theme->cyan, theme->bg, row->name, 10);
        x += 11;
        plane_putn(p, y, x, theme->white, theme->bg, row->size, 10);
        x += 11;
        plane_putn(p, y, x, theme->green, theme->bg, kind, 10);
        x += 11;
        right_w = rect->col + rect->width - 1 - x;
        if (right_w > 0) plane_putn(p, y, x, theme->white, theme->bg, row->model, right_w);
    }
}

static void render_disk_io(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                           const LuloDizkSnapshot *snap)
{
    int visible_rows;
    int name_w;
    int bar_w;
    int data_w;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " I/O ", theme->white);
    if (snap && snap->iostat_count > 0) {
        char meta[16];
        snprintf(meta, sizeof(meta), "%d", snap->iostat_count);
        draw_inner_meta(p, theme, rect, meta, theme->green);
    }

    visible_rows = rect_inner_rows(rect);
    if (!snap || snap->iostat_count <= 0) {
        plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg,
                   "no disk activity data", rect->width - 4);
        return;
    }

    name_w = rect->width >= 38 ? 10 : 8;
    data_w = rect->width >= 46 ? 9 : 7;
    bar_w = rect_inner_cols(rect) - name_w - data_w * 2 - 8;
    if (bar_w < 6) bar_w = 6;

    for (int i = 0; i < visible_rows; i++) {
        int y = rect->row + 1 + i;
        int x = rect->col + 1;
        char pctbuf[8];
        const LuloDizkIoRow *row;

        if (i >= snap->iostat_count) break;
        row = &snap->iostats[i];
        plane_putn(p, y, x, theme->cyan, theme->bg, row->name, name_w);
        x += name_w + 1;
        render_inline_meter(p, theme, y, x, bar_w, row->util_pct, disk_usage_fill(theme, row->util_pct));
        x += bar_w + 1;
        snprintf(pctbuf, sizeof(pctbuf), "%3d%%", row->util_pct);
        plane_putn(p, y, x, disk_usage_text(theme, row->util_pct), theme->bg, pctbuf, 4);
        x += 5;
        plane_putn(p, y, x, theme->green, theme->bg, row->rd_bytes, data_w);
        x += data_w + 1;
        plane_putn(p, y, x, theme->orange, theme->bg, row->wr_bytes, data_w);
    }
}

static void render_disk_queue(struct ncplane *p, const Theme *theme, const LuloRect *rect,
                              const LuloDizkSnapshot *snap)
{
    int visible_rows;
    int tunable_rows;
    int swap_rows;

    if (!rect_valid(rect)) return;
    draw_inner_box(p, theme, rect, theme->border_panel, " Queue / Swap ", theme->white);
    if (snap) {
        char meta[32];
        snprintf(meta, sizeof(meta), "q %d  s %d", snap->tunable_count, snap->swap_count);
        draw_inner_meta(p, theme, rect, meta, theme->green);
    }

    visible_rows = rect_inner_rows(rect);
    tunable_rows = snap ? snap->tunable_count : 0;
    if (snap && snap->swap_count > 0 && visible_rows >= 3) {
        swap_rows = clamp_int(snap->swap_count, 1, visible_rows / 2);
        tunable_rows = clamp_int(tunable_rows, 0, visible_rows - swap_rows);
    } else {
        swap_rows = 0;
        tunable_rows = clamp_int(tunable_rows, 0, visible_rows);
    }

    if ((!snap || snap->tunable_count <= 0) && (!snap || snap->swap_count <= 0)) {
        plane_putn(p, rect->row + 1, rect->col + 2, theme->white, theme->bg,
                   "no queue or swap data", rect->width - 4);
        return;
    }

    for (int i = 0; i < tunable_rows; i++) {
        int y = rect->row + 1 + i;
        int x = rect->col + 1;
        int tail_w;
        char buf[64];
        const LuloDizkTunableRow *row = &snap->tunables[i];

        plane_putn(p, y, x, theme->cyan, theme->bg, row->name, 10);
        x += 11;
        plane_putn(p, y, x, theme->green, theme->bg, row->scheduler, 10);
        x += 11;
        snprintf(buf, sizeof(buf), "%s %d/%d", row->cache, row->read_ahead_kb, row->nr_requests);
        plane_putn(p, y, x, theme->yellow, theme->bg, buf, 12);
        x += 13;
        tail_w = rect->col + rect->width - 1 - x;
        if (tail_w > 0) plane_putn(p, y, x, theme->white, theme->bg, row->state, tail_w);
    }

    for (int i = 0; i < swap_rows; i++) {
        int y = rect->row + 1 + tunable_rows + i;
        int x = rect->col + 1;
        int bar_w = clamp_int(rect_inner_cols(rect) - 28, 8, 20);
        char pctbuf[8];
        char usedbuf[24];
        const LuloDizkSwapRow *row = &snap->swaps[i];

        plane_putn(p, y, x, theme->cyan, theme->bg, row->name, 10);
        x += 11;
        render_inline_meter(p, theme, y, x, bar_w, row->pct, disk_usage_fill(theme, row->pct));
        x += bar_w + 1;
        snprintf(pctbuf, sizeof(pctbuf), "%3d%%", row->pct);
        plane_putn(p, y, x, disk_usage_text(theme, row->pct), theme->bg, pctbuf, 4);
        x += 5;
        snprintf(usedbuf, sizeof(usedbuf), "%s/%s", row->used, row->size);
        plane_putn(p, y, x, theme->white, theme->bg, usedbuf, rect->col + rect->width - 1 - x);
    }
}

static void render_disk_widget(Ui *ui, const LuloDizkSnapshot *snap, const LuloDizkState *state)
{
    DiskWidgetLayout layout;
    unsigned rows = 0;
    unsigned cols = 0;

    if (!ui->disk) return;
    ncplane_dim_yx(ui->disk, &rows, &cols);
    plane_clear_inner(ui->disk, ui->theme, (int)rows, (int)cols);
    build_disk_widget_layout(ui, &layout);
    render_disk_filesystems(ui->disk, ui->theme, &layout.fs, snap, state);
    if (layout.show_dev) render_disk_devices(ui->disk, ui->theme, &layout.dev, snap);
    if (layout.show_io) render_disk_io(ui->disk, ui->theme, &layout.io, snap);
    if (layout.show_queue) render_disk_queue(ui->disk, ui->theme, &layout.queue, snap);
}

static void render_disk_status(Ui *ui, const LuloDizkSnapshot *snap, const LuloDizkState *state)
{
    char buf[160];
    int visible_rows;

    if (!ui->load) return;
    plane_reset(ui->load, ui->theme);
    visible_rows = disk_visible_rows(ui);
    if (!snap) return;
    snprintf(buf, sizeof(buf),
             "fs %d  dev %d  io %d  queue %d  swap %d  fstab %d  fs-scroll %d-%d/%d",
             snap->fs_count, snap->blockdev_count, snap->iostat_count,
             snap->tunable_count, snap->swap_count, snap->fstab_count,
             snap->fs_count > 0 ? state->scroll + 1 : 0,
             snap->fs_count > 0 ? clamp_int(state->scroll + visible_rows, 1, snap->fs_count) : 0,
             snap->fs_count > 0 ? snap->fs_count : 0);
    plane_putn(ui->load, 0, 0, ui->theme->white, ui->theme->bg, buf, ui->lo.load.width - 2);
}

static void render_static(Ui *ui, AppPage page, AppState *app)
{
    render_top_frame(ui);
    render_tabs(ui, page);
    render_footer(ui, page, app_status_current(app));
}

static void render_cpu_page(Ui *ui, const AppState *app, const CpuInfo *ci, DashboardState *dash,
                            const CpuStat *sa, const CpuStat *sb,
                            const CpuFreq *freqs, int nfreq,
                            const MemInfo *mi, const LoadInfo *li,
                            const LuloProcSnapshot *proc_snap, const LuloProcState *proc_state,
                            const double *cpu_temps, int ncpu_temp,
                            int append_sample, int render_proc)
{
    render_header_widget(ui, dash, app);
    render_cpu_widget(ui, ci, dash, sa, sb, freqs, nfreq, cpu_temps, ncpu_temp, append_sample);
    render_memory_widget(ui, mi);
    if (render_proc) render_process_widget(ui, proc_snap, proc_state);
    render_load_widget(ui, li, sb->ncpu);
}

static void render_disk_page(Ui *ui, const AppState *app, const DashboardState *dash,
                             const LuloDizkSnapshot *dizk_snap, const LuloDizkState *dizk_state)
{
    render_header_widget(ui, dash, app);
    render_disk_widget(ui, dizk_snap, dizk_state);
    render_disk_status(ui, dizk_snap, dizk_state);
}

static void render_process_only(Ui *ui, const LuloProcSnapshot *proc_snap, const LuloProcState *proc_state)
{
    render_process_widget(ui, proc_snap, proc_state);
}

static void render_disk_only(Ui *ui, const LuloDizkSnapshot *dizk_snap, const LuloDizkState *dizk_state)
{
    render_disk_widget(ui, dizk_snap, dizk_state);
    render_disk_status(ui, dizk_snap, dizk_state);
}

static void render_header_only(Ui *ui, const DashboardState *dash, const AppState *app)
{
    render_header_widget(ui, dash, app);
}

static void render_header_cpu(Ui *ui, const AppState *app, const CpuInfo *ci, DashboardState *dash,
                              const CpuStat *sa, const CpuStat *sb,
                              const CpuFreq *freqs, int nfreq,
                              const double *cpu_temps, int ncpu_temp)
{
    render_header_widget(ui, dash, app);
    render_cpu_widget(ui, ci, dash, sa, sb, freqs, nfreq, cpu_temps, ncpu_temp, 0);
}

static InputAction decode_notcurses_input(uint32_t id)
{
    switch (id) {
    case 'q':
    case 'Q':
    case NCKEY_ESC:
    case NCKEY_ENTER:
    case '\r':
    case '\n':
        return INPUT_QUIT;
    case '+':
    case '=':
        return INPUT_SAMPLE_FASTER;
    case '-':
    case '_':
        return INPUT_SAMPLE_SLOWER;
    case 't':
    case 'T':
        return INPUT_TOGGLE_HEAT;
    case 'p':
    case 'P':
        return INPUT_TOGGLE_PROC_CPU;
    case 'r':
    case 'R':
        return INPUT_CYCLE_PROC_REFRESH;
    case '\t':
    case ']':
        return INPUT_TAB_NEXT;
    case '[':
        return INPUT_TAB_PREV;
    case ' ':
        return INPUT_TOGGLE_BRANCH;
    case 'c':
        return INPUT_COLLAPSE_ALL;
    case 'e':
    case 'E':
        return INPUT_EXPAND_ALL;
    case 'x':
        return INPUT_SIGNAL_TERM;
    case 'X':
        return INPUT_SIGNAL_KILL;
    case 'k':
    case 'K':
    case NCKEY_UP:
    case NCKEY_SCROLL_UP:
        return INPUT_SCROLL_UP;
    case 'j':
    case 'J':
    case NCKEY_DOWN:
    case NCKEY_SCROLL_DOWN:
        return INPUT_SCROLL_DOWN;
    case 'u':
    case 'U':
    case NCKEY_PGUP:
        return INPUT_PAGE_UP;
    case 'd':
    case 'D':
    case NCKEY_PGDOWN:
        return INPUT_PAGE_DOWN;
    case 'g':
    case NCKEY_HOME:
        return INPUT_HOME;
    case 'G':
    case NCKEY_END:
        return INPUT_END;
    case NCKEY_LEFT:
        return INPUT_TAB_PREV;
    case NCKEY_RIGHT:
        return INPUT_TAB_NEXT;
    case NCKEY_RESIZE:
        return INPUT_RESIZE;
    default:
        return INPUT_NONE;
    }
}

static const LuloProcRow *selected_proc_row(const LuloProcSnapshot *snap, const LuloProcState *state)
{
    if (!snap || !state || snap->count <= 0) return NULL;
    if (state->selected >= 0 && state->selected < snap->count) return &snap->rows[state->selected];
    if (state->selected_pid > 0) {
        for (int i = 0; i < snap->count; i++) {
            if (snap->rows[i].pid == state->selected_pid &&
                snap->rows[i].is_thread == state->selected_is_thread) {
                return &snap->rows[i];
            }
        }
    }
    return &snap->rows[0];
}

static int signal_selected_process(const LuloProcSnapshot *snap, const LuloProcState *state,
                                   int sig, AppState *app)
{
    const LuloProcRow *row = selected_proc_row(snap, state);
    int target;

    if (!row) {
        app_status_set(app, "no process selected");
        return 0;
    }
    target = row->is_thread && row->tgid > 0 ? row->tgid : row->pid;
    if (target <= 0) {
        app_status_set(app, "invalid pid");
        return 0;
    }
    if (kill(target, sig) < 0) {
        app_status_set(app, "signal %d to %d failed: %s", sig, target, strerror(errno));
        return 0;
    }
    app_status_set(app, "sent %s to %d", strsignal(sig), target);
    return 1;
}

static int handle_tab_click(Ui *ui, int global_y, int global_x, AppState *app, RenderFlags *render)
{
    int tab_row = ui->lo.header.row + ui->lo.header.height;

    if (global_y != tab_row) return 0;
    for (int i = 0; i < APP_PAGE_COUNT; i++) {
        if (global_x >= ui->tab_hits[i].x && global_x < ui->tab_hits[i].x + ui->tab_hits[i].width) {
            if (app->active_page != (AppPage)i) {
                app->active_page = (AppPage)i;
                render->need_rebuild = 1;
                render->need_render = 1;
            }
            return 1;
        }
    }
    return 0;
}

static void schedule_proc_selection_redraw(RenderFlags *render,
                                           int prev_selected, int prev_scroll,
                                           int new_selected, int new_scroll);

static int handle_proc_click(Ui *ui, int global_y, int global_x,
                             const LuloProcSnapshot *snap, LuloProcState *proc_state,
                             RenderFlags *render)
{
    int inner_left = ui->lo.proc.col + 1;
    int inner_right = ui->lo.proc.col + ui->lo.proc.width - 1;

    if (!ui->proc || !snap || snap->count <= 0) return 0;
    if (global_x < inner_left || global_x >= inner_right) return 0;
    if (global_y == ui->lo.proc.row && ui->proc_table.controls_visible) {
        if (global_x >= ui->proc_table.collapse_all_x &&
            global_x < ui->proc_table.collapse_all_x + ui->proc_table.collapse_all_w) {
            if (lulo_proc_collapse_all(proc_state, snap) > 0) {
                render->need_proc_refresh = 1;
                render->need_render = 1;
            }
            return 1;
        }
        if (global_x >= ui->proc_table.expand_all_x &&
            global_x < ui->proc_table.expand_all_x + ui->proc_table.expand_all_w) {
            lulo_proc_expand_all(proc_state);
            render->need_proc_refresh = 1;
            render->need_render = 1;
            return 1;
        }
    }
    if (global_y == ui->lo.proc.row + 1) {
        int local_x = global_x - ui->lo.proc.col;

        for (int i = 0; i < LULO_PROC_SORT_COUNT; i++) {
            ProcHeaderHit *hit = &ui->proc_table.headers[i];
            if (!hit->visible) continue;
            if (local_x >= hit->x && local_x < hit->x + hit->width) {
                lulo_proc_sort_toggle(proc_state, hit->sort_key);
                render->need_proc_refresh = 1;
                render->need_render = 1;
                return 1;
            }
        }
        return 1;
    }
    if (global_y < ui->lo.proc.row + 2 || global_y >= ui->lo.proc.row + ui->lo.proc.height - 1) return 0;
    {
        int row_slot = global_y - (ui->lo.proc.row + 2);
        int row_index = proc_state->scroll + row_slot;

        if (row_index < 0 || row_index >= snap->count) return 0;
        render->proc_prev_selected = proc_state->selected;
        render->proc_prev_scroll = proc_state->scroll;
        proc_state->selected = row_index;
        proc_state->selected_pid = snap->rows[row_index].pid;
        proc_state->selected_is_thread = snap->rows[row_index].is_thread;
        if (snap->rows[row_index].has_children) {
            if (lulo_proc_toggle_row(proc_state, snap, row_index) > 0) {
                render->need_proc_refresh = 1;
            }
        } else {
            lulo_proc_view_sync(proc_state, snap, ui->lo.proc_rows);
            schedule_proc_selection_redraw(render, render->proc_prev_selected, render->proc_prev_scroll,
                                           proc_state->selected, proc_state->scroll);
        }
        render->need_render = 1;
        return 1;
    }
}

static int handle_mouse_click(Ui *ui, int global_y, int global_x, AppState *app,
                              const LuloProcSnapshot *proc_snap, LuloProcState *proc_state,
                              RenderFlags *render)
{
    if (handle_tab_click(ui, global_y, global_x, app, render)) return 1;
    if (app->active_page == APP_PAGE_CPU &&
        handle_proc_click(ui, global_y, global_x, proc_snap, proc_state, render)) {
        return 1;
    }
    return 0;
}

static void schedule_proc_selection_redraw(RenderFlags *render,
                                           int prev_selected, int prev_scroll,
                                           int new_selected, int new_scroll)
{
    if (!render) return;
    if (render->need_proc || render->need_proc_refresh || render->need_proc_body_only) return;
    if (new_scroll != prev_scroll || new_selected == prev_selected) {
        render->need_proc_body_only = 1;
        render->need_proc_cursor_only = 0;
        return;
    }
    if (render->need_proc_cursor_only) {
        render->need_proc_body_only = 1;
        render->need_proc_cursor_only = 0;
        return;
    }
    render->need_proc_cursor_only = 1;
    render->proc_prev_selected = prev_selected;
    render->proc_prev_scroll = prev_scroll;
}

static void apply_input_action(Ui *ui, InputAction action, AppState *app,
                               const LuloProcSnapshot *proc_snap, LuloProcState *proc_state,
                               const LuloDizkSnapshot *dizk_snap, LuloDizkState *dizk_state,
                               DashboardState *dash, int *sample_ms, long long *deadline,
                               int *exit_requested, int *need_resize, RenderFlags *render,
                               int scroll_units)
{
    int prev_selected = proc_state ? proc_state->selected : 0;
    int prev_scroll = proc_state ? proc_state->scroll : 0;

    switch (action) {
    case INPUT_QUIT:
        *exit_requested = 1;
        break;
    case INPUT_SAMPLE_FASTER:
        *sample_ms = lulo_adjust_sample_ms(*sample_ms, -1);
        dash->sample_ms = *sample_ms;
        *deadline = mono_ms_now() + *sample_ms;
        render->need_header = 1;
        render->need_render = 1;
        break;
    case INPUT_SAMPLE_SLOWER:
        *sample_ms = lulo_adjust_sample_ms(*sample_ms, +1);
        dash->sample_ms = *sample_ms;
        *deadline = mono_ms_now() + *sample_ms;
        render->need_header = 1;
        render->need_render = 1;
        break;
    case INPUT_TOGGLE_HEAT:
        dash->heat_mode = lulo_next_heat_mode(dash->heat_mode);
        if (app->active_page == APP_PAGE_CPU) render->need_cpu = 1;
        else render->need_header = 1;
        render->need_render = 1;
        break;
    case INPUT_TOGGLE_PROC_CPU:
        app->proc_cpu_mode = lulo_next_proc_cpu_mode(app->proc_cpu_mode);
        app_status_set(app, "proc cpu %s", lulo_proc_cpu_mode_name(app->proc_cpu_mode));
        render->need_header = 1;
        render->need_footer = 1;
        if (app->active_page == APP_PAGE_CPU) render->need_proc_refresh = 1;
        render->need_render = 1;
        break;
    case INPUT_CYCLE_PROC_REFRESH:
        app->proc_refresh_ms = next_proc_refresh_ms(app->proc_refresh_ms);
        app_status_set(app, "proc refresh %dms",
                       effective_proc_refresh_ms(*sample_ms, app->proc_refresh_ms));
        render->need_header = 1;
        render->need_footer = 1;
        if (app->active_page == APP_PAGE_CPU) render->need_proc_refresh = 1;
        render->need_render = 1;
        break;
    case INPUT_SCROLL_UP:
        if (app->active_page == APP_PAGE_CPU) {
            int delta = scroll_units > 0 ? scroll_units : 1;

            lulo_proc_view_move(proc_state, proc_snap, ui->lo.proc_rows, -delta);
            schedule_proc_selection_redraw(render, prev_selected, prev_scroll,
                                           proc_state->selected, proc_state->scroll);
        } else {
            int delta = scroll_units > 0 ? scroll_units : 1;

            lulo_dizk_view_move(dizk_state, dizk_snap, disk_visible_rows(ui), -delta);
            render->need_disk = 1;
        }
        render->need_render = 1;
        break;
    case INPUT_SCROLL_DOWN:
        if (app->active_page == APP_PAGE_CPU) {
            int delta = scroll_units > 0 ? scroll_units : 1;

            lulo_proc_view_move(proc_state, proc_snap, ui->lo.proc_rows, +delta);
            schedule_proc_selection_redraw(render, prev_selected, prev_scroll,
                                           proc_state->selected, proc_state->scroll);
        } else {
            int delta = scroll_units > 0 ? scroll_units : 1;

            lulo_dizk_view_move(dizk_state, dizk_snap, disk_visible_rows(ui), +delta);
            render->need_disk = 1;
        }
        render->need_render = 1;
        break;
    case INPUT_PAGE_UP:
        if (app->active_page == APP_PAGE_CPU) {
            lulo_proc_view_page(proc_state, proc_snap, ui->lo.proc_rows, -1);
            schedule_proc_selection_redraw(render, prev_selected, prev_scroll,
                                           proc_state->selected, proc_state->scroll);
        } else {
            lulo_dizk_view_page(dizk_state, dizk_snap, disk_visible_rows(ui), -1);
            render->need_disk = 1;
        }
        render->need_render = 1;
        break;
    case INPUT_PAGE_DOWN:
        if (app->active_page == APP_PAGE_CPU) {
            lulo_proc_view_page(proc_state, proc_snap, ui->lo.proc_rows, +1);
            schedule_proc_selection_redraw(render, prev_selected, prev_scroll,
                                           proc_state->selected, proc_state->scroll);
        } else {
            lulo_dizk_view_page(dizk_state, dizk_snap, disk_visible_rows(ui), +1);
            render->need_disk = 1;
        }
        render->need_render = 1;
        break;
    case INPUT_HOME:
        if (app->active_page == APP_PAGE_CPU) {
            lulo_proc_view_home(proc_state, proc_snap, ui->lo.proc_rows);
            schedule_proc_selection_redraw(render, prev_selected, prev_scroll,
                                           proc_state->selected, proc_state->scroll);
        } else {
            lulo_dizk_view_home(dizk_state);
            render->need_disk = 1;
        }
        render->need_render = 1;
        break;
    case INPUT_END:
        if (app->active_page == APP_PAGE_CPU) {
            lulo_proc_view_end(proc_state, proc_snap, ui->lo.proc_rows);
            schedule_proc_selection_redraw(render, prev_selected, prev_scroll,
                                           proc_state->selected, proc_state->scroll);
        } else {
            lulo_dizk_view_end(dizk_state, dizk_snap, disk_visible_rows(ui));
            render->need_disk = 1;
        }
        render->need_render = 1;
        break;
    case INPUT_TAB_NEXT:
        app->active_page = (AppPage)((app->active_page + 1) % APP_PAGE_COUNT);
        render->need_rebuild = 1;
        render->need_render = 1;
        break;
    case INPUT_TAB_PREV:
        app->active_page = (AppPage)((app->active_page + APP_PAGE_COUNT - 1) % APP_PAGE_COUNT);
        render->need_rebuild = 1;
        render->need_render = 1;
        break;
    case INPUT_TOGGLE_BRANCH:
        if (app->active_page == APP_PAGE_CPU &&
            lulo_proc_toggle_row(proc_state, proc_snap, proc_state->selected) > 0) {
            render->need_proc_refresh = 1;
            render->need_render = 1;
        }
        break;
    case INPUT_COLLAPSE_ALL:
        if (app->active_page == APP_PAGE_CPU && lulo_proc_collapse_all(proc_state, proc_snap) > 0) {
            render->need_proc_refresh = 1;
            render->need_render = 1;
        }
        break;
    case INPUT_EXPAND_ALL:
        if (app->active_page == APP_PAGE_CPU) {
            lulo_proc_expand_all(proc_state);
            render->need_proc_refresh = 1;
            render->need_render = 1;
        }
        break;
    case INPUT_SIGNAL_TERM:
        if (app->active_page == APP_PAGE_CPU) {
            int ok = signal_selected_process(proc_snap, proc_state, SIGTERM, app);
            render->need_footer = 1;
            if (ok) render->need_proc_refresh = 1;
            render->need_render = 1;
        }
        break;
    case INPUT_SIGNAL_KILL:
        if (app->active_page == APP_PAGE_CPU) {
            int ok = signal_selected_process(proc_snap, proc_state, SIGKILL, app);
            render->need_footer = 1;
            if (ok) render->need_proc_refresh = 1;
            render->need_render = 1;
        }
        break;
    case INPUT_RESIZE:
        *need_resize = 1;
        break;
    case INPUT_NONE:
    default:
        break;
    }
}

static void render_pending(Ui *ui, AppState *app, const CpuInfo *ci, DashboardState *dash,
                           const CpuStat *sa, const CpuStat *sb,
                           const CpuFreq *freqs, int nfreq,
                           const double *cpu_temps, int ncpu_temp,
                           const LuloProcSnapshot *proc_snap, const LuloProcState *proc_state,
                           const LuloDizkSnapshot *dizk_snap, const LuloDizkState *dizk_state,
                           const RenderFlags *render)
{
    if (!render->need_render) return;
    if (render->need_tabs) render_tabs(ui, app->active_page);
    if (render->need_footer) render_footer(ui, app->active_page, app_status_current(app));
    if (app->active_page == APP_PAGE_CPU) {
        if (render->need_cpu) render_header_cpu(ui, app, ci, dash, sa, sb, freqs, nfreq, cpu_temps, ncpu_temp);
        else if (render->need_header) render_header_only(ui, dash, app);
        if (render->need_proc) render_process_only(ui, proc_snap, proc_state);
        else if (render->need_proc_body_only) render_process_body_only(ui, proc_snap, proc_state);
        else if (render->need_proc_cursor_only) {
            render_process_cursor_only(ui, proc_snap, proc_state,
                                       render->proc_prev_selected, render->proc_prev_scroll);
        }
    } else {
        if (render->need_header) render_header_only(ui, dash, app);
        if (render->need_disk || render->need_load) render_disk_only(ui, dizk_snap, dizk_state);
    }
    notcurses_render(ui->nc);
}

int main(int argc, char *argv[])
{
    int no_color = 0;
    int opt;
    int sample_ms = 1000;
    int append_sample = 1;
    int exit_requested = 0;
    int need_resize = 1;
    int need_rebuild = 1;
    int last_w = -1;
    int last_h = -1;
    InputBackend requested_backend = INPUT_BACKEND_AUTO;
    InputBackend input_backend = INPUT_BACKEND_RAW;
    CpuInfo ci;
    CpuStat stat_a;
    CpuStat stat_b;
    DashboardState dash;
    LuloProcState proc_state;
    LuloDizkState dizk_state;
    LuloProcSnapshot proc_snap = {0};
    AppState app = {
        .active_page = APP_PAGE_CPU,
        .proc_refresh_ms = 1000,
        .proc_cpu_mode = LULO_PROC_CPU_PER_CORE,
    };
    Ui ui;
    RawInput rawin;
    NcInputThread ncin;
    notcurses_options opts;
    DebugLog dlog;
    const char *env_input_backend;
    unsigned long long proc_cpu_accum_delta = 0;
    unsigned long long proc_last_total_delta = 0;
    long long proc_due_ms = 0;
    int proc_snapshot_valid = 0;
    LuloProcCpuMode proc_snapshot_mode = LULO_PROC_CPU_PER_CORE;

    while ((opt = getopt(argc, argv, "ni:h")) != -1) {
        switch (opt) {
        case 'n':
            no_color = 1;
            break;
        case 'i':
            if (parse_input_backend(optarg, &requested_backend) < 0) {
                fprintf(stderr, "invalid input backend '%s' (use auto|nc|raw)\n", optarg);
                return 1;
            }
            break;
        case 'h':
            fprintf(stderr,
                    "Usage: lulo [-n] [-i auto|nc|raw] [-h]\n"
                    "  -n  monochrome theme\n"
                    "  -i  input backend: auto, nc, raw\n"
                    "  -h  this help\n");
            return 0;
        default:
            return 1;
        }
    }

    env_input_backend = getenv("LULO_INPUT");
    if (requested_backend == INPUT_BACKEND_AUTO && env_input_backend && *env_input_backend) {
        if (parse_input_backend(env_input_backend, &requested_backend) < 0) {
            fprintf(stderr, "invalid LULO_INPUT '%s' (use auto|nc|raw)\n", env_input_backend);
            return 1;
        }
    }
    input_backend = requested_backend == INPUT_BACKEND_AUTO ? auto_input_backend() : requested_backend;

    memset(&ui, 0, sizeof(ui));
    debug_log_open(&dlog);
    setlocale(LC_ALL, "");
    memset(&opts, 0, sizeof(opts));
    opts.flags = NCOPTION_SUPPRESS_BANNERS | NCOPTION_INHIBIT_SETLOCALE;
    ui.theme = no_color ? &theme_mono : &theme_color;
    debug_log_stage(&dlog, "before_init");
    ui.nc = notcurses_init(&opts, NULL);
    if (!ui.nc) {
        fprintf(stderr, "failed to initialize Notcurses\n");
        debug_log_close(&dlog);
        return 1;
    }
    debug_log_stage(&dlog, "after_init");
    ui.std = notcurses_stdplane(ui.nc);
    debug_log_message(&dlog, "input-backend-requested", input_backend_name(requested_backend));

    if (input_backend == INPUT_BACKEND_NOTCURSES) {
        notcurses_mice_enable(ui.nc, NCMICE_BUTTON_EVENT);
    }

    lulo_gather_cpu_info(&ci);
    lulo_dashboard_init(&dash, &ci, sample_ms);
    lulo_proc_state_init(&proc_state);
    lulo_dizk_state_init(&dizk_state);
    if (lulo_read_cpu_stat(&stat_a) < 0) {
        fprintf(stderr, "failed to read /proc/stat\n");
        notcurses_stop(ui.nc);
        debug_log_close(&dlog);
        return 1;
    }
    sleep_ms(200);
    if (lulo_read_cpu_stat(&stat_b) < 0) {
        fprintf(stderr, "failed to read /proc/stat\n");
        lulo_proc_state_cleanup(&proc_state);
        notcurses_stop(ui.nc);
        debug_log_close(&dlog);
        return 1;
    }
    if (input_backend == INPUT_BACKEND_RAW) {
        if (raw_input_enable(&rawin) < 0) {
            fprintf(stderr, "failed to switch terminal to raw input mode\n");
            lulo_proc_state_cleanup(&proc_state);
            lulo_dizk_state_cleanup(&dizk_state);
            notcurses_stop(ui.nc);
            debug_log_close(&dlog);
            return 1;
        }
        terminal_mouse_enable();
    } else if (nc_input_start(&ncin, ui.nc, &dlog) < 0) {
        fprintf(stderr, "failed to start Notcurses input thread\n");
        lulo_proc_state_cleanup(&proc_state);
        lulo_dizk_state_cleanup(&dizk_state);
        notcurses_stop(ui.nc);
        debug_log_close(&dlog);
        return 1;
    }
    debug_log_message(&dlog, "input-backend-active", input_backend_name(input_backend));
    debug_log_stage(&dlog, "after_input_enable");

    while (!exit_requested) {
        int term_rows = last_h;
        int term_cols = last_w;
        CpuFreq freqs[LULO_MAX_CPUS] = {0};
        MemInfo mi = {0};
        LoadInfo li = {0};
        LuloDizkSnapshot dizk_snap = {0};
        double cpu_temps[64] = {0};
        int nfreq = 0;
        int ncpu_temp = 0;
        int full_redraw = 0;
        int proc_refreshed = 0;
        unsigned long long total_delta;

        if (terminal_get_size(&term_rows, &term_cols) == 0 &&
            (term_rows != last_h || term_cols != last_w)) {
            need_resize = 1;
            need_rebuild = 1;
        }
        if (app.active_page == APP_PAGE_CPU) {
            nfreq = lulo_gather_cpu_freq(freqs, LULO_MAX_CPUS);
            ncpu_temp = lulo_read_cpu_temps(cpu_temps, 64);
        }
        if (need_rebuild) {
            if (need_resize && notcurses_refresh(ui.nc, NULL, NULL) < 0) break;
            if (terminal_get_size(&term_rows, &term_cols) < 0) {
                unsigned rows;
                unsigned cols;
                ncplane_dim_yx(ui.std, &rows, &cols);
                term_rows = (int)rows;
                term_cols = (int)cols;
            }
            {
                TopLayout lo = lulo_build_layout(term_cols, term_rows, stat_b.ncpu,
                                                 app.active_page == APP_PAGE_CPU && ncpu_temp > 0);
                if (ui_rebuild_planes(&ui, &lo, app.active_page) < 0) {
                    fprintf(stderr, "failed to create widget planes\n");
                    break;
                }
            }
            render_static(&ui, app.active_page, &app);
            last_h = term_rows;
            last_w = term_cols;
            need_resize = 0;
            need_rebuild = 0;
            full_redraw = 1;
        }

        total_delta = lulo_cpu_total_delta(&stat_a, &stat_b);
        if (append_sample) proc_cpu_accum_delta += total_delta;
        if (append_sample && app.active_page != APP_PAGE_CPU) {
            for (int i = 0; i < stat_b.ncpu; i++) {
                int heat_raw = lulo_cpu_heat_pct(&stat_a.cpu[i + 1], &stat_b.cpu[i + 1], HEAT_MODE_RAW);
                int heat_task = lulo_cpu_heat_pct(&stat_a.cpu[i + 1], &stat_b.cpu[i + 1], HEAT_MODE_TASK);
                lulo_dashboard_append_heat(&dash, i, heat_raw, heat_task);
            }
        }

        if (app.active_page == APP_PAGE_CPU) {
            lulo_gather_meminfo(&mi);
            lulo_gather_loadavg(&li);
            if (!proc_snapshot_valid || proc_snapshot_mode != app.proc_cpu_mode || mono_ms_now() >= proc_due_ms) {
                unsigned long long proc_delta = proc_cpu_accum_delta > 0 ? proc_cpu_accum_delta :
                                                (proc_last_total_delta > 0 ? proc_last_total_delta : total_delta);

                lulo_proc_snapshot_free(&proc_snap);
                if (lulo_proc_snapshot_gather(&proc_snap, &proc_state, proc_delta, mi.total, stat_b.ncpu,
                                              app.proc_cpu_mode) < 0) {
                    fprintf(stderr, "failed to gather process snapshot\n");
                    break;
                }
                proc_snapshot_valid = 1;
                proc_snapshot_mode = app.proc_cpu_mode;
                proc_refreshed = 1;
                proc_last_total_delta = proc_delta;
                proc_cpu_accum_delta = 0;
                proc_due_ms = mono_ms_now() + effective_proc_refresh_ms(sample_ms, app.proc_refresh_ms);
            }
            lulo_proc_view_sync(&proc_state, &proc_snap, ui.lo.proc_rows);
        } else {
            if (lulo_dizk_snapshot_gather(&dizk_snap, &dizk_state, ui.lo.top.width - 8) < 0) {
                fprintf(stderr, "failed to gather dizk snapshot\n");
                break;
            }
            lulo_dizk_view_sync(&dizk_state, &dizk_snap, disk_visible_rows(&ui));
        }

        if (full_redraw) render_static(&ui, app.active_page, &app);
        if (app.active_page == APP_PAGE_CPU) {
            render_cpu_page(&ui, &app, &ci, &dash, &stat_a, &stat_b, freqs, nfreq, &mi, &li,
                            &proc_snap, &proc_state, cpu_temps, ncpu_temp, append_sample,
                            full_redraw || proc_refreshed);
        } else {
            render_disk_page(&ui, &app, &dash, &dizk_snap, &dizk_state);
        }
        debug_log_stage(&dlog, "before_render");
        if (notcurses_render(ui.nc) < 0) {
            lulo_dizk_snapshot_free(&dizk_snap);
            break;
        }
        debug_log_stage(&dlog, "after_render");
        append_sample = 0;

        {
            long long deadline = mono_ms_now() + sample_ms;
            int resample = 0;
            struct pollfd pfd = {
                .fd = input_backend == INPUT_BACKEND_RAW ? STDIN_FILENO : ncin.pipefd[0],
                .events = POLLIN
            };

            while (!resample && !exit_requested) {
                RenderFlags render = {0};
                int timeout_ms = ms_until_deadline(deadline);

                if (timeout_ms > 100) timeout_ms = 100;
                if (timeout_ms < 0) timeout_ms = 0;

                if (input_backend == INPUT_BACKEND_RAW) {
                    DecodedInput in;
                    int pr = poll(&pfd, 1, timeout_ms);

                    if (pr < 0) {
                        if (errno == EINTR) {
                            if (terminal_get_size(&term_rows, &term_cols) == 0 &&
                                (term_rows != last_h || term_cols != last_w)) {
                                need_resize = 1;
                                need_rebuild = 1;
                                resample = 1;
                            }
                            continue;
                        }
                        debug_log_errno(&dlog, "poll-error");
                        exit_requested = 1;
                        break;
                    }
                    if (pr == 0) {
                        debug_log_poll(&dlog, "poll-timeout", pfd.fd, 0);
                        if (terminal_get_size(&term_rows, &term_cols) == 0 &&
                            (term_rows != last_h || term_cols != last_w)) {
                            need_resize = 1;
                            need_rebuild = 1;
                            resample = 1;
                            continue;
                        }
                        if (ms_until_deadline(deadline) == 0) resample = 1;
                        continue;
                    }
                    debug_log_poll(&dlog, "poll-ready", pfd.fd, pfd.revents);
                    if (!(pfd.revents & POLLIN)) {
                        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                            debug_log_errno(&dlog, "poll-error");
                        }
                        if (ms_until_deadline(deadline) == 0) resample = 1;
                        continue;
                    }

                    {
                        ssize_t nr = raw_input_fill(&rawin);
                        if (nr < 0) {
                            debug_log_errno(&dlog, "read-error");
                            exit_requested = 1;
                            break;
                        }
                        if (nr > 0) debug_log_bytes(&dlog, "read", rawin.buf, rawin.len);
                    }

                    while (raw_input_decode_one(&rawin, &in)) {
                        debug_log_action(&dlog, "dispatch", &in);
                        if (in.mouse_press && in.mouse_button == 1) {
                            handle_mouse_click(&ui, in.mouse_y, in.mouse_x, &app, &proc_snap, &proc_state, &render);
                        } else {
                            int scroll_units = scroll_units_for_input(&app, in.action, in.mouse_wheel, in.key_repeat);

                            apply_input_action(&ui, in.action, &app, &proc_snap, &proc_state, &dizk_snap, &dizk_state,
                                               &dash, &sample_ms, &deadline, &exit_requested, &need_resize, &render,
                                               scroll_units);
                        }
                        if (need_resize || render.need_rebuild || exit_requested) break;
                    }
                } else {
                    int pr = poll(&pfd, 1, timeout_ms);

                    if (pr < 0) {
                        if (errno == EINTR) {
                            if (terminal_get_size(&term_rows, &term_cols) == 0 &&
                                (term_rows != last_h || term_cols != last_w)) {
                                need_resize = 1;
                                need_rebuild = 1;
                                resample = 1;
                            }
                            continue;
                        }
                        debug_log_errno(&dlog, "nc-poll-error");
                        exit_requested = 1;
                        break;
                    }
                    if (pr == 0) {
                        debug_log_message(&dlog, "nc-timeout", "0");
                        if (terminal_get_size(&term_rows, &term_cols) == 0 &&
                            (term_rows != last_h || term_cols != last_w)) {
                            need_resize = 1;
                            need_rebuild = 1;
                            resample = 1;
                            continue;
                        }
                        if (ms_until_deadline(deadline) == 0) resample = 1;
                        continue;
                    }
                    debug_log_poll(&dlog, "nc-poll-ready", pfd.fd, pfd.revents);
                    if (!(pfd.revents & POLLIN)) {
                        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                            debug_log_errno(&dlog, "nc-poll-error");
                        }
                        if (ms_until_deadline(deadline) == 0) resample = 1;
                        continue;
                    }
                    nc_input_begin_drain(&ncin);
                    for (;;) {
                        ncinput ni;
                        uint32_t id;
                        InputAction action;

                        if (!nc_input_pop(&ncin, &id, &ni)) break;
                        action = decode_notcurses_input(id);
                        debug_log_nc_event(&dlog, "dispatch-nc", id, &ni);
                        if (id == NCKEY_BUTTON1 && ni.evtype != NCTYPE_RELEASE) {
                            handle_mouse_click(&ui, ni.y + 1, ni.x + 1, &app, &proc_snap, &proc_state, &render);
                        } else {
                            int scroll_units = scroll_units_for_input(&app, action,
                                                                       id == NCKEY_SCROLL_UP || id == NCKEY_SCROLL_DOWN,
                                                                       ni.evtype == NCTYPE_REPEAT);

                            apply_input_action(&ui, action, &app, &proc_snap, &proc_state, &dizk_snap, &dizk_state,
                                               &dash, &sample_ms, &deadline, &exit_requested, &need_resize, &render,
                                               scroll_units);
                        }
                        if (need_resize || render.need_rebuild || exit_requested) break;
                    }
                }

                if (render.need_proc_refresh && app.active_page == APP_PAGE_CPU) {
                    unsigned long long proc_delta = proc_cpu_accum_delta > 0 ? proc_cpu_accum_delta :
                                                    (proc_last_total_delta > 0 ? proc_last_total_delta : total_delta);

                    lulo_proc_snapshot_free(&proc_snap);
                    if (lulo_proc_snapshot_gather(&proc_snap, &proc_state, proc_delta, mi.total, stat_b.ncpu,
                                                  app.proc_cpu_mode) < 0) {
                        exit_requested = 1;
                        break;
                    }
                    proc_snapshot_valid = 1;
                    proc_snapshot_mode = app.proc_cpu_mode;
                    proc_last_total_delta = proc_delta;
                    proc_cpu_accum_delta = 0;
                    proc_due_ms = mono_ms_now() + effective_proc_refresh_ms(sample_ms, app.proc_refresh_ms);
                    lulo_proc_view_sync(&proc_state, &proc_snap, ui.lo.proc_rows);
                    render.need_proc = 1;
                }
                if (render.need_disk_refresh && app.active_page == APP_PAGE_DIZK) {
                    lulo_dizk_snapshot_free(&dizk_snap);
                    if (lulo_dizk_snapshot_gather(&dizk_snap, &dizk_state, ui.lo.top.width - 8) < 0) {
                        exit_requested = 1;
                        break;
                    }
                    lulo_dizk_view_sync(&dizk_state, &dizk_snap, disk_visible_rows(&ui));
                    render.need_disk = 1;
                }
                if (render.need_rebuild) {
                    need_rebuild = 1;
                    resample = 1;
                    continue;
                }

                if (!need_resize && !exit_requested) {
                    render_pending(&ui, &app, &ci, &dash, &stat_a, &stat_b, freqs, nfreq,
                                   cpu_temps, ncpu_temp, &proc_snap, &proc_state, &dizk_snap, &dizk_state, &render);
                }
                if (ms_until_deadline(deadline) == 0) resample = 1;
            }
        }

        lulo_dizk_snapshot_free(&dizk_snap);
        if (exit_requested) break;
        if (need_resize || need_rebuild) continue;
        stat_a = stat_b;
        if (lulo_read_cpu_stat(&stat_b) < 0) break;
        append_sample = 1;
    }

    ui_destroy_planes(&ui);
    if (input_backend == INPUT_BACKEND_RAW) {
        terminal_mouse_disable();
        raw_input_disable(&rawin);
    } else {
        nc_input_stop(&ncin);
    }
    lulo_proc_snapshot_free(&proc_snap);
    lulo_proc_state_cleanup(&proc_state);
    lulo_dizk_state_cleanup(&dizk_state);
    notcurses_stop(ui.nc);
    debug_log_close(&dlog);
    return exit_requested ? 0 : 1;
}
