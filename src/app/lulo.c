/* lulo.c — Notcurses frontend for lulo
 *
 * Build:
 *   make
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
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "lulo_app.h"
#include "lulo_edit.h"

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

void plane_reset(struct ncplane *p, const Theme *theme)
{
    ncplane_set_base(p, " ", 0, rgb_channels(theme->fg, theme->bg));
    ncplane_erase(p);
}

void plane_putn(struct ncplane *p, int y, int x, Rgb fg, Rgb bg, const char *text, int width)
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

void plane_fill(struct ncplane *p, int y, int x, int width, Rgb fg, Rgb bg)
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

int clamp_int(int value, int lo, int hi)
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

static const char *app_page_name(AppPage page)
{
    switch (page) {
    case APP_PAGE_SCHED:
        return "SCHED";
    case APP_PAGE_CGROUPS:
        return "CGROUPS";
    case APP_PAGE_UDEV:
        return "UDEV";
    case APP_PAGE_TUNE:
        return "TUNE";
    case APP_PAGE_SYSTEMD:
        return "SYSTEMD";
    case APP_PAGE_DIZK:
        return "DISK";
    case APP_PAGE_CPU:
    default:
        return "CPU";
    }
}

static const char *footer_hint(AppPage page)
{
    switch (page) {
    case APP_PAGE_SCHED:
    case APP_PAGE_CGROUPS:
    case APP_PAGE_UDEV:
    case APP_PAGE_SYSTEMD:
    case APP_PAGE_TUNE:
        return "q exit   Tab page   Shift-Tab subview   ? help";
    case APP_PAGE_DIZK:
    case APP_PAGE_CPU:
    default:
        return "q exit   Tab page   ? help";
    }
}

static const char *help_title(AppPage page)
{
    switch (page) {
    case APP_PAGE_SCHED:
        return " Scheduler Help ";
    case APP_PAGE_CGROUPS:
        return " Cgroups Help ";
    case APP_PAGE_UDEV:
        return " Udev Help ";
    case APP_PAGE_TUNE:
        return " Tunables Help ";
    case APP_PAGE_SYSTEMD:
        return " Systemd Help ";
    case APP_PAGE_DIZK:
        return " Disk Help ";
    case APP_PAGE_CPU:
    default:
        return " CPU Help ";
    }
}

typedef struct {
    const char *keys;
    const char *desc;
} HelpEntry;

static const HelpEntry *help_entries(AppPage page, int *count)
{
    static const HelpEntry cpu_lines[] = {
        {"Tab", "switch top-level pages"},
        {"j/k or up/down", "move the process selection"},
        {"left/right", "scroll long commands horizontally"},
        {"PgUp/PgDn, Home/End", "jump the process list"},
        {"space", "toggle the selected branch"},
        {"c / e", "collapse or expand all branches"},
        {"x / X", "send TERM or KILL to selected process"},
        {"+ / -", "change sample interval"},
        {"p / r", "change proc CPU mode and proc refresh"},
        {"? / Esc / click", "close help"},
    };
    static const HelpEntry disk_lines[] = {
        {"Tab", "switch top-level pages"},
        {"j/k or arrows", "scroll filesystems"},
        {"PgUp/PgDn, Home/End", "jump the filesystem list"},
        {"? / Esc / click", "close help"},
    };
    static const HelpEntry sched_lines[] = {
        {"Tab", "switch top-level pages"},
        {"Shift-Tab", "change scheduler subview"},
        {"click Profiles/Rules/Live", "change scheduler view"},
        {"j/k or arrows", "move the active pane"},
        {"PgUp/PgDn, Home/End", "jump the active pane"},
        {"f", "toggle list / preview focus"},
        {"space", "open the selected entry"},
        {"i", "edit the selected file with $VISUAL/$EDITOR"},
        {"n", "create a new profile or rule file"},
        {"d", "delete the selected file-backed entry"},
        {"R", "reload scheduler config"},
        {"? / Esc / click", "close help"},
    };
    static const HelpEntry cgroups_lines[] = {
        {"Tab", "switch top-level pages"},
        {"Shift-Tab", "change cgroups subview"},
        {"click Tree/Files/Config", "change cgroups view"},
        {"j/k or arrows", "move the active pane"},
        {"PgUp/PgDn, Home/End", "jump the active pane"},
        {"f", "toggle list / preview focus"},
        {"space", "open the selected cgroup in Tree"},
        {"i", "edit the selected cgroup file or config"},
        {"R", "reload the active cgroups view"},
        {"? / Esc / click", "close help"},
    };
    static const HelpEntry udev_lines[] = {
        {"Tab", "switch top-level pages"},
        {"Shift-Tab", "change udev subview"},
        {"click Rules/Hwdb/Devices", "change udev view"},
        {"j/k or arrows", "move the active pane"},
        {"PgUp/PgDn, Home/End", "jump the active pane"},
        {"f", "toggle list / preview focus"},
        {"space", "open or close the selected item"},
        {"i", "edit the selected rule or hwdb file"},
        {"R", "reload the active udev view"},
        {"? / Esc / click", "close help"},
    };
    static const HelpEntry systemd_lines[] = {
        {"Tab", "switch top-level pages"},
        {"Shift-Tab", "change systemd subview"},
        {"click Services/Deps/Config", "change systemd view"},
        {"j/k or arrows", "move the active pane"},
        {"PgUp/PgDn, Home/End", "jump the active pane"},
        {"f", "toggle list / preview focus"},
        {"space", "open or close the selected item"},
        {"i", "edit the selected config or opened unit file"},
        {"? / Esc / click", "close help"},
    };
    static const HelpEntry tune_lines[] = {
        {"Tab", "switch top-level pages"},
        {"Shift-Tab", "change tunables subview"},
        {"click Explore/Snapshots/Presets", "change tune view"},
        {"j/k or arrows", "move the active pane"},
        {"PgUp/PgDn, Home/End", "jump the active pane"},
        {"f", "toggle list / preview focus"},
        {"space", "open the selected row or saved bundle"},
        {"i", "edit a tunable inline or open a saved bundle file"},
        {"n", "create a new snapshot or preset bundle file"},
        {"m", "rename the selected snapshot or preset"},
        {"d", "delete the selected snapshot or preset"},
        {"Enter", "stage the edited value"},
        {"Esc", "cancel inline edit"},
        {"a", "apply the staged value or selected bundle"},
        {"s / S", "save snapshot or preset from Explore"},
        {"? / click", "close help"},
    };

    switch (page) {
    case APP_PAGE_SCHED:
        *count = (int)(sizeof(sched_lines) / sizeof(sched_lines[0]));
        return sched_lines;
    case APP_PAGE_CGROUPS:
        *count = (int)(sizeof(cgroups_lines) / sizeof(cgroups_lines[0]));
        return cgroups_lines;
    case APP_PAGE_UDEV:
        *count = (int)(sizeof(udev_lines) / sizeof(udev_lines[0]));
        return udev_lines;
    case APP_PAGE_TUNE:
        *count = (int)(sizeof(tune_lines) / sizeof(tune_lines[0]));
        return tune_lines;
    case APP_PAGE_SYSTEMD:
        *count = (int)(sizeof(systemd_lines) / sizeof(systemd_lines[0]));
        return systemd_lines;
    case APP_PAGE_DIZK:
        *count = (int)(sizeof(disk_lines) / sizeof(disk_lines[0]));
        return disk_lines;
    case APP_PAGE_CPU:
    default:
        *count = (int)(sizeof(cpu_lines) / sizeof(cpu_lines[0]));
        return cpu_lines;
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

static int effective_sched_refresh_ms(const LuloSchedSnapshot *snap)
{
    int interval = snap && snap->watcher_interval_ms > 0 ? snap->watcher_interval_ms : 1000;

    if (interval < 250) interval = 250;
    if (interval > 5000) interval = 5000;
    return interval;
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

void sched_status_set(AppState *app, const char *fmt, ...)
{
    va_list ap;

    if (!app) return;
    va_start(ap, fmt);
    vsnprintf(app->sched_status, sizeof(app->sched_status), fmt, ap);
    va_end(ap);
    app->sched_status_until_ms = mono_ms_now() + 2500;
}

const char *sched_status_current(AppState *app)
{
    if (!app || !app->sched_status[0]) return NULL;
    if (mono_ms_now() > app->sched_status_until_ms) {
        app->sched_status[0] = '\0';
        app->sched_status_until_ms = 0;
        return NULL;
    }
    return app->sched_status;
}

void cgroups_status_set(AppState *app, const char *fmt, ...)
{
    va_list ap;

    if (!app) return;
    va_start(ap, fmt);
    vsnprintf(app->cgroups_status, sizeof(app->cgroups_status), fmt, ap);
    va_end(ap);
    app->cgroups_status_until_ms = mono_ms_now() + 2500;
}

const char *cgroups_status_current(AppState *app)
{
    if (!app || !app->cgroups_status[0]) return NULL;
    if (mono_ms_now() > app->cgroups_status_until_ms) {
        app->cgroups_status[0] = '\0';
        app->cgroups_status_until_ms = 0;
        return NULL;
    }
    return app->cgroups_status;
}

void udev_status_set(AppState *app, const char *fmt, ...)
{
    va_list ap;

    if (!app) return;
    va_start(ap, fmt);
    vsnprintf(app->udev_status, sizeof(app->udev_status), fmt, ap);
    va_end(ap);
    app->udev_status_until_ms = mono_ms_now() + 2500;
}

const char *udev_status_current(AppState *app)
{
    if (!app || !app->udev_status[0]) return NULL;
    if (mono_ms_now() > app->udev_status_until_ms) {
        app->udev_status[0] = '\0';
        app->udev_status_until_ms = 0;
        return NULL;
    }
    return app->udev_status;
}

void tune_status_set(AppState *app, const char *fmt, ...)
{
    va_list ap;

    if (!app) return;
    va_start(ap, fmt);
    vsnprintf(app->tune_status, sizeof(app->tune_status), fmt, ap);
    va_end(ap);
    app->tune_status_until_ms = mono_ms_now() + 2500;
}

const char *tune_status_current(AppState *app)
{
    if (!app || !app->tune_status[0]) return NULL;
    if (mono_ms_now() > app->tune_status_until_ms) {
        app->tune_status[0] = '\0';
        app->tune_status_until_ms = 0;
        return NULL;
    }
    return app->tune_status;
}

void tune_edit_prompt_refresh(AppState *app)
{
    const char *name;
    const char *slash;
    int label_len;
    int max_value_len;

    if (!app || !app->tune_edit_active) return;
    slash = strrchr(app->tune_edit_path, '/');
    name = slash ? slash + 1 : app->tune_edit_path;
    if (!name || !*name) name = "value";
    label_len = (int)strlen(name);
    if (label_len > 96) label_len = 96;
    max_value_len = (int)sizeof(app->tune_edit_prompt) - 9 - label_len;
    if (max_value_len < 0) max_value_len = 0;
    snprintf(app->tune_edit_prompt, sizeof(app->tune_edit_prompt),
             "edit %.*s = %.*s", label_len, name, max_value_len, app->tune_edit_value);
}

void tune_edit_prompt_format(const AppState *app, char *buf, size_t len)
{
    const char *name;
    const char *slash;
    int label_len;
    int max_value_len;

    if (!buf || len == 0) return;
    buf[0] = '\0';
    if (!app || !app->tune_edit_active) return;
    slash = strrchr(app->tune_edit_path, '/');
    name = slash ? slash + 1 : app->tune_edit_path;
    if (!name || !*name) name = "value";
    label_len = (int)strlen(name);
    if (label_len > 96) label_len = 96;
    max_value_len = (int)len - 9 - label_len;
    if (max_value_len < 0) max_value_len = 0;
    snprintf(buf, len, "edit %.*s = %.*s", label_len, name, max_value_len, app->tune_edit_value);
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

static const char *path_basename_local(const char *path)
{
    const char *slash;

    if (!path || !*path) return "";
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void ui_forget_planes(Ui *ui)
{
    if (!ui) return;
    ui->std = NULL;
    ui->modal = NULL;
    ui->header = NULL;
    ui->tabs = NULL;
    ui->top = NULL;
    ui->cpu = NULL;
    ui->mem = NULL;
    ui->proc = NULL;
    ui->disk = NULL;
    ui->sched = NULL;
    ui->cgroups = NULL;
    ui->udev = NULL;
    ui->systemd = NULL;
    ui->tune = NULL;
    ui->load = NULL;
    ui->footer = NULL;
    ui->term_h = 0;
    ui->term_w = 0;
    memset(&ui->lo, 0, sizeof(ui->lo));
    memset(&ui->proc_table, 0, sizeof(ui->proc_table));
    memset(ui->tab_hits, 0, sizeof(ui->tab_hits));
}

static void ui_suspend_for_external_editor(Ui *ui, InputBackend input_backend, RawInput *rawin)
{
    if (input_backend == INPUT_BACKEND_RAW) {
        terminal_mouse_disable();
        raw_input_disable(rawin);
    }
    if (ui && ui->nc) {
        notcurses_stop(ui->nc);
        ui->nc = NULL;
    }
    ui_forget_planes(ui);
}

static int ui_resume_after_external_editor(Ui *ui, InputBackend input_backend, RawInput *rawin,
                                           DebugLog *dlog, char *err, size_t errlen)
{
    struct notcurses_options opts;

    if (err && errlen > 0) err[0] = '\0';
    memset(&opts, 0, sizeof(opts));
    opts.flags = NCOPTION_SUPPRESS_BANNERS | NCOPTION_INHIBIT_SETLOCALE;
    debug_log_stage(dlog, "before_resume_init");
    ui->nc = notcurses_core_init(&opts, NULL);
    if (!ui->nc) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to reinitialize Notcurses");
        return -1;
    }
    ui->std = notcurses_stdplane(ui->nc);
    if (input_backend == INPUT_BACKEND_NOTCURSES) {
        notcurses_mice_enable(ui->nc, NCMICE_BUTTON_EVENT);
    } else if (input_backend == INPUT_BACKEND_RAW) {
        if (raw_input_enable(rawin) < 0) {
            if (err && errlen > 0) snprintf(err, errlen, "failed to restore raw input mode");
            notcurses_stop(ui->nc);
            ui->nc = NULL;
            ui_forget_planes(ui);
            return -1;
        }
        terminal_mouse_enable();
    }
    debug_log_stage(dlog, "after_resume_init");
    return 0;
}

static int launch_external_editor(const char *path, char *err, size_t errlen)
{
    pid_t pid;
    int status = 0;

    if (err && errlen > 0) err[0] = '\0';
    if (!path || !*path) {
        if (err && errlen > 0) snprintf(err, errlen, "invalid edit path");
        errno = EINVAL;
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "fork editor: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execl("/bin/sh", "sh", "-lc",
              "exec ${VISUAL:-${EDITOR:-vi}} \"$1\"",
              "sh", path, (char *)NULL);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            if (err && errlen > 0) snprintf(err, errlen, "wait editor: %s", strerror(errno));
            return -1;
        }
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 1;
    if (WIFEXITED(status) && (WEXITSTATUS(status) == 130 || WEXITSTATUS(status) == 143)) return 0;
    if (WIFSIGNALED(status) && (WTERMSIG(status) == SIGINT || WTERMSIG(status) == SIGTERM)) return 0;
    if (err && errlen > 0) {
        if (WIFEXITED(status)) snprintf(err, errlen, "editor exited with status %d", WEXITSTATUS(status));
        else if (WIFSIGNALED(status)) snprintf(err, errlen, "editor terminated by signal %d", WTERMSIG(status));
        else snprintf(err, errlen, "editor failed");
    }
    return -1;
}

static int run_external_edit(Ui *ui, InputBackend input_backend, RawInput *rawin, DebugLog *dlog,
                             const char *path, int *fatal, char *err, size_t errlen)
{
    LuloEditSession session;
    int editor_rc;

    if (fatal) *fatal = 0;
    if (err && errlen > 0) err[0] = '\0';
    lulo_edit_session_init(&session);
    if (lulo_edit_session_begin(path, &session, err, errlen) < 0) return -1;

    ui_suspend_for_external_editor(ui, input_backend, rawin);
    editor_rc = launch_external_editor(session.edit_path, err, errlen);
    if (ui_resume_after_external_editor(ui, input_backend, rawin, dlog, err, errlen) < 0) {
        if (fatal) *fatal = 1;
        return -1;
    }
    if (editor_rc <= 0) {
        if (lulo_edit_session_cancel(&session, NULL, 0) < 0) lulo_edit_session_clear(&session);
        return editor_rc;
    }
    if (lulo_edit_session_commit(&session, err, errlen) < 0) return -1;
    return 1;
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
    if (action != INPUT_SCROLL_UP &&
        action != INPUT_SCROLL_DOWN &&
        action != INPUT_SCROLL_LEFT &&
        action != INPUT_SCROLL_RIGHT) {
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

static Rgb heat_cell_color(const Theme *theme, int pct)
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

static Rgb proc_io_color(const Theme *theme, int io_class, int io_priority)
{
    switch (io_class) {
    case 1:
        return io_priority <= 1 ? theme->red : theme->orange;
    case 2:
        if (io_priority <= 1) return theme->green;
        if (io_priority <= 3) return theme->cyan;
        if (io_priority <= 5) return theme->blue;
        return theme->dim;
    case 3:
        return theme->dim;
    case 0:
    default:
        return theme->white;
    }
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

static void build_proc_table_layout(Ui *ui);

static int proc_command_visible_width(const ProcTableLayout *pt, const LuloProcRow *row)
{
    if (!pt || !row) return 1;
    if (row->label_prefix_len > 0 &&
        row->label_prefix_len < LULO_PROC_LABEL_MAX &&
        row->label_prefix_cols > 0 &&
        row->label_prefix_cols < pt->cmd_w) {
        int width = pt->cmd_w - row->label_prefix_cols;
        return width > 0 ? width : 1;
    }
    return pt->cmd_w > 0 ? pt->cmd_w : 1;
}

static int proc_command_hscroll_max(const LuloProcSnapshot *snap, const ProcTableLayout *pt)
{
    int max_scroll = 0;

    if (!snap || !pt) return 0;
    for (int i = 0; i < snap->count; i++) {
        const LuloProcRow *row = &snap->rows[i];
        const char *command = row->label;
        int visible = proc_command_visible_width(pt, row);
        int len;

        if (row->label_prefix_len > 0 &&
            row->label_prefix_len < LULO_PROC_LABEL_MAX &&
            row->label_prefix_cols > 0 &&
            row->label_prefix_cols < pt->cmd_w) {
            command = row->label + row->label_prefix_len;
        }
        len = (int)strlen(command);
        if (len > visible && len - visible > max_scroll) max_scroll = len - visible;
    }
    return max_scroll;
}

static int proc_command_scroll(const LuloProcSnapshot *snap, const ProcTableLayout *pt, const LuloProcState *state)
{
    int scroll = state ? state->x_scroll : 0;
    int max_scroll = proc_command_hscroll_max(snap, pt);

    if (scroll < 0) return 0;
    if (scroll > max_scroll) return max_scroll;
    return scroll;
}

static int scroll_proc_horizontally(Ui *ui, const LuloProcSnapshot *snap, LuloProcState *state, int delta)
{
    int max_scroll;
    int next;

    if (!ui || !snap || !state || delta == 0) return 0;
    build_proc_table_layout(ui);
    max_scroll = proc_command_hscroll_max(snap, &ui->proc_table);
    next = state->x_scroll + delta;
    if (next < 0) next = 0;
    if (next > max_scroll) next = max_scroll;
    if (next == state->x_scroll) return 0;
    state->x_scroll = next;
    return 1;
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
    destroy_plane(&ui->modal);
    destroy_plane(&ui->footer);
    destroy_plane(&ui->load);
    destroy_plane(&ui->tune);
    destroy_plane(&ui->systemd);
    destroy_plane(&ui->cgroups);
    destroy_plane(&ui->sched);
    destroy_plane(&ui->disk);
    destroy_plane(&ui->proc);
    destroy_plane(&ui->mem);
    destroy_plane(&ui->cpu);
    destroy_plane(&ui->udev);
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
    } else if (page == APP_PAGE_DIZK) {
        int disk_height = lo->top.height - 3;
        if (disk_height > 0) {
            ui->disk = create_plane(ui->std, lo->top.row + 1, lo->top.col + 1, disk_height, lo->top.width - 2, "disk");
        }
    } else if (page == APP_PAGE_TUNE) {
        int tune_height = lo->top.height - 3;
        if (tune_height > 0) {
            ui->tune = create_plane(ui->std, lo->top.row + 1, lo->top.col + 1,
                                    tune_height, lo->top.width - 2, "tune");
        }
    } else if (page == APP_PAGE_SCHED) {
        int sched_height = lo->top.height - 3;
        if (sched_height > 0) {
            ui->sched = create_plane(ui->std, lo->top.row + 1, lo->top.col + 1,
                                     sched_height, lo->top.width - 2, "sched");
        }
    } else if (page == APP_PAGE_CGROUPS) {
        int cgroups_height = lo->top.height - 3;
        if (cgroups_height > 0) {
            ui->cgroups = create_plane(ui->std, lo->top.row + 1, lo->top.col + 1,
                                       cgroups_height, lo->top.width - 2, "cgroups");
        }
    } else if (page == APP_PAGE_UDEV) {
        int udev_height = lo->top.height - 3;
        if (udev_height > 0) {
            ui->udev = create_plane(ui->std, lo->top.row + 1, lo->top.col + 1,
                                    udev_height, lo->top.width - 2, "udev");
        }
    } else {
        int systemd_height = lo->top.height - 3;
        if (systemd_height > 0) {
            ui->systemd = create_plane(ui->std, lo->top.row + 1, lo->top.col + 1,
                                       systemd_height, lo->top.width - 2, "systemd");
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
    if (page == APP_PAGE_TUNE && !ui->tune) {
        return -1;
    }
    if (page == APP_PAGE_SCHED && !ui->sched) {
        return -1;
    }
    if (page == APP_PAGE_CGROUPS && !ui->cgroups) {
        return -1;
    }
    if (page == APP_PAGE_UDEV && !ui->udev) {
        return -1;
    }
    if (page == APP_PAGE_SYSTEMD && !ui->systemd) {
        return -1;
    }
    if (page == APP_PAGE_CPU) {
        if (ui->mem) draw_box_title(ui->mem, ui->theme, ui->theme->border_panel, " Memory ", ui->theme->white);
        if (ui->proc) draw_box_title(ui->proc, ui->theme, ui->theme->border_panel, " Process Tree ", ui->theme->white);
    } else if (ui->disk) {
        draw_box_title(ui->disk, ui->theme, ui->theme->border_panel, " Disk ", ui->theme->white);
    } else if (ui->sched) {
        draw_box_title(ui->sched, ui->theme, ui->theme->border_panel, " Scheduler ", ui->theme->white);
    } else if (ui->cgroups) {
        draw_box_title(ui->cgroups, ui->theme, ui->theme->border_panel, " Cgroups ", ui->theme->white);
    } else if (ui->udev) {
        draw_box_title(ui->udev, ui->theme, ui->theme->border_panel, " Udev ", ui->theme->white);
    } else if (ui->tune) {
        draw_box_title(ui->tune, ui->theme, ui->theme->border_panel, " Tunables ", ui->theme->white);
    } else if (ui->systemd) {
        draw_box_title(ui->systemd, ui->theme, ui->theme->border_panel, " Systemd ", ui->theme->white);
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
    char proc_buf[32];
    char proc_cpu_buf[24];
    int row;
    int x;
    int remaining;

    draw_box_title(ui->header, ui->theme, ui->theme->border_header, NULL, ui->theme->fg);
    if (ui->lo.header.height < 3) return;

    strftime(tb, sizeof(tb), "%a %d %b %Y  %H:%M:%S", localtime(&(time_t){ time(NULL) }));
    snprintf(sample_buf, sizeof(sample_buf), "%dms", dash->sample_ms);
    snprintf(proc_buf, sizeof(proc_buf), "proc %dms",
             app ? effective_proc_refresh_ms(dash->sample_ms, app->proc_refresh_ms) : 1000);
    snprintf(proc_cpu_buf, sizeof(proc_cpu_buf), "cpu %s",
             app ? lulo_proc_cpu_mode_name(app->proc_cpu_mode) : lulo_proc_cpu_mode_name(LULO_PROC_CPU_TOTAL));

    row = 1;
    x = 2;
    remaining = ui->lo.header.width - 4;
    x += plane_segment_fit(ui->header, row, x, remaining, ui->theme->cyan, ui->theme->bg, "LULO");
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

static void render_cpu_headers(Ui *ui)
{
    char buf[64];
    int x = 0;

    plane_fill(ui->cpu, 0, 0, ui->lo.cpu.width, ui->theme->bg, ui->theme->bg);
    snprintf(buf, sizeof(buf), "%-*s", ui->lo.cpu_label_w, "CPU");
    plane_putn(ui->cpu, 0, x, ui->theme->white, ui->theme->bg, buf, ui->lo.cpu_label_w);
    x += ui->lo.cpu_label_w + 2;
    snprintf(buf, sizeof(buf), "%-*s", ui->lo.cpu_hist_w + 4, "HISTORY");
    plane_putn(ui->cpu, 0, x, ui->theme->cyan, ui->theme->bg, buf, ui->lo.cpu_hist_w + 4);
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

static void render_heatmap(Ui *ui, int y, int x, const unsigned char *hist, int width)
{
    for (int i = 0; i < width; i++) {
        if (hist[i] <= 0) continue;
        plane_color(ui->cpu, heat_cell_color(ui->theme, hist[i]), ui->theme->bg);
        ncplane_putstr_yx(ui->cpu, y, x + i, "█");
    }
}

static void render_cpu_widget(Ui *ui, const CpuInfo *ci, DashboardState *dash,
                              const CpuStat *sa, const CpuStat *sb,
                              const CpuFreq *freqs, int nfreq,
                              const double *cpu_temps, int ncpu_temp,
                              int append_sample)
{
    render_cpu_headers(ui);
    for (int i = 0; i < ui->lo.cpu_rows; i++) {
        char label[32];
        char buf[64];
        int y = i + 1;
        int x = 0;
        int pct = lulo_cpu_pct(&sa->cpu[i + 1], &sb->cpu[i + 1]);
        int heat = lulo_cpu_heat_pct(&sa->cpu[i + 1], &sb->cpu[i + 1]);
        long mhz = i < nfreq ? freqs[i].cur_khz / 1000 : 0;
        const unsigned char *hist;

        if (append_sample) lulo_dashboard_append_heat(dash, i, heat);
        hist = lulo_dashboard_history(dash, i, ui->lo.cpu_hist_w);

        plane_fill(ui->cpu, y, 0, ui->lo.cpu.width, ui->theme->bg, ui->theme->bg);
        snprintf(label, sizeof(label), "cpu%d", i);
        snprintf(buf, sizeof(buf), "%-*s", ui->lo.cpu_label_w, label);
        plane_putn(ui->cpu, y, x, ui->theme->white, ui->theme->bg, buf, ui->lo.cpu_label_w);
        x += ui->lo.cpu_label_w + 2;
        render_heatmap(ui, y, x, hist, ui->lo.cpu_hist_w);
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

static void render_help_overlay(Ui *ui, AppPage page)
{
    const HelpEntry *entries;
    const char *title;
    int count = 0;
    int max_key_width = 0;
    int max_desc_width = 0;
    int max_width = 0;
    int width;
    int height;
    int row;
    int col;
    int y;

    if (!ui || !ui->std) return;
    entries = help_entries(page, &count);
    title = help_title(page);
    for (int i = 0; i < count; i++) {
        int key_w = (int)strlen(entries[i].keys);
        int desc_w = (int)strlen(entries[i].desc);

        if (key_w > max_key_width) max_key_width = key_w;
        if (desc_w > max_desc_width) max_desc_width = desc_w;
    }
    max_width = max_key_width + 3 + max_desc_width;
    if ((int)strlen(title) > max_width) max_width = (int)strlen(title);
    width = max_width + 6;
    height = count * 2 + 4;
    if (width > ui->term_w - 4) width = ui->term_w - 4;
    if (height > ui->term_h - 2) height = ui->term_h - 2;
    if (width < 36) width = 36;
    if (height < 6) height = 6;
    row = (ui->term_h - height) / 2 + 1;
    col = (ui->term_w - width) / 2 + 1;
    if (row < 1) row = 1;
    if (col < 1) col = 1;

    destroy_plane(&ui->modal);
    ui->modal = create_plane(ui->std, row, col, height, width, "help");
    if (!ui->modal) return;

    draw_box_title(ui->modal, ui->theme, ui->theme->border_panel, title, ui->theme->white);
    for (y = 1; y < height - 1; y++) {
        plane_fill(ui->modal, y, 1, width - 2, ui->theme->bg, ui->theme->bg);
    }
    for (int i = 0; i < count; i++) {
        int entry_y = 2 + i * 2;

        if (entry_y >= height - 1) break;
        plane_putn(ui->modal, entry_y, 3, ui->theme->green, ui->theme->bg,
                   entries[i].keys, max_key_width);
        plane_putn(ui->modal, entry_y, 3 + max_key_width + 3, ui->theme->white, ui->theme->bg,
                   entries[i].desc, width - (3 + max_key_width + 6));
    }
    ncplane_move_top(ui->modal);
}

static int help_overlay_consume_input(Ui *ui, AppState *app, const DecodedInput *in,
                                      InputAction action, RenderFlags *render)
{
    (void)ui;
    if (!app || !app->help_visible || !in) return 0;
    if (action == INPUT_RESIZE) return 0;
    if (action != INPUT_NONE || in->mouse_press || in->mouse_release || in->mouse_wheel ||
        in->text_len > 0 || in->backspace || in->submit || in->cancel) {
        app->help_visible = 0;
        render->need_render = 1;
        return 1;
    }
    return 1;
}

static void build_proc_table_layout(Ui *ui)
{
    ProcTableLayout *pt = &ui->proc_table;
    int inner_w;
    int fixed_w;
    int gaps;
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
    pt->io_w = 4;
    pt->show_user = inner_w >= 58;
    pt->show_mem = inner_w >= 72;
    pt->show_time = inner_w >= 86;
    pt->user_w = pt->show_user ? (inner_w >= 68 ? 8 : 6) : 0;
    pt->mem_w = pt->show_mem ? 5 : 0;
    pt->time_w = pt->show_time ? 8 : 0;
    fixed_w = pt->pid_w + pt->policy_w + pt->prio_w + pt->nice_w + pt->cpu_w + pt->io_w;
    gaps = 6;
    if (pt->show_user) {
        fixed_w += pt->user_w;
        gaps++;
    }
    if (pt->show_mem) {
        fixed_w += pt->mem_w;
        gaps++;
    }
    if (pt->show_time) {
        fixed_w += pt->time_w;
        gaps++;
    }
    cmd_w = inner_w - fixed_w - gaps;
    if (cmd_w < 10 && pt->show_time) {
        pt->show_time = 0;
        fixed_w -= pt->time_w;
        gaps--;
        pt->time_w = 0;
        cmd_w = inner_w - fixed_w - gaps;
    }
    if (cmd_w < 10 && pt->show_mem) {
        pt->show_mem = 0;
        fixed_w -= pt->mem_w;
        gaps--;
        pt->mem_w = 0;
        cmd_w = inner_w - fixed_w - gaps;
    }
    if (cmd_w < 10 && pt->show_user) {
        pt->show_user = 0;
        fixed_w -= pt->user_w;
        gaps--;
        pt->user_w = 0;
        cmd_w = inner_w - fixed_w - gaps;
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
    char core_buf[24];
    int text_len;
    int pad;

    if (!ui->proc || !snap || snap->count <= 0 || !state) return;
    count_digits = clamp_int(digits_int(snap->count), 1, 9);
    field_w = count_digits * 2 + 1;
    rows_x = ui->lo.proc.width - field_w - 2;
    if (rows_x < 2) return;
    snprintf(core_buf, sizeof(core_buf), "%d/%d", state->selected + 1, snap->count);
    text_len = (int)strlen(core_buf);
    pad = field_w - text_len;
    if (pad < 0) pad = 0;
    snprintf(rows_buf, sizeof(rows_buf), "%*s%s", pad, "", core_buf);
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
        format_sort_header(buf, sizeof(buf), "IO", state, LULO_PROC_SORT_IO, pt->io_w, 1);
        plane_putn(ui->proc, 1, x, ui->theme->blue, ui->theme->bg, buf, pt->io_w);
        set_proc_header_hit(pt, LULO_PROC_SORT_IO, x, pt->io_w, 1);
        x += pt->io_w + 1;
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
    char io_buf[8];
    char policy_buf[8];
    char prio_buf[16];
    char out[1024];

    plane_fill(ui->proc, y, 1, pt->inner_w, row_bg, row_bg);
    if (!snap || row_index < 0 || row_index >= snap->count) return;

    const LuloProcRow *row = &snap->rows[row_index];
    lulo_format_proc_pct(cpu_buf, sizeof(cpu_buf), row->cpu_tenths);
    lulo_format_proc_pct(mem_buf, sizeof(mem_buf), row->mem_tenths);
    lulo_format_proc_time(time_buf, sizeof(time_buf), row->time_cs);
    lulo_sched_format_io(io_buf, sizeof(io_buf), row->io_class, row->io_priority);
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

    snprintf(out, sizeof(out), "%-*s", pt->io_w, io_buf);
    plane_putn(ui->proc, y, x, selected ? ui->theme->select_fg : proc_io_color(ui->theme, row->io_class, row->io_priority),
               row_bg, out, pt->io_w);
    x += pt->io_w + 1;

    if (row->label_prefix_len > 0 &&
        row->label_prefix_len < (int)sizeof(row->label) &&
        row->label_prefix_cols < pt->cmd_w) {
        const char *command = row->label + row->label_prefix_len;
        int cmd_only_w = pt->cmd_w - row->label_prefix_cols;
        int body_scroll = pt->cmd_scroll;
        int command_len = (int)strlen(command);

        if (body_scroll > command_len) body_scroll = command_len;
        snprintf(out, sizeof(out), "%.*s", row->label_prefix_len, row->label);
        plane_putn(ui->proc, y, x, selected ? ui->theme->select_fg : ui->theme->branch,
                   row_bg, out, row->label_prefix_cols);
        x += row->label_prefix_cols;
        snprintf(out, sizeof(out), "%-*.*s", cmd_only_w, cmd_only_w, command + body_scroll);
        plane_putn(ui->proc, y, x, selected ? ui->theme->select_fg : proc_command_color(ui->theme, row),
                   row_bg, out, cmd_only_w);
    } else {
        const char *command = row->label;
        int body_scroll = pt->cmd_scroll;
        int command_len = (int)strlen(command);

        if (body_scroll > command_len) body_scroll = command_len;
        snprintf(out, sizeof(out), "%-*.*s", pt->cmd_w, pt->cmd_w, command + body_scroll);
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
    ui->proc_table.cmd_scroll = proc_command_scroll(snap, &ui->proc_table, state);
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
    ui->proc_table.cmd_scroll = proc_command_scroll(snap, &ui->proc_table, state);
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
    ui->proc_table.cmd_scroll = proc_command_scroll(snap, &ui->proc_table, state);
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

static void fill_decoded_input_from_nc(DecodedInput *out, uint32_t id, const ncinput *ni, InputAction action)
{
    memset(out, 0, sizeof(*out));
    out->action = action;
    out->key_repeat = ni && ni->evtype == NCTYPE_REPEAT;
    if (id == 27) {
        out->cancel = 1;
        return;
    }
    if (id == NCKEY_ENTER) {
        out->submit = 1;
        return;
    }
    if (id == NCKEY_BACKSPACE) {
        out->backspace = 1;
        return;
    }
    if (id < 0x80 && isprint((int)id)) {
        out->text[0] = (char)id;
        out->text[1] = '\0';
        out->text_len = 1;
    } else if (ni && ni->utf8[0]) {
        snprintf(out->text, sizeof(out->text), "%s", ni->utf8);
        out->text_len = (int)strlen(out->text);
    }
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

static void render_sched_page(Ui *ui, AppState *app, const DashboardState *dash,
                              const LuloSchedSnapshot *sched_snap, const LuloSchedState *sched_state,
                              const LuloSchedBackendStatus *sched_backend_status)
{
    render_header_widget(ui, dash, app);
    render_sched_widget(ui, sched_snap, sched_state);
    render_sched_status(ui, sched_snap, sched_state, sched_backend_status, app);
}

static void render_cgroups_page(Ui *ui, AppState *app, const DashboardState *dash,
                                const LuloCgroupsSnapshot *cgroups_snap, const LuloCgroupsState *cgroups_state,
                                const LuloCgroupsBackendStatus *cgroups_backend_status)
{
    render_header_widget(ui, dash, app);
    render_cgroups_widget(ui, cgroups_snap, cgroups_state);
    render_cgroups_status(ui, cgroups_snap, cgroups_state, cgroups_backend_status, app);
}

static void render_udev_page(Ui *ui, AppState *app, const DashboardState *dash,
                             const LuloUdevSnapshot *udev_snap, const LuloUdevState *udev_state,
                             const LuloUdevBackendStatus *udev_backend_status)
{
    render_header_widget(ui, dash, app);
    render_udev_widget(ui, udev_snap, udev_state);
    render_udev_status(ui, udev_snap, udev_state, udev_backend_status, app);
}

static void render_systemd_page(Ui *ui, const AppState *app, const DashboardState *dash,
                                const LuloSystemdSnapshot *systemd_snap, const LuloSystemdState *systemd_state,
                                const LuloSystemdBackendStatus *systemd_backend_status)
{
    render_header_widget(ui, dash, app);
    render_systemd_widget(ui, systemd_snap, systemd_state);
    render_systemd_status(ui, systemd_snap, systemd_state, systemd_backend_status);
}

static void render_tune_page(Ui *ui, AppState *app, const DashboardState *dash,
                             const LuloTuneSnapshot *tune_snap, const LuloTuneState *tune_state,
                             const LuloTuneBackendStatus *tune_backend_status)
{
    render_header_widget(ui, dash, app);
    render_tune_widget(ui, tune_snap, tune_state, app);
    render_tune_status(ui, tune_snap, tune_state, tune_backend_status, app);
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

static void render_sched_only(Ui *ui, const LuloSchedSnapshot *sched_snap,
                              const LuloSchedState *sched_state,
                              AppState *app,
                              const LuloSchedBackendStatus *sched_backend_status)
{
    render_sched_widget(ui, sched_snap, sched_state);
    render_sched_status(ui, sched_snap, sched_state, sched_backend_status, app);
}

static void render_cgroups_only(Ui *ui, const LuloCgroupsSnapshot *cgroups_snap,
                                const LuloCgroupsState *cgroups_state,
                                AppState *app,
                                const LuloCgroupsBackendStatus *cgroups_backend_status)
{
    render_cgroups_widget(ui, cgroups_snap, cgroups_state);
    render_cgroups_status(ui, cgroups_snap, cgroups_state, cgroups_backend_status, app);
}

static void render_udev_only(Ui *ui, const LuloUdevSnapshot *udev_snap,
                             const LuloUdevState *udev_state,
                             AppState *app,
                             const LuloUdevBackendStatus *udev_backend_status)
{
    render_udev_widget(ui, udev_snap, udev_state);
    render_udev_status(ui, udev_snap, udev_state, udev_backend_status, app);
}

static void render_systemd_only(Ui *ui, const LuloSystemdSnapshot *systemd_snap,
                                const LuloSystemdState *systemd_state,
                                const LuloSystemdBackendStatus *systemd_backend_status)
{
    render_systemd_widget(ui, systemd_snap, systemd_state);
    render_systemd_status(ui, systemd_snap, systemd_state, systemd_backend_status);
}

static void render_tune_only(Ui *ui, const LuloTuneSnapshot *tune_snap,
                             const LuloTuneState *tune_state,
                             AppState *app,
                             const LuloTuneBackendStatus *tune_backend_status)
{
    render_tune_widget(ui, tune_snap, tune_state, app);
    render_tune_status(ui, tune_snap, tune_state, tune_backend_status, app);
}

static int systemd_backend_status_changed(const LuloSystemdBackendStatus *a,
                                          const LuloSystemdBackendStatus *b)
{
    if (!a || !b) return 1;
    return a->busy != b->busy ||
           a->have_snapshot != b->have_snapshot ||
           a->loading_full != b->loading_full ||
           a->loading_active != b->loading_active ||
           a->generation != b->generation ||
           strcmp(a->error, b->error) != 0;
}

static int poll_systemd_backend(Ui *ui, AppState *app, LuloSystemdBackend *backend,
                                LuloSystemdSnapshot *snap, LuloSystemdState *state,
                                LuloSystemdBackendStatus *status, unsigned *generation,
                                RenderFlags *render)
{
    LuloSystemdBackendStatus prev_status = *status;
    int changed = lulo_systemd_backend_consume(backend, snap, generation, status);

    if (changed < 0) return -1;
    if (changed > 0) {
        lulo_systemd_view_sync(state, snap,
                               systemd_list_rows_visible(ui, state),
                               systemd_preview_rows_visible(ui, state));
        if (app->active_page == APP_PAGE_SYSTEMD) {
            render->need_systemd = 1;
            render->need_render = 1;
        }
    } else if (app->active_page == APP_PAGE_SYSTEMD && systemd_backend_status_changed(&prev_status, status)) {
        render->need_systemd = 1;
        render->need_render = 1;
    }
    return changed;
}

static int tune_backend_status_changed(const LuloTuneBackendStatus *a,
                                       const LuloTuneBackendStatus *b)
{
    if (!a || !b) return 0;
    return a->busy != b->busy ||
           a->have_snapshot != b->have_snapshot ||
           a->loading_full != b->loading_full ||
           a->loading_active != b->loading_active ||
           a->saving_snapshot != b->saving_snapshot ||
           a->saving_preset != b->saving_preset ||
           a->applying_selected != b->applying_selected ||
           a->generation != b->generation ||
           strcmp(a->error, b->error) != 0;
}

static int poll_tune_backend(Ui *ui, AppState *app, LuloTuneBackend *backend,
                             LuloTuneSnapshot *snap, LuloTuneState *state,
                             LuloTuneBackendStatus *status, unsigned *generation,
                             RenderFlags *render)
{
    LuloTuneBackendStatus prev_status = *status;
    int changed = lulo_tune_backend_consume(backend, snap, generation, status);

    if (changed < 0) return -1;
    if (changed > 0) {
        if (prev_status.saving_snapshot && !status->saving_snapshot && !status->error[0]) {
            tune_status_set(app, "snapshot saved");
        } else if (prev_status.saving_preset && !status->saving_preset && !status->error[0]) {
            tune_status_set(app, "preset saved");
        } else if (prev_status.applying_selected && !status->applying_selected && !status->error[0]) {
            tune_status_set(app, "saved config applied");
        }
        lulo_tune_view_sync(state, snap,
                            tune_list_rows_visible(ui, state),
                            tune_preview_rows_visible(ui, state));
        if (app->active_page == APP_PAGE_TUNE) {
            render->need_tune = 1;
            render->need_render = 1;
        }
    } else if (app->active_page == APP_PAGE_TUNE && tune_backend_status_changed(&prev_status, status)) {
        render->need_tune = 1;
        render->need_render = 1;
    }
    return changed;
}

static int sched_backend_status_changed(const LuloSchedBackendStatus *a,
                                        const LuloSchedBackendStatus *b)
{
    if (!a || !b) return 0;
    return a->busy != b->busy ||
           a->have_snapshot != b->have_snapshot ||
           a->loading_full != b->loading_full ||
           a->loading_active != b->loading_active ||
           a->reloading != b->reloading ||
           a->generation != b->generation ||
           strcmp(a->error, b->error) != 0;
}

static int cgroups_backend_status_changed(const LuloCgroupsBackendStatus *a,
                                          const LuloCgroupsBackendStatus *b)
{
    if (!a || !b) return 0;
    return a->busy != b->busy ||
           a->have_snapshot != b->have_snapshot ||
           a->loading_full != b->loading_full ||
           a->loading_active != b->loading_active ||
           a->generation != b->generation ||
           strcmp(a->error, b->error) != 0;
}

static int udev_backend_status_changed(const LuloUdevBackendStatus *a,
                                       const LuloUdevBackendStatus *b)
{
    if (!a || !b) return 0;
    return a->busy != b->busy ||
           a->have_snapshot != b->have_snapshot ||
           a->loading_full != b->loading_full ||
           a->loading_active != b->loading_active ||
           a->generation != b->generation ||
           strcmp(a->error, b->error) != 0;
}

static int poll_sched_backend(Ui *ui, AppState *app, LuloSchedBackend *backend,
                              LuloSchedSnapshot *snap, LuloSchedState *state,
                              LuloSchedBackendStatus *status, unsigned *generation,
                              RenderFlags *render)
{
    LuloSchedBackendStatus prev_status = *status;
    int changed = lulo_sched_backend_consume(backend, snap, generation, status);

    if (changed < 0) return -1;
    if (changed > 0) {
        lulo_sched_view_sync(state, snap,
                             sched_list_rows_visible(ui, state),
                             sched_preview_rows_visible(ui, state));
        if (app->active_page == APP_PAGE_SCHED) {
            render->need_sched = 1;
            render->need_render = 1;
        }
    } else if (app->active_page == APP_PAGE_SCHED && sched_backend_status_changed(&prev_status, status)) {
        render->need_sched = 1;
        render->need_render = 1;
    }
    return changed;
}

static int poll_cgroups_backend(Ui *ui, AppState *app, LuloCgroupsBackend *backend,
                                LuloCgroupsSnapshot *snap, LuloCgroupsState *state,
                                LuloCgroupsBackendStatus *status, unsigned *generation,
                                RenderFlags *render)
{
    LuloCgroupsBackendStatus prev_status = *status;
    int changed = lulo_cgroups_backend_consume(backend, snap, generation, status);

    if (changed < 0) return -1;
    if (changed > 0) {
        lulo_cgroups_view_sync(state, snap,
                               cgroups_list_rows_visible(ui, state),
                               cgroups_preview_rows_visible(ui, state));
        if (app->active_page == APP_PAGE_CGROUPS) {
            render->need_cgroups = 1;
            render->need_render = 1;
        }
    } else if (app->active_page == APP_PAGE_CGROUPS && cgroups_backend_status_changed(&prev_status, status)) {
        render->need_cgroups = 1;
        render->need_render = 1;
    }
    return changed;
}

static int poll_udev_backend(Ui *ui, AppState *app, LuloUdevBackend *backend,
                             LuloUdevSnapshot *snap, LuloUdevState *state,
                             LuloUdevBackendStatus *status, unsigned *generation,
                             RenderFlags *render)
{
    LuloUdevBackendStatus prev_status = *status;
    int changed = lulo_udev_backend_consume(backend, snap, generation, status);

    if (changed < 0) return -1;
    if (changed > 0) {
        lulo_udev_view_sync(state, snap,
                            udev_list_rows_visible(ui, state),
                            udev_preview_rows_visible(ui, state));
        if (app->active_page == APP_PAGE_UDEV) {
            render->need_udev = 1;
            render->need_render = 1;
        }
    } else if (app->active_page == APP_PAGE_UDEV && udev_backend_status_changed(&prev_status, status)) {
        render->need_udev = 1;
        render->need_render = 1;
    }
    return changed;
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
                if (app->active_page == APP_PAGE_SCHED) render->need_sched_refresh = 1;
                else if (app->active_page == APP_PAGE_CGROUPS) render->need_cgroups_refresh_full = 1;
                else if (app->active_page == APP_PAGE_UDEV) render->need_udev_refresh_full = 1;
                else if (app->active_page == APP_PAGE_SYSTEMD) render->need_systemd_refresh = 1;
                else if (app->active_page == APP_PAGE_TUNE) render->need_tune_refresh_full = 1;
                render->need_rebuild = 1;
                render->need_render = 1;
            }
            return 1;
        }
    }
    return 0;
}

static int point_on_page_tabs(Ui *ui, int global_y, int global_x)
{
    int tab_row = ui->lo.header.row + ui->lo.header.height;

    if (global_y != tab_row) return 0;
    for (int i = 0; i < APP_PAGE_COUNT; i++) {
        if (global_x >= ui->tab_hits[i].x && global_x < ui->tab_hits[i].x + ui->tab_hits[i].width) {
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

static int point_on_inner_tabs(Ui *ui, AppPage page,
                               const LuloSchedState *sched_state,
                               const LuloCgroupsState *cgroups_state,
                               const LuloUdevState *udev_state,
                               const LuloTuneState *tune_state,
                               const LuloSystemdState *systemd_state,
                               int global_y, int global_x)
{
    if (page == APP_PAGE_SCHED && ui->sched && sched_state) {
        return point_on_sched_view_tabs(ui, sched_state, global_y, global_x);
    }
    if (page == APP_PAGE_CGROUPS && ui->cgroups && cgroups_state) {
        return point_on_cgroups_view_tabs(ui, cgroups_state, global_y, global_x);
    }
    if (page == APP_PAGE_UDEV && ui->udev && udev_state) {
        return point_on_udev_view_tabs(ui, udev_state, global_y, global_x);
    }
    if (page == APP_PAGE_TUNE && ui->tune && tune_state) {
        return point_on_tune_view_tabs(ui, tune_state, global_y, global_x);
    }
    if (page == APP_PAGE_SYSTEMD && ui->systemd && systemd_state) {
        return point_on_systemd_view_tabs(ui, systemd_state, global_y, global_x);
    }
    return 0;
}

static int handle_mouse_wheel_target(Ui *ui, AppState *app,
                                     LuloSchedState *sched_state,
                                     LuloCgroupsState *cgroups_state,
                                     LuloUdevState *udev_state,
                                     LuloTuneState *tune_state,
                                     LuloSystemdState *systemd_state,
                                     RenderFlags *render,
                                     int global_y, int global_x)
{
    if (!ui || !app) return 0;
    if (point_on_page_tabs(ui, global_y, global_x)) return 0;
    if (point_on_inner_tabs(ui, app->active_page, sched_state, cgroups_state, udev_state, tune_state, systemd_state, global_y, global_x)) return 0;
    switch (app->active_page) {
    case APP_PAGE_CPU:
        return global_y >= ui->lo.proc.row + 2 &&
               global_y < ui->lo.proc.row + ui->lo.proc.height - 1 &&
               global_x >= ui->lo.proc.col + 1 &&
               global_x < ui->lo.proc.col + ui->lo.proc.width - 1;
    case APP_PAGE_DIZK:
        return ui->disk &&
               global_y >= ui->lo.top.row + 1 &&
               global_y < ui->lo.top.row + ui->lo.top.height - 2 &&
               global_x >= ui->lo.top.col + 1 &&
               global_x < ui->lo.top.col + ui->lo.top.width - 1;
    case APP_PAGE_SCHED:
        return handle_sched_wheel_target(ui, sched_state, render, global_y, global_x);
    case APP_PAGE_CGROUPS:
        return handle_cgroups_wheel_target(ui, cgroups_state, render, global_y, global_x);
    case APP_PAGE_UDEV:
        return handle_udev_wheel_target(ui, udev_state, render, global_y, global_x);
    case APP_PAGE_SYSTEMD:
        return handle_systemd_wheel_target(ui, systemd_state, render, global_y, global_x);
    case APP_PAGE_TUNE:
        return handle_tune_wheel_target(ui, tune_state, render, global_y, global_x);
    default:
        return 0;
    }
}

static int handle_mouse_click(Ui *ui, int global_y, int global_x, AppState *app,
                              const LuloProcSnapshot *proc_snap, LuloProcState *proc_state,
                              const LuloSchedSnapshot *sched_snap, LuloSchedState *sched_state,
                              const LuloCgroupsSnapshot *cgroups_snap, LuloCgroupsState *cgroups_state,
                              const LuloUdevSnapshot *udev_snap, LuloUdevState *udev_state,
                              const LuloTuneSnapshot *tune_snap, LuloTuneState *tune_state,
                              const LuloSystemdSnapshot *systemd_snap, LuloSystemdState *systemd_state,
                              RenderFlags *render)
{
    if (app->help_visible) {
        app->help_visible = 0;
        render->need_render = 1;
        return 1;
    }
    if (handle_tab_click(ui, global_y, global_x, app, render)) return 1;
    if (app->active_page == APP_PAGE_CPU &&
        handle_proc_click(ui, global_y, global_x, proc_snap, proc_state, render)) {
        return 1;
    }
    if (app->active_page == APP_PAGE_SCHED &&
        handle_sched_click(ui, global_y, global_x, sched_snap, sched_state, render)) {
        return 1;
    }
    if (app->active_page == APP_PAGE_CGROUPS &&
        handle_cgroups_click(ui, global_y, global_x, cgroups_snap, cgroups_state, render)) {
        return 1;
    }
    if (app->active_page == APP_PAGE_UDEV &&
        handle_udev_click(ui, global_y, global_x, udev_snap, udev_state, render)) {
        return 1;
    }
    if (app->active_page == APP_PAGE_TUNE &&
        handle_tune_click(ui, global_y, global_x, tune_snap, tune_state, render)) {
        return 1;
    }
    if (app->active_page == APP_PAGE_SYSTEMD &&
        handle_systemd_click(ui, global_y, global_x, systemd_snap, systemd_state, render)) {
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

static void update_sched_render_flags(RenderFlags *render, const LuloSchedState *state,
                                      int prev_view,
                                      int prev_profile_cursor, int prev_profile_selected,
                                      int prev_profile_list_scroll, int prev_profile_detail_scroll,
                                      int prev_rule_cursor, int prev_rule_selected,
                                      int prev_rule_list_scroll, int prev_rule_detail_scroll,
                                      int prev_live_cursor, int prev_live_selected,
                                      int prev_live_list_scroll, int prev_live_detail_scroll,
                                      int prev_focus)
{
    if (!render || !state) return;
    if ((int)state->view != prev_view) {
        render->need_sched_refresh = 1;
        return;
    }
    switch (state->view) {
    case LULO_SCHED_VIEW_RULES:
        if (state->rule_selected != prev_rule_selected) render->need_sched_refresh = 1;
        else if (state->rule_cursor != prev_rule_cursor ||
                 state->rule_list_scroll != prev_rule_list_scroll ||
                 state->rule_detail_scroll != prev_rule_detail_scroll ||
                 state->focus_preview != prev_focus) render->need_sched = 1;
        break;
    case LULO_SCHED_VIEW_LIVE:
        if (state->live_selected != prev_live_selected) render->need_sched_refresh = 1;
        else if (state->live_cursor != prev_live_cursor ||
                 state->live_list_scroll != prev_live_list_scroll ||
                 state->live_detail_scroll != prev_live_detail_scroll ||
                 state->focus_preview != prev_focus) render->need_sched = 1;
        break;
    case LULO_SCHED_VIEW_PROFILES:
    default:
        if (state->profile_selected != prev_profile_selected) render->need_sched_refresh = 1;
        else if (state->profile_cursor != prev_profile_cursor ||
                 state->profile_list_scroll != prev_profile_list_scroll ||
                 state->profile_detail_scroll != prev_profile_detail_scroll ||
                 state->focus_preview != prev_focus) render->need_sched = 1;
        break;
    }
}

static void update_systemd_render_flags(RenderFlags *render, const LuloSystemdState *state,
                                        int prev_cursor, int prev_selected,
                                        int prev_list_scroll, int prev_file_scroll,
                                        int prev_config_cursor, int prev_config_selected,
                                        int prev_config_list_scroll, int prev_config_file_scroll,
                                        int prev_focus)
{
    if (!render || !state) return;
    if (state->view == LULO_SYSTEMD_VIEW_CONFIG) {
        if (state->config_selected != prev_config_selected) render->need_systemd_refresh = 1;
        else if (state->config_cursor != prev_config_cursor ||
                 state->config_list_scroll != prev_config_list_scroll ||
                 state->config_file_scroll != prev_config_file_scroll ||
                 state->focus_preview != prev_focus) render->need_systemd = 1;
    } else {
        if (state->selected != prev_selected) render->need_systemd_refresh = 1;
        else if (state->cursor != prev_cursor ||
                 state->list_scroll != prev_list_scroll ||
                 state->file_scroll != prev_file_scroll ||
                 state->focus_preview != prev_focus) render->need_systemd = 1;
    }
}

static void update_tune_render_flags(RenderFlags *render, const LuloTuneState *state,
                                     int prev_view, const char *prev_browse_path,
                                     int prev_cursor, int prev_selected,
                                     int prev_list_scroll, int prev_detail_scroll,
                                     int prev_snapshot_cursor, int prev_snapshot_selected,
                                     int prev_snapshot_list_scroll, int prev_snapshot_detail_scroll,
                                     int prev_preset_cursor, int prev_preset_selected,
                                     int prev_preset_list_scroll, int prev_preset_detail_scroll,
                                     int prev_focus)
{
    if (!render || !state) return;
    if (strcmp(state->browse_path, prev_browse_path) != 0) {
        render->need_tune_refresh_full = 1;
        return;
    }
    if ((int)state->view != prev_view) {
        render->need_tune_refresh = 1;
        return;
    }
    switch (state->view) {
    case LULO_TUNE_VIEW_SNAPSHOTS:
        if (state->snapshot_selected != prev_snapshot_selected) render->need_tune_refresh = 1;
        else if (state->snapshot_cursor != prev_snapshot_cursor ||
                 state->snapshot_list_scroll != prev_snapshot_list_scroll ||
                 state->snapshot_detail_scroll != prev_snapshot_detail_scroll ||
                 state->focus_preview != prev_focus) render->need_tune = 1;
        break;
    case LULO_TUNE_VIEW_PRESETS:
        if (state->preset_selected != prev_preset_selected) render->need_tune_refresh = 1;
        else if (state->preset_cursor != prev_preset_cursor ||
                 state->preset_list_scroll != prev_preset_list_scroll ||
                 state->preset_detail_scroll != prev_preset_detail_scroll ||
                 state->focus_preview != prev_focus) render->need_tune = 1;
        break;
    case LULO_TUNE_VIEW_EXPLORE:
    default:
        if (state->selected != prev_selected) render->need_tune_refresh = 1;
        else if (state->cursor != prev_cursor ||
                 state->list_scroll != prev_list_scroll ||
                 state->detail_scroll != prev_detail_scroll ||
                 state->focus_preview != prev_focus) render->need_tune = 1;
        break;
    }
}

static void update_cgroups_render_flags(RenderFlags *render, const LuloCgroupsState *state,
                                        int prev_view, const char *prev_browse_path,
                                        int prev_tree_cursor, int prev_tree_selected,
                                        int prev_tree_list_scroll, int prev_tree_detail_scroll,
                                        int prev_file_cursor, int prev_file_selected,
                                        int prev_file_list_scroll, int prev_file_detail_scroll,
                                        int prev_config_cursor, int prev_config_selected,
                                        int prev_config_list_scroll, int prev_config_detail_scroll,
                                        int prev_focus)
{
    if (!render || !state) return;
    if (strcmp(state->browse_path, prev_browse_path) != 0) {
        render->need_cgroups_refresh_full = 1;
        return;
    }
    if ((int)state->view != prev_view) {
        render->need_cgroups_refresh = 1;
        return;
    }
    switch (state->view) {
    case LULO_CGROUPS_VIEW_FILES:
        if (state->file_selected != prev_file_selected) render->need_cgroups_refresh = 1;
        else if (state->file_cursor != prev_file_cursor ||
                 state->file_list_scroll != prev_file_list_scroll ||
                 state->file_detail_scroll != prev_file_detail_scroll ||
                 state->focus_preview != prev_focus) render->need_cgroups = 1;
        break;
    case LULO_CGROUPS_VIEW_CONFIG:
        if (state->config_selected != prev_config_selected) render->need_cgroups_refresh = 1;
        else if (state->config_cursor != prev_config_cursor ||
                 state->config_list_scroll != prev_config_list_scroll ||
                 state->config_detail_scroll != prev_config_detail_scroll ||
                 state->focus_preview != prev_focus) render->need_cgroups = 1;
        break;
    case LULO_CGROUPS_VIEW_TREE:
    default:
        if (state->tree_selected != prev_tree_selected) render->need_cgroups_refresh = 1;
        else if (state->tree_cursor != prev_tree_cursor ||
                 state->tree_list_scroll != prev_tree_list_scroll ||
                 state->tree_detail_scroll != prev_tree_detail_scroll ||
                 state->focus_preview != prev_focus) render->need_cgroups = 1;
        break;
    }
}

static void update_udev_render_flags(RenderFlags *render, const LuloUdevState *state,
                                     int prev_view,
                                     int prev_rule_cursor, int prev_rule_selected,
                                     int prev_rule_list_scroll, int prev_rule_detail_scroll,
                                     int prev_hwdb_cursor, int prev_hwdb_selected,
                                     int prev_hwdb_list_scroll, int prev_hwdb_detail_scroll,
                                     int prev_device_cursor, int prev_device_selected,
                                     int prev_device_list_scroll, int prev_device_detail_scroll,
                                     int prev_focus)
{
    if (!render || !state) return;
    if ((int)state->view != prev_view) {
        render->need_udev_refresh = 1;
        return;
    }
    switch (state->view) {
    case LULO_UDEV_VIEW_HWDB:
        if (state->hwdb_selected != prev_hwdb_selected) render->need_udev_refresh = 1;
        else if (state->hwdb_cursor != prev_hwdb_cursor ||
                 state->hwdb_list_scroll != prev_hwdb_list_scroll ||
                 state->hwdb_detail_scroll != prev_hwdb_detail_scroll ||
                 state->focus_preview != prev_focus) render->need_udev = 1;
        break;
    case LULO_UDEV_VIEW_DEVICES:
        if (state->device_selected != prev_device_selected) render->need_udev_refresh = 1;
        else if (state->device_cursor != prev_device_cursor ||
                 state->device_list_scroll != prev_device_list_scroll ||
                 state->device_detail_scroll != prev_device_detail_scroll ||
                 state->focus_preview != prev_focus) render->need_udev = 1;
        break;
    case LULO_UDEV_VIEW_RULES:
    default:
        if (state->rule_selected != prev_rule_selected) render->need_udev_refresh = 1;
        else if (state->rule_cursor != prev_rule_cursor ||
                 state->rule_list_scroll != prev_rule_list_scroll ||
                 state->rule_detail_scroll != prev_rule_detail_scroll ||
                 state->focus_preview != prev_focus) render->need_udev = 1;
        break;
    }
}

static void apply_input_action(Ui *ui, InputAction action, AppState *app,
                               const LuloProcSnapshot *proc_snap, LuloProcState *proc_state,
                               const LuloDizkSnapshot *dizk_snap, LuloDizkState *dizk_state,
                               const LuloSchedSnapshot *sched_snap, LuloSchedState *sched_state,
                               const LuloCgroupsSnapshot *cgroups_snap, LuloCgroupsState *cgroups_state,
                               const LuloUdevSnapshot *udev_snap, LuloUdevState *udev_state,
                               const LuloTuneSnapshot *tune_snap, LuloTuneState *tune_state,
                               const LuloSystemdSnapshot *systemd_snap, LuloSystemdState *systemd_state,
                               InputBackend input_backend, RawInput *rawin, DebugLog *dlog,
                               DashboardState *dash, int *sample_ms, long long *deadline,
                               int *exit_requested, int *need_resize, RenderFlags *render,
                               int scroll_units)
{
    int prev_selected = proc_state ? proc_state->selected : 0;
    int prev_scroll = proc_state ? proc_state->scroll : 0;
    int prev_sched_view = sched_state ? (int)sched_state->view : 0;
    int prev_sched_profile_cursor = sched_state ? sched_state->profile_cursor : -1;
    int prev_sched_profile_selected = sched_state ? sched_state->profile_selected : -1;
    int prev_sched_profile_list_scroll = sched_state ? sched_state->profile_list_scroll : 0;
    int prev_sched_profile_detail_scroll = sched_state ? sched_state->profile_detail_scroll : 0;
    int prev_sched_rule_cursor = sched_state ? sched_state->rule_cursor : -1;
    int prev_sched_rule_selected = sched_state ? sched_state->rule_selected : -1;
    int prev_sched_rule_list_scroll = sched_state ? sched_state->rule_list_scroll : 0;
    int prev_sched_rule_detail_scroll = sched_state ? sched_state->rule_detail_scroll : 0;
    int prev_sched_live_cursor = sched_state ? sched_state->live_cursor : -1;
    int prev_sched_live_selected = sched_state ? sched_state->live_selected : -1;
    int prev_sched_live_list_scroll = sched_state ? sched_state->live_list_scroll : 0;
    int prev_sched_live_detail_scroll = sched_state ? sched_state->live_detail_scroll : 0;
    int prev_sched_focus = sched_state ? sched_state->focus_preview : 0;
    int prev_cgroups_view = cgroups_state ? (int)cgroups_state->view : 0;
    char prev_cgroups_browse_path[320] = {0};
    int prev_cgroups_tree_cursor = cgroups_state ? cgroups_state->tree_cursor : -1;
    int prev_cgroups_tree_selected = cgroups_state ? cgroups_state->tree_selected : -1;
    int prev_cgroups_tree_list_scroll = cgroups_state ? cgroups_state->tree_list_scroll : 0;
    int prev_cgroups_tree_detail_scroll = cgroups_state ? cgroups_state->tree_detail_scroll : 0;
    int prev_cgroups_file_cursor = cgroups_state ? cgroups_state->file_cursor : -1;
    int prev_cgroups_file_selected = cgroups_state ? cgroups_state->file_selected : -1;
    int prev_cgroups_file_list_scroll = cgroups_state ? cgroups_state->file_list_scroll : 0;
    int prev_cgroups_file_detail_scroll = cgroups_state ? cgroups_state->file_detail_scroll : 0;
    int prev_cgroups_config_cursor = cgroups_state ? cgroups_state->config_cursor : -1;
    int prev_cgroups_config_selected = cgroups_state ? cgroups_state->config_selected : -1;
    int prev_cgroups_config_list_scroll = cgroups_state ? cgroups_state->config_list_scroll : 0;
    int prev_cgroups_config_detail_scroll = cgroups_state ? cgroups_state->config_detail_scroll : 0;
    int prev_cgroups_focus = cgroups_state ? cgroups_state->focus_preview : 0;
    int prev_udev_view = udev_state ? (int)udev_state->view : 0;
    int prev_udev_rule_cursor = udev_state ? udev_state->rule_cursor : -1;
    int prev_udev_rule_selected = udev_state ? udev_state->rule_selected : -1;
    int prev_udev_rule_list_scroll = udev_state ? udev_state->rule_list_scroll : 0;
    int prev_udev_rule_detail_scroll = udev_state ? udev_state->rule_detail_scroll : 0;
    int prev_udev_hwdb_cursor = udev_state ? udev_state->hwdb_cursor : -1;
    int prev_udev_hwdb_selected = udev_state ? udev_state->hwdb_selected : -1;
    int prev_udev_hwdb_list_scroll = udev_state ? udev_state->hwdb_list_scroll : 0;
    int prev_udev_hwdb_detail_scroll = udev_state ? udev_state->hwdb_detail_scroll : 0;
    int prev_udev_device_cursor = udev_state ? udev_state->device_cursor : -1;
    int prev_udev_device_selected = udev_state ? udev_state->device_selected : -1;
    int prev_udev_device_list_scroll = udev_state ? udev_state->device_list_scroll : 0;
    int prev_udev_device_detail_scroll = udev_state ? udev_state->device_detail_scroll : 0;
    int prev_udev_focus = udev_state ? udev_state->focus_preview : 0;
    int prev_systemd_cursor = systemd_state ? systemd_state->cursor : -1;
    int prev_systemd_selected = systemd_state ? systemd_state->selected : 0;
    int prev_systemd_config_cursor = systemd_state ? systemd_state->config_cursor : -1;
    int prev_systemd_config_selected = systemd_state ? systemd_state->config_selected : 0;
    int prev_systemd_list_scroll = systemd_state ? systemd_state->list_scroll : 0;
    int prev_systemd_file_scroll = systemd_state ? systemd_state->file_scroll : 0;
    int prev_systemd_config_list_scroll = systemd_state ? systemd_state->config_list_scroll : 0;
    int prev_systemd_config_file_scroll = systemd_state ? systemd_state->config_file_scroll : 0;
    int prev_systemd_focus = systemd_state ? systemd_state->focus_preview : 0;
    int prev_tune_view = tune_state ? (int)tune_state->view : 0;
    char prev_tune_browse_path[320] = {0};
    int prev_tune_cursor = tune_state ? tune_state->cursor : -1;
    int prev_tune_selected = tune_state ? tune_state->selected : -1;
    int prev_tune_list_scroll = tune_state ? tune_state->list_scroll : 0;
    int prev_tune_detail_scroll = tune_state ? tune_state->detail_scroll : 0;
    int prev_tune_snapshot_cursor = tune_state ? tune_state->snapshot_cursor : -1;
    int prev_tune_snapshot_selected = tune_state ? tune_state->snapshot_selected : -1;
    int prev_tune_snapshot_list_scroll = tune_state ? tune_state->snapshot_list_scroll : 0;
    int prev_tune_snapshot_detail_scroll = tune_state ? tune_state->snapshot_detail_scroll : 0;
    int prev_tune_preset_cursor = tune_state ? tune_state->preset_cursor : -1;
    int prev_tune_preset_selected = tune_state ? tune_state->preset_selected : -1;
    int prev_tune_preset_list_scroll = tune_state ? tune_state->preset_list_scroll : 0;
    int prev_tune_preset_detail_scroll = tune_state ? tune_state->preset_detail_scroll : 0;
    int prev_tune_focus = tune_state ? tune_state->focus_preview : 0;

    if (tune_state) snprintf(prev_tune_browse_path, sizeof(prev_tune_browse_path), "%s", tune_state->browse_path);
    if (cgroups_state) snprintf(prev_cgroups_browse_path, sizeof(prev_cgroups_browse_path), "%s", cgroups_state->browse_path);

    switch (action) {
    case INPUT_TOGGLE_HELP:
        app->help_visible = !app->help_visible;
        render->need_render = 1;
        break;
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
    case INPUT_TOGGLE_FOCUS:
        if (app->active_page == APP_PAGE_SCHED) {
            lulo_sched_toggle_focus(sched_state, sched_snap,
                                    sched_list_rows_visible(ui, sched_state),
                                    sched_preview_rows_visible(ui, sched_state));
            render->need_sched = 1;
            render->need_render = 1;
        } else if (app->active_page == APP_PAGE_CGROUPS) {
            lulo_cgroups_toggle_focus(cgroups_state, cgroups_snap,
                                      cgroups_list_rows_visible(ui, cgroups_state),
                                      cgroups_preview_rows_visible(ui, cgroups_state));
            render->need_cgroups = 1;
            render->need_render = 1;
        } else if (app->active_page == APP_PAGE_UDEV) {
            lulo_udev_toggle_focus(udev_state, udev_snap,
                                   udev_list_rows_visible(ui, udev_state),
                                   udev_preview_rows_visible(ui, udev_state));
            render->need_udev = 1;
            render->need_render = 1;
        } else if (app->active_page == APP_PAGE_SYSTEMD) {
            lulo_systemd_toggle_focus(systemd_state, systemd_snap,
                                      systemd_list_rows_visible(ui, systemd_state),
                                      systemd_preview_rows_visible(ui, systemd_state));
            render->need_systemd = 1;
            render->need_render = 1;
        } else if (app->active_page == APP_PAGE_TUNE) {
            lulo_tune_toggle_focus(tune_state, tune_snap,
                                   tune_list_rows_visible(ui, tune_state),
                                   tune_preview_rows_visible(ui, tune_state));
            render->need_tune = 1;
            render->need_render = 1;
        }
        break;
    case INPUT_SCROLL_UP:
        if (app->active_page == APP_PAGE_CPU) {
            int delta = scroll_units > 0 ? scroll_units : 1;

            lulo_proc_view_move(proc_state, proc_snap, ui->lo.proc_rows, -delta);
            schedule_proc_selection_redraw(render, prev_selected, prev_scroll,
                                           proc_state->selected, proc_state->scroll);
        } else if (app->active_page == APP_PAGE_DIZK) {
            int delta = scroll_units > 0 ? scroll_units : 1;

            lulo_dizk_view_move(dizk_state, dizk_snap, disk_visible_rows(ui), -delta);
            render->need_disk = 1;
        } else if (app->active_page == APP_PAGE_SCHED) {
            int delta = scroll_units > 0 ? scroll_units : 1;

            lulo_sched_view_move(sched_state, sched_snap,
                                 sched_list_rows_visible(ui, sched_state),
                                 sched_preview_rows_visible(ui, sched_state), -delta);
            update_sched_render_flags(render, sched_state,
                                      prev_sched_view,
                                      prev_sched_profile_cursor, prev_sched_profile_selected,
                                      prev_sched_profile_list_scroll, prev_sched_profile_detail_scroll,
                                      prev_sched_rule_cursor, prev_sched_rule_selected,
                                      prev_sched_rule_list_scroll, prev_sched_rule_detail_scroll,
                                      prev_sched_live_cursor, prev_sched_live_selected,
                                      prev_sched_live_list_scroll, prev_sched_live_detail_scroll,
                                      prev_sched_focus);
        } else if (app->active_page == APP_PAGE_CGROUPS) {
            int delta = scroll_units > 0 ? scroll_units : 1;

            lulo_cgroups_view_move(cgroups_state, cgroups_snap,
                                   cgroups_list_rows_visible(ui, cgroups_state),
                                   cgroups_preview_rows_visible(ui, cgroups_state), -delta);
            update_cgroups_render_flags(render, cgroups_state,
                                        prev_cgroups_view, prev_cgroups_browse_path,
                                        prev_cgroups_tree_cursor, prev_cgroups_tree_selected,
                                        prev_cgroups_tree_list_scroll, prev_cgroups_tree_detail_scroll,
                                        prev_cgroups_file_cursor, prev_cgroups_file_selected,
                                        prev_cgroups_file_list_scroll, prev_cgroups_file_detail_scroll,
                                        prev_cgroups_config_cursor, prev_cgroups_config_selected,
                                        prev_cgroups_config_list_scroll, prev_cgroups_config_detail_scroll,
                                        prev_cgroups_focus);
        } else if (app->active_page == APP_PAGE_UDEV) {
            int delta = scroll_units > 0 ? scroll_units : 1;

            lulo_udev_view_move(udev_state, udev_snap,
                                udev_list_rows_visible(ui, udev_state),
                                udev_preview_rows_visible(ui, udev_state), -delta);
            update_udev_render_flags(render, udev_state,
                                     prev_udev_view,
                                     prev_udev_rule_cursor, prev_udev_rule_selected,
                                     prev_udev_rule_list_scroll, prev_udev_rule_detail_scroll,
                                     prev_udev_hwdb_cursor, prev_udev_hwdb_selected,
                                     prev_udev_hwdb_list_scroll, prev_udev_hwdb_detail_scroll,
                                     prev_udev_device_cursor, prev_udev_device_selected,
                                     prev_udev_device_list_scroll, prev_udev_device_detail_scroll,
                                     prev_udev_focus);
        } else if (app->active_page == APP_PAGE_TUNE) {
            int delta = scroll_units > 0 ? scroll_units : 1;

            lulo_tune_view_move(tune_state, tune_snap,
                                tune_list_rows_visible(ui, tune_state),
                                tune_preview_rows_visible(ui, tune_state), -delta);
            update_tune_render_flags(render, tune_state,
                                     prev_tune_view, prev_tune_browse_path,
                                     prev_tune_cursor, prev_tune_selected,
                                     prev_tune_list_scroll, prev_tune_detail_scroll,
                                     prev_tune_snapshot_cursor, prev_tune_snapshot_selected,
                                     prev_tune_snapshot_list_scroll, prev_tune_snapshot_detail_scroll,
                                     prev_tune_preset_cursor, prev_tune_preset_selected,
                                     prev_tune_preset_list_scroll, prev_tune_preset_detail_scroll,
                                     prev_tune_focus);
        } else {
            int delta = scroll_units > 0 ? scroll_units : 1;

            lulo_systemd_view_move(systemd_state, systemd_snap,
                                   systemd_list_rows_visible(ui, systemd_state),
                                   systemd_preview_rows_visible(ui, systemd_state), -delta);
            update_systemd_render_flags(render, systemd_state,
                                        prev_systemd_cursor, prev_systemd_selected,
                                        prev_systemd_list_scroll, prev_systemd_file_scroll,
                                        prev_systemd_config_cursor, prev_systemd_config_selected,
                                        prev_systemd_config_list_scroll, prev_systemd_config_file_scroll,
                                        prev_systemd_focus);
        }
        render->need_render = 1;
        break;
    case INPUT_SCROLL_DOWN:
        if (app->active_page == APP_PAGE_CPU) {
            int delta = scroll_units > 0 ? scroll_units : 1;

            lulo_proc_view_move(proc_state, proc_snap, ui->lo.proc_rows, +delta);
            schedule_proc_selection_redraw(render, prev_selected, prev_scroll,
                                           proc_state->selected, proc_state->scroll);
        } else if (app->active_page == APP_PAGE_DIZK) {
            int delta = scroll_units > 0 ? scroll_units : 1;

            lulo_dizk_view_move(dizk_state, dizk_snap, disk_visible_rows(ui), +delta);
            render->need_disk = 1;
        } else if (app->active_page == APP_PAGE_SCHED) {
            int delta = scroll_units > 0 ? scroll_units : 1;

            lulo_sched_view_move(sched_state, sched_snap,
                                 sched_list_rows_visible(ui, sched_state),
                                 sched_preview_rows_visible(ui, sched_state), +delta);
            update_sched_render_flags(render, sched_state,
                                      prev_sched_view,
                                      prev_sched_profile_cursor, prev_sched_profile_selected,
                                      prev_sched_profile_list_scroll, prev_sched_profile_detail_scroll,
                                      prev_sched_rule_cursor, prev_sched_rule_selected,
                                      prev_sched_rule_list_scroll, prev_sched_rule_detail_scroll,
                                      prev_sched_live_cursor, prev_sched_live_selected,
                                      prev_sched_live_list_scroll, prev_sched_live_detail_scroll,
                                      prev_sched_focus);
        } else if (app->active_page == APP_PAGE_CGROUPS) {
            int delta = scroll_units > 0 ? scroll_units : 1;

            lulo_cgroups_view_move(cgroups_state, cgroups_snap,
                                   cgroups_list_rows_visible(ui, cgroups_state),
                                   cgroups_preview_rows_visible(ui, cgroups_state), +delta);
            update_cgroups_render_flags(render, cgroups_state,
                                        prev_cgroups_view, prev_cgroups_browse_path,
                                        prev_cgroups_tree_cursor, prev_cgroups_tree_selected,
                                        prev_cgroups_tree_list_scroll, prev_cgroups_tree_detail_scroll,
                                        prev_cgroups_file_cursor, prev_cgroups_file_selected,
                                        prev_cgroups_file_list_scroll, prev_cgroups_file_detail_scroll,
                                        prev_cgroups_config_cursor, prev_cgroups_config_selected,
                                        prev_cgroups_config_list_scroll, prev_cgroups_config_detail_scroll,
                                        prev_cgroups_focus);
        } else if (app->active_page == APP_PAGE_UDEV) {
            int delta = scroll_units > 0 ? scroll_units : 1;

            lulo_udev_view_move(udev_state, udev_snap,
                                udev_list_rows_visible(ui, udev_state),
                                udev_preview_rows_visible(ui, udev_state), +delta);
            update_udev_render_flags(render, udev_state,
                                     prev_udev_view,
                                     prev_udev_rule_cursor, prev_udev_rule_selected,
                                     prev_udev_rule_list_scroll, prev_udev_rule_detail_scroll,
                                     prev_udev_hwdb_cursor, prev_udev_hwdb_selected,
                                     prev_udev_hwdb_list_scroll, prev_udev_hwdb_detail_scroll,
                                     prev_udev_device_cursor, prev_udev_device_selected,
                                     prev_udev_device_list_scroll, prev_udev_device_detail_scroll,
                                     prev_udev_focus);
        } else if (app->active_page == APP_PAGE_TUNE) {
            int delta = scroll_units > 0 ? scroll_units : 1;

            lulo_tune_view_move(tune_state, tune_snap,
                                tune_list_rows_visible(ui, tune_state),
                                tune_preview_rows_visible(ui, tune_state), +delta);
            update_tune_render_flags(render, tune_state,
                                     prev_tune_view, prev_tune_browse_path,
                                     prev_tune_cursor, prev_tune_selected,
                                     prev_tune_list_scroll, prev_tune_detail_scroll,
                                     prev_tune_snapshot_cursor, prev_tune_snapshot_selected,
                                     prev_tune_snapshot_list_scroll, prev_tune_snapshot_detail_scroll,
                                     prev_tune_preset_cursor, prev_tune_preset_selected,
                                     prev_tune_preset_list_scroll, prev_tune_preset_detail_scroll,
                                     prev_tune_focus);
        } else {
            int delta = scroll_units > 0 ? scroll_units : 1;

            lulo_systemd_view_move(systemd_state, systemd_snap,
                                   systemd_list_rows_visible(ui, systemd_state),
                                   systemd_preview_rows_visible(ui, systemd_state), +delta);
            update_systemd_render_flags(render, systemd_state,
                                        prev_systemd_cursor, prev_systemd_selected,
                                        prev_systemd_list_scroll, prev_systemd_file_scroll,
                                        prev_systemd_config_cursor, prev_systemd_config_selected,
                                        prev_systemd_config_list_scroll, prev_systemd_config_file_scroll,
                                        prev_systemd_focus);
        }
        render->need_render = 1;
        break;
    case INPUT_SCROLL_LEFT:
        if (app->active_page == APP_PAGE_CPU) {
            int delta = (scroll_units > 0 ? scroll_units : 1) * 4;

            if (scroll_proc_horizontally(ui, proc_snap, proc_state, -delta)) {
                render->need_proc_body_only = 1;
                render->need_proc_cursor_only = 0;
                render->need_render = 1;
            }
        }
        break;
    case INPUT_SCROLL_RIGHT:
        if (app->active_page == APP_PAGE_CPU) {
            int delta = (scroll_units > 0 ? scroll_units : 1) * 4;

            if (scroll_proc_horizontally(ui, proc_snap, proc_state, +delta)) {
                render->need_proc_body_only = 1;
                render->need_proc_cursor_only = 0;
                render->need_render = 1;
            }
        }
        break;
    case INPUT_PAGE_UP:
        if (app->active_page == APP_PAGE_CPU) {
            lulo_proc_view_page(proc_state, proc_snap, ui->lo.proc_rows, -1);
            schedule_proc_selection_redraw(render, prev_selected, prev_scroll,
                                           proc_state->selected, proc_state->scroll);
        } else if (app->active_page == APP_PAGE_DIZK) {
            lulo_dizk_view_page(dizk_state, dizk_snap, disk_visible_rows(ui), -1);
            render->need_disk = 1;
        } else if (app->active_page == APP_PAGE_SCHED) {
            lulo_sched_view_page(sched_state, sched_snap,
                                 sched_list_rows_visible(ui, sched_state),
                                 sched_preview_rows_visible(ui, sched_state), -1);
            update_sched_render_flags(render, sched_state,
                                      prev_sched_view,
                                      prev_sched_profile_cursor, prev_sched_profile_selected,
                                      prev_sched_profile_list_scroll, prev_sched_profile_detail_scroll,
                                      prev_sched_rule_cursor, prev_sched_rule_selected,
                                      prev_sched_rule_list_scroll, prev_sched_rule_detail_scroll,
                                      prev_sched_live_cursor, prev_sched_live_selected,
                                      prev_sched_live_list_scroll, prev_sched_live_detail_scroll,
                                      prev_sched_focus);
        } else if (app->active_page == APP_PAGE_CGROUPS) {
            lulo_cgroups_view_page(cgroups_state, cgroups_snap,
                                   cgroups_list_rows_visible(ui, cgroups_state),
                                   cgroups_preview_rows_visible(ui, cgroups_state), -1);
            update_cgroups_render_flags(render, cgroups_state,
                                        prev_cgroups_view, prev_cgroups_browse_path,
                                        prev_cgroups_tree_cursor, prev_cgroups_tree_selected,
                                        prev_cgroups_tree_list_scroll, prev_cgroups_tree_detail_scroll,
                                        prev_cgroups_file_cursor, prev_cgroups_file_selected,
                                        prev_cgroups_file_list_scroll, prev_cgroups_file_detail_scroll,
                                        prev_cgroups_config_cursor, prev_cgroups_config_selected,
                                        prev_cgroups_config_list_scroll, prev_cgroups_config_detail_scroll,
                                        prev_cgroups_focus);
        } else if (app->active_page == APP_PAGE_UDEV) {
            lulo_udev_view_page(udev_state, udev_snap,
                                udev_list_rows_visible(ui, udev_state),
                                udev_preview_rows_visible(ui, udev_state), -1);
            update_udev_render_flags(render, udev_state,
                                     prev_udev_view,
                                     prev_udev_rule_cursor, prev_udev_rule_selected,
                                     prev_udev_rule_list_scroll, prev_udev_rule_detail_scroll,
                                     prev_udev_hwdb_cursor, prev_udev_hwdb_selected,
                                     prev_udev_hwdb_list_scroll, prev_udev_hwdb_detail_scroll,
                                     prev_udev_device_cursor, prev_udev_device_selected,
                                     prev_udev_device_list_scroll, prev_udev_device_detail_scroll,
                                     prev_udev_focus);
        } else if (app->active_page == APP_PAGE_TUNE) {
            lulo_tune_view_page(tune_state, tune_snap,
                                tune_list_rows_visible(ui, tune_state),
                                tune_preview_rows_visible(ui, tune_state), -1);
            update_tune_render_flags(render, tune_state,
                                     prev_tune_view, prev_tune_browse_path,
                                     prev_tune_cursor, prev_tune_selected,
                                     prev_tune_list_scroll, prev_tune_detail_scroll,
                                     prev_tune_snapshot_cursor, prev_tune_snapshot_selected,
                                     prev_tune_snapshot_list_scroll, prev_tune_snapshot_detail_scroll,
                                     prev_tune_preset_cursor, prev_tune_preset_selected,
                                     prev_tune_preset_list_scroll, prev_tune_preset_detail_scroll,
                                     prev_tune_focus);
        } else {
            lulo_systemd_view_page(systemd_state, systemd_snap,
                                   systemd_list_rows_visible(ui, systemd_state),
                                   systemd_preview_rows_visible(ui, systemd_state), -1);
            update_systemd_render_flags(render, systemd_state,
                                        prev_systemd_cursor, prev_systemd_selected,
                                        prev_systemd_list_scroll, prev_systemd_file_scroll,
                                        prev_systemd_config_cursor, prev_systemd_config_selected,
                                        prev_systemd_config_list_scroll, prev_systemd_config_file_scroll,
                                        prev_systemd_focus);
        }
        render->need_render = 1;
        break;
    case INPUT_PAGE_DOWN:
        if (app->active_page == APP_PAGE_CPU) {
            lulo_proc_view_page(proc_state, proc_snap, ui->lo.proc_rows, +1);
            schedule_proc_selection_redraw(render, prev_selected, prev_scroll,
                                           proc_state->selected, proc_state->scroll);
        } else if (app->active_page == APP_PAGE_DIZK) {
            lulo_dizk_view_page(dizk_state, dizk_snap, disk_visible_rows(ui), +1);
            render->need_disk = 1;
        } else if (app->active_page == APP_PAGE_SCHED) {
            lulo_sched_view_page(sched_state, sched_snap,
                                 sched_list_rows_visible(ui, sched_state),
                                 sched_preview_rows_visible(ui, sched_state), +1);
            update_sched_render_flags(render, sched_state,
                                      prev_sched_view,
                                      prev_sched_profile_cursor, prev_sched_profile_selected,
                                      prev_sched_profile_list_scroll, prev_sched_profile_detail_scroll,
                                      prev_sched_rule_cursor, prev_sched_rule_selected,
                                      prev_sched_rule_list_scroll, prev_sched_rule_detail_scroll,
                                      prev_sched_live_cursor, prev_sched_live_selected,
                                      prev_sched_live_list_scroll, prev_sched_live_detail_scroll,
                                      prev_sched_focus);
        } else if (app->active_page == APP_PAGE_CGROUPS) {
            lulo_cgroups_view_page(cgroups_state, cgroups_snap,
                                   cgroups_list_rows_visible(ui, cgroups_state),
                                   cgroups_preview_rows_visible(ui, cgroups_state), +1);
            update_cgroups_render_flags(render, cgroups_state,
                                        prev_cgroups_view, prev_cgroups_browse_path,
                                        prev_cgroups_tree_cursor, prev_cgroups_tree_selected,
                                        prev_cgroups_tree_list_scroll, prev_cgroups_tree_detail_scroll,
                                        prev_cgroups_file_cursor, prev_cgroups_file_selected,
                                        prev_cgroups_file_list_scroll, prev_cgroups_file_detail_scroll,
                                        prev_cgroups_config_cursor, prev_cgroups_config_selected,
                                        prev_cgroups_config_list_scroll, prev_cgroups_config_detail_scroll,
                                        prev_cgroups_focus);
        } else if (app->active_page == APP_PAGE_UDEV) {
            lulo_udev_view_page(udev_state, udev_snap,
                                udev_list_rows_visible(ui, udev_state),
                                udev_preview_rows_visible(ui, udev_state), +1);
            update_udev_render_flags(render, udev_state,
                                     prev_udev_view,
                                     prev_udev_rule_cursor, prev_udev_rule_selected,
                                     prev_udev_rule_list_scroll, prev_udev_rule_detail_scroll,
                                     prev_udev_hwdb_cursor, prev_udev_hwdb_selected,
                                     prev_udev_hwdb_list_scroll, prev_udev_hwdb_detail_scroll,
                                     prev_udev_device_cursor, prev_udev_device_selected,
                                     prev_udev_device_list_scroll, prev_udev_device_detail_scroll,
                                     prev_udev_focus);
        } else if (app->active_page == APP_PAGE_TUNE) {
            lulo_tune_view_page(tune_state, tune_snap,
                                tune_list_rows_visible(ui, tune_state),
                                tune_preview_rows_visible(ui, tune_state), +1);
            update_tune_render_flags(render, tune_state,
                                     prev_tune_view, prev_tune_browse_path,
                                     prev_tune_cursor, prev_tune_selected,
                                     prev_tune_list_scroll, prev_tune_detail_scroll,
                                     prev_tune_snapshot_cursor, prev_tune_snapshot_selected,
                                     prev_tune_snapshot_list_scroll, prev_tune_snapshot_detail_scroll,
                                     prev_tune_preset_cursor, prev_tune_preset_selected,
                                     prev_tune_preset_list_scroll, prev_tune_preset_detail_scroll,
                                     prev_tune_focus);
        } else {
            lulo_systemd_view_page(systemd_state, systemd_snap,
                                   systemd_list_rows_visible(ui, systemd_state),
                                   systemd_preview_rows_visible(ui, systemd_state), +1);
            update_systemd_render_flags(render, systemd_state,
                                        prev_systemd_cursor, prev_systemd_selected,
                                        prev_systemd_list_scroll, prev_systemd_file_scroll,
                                        prev_systemd_config_cursor, prev_systemd_config_selected,
                                        prev_systemd_config_list_scroll, prev_systemd_config_file_scroll,
                                        prev_systemd_focus);
        }
        render->need_render = 1;
        break;
    case INPUT_HOME:
        if (app->active_page == APP_PAGE_CPU) {
            lulo_proc_view_home(proc_state, proc_snap, ui->lo.proc_rows);
            schedule_proc_selection_redraw(render, prev_selected, prev_scroll,
                                           proc_state->selected, proc_state->scroll);
        } else if (app->active_page == APP_PAGE_DIZK) {
            lulo_dizk_view_home(dizk_state);
            render->need_disk = 1;
        } else if (app->active_page == APP_PAGE_SCHED) {
            lulo_sched_view_home(sched_state, sched_snap,
                                 sched_list_rows_visible(ui, sched_state),
                                 sched_preview_rows_visible(ui, sched_state));
            update_sched_render_flags(render, sched_state,
                                      prev_sched_view,
                                      prev_sched_profile_cursor, prev_sched_profile_selected,
                                      prev_sched_profile_list_scroll, prev_sched_profile_detail_scroll,
                                      prev_sched_rule_cursor, prev_sched_rule_selected,
                                      prev_sched_rule_list_scroll, prev_sched_rule_detail_scroll,
                                      prev_sched_live_cursor, prev_sched_live_selected,
                                      prev_sched_live_list_scroll, prev_sched_live_detail_scroll,
                                      prev_sched_focus);
        } else if (app->active_page == APP_PAGE_CGROUPS) {
            lulo_cgroups_view_home(cgroups_state, cgroups_snap,
                                   cgroups_list_rows_visible(ui, cgroups_state),
                                   cgroups_preview_rows_visible(ui, cgroups_state));
            update_cgroups_render_flags(render, cgroups_state,
                                        prev_cgroups_view, prev_cgroups_browse_path,
                                        prev_cgroups_tree_cursor, prev_cgroups_tree_selected,
                                        prev_cgroups_tree_list_scroll, prev_cgroups_tree_detail_scroll,
                                        prev_cgroups_file_cursor, prev_cgroups_file_selected,
                                        prev_cgroups_file_list_scroll, prev_cgroups_file_detail_scroll,
                                        prev_cgroups_config_cursor, prev_cgroups_config_selected,
                                        prev_cgroups_config_list_scroll, prev_cgroups_config_detail_scroll,
                                        prev_cgroups_focus);
        } else if (app->active_page == APP_PAGE_UDEV) {
            lulo_udev_view_home(udev_state, udev_snap,
                                udev_list_rows_visible(ui, udev_state),
                                udev_preview_rows_visible(ui, udev_state));
            update_udev_render_flags(render, udev_state,
                                     prev_udev_view,
                                     prev_udev_rule_cursor, prev_udev_rule_selected,
                                     prev_udev_rule_list_scroll, prev_udev_rule_detail_scroll,
                                     prev_udev_hwdb_cursor, prev_udev_hwdb_selected,
                                     prev_udev_hwdb_list_scroll, prev_udev_hwdb_detail_scroll,
                                     prev_udev_device_cursor, prev_udev_device_selected,
                                     prev_udev_device_list_scroll, prev_udev_device_detail_scroll,
                                     prev_udev_focus);
        } else if (app->active_page == APP_PAGE_TUNE) {
            lulo_tune_view_home(tune_state, tune_snap,
                                tune_list_rows_visible(ui, tune_state),
                                tune_preview_rows_visible(ui, tune_state));
            update_tune_render_flags(render, tune_state,
                                     prev_tune_view, prev_tune_browse_path,
                                     prev_tune_cursor, prev_tune_selected,
                                     prev_tune_list_scroll, prev_tune_detail_scroll,
                                     prev_tune_snapshot_cursor, prev_tune_snapshot_selected,
                                     prev_tune_snapshot_list_scroll, prev_tune_snapshot_detail_scroll,
                                     prev_tune_preset_cursor, prev_tune_preset_selected,
                                     prev_tune_preset_list_scroll, prev_tune_preset_detail_scroll,
                                     prev_tune_focus);
        } else {
            lulo_systemd_view_home(systemd_state, systemd_snap,
                                   systemd_list_rows_visible(ui, systemd_state),
                                   systemd_preview_rows_visible(ui, systemd_state));
            update_systemd_render_flags(render, systemd_state,
                                        prev_systemd_cursor, prev_systemd_selected,
                                        prev_systemd_list_scroll, prev_systemd_file_scroll,
                                        prev_systemd_config_cursor, prev_systemd_config_selected,
                                        prev_systemd_config_list_scroll, prev_systemd_config_file_scroll,
                                        prev_systemd_focus);
        }
        render->need_render = 1;
        break;
    case INPUT_END:
        if (app->active_page == APP_PAGE_CPU) {
            lulo_proc_view_end(proc_state, proc_snap, ui->lo.proc_rows);
            schedule_proc_selection_redraw(render, prev_selected, prev_scroll,
                                           proc_state->selected, proc_state->scroll);
        } else if (app->active_page == APP_PAGE_DIZK) {
            lulo_dizk_view_end(dizk_state, dizk_snap, disk_visible_rows(ui));
            render->need_disk = 1;
        } else if (app->active_page == APP_PAGE_SCHED) {
            lulo_sched_view_end(sched_state, sched_snap,
                                sched_list_rows_visible(ui, sched_state),
                                sched_preview_rows_visible(ui, sched_state));
            update_sched_render_flags(render, sched_state,
                                      prev_sched_view,
                                      prev_sched_profile_cursor, prev_sched_profile_selected,
                                      prev_sched_profile_list_scroll, prev_sched_profile_detail_scroll,
                                      prev_sched_rule_cursor, prev_sched_rule_selected,
                                      prev_sched_rule_list_scroll, prev_sched_rule_detail_scroll,
                                      prev_sched_live_cursor, prev_sched_live_selected,
                                      prev_sched_live_list_scroll, prev_sched_live_detail_scroll,
                                      prev_sched_focus);
        } else if (app->active_page == APP_PAGE_CGROUPS) {
            lulo_cgroups_view_end(cgroups_state, cgroups_snap,
                                  cgroups_list_rows_visible(ui, cgroups_state),
                                  cgroups_preview_rows_visible(ui, cgroups_state));
            update_cgroups_render_flags(render, cgroups_state,
                                        prev_cgroups_view, prev_cgroups_browse_path,
                                        prev_cgroups_tree_cursor, prev_cgroups_tree_selected,
                                        prev_cgroups_tree_list_scroll, prev_cgroups_tree_detail_scroll,
                                        prev_cgroups_file_cursor, prev_cgroups_file_selected,
                                        prev_cgroups_file_list_scroll, prev_cgroups_file_detail_scroll,
                                        prev_cgroups_config_cursor, prev_cgroups_config_selected,
                                        prev_cgroups_config_list_scroll, prev_cgroups_config_detail_scroll,
                                        prev_cgroups_focus);
        } else if (app->active_page == APP_PAGE_UDEV) {
            lulo_udev_view_end(udev_state, udev_snap,
                               udev_list_rows_visible(ui, udev_state),
                               udev_preview_rows_visible(ui, udev_state));
            update_udev_render_flags(render, udev_state,
                                     prev_udev_view,
                                     prev_udev_rule_cursor, prev_udev_rule_selected,
                                     prev_udev_rule_list_scroll, prev_udev_rule_detail_scroll,
                                     prev_udev_hwdb_cursor, prev_udev_hwdb_selected,
                                     prev_udev_hwdb_list_scroll, prev_udev_hwdb_detail_scroll,
                                     prev_udev_device_cursor, prev_udev_device_selected,
                                     prev_udev_device_list_scroll, prev_udev_device_detail_scroll,
                                     prev_udev_focus);
        } else if (app->active_page == APP_PAGE_TUNE) {
            lulo_tune_view_end(tune_state, tune_snap,
                               tune_list_rows_visible(ui, tune_state),
                               tune_preview_rows_visible(ui, tune_state));
            update_tune_render_flags(render, tune_state,
                                     prev_tune_view, prev_tune_browse_path,
                                     prev_tune_cursor, prev_tune_selected,
                                     prev_tune_list_scroll, prev_tune_detail_scroll,
                                     prev_tune_snapshot_cursor, prev_tune_snapshot_selected,
                                     prev_tune_snapshot_list_scroll, prev_tune_snapshot_detail_scroll,
                                     prev_tune_preset_cursor, prev_tune_preset_selected,
                                     prev_tune_preset_list_scroll, prev_tune_preset_detail_scroll,
                                     prev_tune_focus);
        } else {
            lulo_systemd_view_end(systemd_state, systemd_snap,
                                  systemd_list_rows_visible(ui, systemd_state),
                                  systemd_preview_rows_visible(ui, systemd_state));
            update_systemd_render_flags(render, systemd_state,
                                        prev_systemd_cursor, prev_systemd_selected,
                                        prev_systemd_list_scroll, prev_systemd_file_scroll,
                                        prev_systemd_config_cursor, prev_systemd_config_selected,
                                        prev_systemd_config_list_scroll, prev_systemd_config_file_scroll,
                                        prev_systemd_focus);
        }
        render->need_render = 1;
        break;
    case INPUT_TAB_NEXT:
        app->active_page = (AppPage)((app->active_page + 1) % APP_PAGE_COUNT);
        if (app->active_page == APP_PAGE_SCHED) render->need_sched_refresh = 1;
        else if (app->active_page == APP_PAGE_CGROUPS) render->need_cgroups_refresh_full = 1;
        else if (app->active_page == APP_PAGE_UDEV) render->need_udev_refresh_full = 1;
        else if (app->active_page == APP_PAGE_SYSTEMD) render->need_systemd_refresh = 1;
        else if (app->active_page == APP_PAGE_TUNE) render->need_tune_refresh_full = 1;
        render->need_rebuild = 1;
        render->need_render = 1;
        break;
    case INPUT_TAB_PREV:
        app->active_page = (AppPage)((app->active_page + APP_PAGE_COUNT - 1) % APP_PAGE_COUNT);
        if (app->active_page == APP_PAGE_SCHED) render->need_sched_refresh = 1;
        else if (app->active_page == APP_PAGE_CGROUPS) render->need_cgroups_refresh_full = 1;
        else if (app->active_page == APP_PAGE_UDEV) render->need_udev_refresh_full = 1;
        else if (app->active_page == APP_PAGE_SYSTEMD) render->need_systemd_refresh = 1;
        else if (app->active_page == APP_PAGE_TUNE) render->need_tune_refresh_full = 1;
        render->need_rebuild = 1;
        render->need_render = 1;
        break;
    case INPUT_VIEW_NEXT:
        if (app->active_page == APP_PAGE_SCHED) {
            lulo_sched_next_view(sched_state);
            render->need_sched_refresh = 1;
            render->need_render = 1;
        } else if (app->active_page == APP_PAGE_CGROUPS) {
            lulo_cgroups_next_view(cgroups_state);
            render->need_cgroups_refresh = 1;
            render->need_render = 1;
        } else if (app->active_page == APP_PAGE_UDEV) {
            lulo_udev_next_view(udev_state);
            render->need_udev_refresh = 1;
            render->need_render = 1;
        } else if (app->active_page == APP_PAGE_SYSTEMD) {
            lulo_systemd_next_view(systemd_state);
            render->need_systemd_refresh = 1;
            render->need_render = 1;
        } else if (app->active_page == APP_PAGE_TUNE) {
            lulo_tune_next_view(tune_state);
            render->need_tune_refresh = 1;
            render->need_render = 1;
        }
        break;
    case INPUT_VIEW_PREV:
        if (app->active_page == APP_PAGE_SCHED) {
            lulo_sched_prev_view(sched_state);
            render->need_sched_refresh = 1;
            render->need_render = 1;
        } else if (app->active_page == APP_PAGE_CGROUPS) {
            lulo_cgroups_prev_view(cgroups_state);
            render->need_cgroups_refresh = 1;
            render->need_render = 1;
        } else if (app->active_page == APP_PAGE_UDEV) {
            lulo_udev_prev_view(udev_state);
            render->need_udev_refresh = 1;
            render->need_render = 1;
        } else if (app->active_page == APP_PAGE_SYSTEMD) {
            lulo_systemd_prev_view(systemd_state);
            render->need_systemd_refresh = 1;
            render->need_render = 1;
        } else if (app->active_page == APP_PAGE_TUNE) {
            lulo_tune_prev_view(tune_state);
            render->need_tune_refresh = 1;
            render->need_render = 1;
        }
        break;
    case INPUT_TOGGLE_BRANCH:
        if (app->active_page == APP_PAGE_CPU &&
            lulo_proc_toggle_row(proc_state, proc_snap, proc_state->selected) > 0) {
            render->need_proc_refresh = 1;
            render->need_render = 1;
        } else if (app->active_page == APP_PAGE_SCHED &&
                   lulo_sched_open_current(sched_state, sched_snap,
                                           sched_list_rows_visible(ui, sched_state),
                                           sched_preview_rows_visible(ui, sched_state))) {
            render->need_sched_refresh = 1;
            render->need_render = 1;
        } else if (app->active_page == APP_PAGE_CGROUPS) {
            int rc = lulo_cgroups_open_current(cgroups_state, cgroups_snap,
                                               cgroups_list_rows_visible(ui, cgroups_state),
                                               cgroups_preview_rows_visible(ui, cgroups_state));
            if (rc == 2) render->need_cgroups_refresh_full = 1;
            else if (rc == 1) render->need_cgroups_refresh = 1;
            if (rc > 0) render->need_render = 1;
        } else if (app->active_page == APP_PAGE_UDEV) {
            int rc = lulo_udev_open_current(udev_state, udev_snap,
                                            udev_list_rows_visible(ui, udev_state),
                                            udev_preview_rows_visible(ui, udev_state));
            if (rc > 0) {
                render->need_udev_refresh = 1;
                render->need_render = 1;
            }
        } else if (app->active_page == APP_PAGE_TUNE) {
            int rc = lulo_tune_open_current(tune_state, tune_snap,
                                            tune_list_rows_visible(ui, tune_state),
                                            tune_preview_rows_visible(ui, tune_state));
            if (rc == 2) render->need_tune_refresh_full = 1;
            else if (rc == 1) render->need_tune_refresh = 1;
            if (rc > 0) render->need_render = 1;
        } else if (app->active_page == APP_PAGE_SYSTEMD &&
                   lulo_systemd_open_current(systemd_state, systemd_snap,
                                             systemd_list_rows_visible(ui, systemd_state),
                                             systemd_preview_rows_visible(ui, systemd_state))) {
            render->need_systemd_refresh = 1;
            render->need_render = 1;
        }
        break;
    case INPUT_RELOAD_PAGE:
        if (app->active_page == APP_PAGE_SCHED) {
            render->need_sched_reload = 1;
            render->need_sched = 1;
            sched_status_set(app, "reloading scheduler config");
            render->need_load = 1;
            render->need_render = 1;
        } else if (app->active_page == APP_PAGE_CGROUPS) {
            render->need_cgroups_refresh_full = 1;
            render->need_cgroups = 1;
            cgroups_status_set(app, "reloading cgroups view");
            render->need_load = 1;
            render->need_render = 1;
        } else if (app->active_page == APP_PAGE_UDEV) {
            render->need_udev_refresh_full = 1;
            render->need_udev = 1;
            udev_status_set(app, "reloading udev view");
            render->need_load = 1;
            render->need_render = 1;
        }
        break;
    case INPUT_NEW_ITEM:
        if (app->active_page == APP_PAGE_SCHED) {
            char new_path[PATH_MAX];
            char *content = NULL;
            char err[192];
            int fatal = 0;
            int rc;

            if (sched_prepare_new_entry(sched_snap, sched_state, new_path, sizeof(new_path),
                                        &content, err, sizeof(err)) < 0) {
                sched_status_set(app, "%s", err[0] ? err : "create failed");
                render->need_load = 1;
                render->need_render = 1;
                break;
            }
            rc = lulo_edit_write_file(new_path, content, err, sizeof(err));
            free(content);
            if (rc < 0) {
                sched_status_set(app, "%s", err[0] ? err : "create failed");
                render->need_load = 1;
                render->need_render = 1;
                break;
            }
            rc = run_external_edit(ui, input_backend, rawin, dlog, new_path, &fatal, err, sizeof(err));
            render->need_rebuild = 1;
            render->need_render = 1;
            if (fatal) {
                fprintf(stderr, "%s\n", err[0] ? err : "failed to restore terminal after editing");
                *exit_requested = 1;
                break;
            }
            if (rc > 0) {
                sched_status_set(app, "saved %s", path_basename_local(new_path));
                render->need_sched_reload = 1;
                render->need_sched = 1;
            } else if (rc == 0) {
                sched_status_set(app, "created %s", path_basename_local(new_path));
                render->need_sched_reload = 1;
                render->need_sched = 1;
            } else {
                sched_status_set(app, "%s", err[0] ? err : "edit failed");
            }
            render->need_load = 1;
        } else if (app->active_page == APP_PAGE_TUNE) {
            char new_path[320];
            char new_id[96];
            char *content = NULL;
            char err[192];
            int fatal = 0;
            int rc;

            if (tune_prepare_new_bundle(tune_snap, tune_state, new_path, sizeof(new_path),
                                        new_id, sizeof(new_id),
                                        &content, err, sizeof(err)) < 0) {
                tune_status_set(app, "%s", err[0] ? err : "create failed");
                render->need_load = 1;
                render->need_render = 1;
                break;
            }
            rc = lulo_edit_write_file(new_path, content, err, sizeof(err));
            free(content);
            if (rc < 0) {
                tune_status_set(app, "%s", err[0] ? err : "create failed");
                render->need_load = 1;
                render->need_render = 1;
                break;
            }
            rc = run_external_edit(ui, input_backend, rawin, dlog, new_path, &fatal, err, sizeof(err));
            render->need_rebuild = 1;
            render->need_render = 1;
            if (fatal) {
                fprintf(stderr, "%s\n", err[0] ? err : "failed to restore terminal after editing");
                *exit_requested = 1;
                break;
            }
            if (tune_state->view == LULO_TUNE_VIEW_SNAPSHOTS) {
                snprintf(tune_state->selected_snapshot_id, sizeof(tune_state->selected_snapshot_id), "%s", new_id);
                tune_state->snapshot_selected = -1;
            } else if (tune_state->view == LULO_TUNE_VIEW_PRESETS) {
                snprintf(tune_state->selected_preset_id, sizeof(tune_state->selected_preset_id), "%s", new_id);
                tune_state->preset_selected = -1;
            }
            if (rc > 0) {
                tune_status_set(app, "saved %s", path_basename_local(new_path));
            } else if (rc == 0) {
                tune_status_set(app, "created %s", path_basename_local(new_path));
            } else {
                tune_status_set(app, "%s", err[0] ? err : "edit failed");
            }
            render->need_tune_refresh_full = 1;
            render->need_tune = 1;
            render->need_load = 1;
        }
        break;
    case INPUT_DELETE_SELECTED:
        if (app->active_page == APP_PAGE_SCHED) {
            const char *delete_path = active_sched_delete_path(sched_snap, sched_state);
            char err[192];

            if (!delete_path) {
                if (sched_state->view == LULO_SCHED_VIEW_RULES && active_sched_is_builtin_rule(sched_snap, sched_state)) {
                    sched_status_set(app, "built-in rules live in scheduler.conf");
                } else {
                    sched_status_set(app, "delete from Profiles or file-backed Rules");
                }
                render->need_load = 1;
                render->need_render = 1;
                break;
            }
            if (lulo_edit_delete_file(delete_path, err, sizeof(err)) < 0) {
                sched_status_set(app, "%s", err[0] ? err : "delete failed");
            } else {
                sched_status_set(app, "deleted %s", path_basename_local(delete_path));
                render->need_sched_reload = 1;
                render->need_sched = 1;
            }
            render->need_load = 1;
            render->need_render = 1;
        } else if (app->active_page == APP_PAGE_TUNE) {
            char delete_path[320];
            char err[192];

            if (!active_tune_bundle_delete_path(tune_snap, tune_state, delete_path, sizeof(delete_path))) {
                tune_status_set(app, "delete from Snapshots or Presets");
                render->need_load = 1;
                render->need_render = 1;
                break;
            }
            if (lulo_edit_delete_file(delete_path, err, sizeof(err)) < 0) {
                tune_status_set(app, "%s", err[0] ? err : "delete failed");
            } else {
                if (tune_state->view == LULO_TUNE_VIEW_SNAPSHOTS) {
                    tune_state->selected_snapshot_id[0] = '\0';
                    tune_state->snapshot_selected = -1;
                } else if (tune_state->view == LULO_TUNE_VIEW_PRESETS) {
                    tune_state->selected_preset_id[0] = '\0';
                    tune_state->preset_selected = -1;
                }
                tune_status_set(app, "deleted %s", path_basename_local(delete_path));
                render->need_tune_refresh_full = 1;
                render->need_tune = 1;
            }
            render->need_load = 1;
            render->need_render = 1;
        }
        break;
    case INPUT_RENAME_SELECTED:
        if (app->active_page == APP_PAGE_TUNE) {
            if (start_tune_bundle_rename(app, tune_snap, tune_state)) {
                render->need_tune = 1;
                render->need_render = 1;
            } else {
                render->need_tune = 1;
                render->need_render = 1;
            }
        }
        break;
    case INPUT_SAVE_SNAPSHOT:
        if (app->active_page == APP_PAGE_TUNE) {
            if (tune_state->view == LULO_TUNE_VIEW_EXPLORE && active_tune_explore_row(tune_snap, tune_state)) {
                render->need_tune_save_snapshot = 1;
                render->need_tune = 1;
                render->need_render = 1;
            } else {
                tune_status_set(app, "save snapshots from Explore");
                render->need_tune = 1;
                render->need_render = 1;
            }
        }
        break;
    case INPUT_SAVE_PRESET:
        if (app->active_page == APP_PAGE_TUNE) {
            if (tune_state->view == LULO_TUNE_VIEW_EXPLORE && active_tune_explore_row(tune_snap, tune_state)) {
                render->need_tune_save_preset = 1;
                render->need_tune = 1;
                render->need_render = 1;
            } else {
                tune_status_set(app, "save presets from Explore");
                render->need_tune = 1;
                render->need_render = 1;
            }
        }
        break;
    case INPUT_EDIT_SELECTED:
        if (app->active_page == APP_PAGE_TUNE) {
            if (tune_state->view == LULO_TUNE_VIEW_EXPLORE) {
                if (start_tune_edit(app, tune_snap, tune_state)) {
                    render->need_tune = 1;
                    render->need_render = 1;
                } else {
                    render->need_tune = 1;
                    render->need_render = 1;
                }
            } else {
                char edit_path[320];
                char err[192];
                int fatal = 0;
                int rc;

                if (!active_tune_bundle_edit_path(tune_snap, tune_state, edit_path, sizeof(edit_path))) {
                    tune_status_set(app, "edit from Snapshots or Presets");
                    render->need_load = 1;
                    render->need_render = 1;
                    break;
                }
                rc = run_external_edit(ui, input_backend, rawin, dlog, edit_path, &fatal, err, sizeof(err));
                render->need_rebuild = 1;
                render->need_render = 1;
                if (fatal) {
                    fprintf(stderr, "%s\n", err[0] ? err : "failed to restore terminal after editing");
                    *exit_requested = 1;
                    break;
                }
                if (rc > 0) {
                    tune_status_set(app, "saved %s", path_basename_local(edit_path));
                } else if (rc == 0) {
                    tune_status_set(app, "edit cancelled");
                } else {
                    tune_status_set(app, "%s", err[0] ? err : "edit failed");
                }
                render->need_tune_refresh_full = 1;
                render->need_tune = 1;
                render->need_load = 1;
            }
        } else if (app->active_page == APP_PAGE_CGROUPS) {
            const char *edit_path = active_cgroups_edit_path(cgroups_snap, cgroups_state);
            char err[192];
            int fatal = 0;
            int rc;

            if (!edit_path) {
                cgroups_status_set(app, "edit from Files or Config");
                render->need_load = 1;
                render->need_render = 1;
                break;
            }
            rc = run_external_edit(ui, input_backend, rawin, dlog, edit_path, &fatal, err, sizeof(err));
            render->need_rebuild = 1;
            render->need_render = 1;
            if (fatal) {
                fprintf(stderr, "%s\n", err[0] ? err : "failed to restore terminal after editing");
                *exit_requested = 1;
                break;
            }
            if (rc > 0) {
                cgroups_status_set(app, "saved %s", path_basename_local(edit_path));
                render->need_cgroups_refresh_full = 1;
                render->need_cgroups = 1;
            } else if (rc == 0) {
                cgroups_status_set(app, "edit cancelled");
            } else {
                cgroups_status_set(app, "%s", err[0] ? err : "edit failed");
            }
            render->need_load = 1;
        } else if (app->active_page == APP_PAGE_UDEV) {
            const char *edit_path = active_udev_edit_path(udev_snap, udev_state);
            char err[192];
            int fatal = 0;
            int rc;

            if (!edit_path) {
                udev_status_set(app, "edit from Rules or Hwdb");
                render->need_load = 1;
                render->need_render = 1;
                break;
            }
            rc = run_external_edit(ui, input_backend, rawin, dlog, edit_path, &fatal, err, sizeof(err));
            render->need_rebuild = 1;
            render->need_render = 1;
            if (fatal) {
                fprintf(stderr, "%s\n", err[0] ? err : "failed to restore terminal after editing");
                *exit_requested = 1;
                break;
            }
            if (rc > 0) {
                udev_status_set(app, "saved %s", path_basename_local(edit_path));
                render->need_udev_refresh_full = 1;
                render->need_udev = 1;
            } else if (rc == 0) {
                udev_status_set(app, "edit cancelled");
            } else {
                udev_status_set(app, "%s", err[0] ? err : "edit failed");
            }
            render->need_load = 1;
        } else if (app->active_page == APP_PAGE_SCHED) {
            const char *edit_path = active_sched_edit_path(sched_snap, sched_state);
            char err[192];
            int fatal = 0;
            int rc;

            if (!edit_path) {
                sched_status_set(app, "edit from Profiles or Rules");
                render->need_load = 1;
                render->need_render = 1;
                break;
            }
            rc = run_external_edit(ui, input_backend, rawin, dlog, edit_path, &fatal, err, sizeof(err));
            render->need_rebuild = 1;
            render->need_render = 1;
            if (fatal) {
                fprintf(stderr, "%s\n", err[0] ? err : "failed to restore terminal after editing");
                *exit_requested = 1;
                break;
            }
            if (rc > 0) {
                sched_status_set(app, "saved %s", path_basename_local(edit_path));
                render->need_sched_reload = 1;
                render->need_sched = 1;
            } else if (rc == 0) {
                sched_status_set(app, "edit cancelled");
            } else {
                sched_status_set(app, "%s", err[0] ? err : "edit failed");
            }
            render->need_load = 1;
        } else if (app->active_page == APP_PAGE_SYSTEMD) {
            const char *edit_path = active_systemd_edit_path(systemd_snap, systemd_state);
            char err[192];
            int fatal = 0;
            int rc;

            if (!edit_path) {
                if (systemd_state->view == LULO_SYSTEMD_VIEW_CONFIG) {
                    app_status_set(app, "edit from Config");
                } else {
                    app_status_set(app, "open a service first");
                }
                render->need_footer = 1;
                render->need_render = 1;
                break;
            }
            rc = run_external_edit(ui, input_backend, rawin, dlog, edit_path, &fatal, err, sizeof(err));
            render->need_rebuild = 1;
            render->need_render = 1;
            if (fatal) {
                fprintf(stderr, "%s\n", err[0] ? err : "failed to restore terminal after editing");
                *exit_requested = 1;
                break;
            }
            if (rc > 0) {
                app_status_set(app, "saved %s", path_basename_local(edit_path));
                render->need_systemd_refresh = 1;
                render->need_systemd = 1;
            } else if (rc == 0) {
                app_status_set(app, "edit cancelled");
            } else {
                app_status_set(app, "%s", err[0] ? err : "edit failed");
            }
            render->need_footer = 1;
        }
        break;
    case INPUT_APPLY_SELECTED:
        if (app->active_page == APP_PAGE_TUNE) {
            if (tune_state->view == LULO_TUNE_VIEW_EXPLORE) {
                if (active_tune_row_is_staged(tune_snap, tune_state)) {
                    render->need_tune_apply_selected = 1;
                    render->need_tune = 1;
                    render->need_render = 1;
                } else {
                    tune_status_set(app, "stage a value first with i");
                    render->need_tune = 1;
                    render->need_render = 1;
                }
            } else if (tune_state->view == LULO_TUNE_VIEW_SNAPSHOTS || tune_state->view == LULO_TUNE_VIEW_PRESETS) {
                render->need_tune_apply_selected = 1;
                render->need_tune = 1;
                render->need_render = 1;
            } else {
                tune_status_set(app, "apply uses the Snapshots or Presets views");
                render->need_tune = 1;
                render->need_render = 1;
            }
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
                           const LuloSchedSnapshot *sched_snap, const LuloSchedState *sched_state,
                           const LuloSchedBackendStatus *sched_backend_status,
                           const LuloCgroupsSnapshot *cgroups_snap, const LuloCgroupsState *cgroups_state,
                           const LuloCgroupsBackendStatus *cgroups_backend_status,
                           const LuloUdevSnapshot *udev_snap, const LuloUdevState *udev_state,
                           const LuloUdevBackendStatus *udev_backend_status,
                           const LuloTuneSnapshot *tune_snap, const LuloTuneState *tune_state,
                           const LuloTuneBackendStatus *tune_backend_status,
                           const LuloSystemdSnapshot *systemd_snap, const LuloSystemdState *systemd_state,
                           const LuloSystemdBackendStatus *systemd_backend_status,
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
    } else if (app->active_page == APP_PAGE_DIZK) {
        if (render->need_header) render_header_only(ui, dash, app);
        if (render->need_disk || render->need_load) render_disk_only(ui, dizk_snap, dizk_state);
    } else if (app->active_page == APP_PAGE_SCHED) {
        if (render->need_header) render_header_only(ui, dash, app);
        if (render->need_sched || render->need_load) render_sched_only(ui, sched_snap, sched_state, app, sched_backend_status);
    } else if (app->active_page == APP_PAGE_CGROUPS) {
        if (render->need_header) render_header_only(ui, dash, app);
        if (render->need_cgroups || render->need_load) {
            render_cgroups_only(ui, cgroups_snap, cgroups_state, app, cgroups_backend_status);
        }
    } else if (app->active_page == APP_PAGE_UDEV) {
        if (render->need_header) render_header_only(ui, dash, app);
        if (render->need_udev || render->need_load) {
            render_udev_only(ui, udev_snap, udev_state, app, udev_backend_status);
        }
    } else if (app->active_page == APP_PAGE_TUNE) {
        if (render->need_header) render_header_only(ui, dash, app);
        if (render->need_tune || render->need_load) render_tune_only(ui, tune_snap, tune_state, app, tune_backend_status);
    } else {
        if (render->need_header) render_header_only(ui, dash, app);
        if (render->need_systemd || render->need_load) {
            render_systemd_only(ui, systemd_snap, systemd_state, systemd_backend_status);
        }
    }
    if (app->help_visible) render_help_overlay(ui, app->active_page);
    else destroy_plane(&ui->modal);
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
    LuloSchedState sched_state;
    LuloSchedBackend sched_backend;
    LuloCgroupsState cgroups_state;
    LuloCgroupsBackend cgroups_backend;
    LuloUdevState udev_state;
    LuloUdevBackend udev_backend;
    LuloTuneState tune_state;
    LuloTuneBackend tune_backend;
    LuloSystemdState systemd_state;
    LuloSystemdBackend systemd_backend;
    LuloProcSnapshot proc_snap = {0};
    LuloSchedSnapshot sched_snap = {0};
    LuloSchedBackendStatus sched_backend_status = {0};
    LuloCgroupsSnapshot cgroups_snap = {0};
    LuloCgroupsBackendStatus cgroups_backend_status = {0};
    LuloUdevSnapshot udev_snap = {0};
    LuloUdevBackendStatus udev_backend_status = {0};
    LuloTuneSnapshot tune_snap = {0};
    LuloTuneBackendStatus tune_backend_status = {0};
    LuloSystemdSnapshot systemd_snap = {0};
    LuloSystemdBackendStatus systemd_backend_status = {0};
    AppState app = {
        .active_page = APP_PAGE_CPU,
        .proc_refresh_ms = 1000,
        .proc_cpu_mode = LULO_PROC_CPU_TOTAL,
    };
    Ui ui;
    RawInput rawin;
    notcurses_options opts;
    DebugLog dlog;
    const char *env_input_backend;
    unsigned long long proc_cpu_accum_delta = 0;
    unsigned long long proc_last_total_delta = 0;
    long long proc_due_ms = 0;
    long long sched_due_ms = 0;
    long long cgroups_due_ms = 0;
    long long udev_due_ms = 0;
    long long tune_due_ms = 0;
    long long systemd_due_ms = 0;
    int proc_snapshot_valid = 0;
    int sched_snapshot_valid = 0;
    int cgroups_snapshot_valid = 0;
    int udev_snapshot_valid = 0;
    int tune_snapshot_valid = 0;
    int systemd_snapshot_valid = 0;
    unsigned sched_generation = 0;
    unsigned cgroups_generation = 0;
    unsigned udev_generation = 0;
    unsigned tune_generation = 0;
    unsigned systemd_generation = 0;
    LuloProcCpuMode proc_snapshot_mode = LULO_PROC_CPU_TOTAL;

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
    ui.nc = notcurses_core_init(&opts, NULL);
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
    lulo_sched_state_init(&sched_state);
    lulo_cgroups_state_init(&cgroups_state);
    lulo_udev_state_init(&udev_state);
    lulo_tune_state_init(&tune_state);
    lulo_systemd_state_init(&systemd_state);
    if (lulo_sched_backend_start(&sched_backend) < 0) {
        fprintf(stderr, "failed to start sched backend\n");
        lulo_systemd_state_cleanup(&systemd_state);
        lulo_tune_state_cleanup(&tune_state);
        lulo_udev_state_cleanup(&udev_state);
        lulo_cgroups_state_cleanup(&cgroups_state);
        lulo_sched_state_cleanup(&sched_state);
        lulo_dizk_state_cleanup(&dizk_state);
        lulo_proc_state_cleanup(&proc_state);
        notcurses_stop(ui.nc);
        debug_log_close(&dlog);
        return 1;
    }
    if (lulo_cgroups_backend_start(&cgroups_backend) < 0) {
        fprintf(stderr, "failed to start cgroups backend\n");
        lulo_sched_backend_stop(&sched_backend);
        lulo_systemd_state_cleanup(&systemd_state);
        lulo_tune_state_cleanup(&tune_state);
        lulo_udev_state_cleanup(&udev_state);
        lulo_cgroups_state_cleanup(&cgroups_state);
        lulo_sched_state_cleanup(&sched_state);
        lulo_dizk_state_cleanup(&dizk_state);
        lulo_proc_state_cleanup(&proc_state);
        notcurses_stop(ui.nc);
        debug_log_close(&dlog);
        return 1;
    }
    if (lulo_udev_backend_start(&udev_backend) < 0) {
        fprintf(stderr, "failed to start udev backend\n");
        lulo_cgroups_backend_stop(&cgroups_backend);
        lulo_sched_backend_stop(&sched_backend);
        lulo_systemd_state_cleanup(&systemd_state);
        lulo_tune_state_cleanup(&tune_state);
        lulo_udev_state_cleanup(&udev_state);
        lulo_cgroups_state_cleanup(&cgroups_state);
        lulo_sched_state_cleanup(&sched_state);
        lulo_dizk_state_cleanup(&dizk_state);
        lulo_proc_state_cleanup(&proc_state);
        notcurses_stop(ui.nc);
        debug_log_close(&dlog);
        return 1;
    }
    if (lulo_tune_backend_start(&tune_backend) < 0) {
        fprintf(stderr, "failed to start tune backend\n");
        lulo_udev_backend_stop(&udev_backend);
        lulo_cgroups_backend_stop(&cgroups_backend);
        lulo_sched_backend_stop(&sched_backend);
        lulo_systemd_state_cleanup(&systemd_state);
        lulo_tune_state_cleanup(&tune_state);
        lulo_udev_state_cleanup(&udev_state);
        lulo_cgroups_state_cleanup(&cgroups_state);
        lulo_sched_state_cleanup(&sched_state);
        lulo_dizk_state_cleanup(&dizk_state);
        lulo_proc_state_cleanup(&proc_state);
        notcurses_stop(ui.nc);
        debug_log_close(&dlog);
        return 1;
    }
    if (lulo_systemd_backend_start(&systemd_backend) < 0) {
        fprintf(stderr, "failed to start systemd backend\n");
        lulo_tune_backend_stop(&tune_backend);
        lulo_udev_backend_stop(&udev_backend);
        lulo_cgroups_backend_stop(&cgroups_backend);
        lulo_sched_backend_stop(&sched_backend);
        lulo_dizk_state_cleanup(&dizk_state);
        lulo_proc_state_cleanup(&proc_state);
        lulo_sched_state_cleanup(&sched_state);
        lulo_cgroups_state_cleanup(&cgroups_state);
        lulo_udev_state_cleanup(&udev_state);
        lulo_tune_state_cleanup(&tune_state);
        lulo_systemd_state_cleanup(&systemd_state);
        notcurses_stop(ui.nc);
        debug_log_close(&dlog);
        return 1;
    }
    lulo_sched_backend_request_full(&sched_backend, &sched_state);
    lulo_sched_backend_status(&sched_backend, &sched_backend_status);
    sched_due_ms = mono_ms_now() + effective_sched_refresh_ms(&sched_snap);
    lulo_cgroups_backend_request_full(&cgroups_backend, &cgroups_state);
    lulo_cgroups_backend_status(&cgroups_backend, &cgroups_backend_status);
    cgroups_due_ms = mono_ms_now() + 5000;
    lulo_udev_backend_request_full(&udev_backend, &udev_state);
    lulo_udev_backend_status(&udev_backend, &udev_backend_status);
    udev_due_ms = mono_ms_now() + 5000;
    lulo_tune_backend_request_full(&tune_backend, &tune_state);
    lulo_tune_backend_status(&tune_backend, &tune_backend_status);
    tune_due_ms = mono_ms_now() + 5000;
    lulo_systemd_backend_request_full(&systemd_backend, &systemd_state);
    lulo_systemd_backend_status(&systemd_backend, &systemd_backend_status);
    systemd_due_ms = mono_ms_now() + 5000;
    if (lulo_read_cpu_stat(&stat_a) < 0) {
        fprintf(stderr, "failed to read /proc/stat\n");
        lulo_sched_backend_stop(&sched_backend);
        lulo_cgroups_backend_stop(&cgroups_backend);
        lulo_udev_backend_stop(&udev_backend);
        lulo_tune_backend_stop(&tune_backend);
        lulo_dizk_state_cleanup(&dizk_state);
        lulo_proc_state_cleanup(&proc_state);
        lulo_sched_state_cleanup(&sched_state);
        lulo_cgroups_state_cleanup(&cgroups_state);
        lulo_udev_state_cleanup(&udev_state);
        lulo_tune_state_cleanup(&tune_state);
        lulo_systemd_state_cleanup(&systemd_state);
        lulo_systemd_backend_stop(&systemd_backend);
        notcurses_stop(ui.nc);
        debug_log_close(&dlog);
        return 1;
    }
    sleep_ms(200);
    if (lulo_read_cpu_stat(&stat_b) < 0) {
        fprintf(stderr, "failed to read /proc/stat\n");
        lulo_proc_state_cleanup(&proc_state);
        lulo_dizk_state_cleanup(&dizk_state);
        lulo_sched_state_cleanup(&sched_state);
        lulo_cgroups_state_cleanup(&cgroups_state);
        lulo_udev_state_cleanup(&udev_state);
        lulo_tune_state_cleanup(&tune_state);
        lulo_systemd_state_cleanup(&systemd_state);
        lulo_sched_backend_stop(&sched_backend);
        lulo_cgroups_backend_stop(&cgroups_backend);
        lulo_udev_backend_stop(&udev_backend);
        lulo_tune_backend_stop(&tune_backend);
        lulo_systemd_backend_stop(&systemd_backend);
        notcurses_stop(ui.nc);
        debug_log_close(&dlog);
        return 1;
    }
    if (input_backend == INPUT_BACKEND_RAW) {
        if (raw_input_enable(&rawin) < 0) {
            fprintf(stderr, "failed to switch terminal to raw input mode\n");
            lulo_proc_state_cleanup(&proc_state);
            lulo_dizk_state_cleanup(&dizk_state);
            lulo_sched_state_cleanup(&sched_state);
            lulo_cgroups_state_cleanup(&cgroups_state);
            lulo_udev_state_cleanup(&udev_state);
            lulo_tune_state_cleanup(&tune_state);
            lulo_systemd_state_cleanup(&systemd_state);
            lulo_sched_backend_stop(&sched_backend);
            lulo_cgroups_backend_stop(&cgroups_backend);
            lulo_udev_backend_stop(&udev_backend);
            lulo_tune_backend_stop(&tune_backend);
            lulo_systemd_backend_stop(&systemd_backend);
            notcurses_stop(ui.nc);
            debug_log_close(&dlog);
            return 1;
        }
        terminal_mouse_enable();
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

        if (poll_sched_backend(&ui, &app, &sched_backend, &sched_snap, &sched_state,
                               &sched_backend_status, &sched_generation, &(RenderFlags){0}) < 0) {
            fprintf(stderr, "failed to consume sched backend snapshot\n");
            break;
        }
        sched_snapshot_valid = sched_backend_status.have_snapshot;
        if (poll_cgroups_backend(&ui, &app, &cgroups_backend, &cgroups_snap, &cgroups_state,
                                 &cgroups_backend_status, &cgroups_generation, &(RenderFlags){0}) < 0) {
            fprintf(stderr, "failed to consume cgroups backend snapshot\n");
            break;
        }
        cgroups_snapshot_valid = cgroups_backend_status.have_snapshot;
        if (poll_udev_backend(&ui, &app, &udev_backend, &udev_snap, &udev_state,
                              &udev_backend_status, &udev_generation, &(RenderFlags){0}) < 0) {
            fprintf(stderr, "failed to consume udev backend snapshot\n");
            break;
        }
        udev_snapshot_valid = udev_backend_status.have_snapshot;
        if (poll_tune_backend(&ui, &app, &tune_backend, &tune_snap, &tune_state,
                              &tune_backend_status, &tune_generation, &(RenderFlags){0}) < 0) {
            fprintf(stderr, "failed to consume tune backend snapshot\n");
            break;
        }
        tune_snapshot_valid = tune_backend_status.have_snapshot;
        if (poll_systemd_backend(&ui, &app, &systemd_backend, &systemd_snap, &systemd_state,
                                 &systemd_backend_status, &systemd_generation, &(RenderFlags){0}) < 0) {
            fprintf(stderr, "failed to consume systemd backend snapshot\n");
            break;
        }
        systemd_snapshot_valid = systemd_backend_status.have_snapshot;
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
                int heat = lulo_cpu_heat_pct(&stat_a.cpu[i + 1], &stat_b.cpu[i + 1]);
                lulo_dashboard_append_heat(&dash, i, heat);
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
        } else if (app.active_page == APP_PAGE_DIZK) {
            if (lulo_dizk_snapshot_gather(&dizk_snap, &dizk_state, ui.lo.top.width - 8) < 0) {
                fprintf(stderr, "failed to gather dizk snapshot\n");
                break;
            }
            lulo_dizk_view_sync(&dizk_state, &dizk_snap, disk_visible_rows(&ui));
        } else if (app.active_page == APP_PAGE_SCHED) {
            if ((!sched_snapshot_valid || mono_ms_now() >= sched_due_ms) &&
                !sched_backend_status.busy) {
                lulo_sched_backend_request_full(&sched_backend, &sched_state);
                lulo_sched_backend_status(&sched_backend, &sched_backend_status);
                sched_due_ms = mono_ms_now() + effective_sched_refresh_ms(&sched_snap);
            }
            lulo_sched_view_sync(&sched_state, &sched_snap,
                                 sched_list_rows_visible(&ui, &sched_state),
                                 sched_preview_rows_visible(&ui, &sched_state));
        } else if (app.active_page == APP_PAGE_CGROUPS) {
            if ((!cgroups_snapshot_valid || mono_ms_now() >= cgroups_due_ms) &&
                !cgroups_backend_status.busy) {
                lulo_cgroups_backend_request_full(&cgroups_backend, &cgroups_state);
                lulo_cgroups_backend_status(&cgroups_backend, &cgroups_backend_status);
                cgroups_due_ms = mono_ms_now() + 5000;
            }
            lulo_cgroups_view_sync(&cgroups_state, &cgroups_snap,
                                   cgroups_list_rows_visible(&ui, &cgroups_state),
                                   cgroups_preview_rows_visible(&ui, &cgroups_state));
        } else if (app.active_page == APP_PAGE_UDEV) {
            if ((!udev_snapshot_valid || mono_ms_now() >= udev_due_ms) &&
                !udev_backend_status.busy) {
                lulo_udev_backend_request_full(&udev_backend, &udev_state);
                lulo_udev_backend_status(&udev_backend, &udev_backend_status);
                udev_due_ms = mono_ms_now() + 5000;
            }
            lulo_udev_view_sync(&udev_state, &udev_snap,
                                udev_list_rows_visible(&ui, &udev_state),
                                udev_preview_rows_visible(&ui, &udev_state));
        } else if (app.active_page == APP_PAGE_TUNE) {
            if ((!tune_snapshot_valid || mono_ms_now() >= tune_due_ms) &&
                !tune_backend_status.busy) {
                lulo_tune_backend_request_full(&tune_backend, &tune_state);
                lulo_tune_backend_status(&tune_backend, &tune_backend_status);
                tune_due_ms = mono_ms_now() + 5000;
            }
            lulo_tune_view_sync(&tune_state, &tune_snap,
                                tune_list_rows_visible(&ui, &tune_state),
                                tune_preview_rows_visible(&ui, &tune_state));
        } else {
            if ((!systemd_snapshot_valid || mono_ms_now() >= systemd_due_ms) &&
                !systemd_backend_status.busy) {
                lulo_systemd_backend_request_full(&systemd_backend, &systemd_state);
                lulo_systemd_backend_status(&systemd_backend, &systemd_backend_status);
                systemd_due_ms = mono_ms_now() + 5000;
            }
            lulo_systemd_view_sync(&systemd_state, &systemd_snap,
                                   systemd_list_rows_visible(&ui, &systemd_state),
                                   systemd_preview_rows_visible(&ui, &systemd_state));
        }

        if (full_redraw) render_static(&ui, app.active_page, &app);
        if (app.active_page == APP_PAGE_CPU) {
            render_cpu_page(&ui, &app, &ci, &dash, &stat_a, &stat_b, freqs, nfreq, &mi, &li,
                            &proc_snap, &proc_state, cpu_temps, ncpu_temp, append_sample,
                            full_redraw || proc_refreshed);
        } else if (app.active_page == APP_PAGE_DIZK) {
            render_disk_page(&ui, &app, &dash, &dizk_snap, &dizk_state);
        } else if (app.active_page == APP_PAGE_SCHED) {
            render_sched_page(&ui, &app, &dash, &sched_snap, &sched_state, &sched_backend_status);
        } else if (app.active_page == APP_PAGE_CGROUPS) {
            render_cgroups_page(&ui, &app, &dash, &cgroups_snap, &cgroups_state, &cgroups_backend_status);
        } else if (app.active_page == APP_PAGE_UDEV) {
            render_udev_page(&ui, &app, &dash, &udev_snap, &udev_state, &udev_backend_status);
        } else if (app.active_page == APP_PAGE_TUNE) {
            render_tune_page(&ui, &app, &dash, &tune_snap, &tune_state, &tune_backend_status);
        } else {
            render_systemd_page(&ui, &app, &dash, &systemd_snap, &systemd_state, &systemd_backend_status);
        }
        if (app.help_visible) render_help_overlay(&ui, app.active_page);
        else destroy_plane(&ui.modal);
        debug_log_stage(&dlog, "before_render");
        if (notcurses_render(ui.nc) < 0) {
            lulo_dizk_snapshot_free(&dizk_snap);
            lulo_systemd_snapshot_free(&systemd_snap);
            break;
        }
        debug_log_stage(&dlog, "after_render");
        append_sample = 0;

        {
            long long deadline = mono_ms_now() + sample_ms;
            int resample = 0;
            struct pollfd pfd = {
                .fd = STDIN_FILENO,
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
                        if (help_overlay_consume_input(&ui, &app, &in, in.action, &render)) {
                            if (need_resize || render.need_rebuild || exit_requested) break;
                            continue;
                        }
                        if (app.tune_edit_active && !in.mouse_press && !in.mouse_release && !in.mouse_wheel) {
                            if (handle_tune_edit_input(&app, &in, &tune_state, &render)) {
                                if (need_resize || render.need_rebuild || exit_requested) break;
                                continue;
                            }
                        }
                        if (app.tune_rename_active && !in.mouse_press && !in.mouse_release && !in.mouse_wheel) {
                            if (handle_tune_bundle_rename_input(&app, &in, &render)) {
                                if (need_resize || render.need_rebuild || exit_requested) break;
                                continue;
                            }
                        }
                        if (in.mouse_wheel &&
                            !handle_mouse_wheel_target(&ui, &app, &sched_state, &cgroups_state, &udev_state,
                                                       &tune_state, &systemd_state, &render,
                                                       in.mouse_y, in.mouse_x)) {
                            continue;
                        }
                        if (in.mouse_press && in.mouse_button == 1) {
                            handle_mouse_click(&ui, in.mouse_y, in.mouse_x, &app, &proc_snap, &proc_state,
                                               &sched_snap, &sched_state,
                                               &cgroups_snap, &cgroups_state,
                                               &udev_snap, &udev_state,
                                               &tune_snap, &tune_state, &systemd_snap, &systemd_state, &render);
                        } else {
                            int scroll_units = scroll_units_for_input(&app, in.action, in.mouse_wheel, in.key_repeat);

                            apply_input_action(&ui, in.action, &app, &proc_snap, &proc_state, &dizk_snap, &dizk_state,
                                               &sched_snap, &sched_state,
                                               &cgroups_snap, &cgroups_state,
                                               &udev_snap, &udev_state,
                                               &tune_snap, &tune_state,
                                               &systemd_snap, &systemd_state, input_backend, &rawin, &dlog,
                                               &dash, &sample_ms, &deadline,
                                               &exit_requested, &need_resize, &render, scroll_units);
                        }
                        if (need_resize || render.need_rebuild || exit_requested) break;
                    }
                } else {
                    const struct timespec wait_ts = {
                        .tv_sec = timeout_ms / 1000,
                        .tv_nsec = (long)(timeout_ms % 1000) * 1000000L,
                    };
                    const struct timespec zero_ts = {0};
                    ncinput ni;
                    uint32_t id;
                    int saved_errno;

                    errno = 0;
                    memset(&ni, 0, sizeof(ni));
                    id = notcurses_get(ui.nc, &wait_ts, &ni);
                    saved_errno = errno;
                    if (id == (uint32_t)-1) {
                        if (saved_errno == 0) {
                            debug_log_message(&dlog, "nc-timeout", "0");
                            if (terminal_get_size(&term_rows, &term_cols) == 0 &&
                                (term_rows != last_h || term_cols != last_w)) {
                                need_resize = 1;
                                need_rebuild = 1;
                                resample = 1;
                            }
                            if (ms_until_deadline(deadline) == 0) resample = 1;
                            continue;
                        }
                        if (saved_errno == EINTR) {
                            if (terminal_get_size(&term_rows, &term_cols) == 0 &&
                                (term_rows != last_h || term_cols != last_w)) {
                                need_resize = 1;
                                need_rebuild = 1;
                                resample = 1;
                            }
                            continue;
                        }
                        debug_log_errno(&dlog, "nc-get-error");
                        exit_requested = 1;
                        break;
                    }
                    for (;;) {
                        InputAction action;
                        DecodedInput in;

                        action = decode_notcurses_input(id);
                        if (id == NCKEY_TAB && (ni.modifiers & NCKEY_MOD_SHIFT)) {
                            action = INPUT_VIEW_NEXT;
                        }
                        fill_decoded_input_from_nc(&in, id, &ni, action);
                        debug_log_nc_event(&dlog, "dispatch-nc", id, &ni);
                        if (help_overlay_consume_input(&ui, &app, &in, action, &render)) {
                            if (need_resize || render.need_rebuild || exit_requested) break;
                            continue;
                        }
                        if (app.tune_edit_active &&
                            id != NCKEY_BUTTON1 &&
                            id != NCKEY_SCROLL_UP &&
                            id != NCKEY_SCROLL_DOWN) {
                            if (handle_tune_edit_input(&app, &in, &tune_state, &render)) {
                                if (need_resize || render.need_rebuild || exit_requested) break;
                                continue;
                            }
                        }
                        if (app.tune_rename_active &&
                            id != NCKEY_BUTTON1 &&
                            id != NCKEY_SCROLL_UP &&
                            id != NCKEY_SCROLL_DOWN) {
                            if (handle_tune_bundle_rename_input(&app, &in, &render)) {
                                if (need_resize || render.need_rebuild || exit_requested) break;
                                continue;
                            }
                        }
                        if ((id == NCKEY_SCROLL_UP || id == NCKEY_SCROLL_DOWN) &&
                            !handle_mouse_wheel_target(&ui, &app, &sched_state, &cgroups_state, &udev_state,
                                                       &tune_state, &systemd_state, &render,
                                                       ni.y + 1, ni.x + 1)) {
                            continue;
                        }
                        if (id == NCKEY_BUTTON1 && ni.evtype != NCTYPE_RELEASE) {
                            handle_mouse_click(&ui, ni.y + 1, ni.x + 1, &app, &proc_snap, &proc_state,
                                               &sched_snap, &sched_state,
                                               &cgroups_snap, &cgroups_state,
                                               &udev_snap, &udev_state,
                                               &tune_snap, &tune_state, &systemd_snap, &systemd_state, &render);
                        } else {
                            int scroll_units = scroll_units_for_input(&app, action,
                                                                       id == NCKEY_SCROLL_UP || id == NCKEY_SCROLL_DOWN,
                                                                       ni.evtype == NCTYPE_REPEAT);

                            apply_input_action(&ui, action, &app, &proc_snap, &proc_state, &dizk_snap, &dizk_state,
                                               &sched_snap, &sched_state,
                                               &cgroups_snap, &cgroups_state,
                                               &udev_snap, &udev_state,
                                               &tune_snap, &tune_state,
                                               &systemd_snap, &systemd_state, input_backend, &rawin, &dlog,
                                               &dash, &sample_ms, &deadline,
                                               &exit_requested, &need_resize, &render, scroll_units);
                        }
                        if (need_resize || render.need_rebuild || exit_requested) break;

                        errno = 0;
                        memset(&ni, 0, sizeof(ni));
                        id = notcurses_get(ui.nc, &zero_ts, &ni);
                        saved_errno = errno;
                        if (id == (uint32_t)-1) {
                            if (saved_errno != 0 && saved_errno != EINTR) {
                                debug_log_errno(&dlog, "nc-get-error");
                                exit_requested = 1;
                            }
                            break;
                        }
                    }
                }

                if (poll_sched_backend(&ui, &app, &sched_backend, &sched_snap, &sched_state,
                                       &sched_backend_status, &sched_generation, &render) < 0) {
                    exit_requested = 1;
                    break;
                }
                sched_snapshot_valid = sched_backend_status.have_snapshot;
                if (poll_cgroups_backend(&ui, &app, &cgroups_backend, &cgroups_snap, &cgroups_state,
                                         &cgroups_backend_status, &cgroups_generation, &render) < 0) {
                    exit_requested = 1;
                    break;
                }
                cgroups_snapshot_valid = cgroups_backend_status.have_snapshot;
                if (poll_udev_backend(&ui, &app, &udev_backend, &udev_snap, &udev_state,
                                      &udev_backend_status, &udev_generation, &render) < 0) {
                    exit_requested = 1;
                    break;
                }
                udev_snapshot_valid = udev_backend_status.have_snapshot;
                if (poll_tune_backend(&ui, &app, &tune_backend, &tune_snap, &tune_state,
                                      &tune_backend_status, &tune_generation, &render) < 0) {
                    exit_requested = 1;
                    break;
                }
                tune_snapshot_valid = tune_backend_status.have_snapshot;
                if (poll_systemd_backend(&ui, &app, &systemd_backend, &systemd_snap, &systemd_state,
                                         &systemd_backend_status, &systemd_generation, &render) < 0) {
                    exit_requested = 1;
                    break;
                }
                systemd_snapshot_valid = systemd_backend_status.have_snapshot;

                if (render.need_disk_refresh && app.active_page == APP_PAGE_DIZK) {
                    lulo_dizk_snapshot_free(&dizk_snap);
                    if (lulo_dizk_snapshot_gather(&dizk_snap, &dizk_state, ui.lo.top.width - 8) < 0) {
                        exit_requested = 1;
                        break;
                    }
                    lulo_dizk_view_sync(&dizk_state, &dizk_snap, disk_visible_rows(&ui));
                    render.need_disk = 1;
                }
                if (render.need_sched_reload && app.active_page == APP_PAGE_SCHED) {
                    if (sched_snapshot_valid) lulo_sched_snapshot_mark_loading(&sched_snap, &sched_state);
                    lulo_sched_backend_request_reload(&sched_backend, &sched_state);
                    lulo_sched_backend_status(&sched_backend, &sched_backend_status);
                    sched_due_ms = mono_ms_now() + effective_sched_refresh_ms(&sched_snap);
                    render.need_sched = 1;
                } else if (render.need_sched_refresh && app.active_page == APP_PAGE_SCHED) {
                    if (lulo_sched_snapshot_refresh_active(&sched_snap, &sched_state) < 0) {
                        exit_requested = 1;
                        break;
                    }
                    render.need_sched = 1;
                }
                if (render.need_cgroups_refresh_full && app.active_page == APP_PAGE_CGROUPS) {
                    if (cgroups_snapshot_valid) lulo_cgroups_snapshot_mark_loading(&cgroups_snap, &cgroups_state);
                    lulo_cgroups_backend_request_full(&cgroups_backend, &cgroups_state);
                    lulo_cgroups_backend_status(&cgroups_backend, &cgroups_backend_status);
                    cgroups_due_ms = mono_ms_now() + 5000;
                    render.need_cgroups = 1;
                } else if (render.need_cgroups_refresh && app.active_page == APP_PAGE_CGROUPS) {
                    if (cgroups_snapshot_valid) lulo_cgroups_snapshot_mark_loading(&cgroups_snap, &cgroups_state);
                    lulo_cgroups_backend_request_active(&cgroups_backend, &cgroups_state);
                    lulo_cgroups_backend_status(&cgroups_backend, &cgroups_backend_status);
                    render.need_cgroups = 1;
                }
                if (render.need_udev_refresh_full && app.active_page == APP_PAGE_UDEV) {
                    if (udev_snapshot_valid) lulo_udev_snapshot_mark_loading(&udev_snap, &udev_state);
                    lulo_udev_backend_request_full(&udev_backend, &udev_state);
                    lulo_udev_backend_status(&udev_backend, &udev_backend_status);
                    udev_due_ms = mono_ms_now() + 5000;
                    render.need_udev = 1;
                } else if (render.need_udev_refresh && app.active_page == APP_PAGE_UDEV) {
                    if (udev_snapshot_valid) lulo_udev_snapshot_mark_loading(&udev_snap, &udev_state);
                    lulo_udev_backend_request_active(&udev_backend, &udev_state);
                    lulo_udev_backend_status(&udev_backend, &udev_backend_status);
                    render.need_udev = 1;
                }
                if (render.need_tune_refresh_full && app.active_page == APP_PAGE_TUNE) {
                    if (tune_snapshot_valid) lulo_tune_snapshot_mark_loading(&tune_snap, &tune_state);
                    lulo_tune_backend_request_full(&tune_backend, &tune_state);
                    lulo_tune_backend_status(&tune_backend, &tune_backend_status);
                    tune_due_ms = mono_ms_now() + 5000;
                    render.need_tune = 1;
                } else if (render.need_tune_refresh && app.active_page == APP_PAGE_TUNE) {
                    if (tune_snapshot_valid) lulo_tune_snapshot_mark_loading(&tune_snap, &tune_state);
                    lulo_tune_backend_request_active(&tune_backend, &tune_state);
                    lulo_tune_backend_status(&tune_backend, &tune_backend_status);
                    render.need_tune = 1;
                }
                if (render.need_tune_save_snapshot && app.active_page == APP_PAGE_TUNE) {
                    lulo_tune_backend_request_save_snapshot(&tune_backend, &tune_state);
                    lulo_tune_backend_status(&tune_backend, &tune_backend_status);
                    tune_due_ms = mono_ms_now() + 5000;
                    render.need_tune = 1;
                }
                if (render.need_tune_save_preset && app.active_page == APP_PAGE_TUNE) {
                    lulo_tune_backend_request_save_preset(&tune_backend, &tune_state);
                    lulo_tune_backend_status(&tune_backend, &tune_backend_status);
                    tune_due_ms = mono_ms_now() + 5000;
                    render.need_tune = 1;
                }
                if (render.need_tune_apply_selected && app.active_page == APP_PAGE_TUNE) {
                    if (tune_snapshot_valid) lulo_tune_snapshot_mark_loading(&tune_snap, &tune_state);
                    lulo_tune_backend_request_apply_selected(&tune_backend, &tune_state);
                    lulo_tune_backend_status(&tune_backend, &tune_backend_status);
                    tune_due_ms = mono_ms_now() + 5000;
                    render.need_tune = 1;
                }
                if (render.need_systemd_refresh && app.active_page == APP_PAGE_SYSTEMD) {
                    if (systemd_snapshot_valid) lulo_systemd_snapshot_mark_loading(&systemd_snap, &systemd_state);
                    if (!systemd_snapshot_valid) {
                        lulo_systemd_backend_request_full(&systemd_backend, &systemd_state);
                        systemd_due_ms = mono_ms_now() + 5000;
                    } else {
                        lulo_systemd_backend_request_active(&systemd_backend, &systemd_state);
                    }
                    lulo_systemd_backend_status(&systemd_backend, &systemd_backend_status);
                    render.need_systemd = 1;
                }
                if (render.need_proc_refresh && app.active_page == APP_PAGE_CPU) {
                    proc_snapshot_valid = 0;
                    proc_due_ms = 0;
                    render.need_proc_refresh = 0;
                    if (!need_resize && !exit_requested) {
                        render_pending(&ui, &app, &ci, &dash, &stat_a, &stat_b, freqs, nfreq,
                                       cpu_temps, ncpu_temp, &proc_snap, &proc_state,
                                       &dizk_snap, &dizk_state, &sched_snap, &sched_state,
                                       &sched_backend_status, &cgroups_snap, &cgroups_state,
                                       &cgroups_backend_status, &udev_snap, &udev_state,
                                       &udev_backend_status, &tune_snap, &tune_state,
                                       &tune_backend_status, &systemd_snap, &systemd_state,
                                       &systemd_backend_status, &render);
                    }
                    resample = 1;
                    continue;
                }
                if (render.need_rebuild) {
                    need_rebuild = 1;
                    resample = 1;
                    continue;
                }

                if (!need_resize && !exit_requested) {
                    render_pending(&ui, &app, &ci, &dash, &stat_a, &stat_b, freqs, nfreq,
                                   cpu_temps, ncpu_temp, &proc_snap, &proc_state,
                                   &dizk_snap, &dizk_state, &sched_snap, &sched_state,
                                   &sched_backend_status, &cgroups_snap, &cgroups_state,
                                   &cgroups_backend_status, &udev_snap, &udev_state,
                                   &udev_backend_status, &tune_snap, &tune_state,
                                   &tune_backend_status, &systemd_snap, &systemd_state,
                                   &systemd_backend_status, &render);
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
    }
    lulo_proc_snapshot_free(&proc_snap);
    lulo_sched_snapshot_free(&sched_snap);
    lulo_cgroups_snapshot_free(&cgroups_snap);
    lulo_udev_snapshot_free(&udev_snap);
    lulo_tune_snapshot_free(&tune_snap);
    lulo_systemd_snapshot_free(&systemd_snap);
    lulo_sched_backend_stop(&sched_backend);
    lulo_cgroups_backend_stop(&cgroups_backend);
    lulo_udev_backend_stop(&udev_backend);
    lulo_tune_backend_stop(&tune_backend);
    lulo_systemd_backend_stop(&systemd_backend);
    lulo_proc_state_cleanup(&proc_state);
    lulo_dizk_state_cleanup(&dizk_state);
    lulo_sched_state_cleanup(&sched_state);
    lulo_cgroups_state_cleanup(&cgroups_state);
    lulo_udev_state_cleanup(&udev_state);
    lulo_tune_state_cleanup(&tune_state);
    lulo_systemd_state_cleanup(&systemd_state);
    notcurses_stop(ui.nc);
    debug_log_close(&dlog);
    return exit_requested ? 0 : 1;
}
