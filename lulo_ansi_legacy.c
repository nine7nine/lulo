/* lulo.c — CPU & system monitor
 *
 * Build:  gcc -O2 -Wno-format-truncation -lm -o lulo lulo.c lulo_proc.c
 * Usage:  ./lulo [-n] [-h]
 *           -n   no color
 *           -h   help
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <dirent.h>
#include <ctype.h>
#include <limits.h>
#include <termios.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <errno.h>

#include "lulo_proc.h"
static void sig_exit(int sig)
{
    (void)sig;
    printf("\033[?25h\033[?1049l");
    fflush(stdout);
    _exit(0);
}
/* ── colors ──────────────────────────────────────────────────── */
typedef struct {
    const char *reset, *bold, *dim;
    const char *cyan, *white, *gray, *green, *yellow, *orange, *red, *blue;
    const char *bg_ok, *bg_warn, *bg_crit, *bg_free;
} Colors;
static const Colors color_on = {
    .reset   = "\033[0m",  .bold   = "\033[1m",        .dim    = "\033[2m",
    .cyan    = "\033[96m", .white  = "\033[97m",        .gray   = "\033[90m",
    .green   = "\033[92m", .yellow = "\033[93m",        .orange = "\033[38;5;214m",
    .red     = "\033[91m", .blue   = "\033[94m",
    .bg_ok   = "\033[48;5;23m", .bg_warn = "\033[48;5;58m",
    .bg_crit = "\033[48;5;52m", .bg_free = "\033[48;5;236m",
};
static const Colors color_off = { "", "", "", "", "", "", "", "", "", "", "", "", "", "", "" };
static const Colors *C = &color_on;
#define THRESH_WARN  75
#define THRESH_CRIT  90
/* ── helpers ─────────────────────────────────────────────────── */
static int term_width(void)
{
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) return w.ws_col;
    const char *cols = getenv("COLUMNS");
    return cols ? atoi(cols) : 120;
}
static int term_height(void)
{
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0) return w.ws_row;
    const char *lines = getenv("LINES");
    return lines ? atoi(lines) : 30;
}
typedef struct {
    int row, col, width, height;
} Rect;
static void move_to(int row, int col)
{
    printf("\033[%d;%dH", row, col);
}
static void print_spaces(int count)
{
    for (int i = 0; i < count; i++) putchar(' ');
}
static void clear_line_span(int row, int col, int width)
{
    if (row <= 0 || col <= 0 || width <= 0) return;
    move_to(row, col);
    print_spaces(width);
}
static void clear_inner_rect(const Rect *r)
{
    if (!r || r->width < 3 || r->height < 3) return;
    for (int y = r->row + 1; y < r->row + r->height - 1; y++) {
        clear_line_span(y, r->col + 1, r->width - 2);
    }
}
static void print_padded_text(const char *text, int width)
{
    if (width <= 0) return;
    printf("%-*.*s", width, width, text ? text : "");
}
static int print_segment_fit(int remaining, const char *color, const char *text)
{
    int n;
    if (remaining <= 0 || !text || !*text) return 0;
    n = (int)strlen(text);
    if (n > remaining) n = remaining;
    if (color && *color) printf("%s", color);
    fwrite(text, 1, (size_t)n, stdout);
    if (color && *color) printf("%s", C->reset);
    return n;
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
static void clear_screen(void)
{
    printf("\033[H\033[2J");
}
static void draw_box(const Rect *r, const char *color)
{
    if (!r || r->width < 2 || r->height < 2) return;
    move_to(r->row, r->col);
    printf("%s┌", color ? color : "");
    for (int i = 0; i < r->width - 2; i++) fputs("─", stdout);
    printf("┐%s", C->reset);
    for (int y = 1; y < r->height - 1; y++) {
        move_to(r->row + y, r->col);
        printf("%s│%s", color ? color : "", C->reset);
        move_to(r->row + y, r->col + r->width - 1);
        printf("%s│%s", color ? color : "", C->reset);
    }
    move_to(r->row + r->height - 1, r->col);
    printf("%s└", color ? color : "");
    for (int i = 0; i < r->width - 2; i++) fputs("─", stdout);
    printf("┘%s", C->reset);
}
static void draw_box_title(const Rect *r, const char *border_color, const char *title, const char *title_color)
{
    if (!r || r->width < 2 || r->height < 2) return;
    draw_box(r, border_color);
    if (title && *title && r->width > 4) {
        move_to(r->row, r->col + 2);
        printf("%s%s%s", title_color ? title_color : C->bold, title, C->reset);
    }
}
static void fmt_size(char *buf, size_t len, unsigned long long bytes)
{
    const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double val = (double)bytes; int i = 0;
    while (val >= 1024.0 && i < 4) { val /= 1024.0; i++; }
    if      (i == 0)       snprintf(buf, len, "%llu B",  bytes);
    else if (val >= 100.0) snprintf(buf, len, "%.0f %s", val, units[i]);
    else if (val >= 10.0)  snprintf(buf, len, "%.1f %s", val, units[i]);
    else                   snprintf(buf, len, "%.2f %s", val, units[i]);
}
static void abbrev_cpu_model(const char *in, char *out, size_t len)
{
    if (!in || !*in) {
        snprintf(out, len, "CPU");
        return;
    }
    const char *intel_core = strstr(in, "Intel(R) Core(TM) ");
    if (intel_core) {
        intel_core += strlen("Intel(R) Core(TM) ");
        const char *at = strstr(intel_core, " @ ");
        size_t n = at ? (size_t)(at - intel_core) : strlen(intel_core);
        if (n >= len) n = len - 1;
        memcpy(out, intel_core, n);
        out[n] = '\0';
        return;
    }
    const char *ryzen = strstr(in, "AMD Ryzen ");
    if (ryzen) {
        const char *at = strstr(ryzen, " with ");
        size_t n = at ? (size_t)(at - ryzen) : strlen(ryzen);
        if (n >= len) n = len - 1;
        memcpy(out, ryzen, n);
        out[n] = '\0';
        return;
    }
    snprintf(out, len, "%s", in);
}
static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}
static int adjust_sample_ms(int current, int delta)
{
    int step = current < 1000 ? 100 : 200;
    return clamp_int(current + delta * step, 100, 5000);
}
static void draw_header_line(int row, const char *hostname, const char *model_short, int sample_ms)
{
    char tb[32];
    strftime(tb, sizeof(tb), "%a %d %b %Y  %H:%M:%S", localtime(&(time_t){time(NULL)}));
    printf("\033[%d;1H", row);
    printf("  %s%sCPUz%s  %s%s%s  ·  %s  ·  %s%dms%s  ·  %s%s\033[K",
           C->cyan, C->bold, C->reset,
           C->white, model_short && *model_short ? model_short : "CPU", C->reset,
           hostname,
           C->cyan, sample_ms, C->reset,
           C->white, tb);
}
static void hr(const char *ch, const char *color, int width)
{
    printf("%s", color);
    for (int i = 0; i < width; i++) printf("%s", ch);
    printf("%s\n", C->reset);
}
static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
static long long mono_ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}
static int ms_until_deadline(long long deadline_ms)
{
    long long now = mono_ms_now();
    long long diff = deadline_ms - now;
    if (diff <= 0) return 0;
    if (diff > 2147483647LL) return 2147483647;
    return (int)diff;
}

static int stdin_read_byte_timeout(char *out, int timeout_ms)
{
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) return 0;
    return read(STDIN_FILENO, out, 1) == 1;
}

static void stdin_drain_pending_bytes(void)
{
    char ch;
    while (stdin_read_byte_timeout(&ch, 0)) {}
}

static const char *pct_color(int pct)
{
    return pct >= THRESH_CRIT ? C->red : pct >= THRESH_WARN ? C->orange : pct >= 50 ? C->yellow : C->green;
}
/* CPU % text color — loosely follows the heatmap gradient but brighter/lighter */
static const char *cpu_pct_color(int pct)
{
    static char buf[32];
    /* reuse heatmap stops but lighten them for text readability */
    static const struct { int p; int r, g, b; } stops[] = {
        {  0,  80, 100, 200},     /* soft indigo    */
        { 10,  70, 140, 230},     /* blue           */
        { 20,  60, 190, 230},     /* cyan           */
        { 35,  80, 220, 140},     /* teal-green     */
        { 50, 140, 220,  60},     /* yellow-green   */
        { 70, 200, 200,  50},     /* warm yellow    */
        { 85, 230, 160,  50},     /* amber          */
        { 95, 240, 100,  40},     /* orange         */
        {100, 240,  60,  40},     /* red            */
    };
    enum { NS = sizeof(stops) / sizeof(stops[0]) };
    if (pct <= 0) pct = 1;
    if (pct > 100) pct = 100;
    int lo = 0;
    for (int s = 0; s < NS - 1; s++)
        if (pct >= stops[s].p && pct <= stops[s+1].p) { lo = s; break; }
    int hi = lo + 1;
    int span = stops[hi].p - stops[lo].p;
    if (span <= 0) span = 1;
    int t = pct - stops[lo].p;
    int r = stops[lo].r + (stops[hi].r - stops[lo].r) * t / span;
    int g = stops[lo].g + (stops[hi].g - stops[lo].g) * t / span;
    int b = stops[lo].b + (stops[hi].b - stops[lo].b) * t / span;
    snprintf(buf, sizeof(buf), "\033[38;2;%d;%d;%dm", r, g, b);
    return buf;
}
/* temperature-based coloring */
static const char *temp_color(double t)
{
    if      (t >= 75) return C->red;
    else if (t >= 65) return C->orange;
    else if (t >= 55) return C->green;
    else              return C->blue;
}
static const char *bar_bg(int pct)
{
    return pct >= THRESH_CRIT ? C->bg_crit : pct >= THRESH_WARN ? C->bg_warn : C->bg_ok;
}
static int read_file_str(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = (fgets(buf, (int)len, f) != NULL);
    fclose(f);
    if (!ok) return -1;
    char *end = buf + strlen(buf) - 1;
    while (end >= buf && (*end == '\n' || *end == '\r' || *end == ' ')) *end-- = '\0';
    return 0;
}
static void print_bar(int pct, int width)
{
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    int used = pct * width / 100, free = width - used;
    if (used > 0) { printf("%s%s", bar_bg(pct), C->white); for (int i=0;i<used;i++) putchar(' '); printf("%s", C->reset); }
    if (free > 0) { printf("%s", C->bg_free); for (int i=0;i<free;i++) putchar(' '); printf("%s", C->reset); }
    printf(" %s%s%3d%%%s", pct_color(pct), C->bold, pct, C->reset);
}

static void print_mini_bar(const char *label, int pct, const char *value, int bar_w)
{
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    int u = pct * bar_w / 100, f = bar_w - u;
    printf("  %s%-5s%s ", C->white, label, C->reset);
    if (u > 0) { printf("%s%s", bar_bg(pct), C->white); for (int i=0;i<u;i++) putchar(' '); printf("%s", C->reset); }
    if (f > 0) { printf("%s", C->bg_free); for (int i=0;i<f;i++) putchar(' '); printf("%s", C->reset); }
    printf(" %s%3d%%%s %s", pct_color(pct), pct, C->reset, value);
}
static void print_mini_bar_hue(const char *label, int pct, const char *value, int bar_w, const char *bg, const char *fg)
{
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    int u = pct * bar_w / 100, f = bar_w - u;
    printf("  %s%-5s%s ", C->white, label, C->reset);
    if (u > 0) { printf("%s%s", bg, C->white); for (int i=0;i<u;i++) putchar(' '); printf("%s", C->reset); }
    if (f > 0) { printf("%s", C->bg_free); for (int i=0;i<f;i++) putchar(' '); printf("%s", C->reset); }
    printf(" %s%3d%%%s %s", fg, pct, C->reset, value);
}


#define MAX_TIMELINE 128

typedef enum {
    HEAT_MODE_RAW = 0,
    HEAT_MODE_TASK,
    HEAT_MODE_COUNT
} HeatMode;

static const char *cpu_row_color(int idx)
{
    (void)idx;
    return C->white;
}

static void timeline_append(unsigned char *hist, int width, int pct)
{
    if (width <= 0) return;
    memmove(hist, hist + 1, (size_t)(width - 1));
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    hist[width - 1] = (unsigned char)pct;
}

static const char *cpu_hist_color(HeatMode mode, int idx, int pct)
{
    typedef struct { int p, r, g, b; } HeatStop;
    (void)idx;
    static const HeatStop raw_stops[] = {
        {  1,  25,  25, 100},     /* indigo         */
        {  8,   0,  60, 190},     /* blue           */
        { 15,   0, 140, 210},     /* cyan           */
        { 25,   0, 200, 150},     /* teal           */
        { 35,  40, 210,  40},     /* green          */
        { 50, 110, 170,   0},     /* yellow-green   */
        { 70, 160, 150,   0},     /* warm yellow    */
        { 85, 185, 110,   0},     /* amber          */
        { 95, 200,  45,   0},     /* orange-red     */
        {100, 210,  15,   0},     /* red            */
    };
    static const HeatStop task_stops[] = {
        {  1,  20,  48,  30},     /* deep green     */
        {  8,  28,  86,  42},     /* forest         */
        { 15,  32, 122,  54},     /* green          */
        { 25,  22, 158,  88},     /* green-teal     */
        { 35,  42, 192, 112},     /* mint           */
        { 50, 120, 205,  70},     /* yellow-green   */
        { 70, 178, 188,  46},     /* warm lime      */
        { 85, 210, 150,  38},     /* amber-green    */
        { 95, 225, 102,  28},     /* orange         */
        {100, 235,  55,  25},     /* red-orange     */
    };
    static char buf[32];
    const HeatStop *stops = mode == HEAT_MODE_TASK ? task_stops : raw_stops;
    int ns = mode == HEAT_MODE_TASK ?
             (int)(sizeof(task_stops) / sizeof(task_stops[0])) :
             (int)(sizeof(raw_stops) / sizeof(raw_stops[0]));
    if (pct <= 0) return C->reset;
    if (pct > 100) pct = 100;
    int lo = 0;
    for (int s = 0; s < ns - 1; s++)
        if (pct >= stops[s].p && pct <= stops[s+1].p) { lo = s; break; }
    int hi = lo + 1;
    int span = stops[hi].p - stops[lo].p;
    if (span <= 0) span = 1;
    int t = pct - stops[lo].p;
    int r = stops[lo].r + (stops[hi].r - stops[lo].r) * t / span;
    int g = stops[lo].g + (stops[hi].g - stops[lo].g) * t / span;
    int b = stops[lo].b + (stops[hi].b - stops[lo].b) * t / span;
    snprintf(buf, sizeof(buf), "[38;2;%d;%d;%dm", r, g, b);
    return buf;
}

static void render_timeline_row_full(const char *prefix, int row, const unsigned char *hist, int width, int pct, int idx)
{
    printf("[%d;1H", row);
    printf("%s", prefix);
    for (int i = 0; i < width; i++) {
        int v = hist[i];
        if (v <= 0) {
            putchar(' ');
            continue;
        }
        printf("%s█%s", cpu_hist_color(HEAT_MODE_RAW, idx - 1, v), C->reset);
    }
    printf(" %s%s%3d%%%s", cpu_pct_color(pct), C->bold, pct, C->reset);
}

static const char *heat_mode_name(HeatMode mode)
{
    return mode == HEAT_MODE_TASK ? "task" : "raw";
}

static const char *heat_mode_accent(HeatMode mode)
{
    return mode == HEAT_MODE_TASK ? C->green : C->cyan;
}

static HeatMode next_heat_mode(HeatMode mode)
{
    return mode == HEAT_MODE_TASK ? HEAT_MODE_RAW : HEAT_MODE_TASK;
}

/* ── CPU info ────────────────────────────────────────────────── */
typedef struct {
    char model[128], vendor[32], flags[4096], microcode[32], stepping[8], family[8];
    int  physical_cores, logical_cores, sockets, cores_per_socket, threads_per_core;
    int  cache_l1d_kb, cache_l2_kb, cache_l3_kb;
    double base_mhz;
} CpuInfo;
static void gather_cpu_info(CpuInfo *ci)
{
    memset(ci, 0, sizeof(*ci));
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return;
    char line[4096];
    int phys_ids[64]; int nphys = 0;
    while (fgets(line, sizeof(line), f)) {
        char *colon = strchr(line, ':'); if (!colon) continue;
        char key[64]; int klen = (int)(colon - line);
        if (klen >= (int)sizeof(key)) klen = sizeof(key)-1;
        memcpy(key, line, klen); key[klen] = '\0';
        char *k = key; while (*k=='\t'||*k==' ') k++;
        char *ke = k+strlen(k)-1; while (ke>k&&(*ke==' '||*ke=='\t')) *ke--='\0';
        char *v = colon+1; while (*v==' ') v++;
        int vlen = strlen(v); while (vlen>0&&(v[vlen-1]=='\n'||v[vlen-1]=='\r'||v[vlen-1]==' ')) v[--vlen]='\0';
        /* val points directly into line — no copy needed for flags */
        const char *val = v;
        if      (!strcmp(k,"model name")&&!ci->model[0])    snprintf(ci->model,sizeof(ci->model),"%s",val);
        else if (!strcmp(k,"vendor_id")&&!ci->vendor[0])    snprintf(ci->vendor,sizeof(ci->vendor),"%s",val);
        else if (!strcmp(k,"cpu MHz")&&ci->base_mhz==0)     ci->base_mhz=atof(val);
        else if (!strcmp(k,"cache size")&&!ci->cache_l2_kb) ci->cache_l2_kb=atoi(val);
        else if (!strcmp(k,"physical id")) {
            int id=atoi(val); int found=0;
            for(int i=0;i<nphys;i++) if(phys_ids[i]==id){found=1;break;}
            if(!found&&nphys<64) phys_ids[nphys++]=id;
        }
        else if (!strcmp(k,"processor"))              ci->logical_cores++;
        else if (!strcmp(k,"cpu cores")&&!ci->cores_per_socket) ci->cores_per_socket=atoi(val);
        else if (!strcmp(k,"stepping")&&!ci->stepping[0])   snprintf(ci->stepping,sizeof(ci->stepping),"%s",val);
        else if (!strcmp(k,"cpu family")&&!ci->family[0])   snprintf(ci->family,sizeof(ci->family),"%s",val);
        else if (!strcmp(k,"microcode")&&!ci->microcode[0]) snprintf(ci->microcode,sizeof(ci->microcode),"%s",val);
        else if (!strcmp(k,"flags")&&!ci->flags[0]) {
            /* store all flags unfiltered — wrapping handles display */
            snprintf(ci->flags, sizeof(ci->flags), "%s", val);
        }
    }
    fclose(f);
    ci->sockets = nphys>0?nphys:1;
    if(!ci->cores_per_socket&&ci->logical_cores) ci->cores_per_socket=ci->logical_cores/ci->sockets;
    ci->physical_cores = ci->sockets*ci->cores_per_socket;
    ci->threads_per_core = ci->physical_cores>0 ? ci->logical_cores/ci->physical_cores : 1;
    char path[256],buf[64];
    snprintf(path,sizeof(path),"/sys/devices/system/cpu/cpu0/cache/index0/size");
    if(read_file_str(path,buf,sizeof(buf))==0) ci->cache_l1d_kb=atoi(buf);
    for(int idx=0;idx<=4;idx++){
        snprintf(path,sizeof(path),"/sys/devices/system/cpu/cpu0/cache/index%d/level",idx);
        char lvl[8]; if(read_file_str(path,lvl,sizeof(lvl))<0) break;
        if(atoi(lvl)==3){
            snprintf(path,sizeof(path),"/sys/devices/system/cpu/cpu0/cache/index%d/size",idx);
            if(read_file_str(path,buf,sizeof(buf))==0) ci->cache_l3_kb=atoi(buf);
        }
    }
}
/* ── CPU stat ────────────────────────────────────────────────── */
#define MAX_CPUS 256
typedef struct { unsigned long long user,nice,sys,idle,iowait,irq,softirq,steal; } CpuTick;
typedef struct { CpuTick cpu[MAX_CPUS+1]; int ncpu; } CpuStat;
static int read_cpu_stat(CpuStat *s)
{
    FILE *f = fopen("/proc/stat","r"); if(!f) return -1;
    char line[256]; s->ncpu=0;
    while(fgets(line,sizeof(line),f)){
        if(strncmp(line,"cpu",3)!=0) break;
        int idx=-1;
        if(line[3]==' ') idx=0;
        else if(isdigit((unsigned char)line[3])) idx=atoi(line+3)+1;
        else continue;
        if(idx>MAX_CPUS) continue;
        CpuTick *t=&s->cpu[idx];
        sscanf(line+(idx==0?4:3+(idx<10?1:idx<100?2:3)+1),
               "%llu %llu %llu %llu %llu %llu %llu %llu",
               &t->user,&t->nice,&t->sys,&t->idle,&t->iowait,&t->irq,&t->softirq,&t->steal);
        if(idx>0&&idx>s->ncpu) s->ncpu=idx;
    }
    fclose(f); return 0;
}
static int cpu_pct(const CpuTick *a, const CpuTick *b)
{
    unsigned long long ia=a->idle+a->iowait, ib=b->idle+b->iowait;
    unsigned long long ta=a->user+a->nice+a->sys+ia+a->irq+a->softirq+a->steal;
    unsigned long long tb=b->user+b->nice+b->sys+ib+b->irq+b->softirq+b->steal;
    unsigned long long dt=tb>ta?tb-ta:0, di=ib>ia?ib-ia:0;
    return dt==0?0:(int)(100ULL*(dt-di)/dt);
}

static int cpu_heat_pct(const CpuTick *a, const CpuTick *b, HeatMode mode)
{
    unsigned long long ia = a->idle + a->iowait, ib = b->idle + b->iowait;
    unsigned long long ta = a->user + a->nice + a->sys + ia + a->irq + a->softirq + a->steal;
    unsigned long long tb = b->user + b->nice + b->sys + ib + b->irq + b->softirq + b->steal;
    unsigned long long dt = tb > ta ? tb - ta : 0;
    unsigned long long work_a, work_b, dw;

    if (dt == 0) return 0;
    if (mode == HEAT_MODE_TASK) {
        work_a = a->user + a->nice + a->sys;
        work_b = b->user + b->nice + b->sys;
    } else {
        work_a = a->user + a->nice + a->sys + a->irq + a->softirq + a->steal;
        work_b = b->user + b->nice + b->sys + b->irq + b->softirq + b->steal;
    }
    dw = work_b > work_a ? work_b - work_a : 0;
    return (int)(100ULL * dw / dt);
}

static unsigned long long cpu_total_delta(const CpuStat *a, const CpuStat *b)
{
    const CpuTick *ta = &a->cpu[0];
    const CpuTick *tb = &b->cpu[0];
    unsigned long long sa = ta->user + ta->nice + ta->sys + ta->idle + ta->iowait +
                            ta->irq + ta->softirq + ta->steal;
    unsigned long long sb = tb->user + tb->nice + tb->sys + tb->idle + tb->iowait +
                            tb->irq + tb->softirq + tb->steal;
    return sb > sa ? sb - sa : 0;
}
/* ── CPU freq ────────────────────────────────────────────────── */
typedef struct { int cpu; long cur_khz,min_khz,max_khz; char governor[32],driver[32]; } CpuFreq;
static int gather_cpu_freq(CpuFreq *freqs, int max)
{
    int n=0;
    for(int cpu=0;cpu<max;cpu++){
        char path[256],buf[64];
        snprintf(path,sizeof(path),"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq",cpu);
        if(read_file_str(path,buf,sizeof(buf))<0) break;
        freqs[n].cpu=cpu; freqs[n].cur_khz=atol(buf);
        snprintf(path,sizeof(path),"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq",cpu);
        freqs[n].min_khz=(read_file_str(path,buf,sizeof(buf))==0)?atol(buf):0;
        snprintf(path,sizeof(path),"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq",cpu);
        freqs[n].max_khz=(read_file_str(path,buf,sizeof(buf))==0)?atol(buf):0;
        snprintf(path,sizeof(path),"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor",cpu);
        if(read_file_str(path,freqs[n].governor,sizeof(freqs[n].governor))<0) strcpy(freqs[n].governor,"-");
        snprintf(path,sizeof(path),"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_driver",cpu);
        if(read_file_str(path,freqs[n].driver,sizeof(freqs[n].driver))<0) strcpy(freqs[n].driver,"-");
        n++;
    }
    return n;
}
/* ── Memory ──────────────────────────────────────────────────── */
typedef struct {
    unsigned long long total,free,available,buffers,cached,shmem,slab,sreclaimable;
    unsigned long long swap_total,swap_free,swap_cached,dirty,writeback;
    unsigned long long hugepages_total,hugepages_free,hugepage_size_kb;
} MemInfo;
static void gather_meminfo(MemInfo *m)
{
    memset(m,0,sizeof(*m));
    FILE *f=fopen("/proc/meminfo","r"); if(!f) return;
    char line[128];
    while(fgets(line,sizeof(line),f)){
        unsigned long long v; char key[64];
        if(sscanf(line,"%63s %llu",key,&v)!=2) continue;
        v*=1024;
        if      (!strcmp(key,"MemTotal:"))       m->total=v;
        else if (!strcmp(key,"MemAvailable:"))   m->available=v;
        else if (!strcmp(key,"Buffers:"))        m->buffers=v;
        else if (!strcmp(key,"Cached:"))         m->cached=v;
        else if (!strcmp(key,"Slab:"))           m->slab=v;
        else if (!strcmp(key,"SwapTotal:"))      m->swap_total=v;
        else if (!strcmp(key,"SwapFree:"))       m->swap_free=v;
        else if (!strcmp(key,"Dirty:"))          m->dirty=v;
        else if (!strcmp(key,"Writeback:"))      m->writeback=v;
        else if (!strcmp(key,"HugePages_Total:"))m->hugepages_total=v/1024;
        else if (!strcmp(key,"HugePages_Free:")) m->hugepages_free=v/1024;
        else if (!strcmp(key,"Hugepagesize:"))   m->hugepage_size_kb=v/1024;
    }
    fclose(f);
}
static void render_mem_row(int idx, const MemInfo *mi, int row, int col, int bw)
{
    /* per-stat bar hues: bg (48) and matching fg (38) */
    static const char *mem_bg[] = {
        "\033[48;2;40;80;160m",   /* used  – steel blue   */
        "\033[48;2;30;120;130m",  /* buff  – teal         */
        "\033[48;2;50;130;70m",   /* cache – forest green */
        "\033[48;2;90;60;140m",   /* slab  – purple       */
        "\033[48;2;150;90;30m",   /* swap  – burnt orange */
        "\033[48;2;100;90;80m",   /* dirty – warm gray    */
    };
    static const char *mem_fg[] = {
        "\033[38;2;80;140;255m",  /* used  – steel blue   */
        "\033[38;2;50;190;200m",  /* buff  – teal         */
        "\033[38;2;80;200;110m",  /* cache – forest green */
        "\033[38;2;150;100;220m", /* slab  – purple       */
        "\033[38;2;220;150;50m",  /* swap  – burnt orange */
        "\033[38;2;160;145;130m", /* dirty – warm gray    */
    };
    char v[12];
    unsigned long long mu = mi->total - mi->available;
    unsigned long long su = mi->swap_total - mi->swap_free;
    printf("\033[%d;%dH", row, col);
    switch (idx) {
    case 0: { int p=mi->total>0?(int)(mu*100ULL/mi->total):0;
              fmt_size(v,sizeof(v),mu); print_mini_bar_hue("used",p,v,bw,mem_bg[0],mem_fg[0]); break; }
    case 1: { int p=mi->total>0?(int)(mi->buffers*100ULL/mi->total):0;
              fmt_size(v,sizeof(v),mi->buffers); print_mini_bar_hue("buff",p,v,bw,mem_bg[1],mem_fg[1]); break; }
    case 2: { int p=mi->total>0?(int)(mi->cached*100ULL/mi->total):0;
              fmt_size(v,sizeof(v),mi->cached); print_mini_bar_hue("cache",p,v,bw,mem_bg[2],mem_fg[2]); break; }
    case 3: { int p=mi->total>0?(int)(mi->slab*100ULL/mi->total):0;
              fmt_size(v,sizeof(v),mi->slab); print_mini_bar_hue("slab",p,v,bw,mem_bg[3],mem_fg[3]); break; }
    case 4: { int p=mi->swap_total>0?(int)(su*100ULL/mi->swap_total):0;
              fmt_size(v,sizeof(v),su); print_mini_bar_hue("swap",p,v,bw,mem_bg[4],mem_fg[4]); break; }
    case 5: { int p=mi->total>0?(int)(mi->dirty*100ULL/mi->total):0;
              fmt_size(v,sizeof(v),mi->dirty); print_mini_bar_hue("dirty",p,v,bw,mem_bg[5],mem_fg[5]); break; }
    case 6: if(mi->hugepages_total>0)
                printf("  %shp%s %llu x %llu kB (%llu free)",
                       C->gray,C->reset,mi->hugepages_total,mi->hugepage_size_kb,mi->hugepages_free);
            break;
    }
}

/* ── Load avg ────────────────────────────────────────────────── */
typedef struct {
    double load1,load5,load15;
    int running,total,procs_blocked;
    long ctxt;
} LoadInfo;
static void gather_loadavg(LoadInfo *li)
{
    memset(li,0,sizeof(*li));
    FILE *f=fopen("/proc/loadavg","r");
    if(f){ int _r=fscanf(f,"%lf %lf %lf %d/%d",&li->load1,&li->load5,&li->load15,&li->running,&li->total);(void)_r;fclose(f);}
    f=fopen("/proc/stat","r");
    if(f){char line[256];while(fgets(line,sizeof(line),f)){
        if(!strncmp(line,"ctxt ",5)) li->ctxt=atol(line+5);
        else if(!strncmp(line,"procs_blocked ",14)) li->procs_blocked=atoi(line+14);
    }fclose(f);}
}
/* ── Softirqs ────────────────────────────────────────────────── */
#define MAX_SIRQ_CPUS 64
#define SIRQ_TYPES 10
static const char *sirq_names[SIRQ_TYPES] = {
    "HI","TIMER","NET_TX","NET_RX","BLOCK","IRQ_POLL","TASKLET","SCHED","HRTIMER","RCU"
};
typedef struct {
    unsigned long long cnt[SIRQ_TYPES][MAX_SIRQ_CPUS];
    int ncpu;
} SoftirqStat;
static void read_softirqs(SoftirqStat *s)
{
    memset(s,0,sizeof(*s));
    FILE *f=fopen("/proc/softirqs","r"); if(!f) return;
    char line[1024];
    /* first line: CPU headers — count CPUs */
    if(fgets(line,sizeof(line),f)){
        char *p=line; int n=0;
        while((p=strstr(p,"CPU"))){ n++; p+=3; }
        s->ncpu=n<MAX_SIRQ_CPUS?n:MAX_SIRQ_CPUS;
    }
    while(fgets(line,sizeof(line),f)){
        char name[16]; if(sscanf(line," %15[^:]",name)!=1) continue;
        /* strip trailing colon */
        int ti=-1;
        for(int i=0;i<SIRQ_TYPES;i++) if(!strcmp(name,sirq_names[i])){ti=i;break;}
        if(ti<0) continue;
        char *p=strchr(line,':'); if(!p) continue; p++;
        for(int c=0;c<s->ncpu;c++){
            unsigned long long v; if(sscanf(p," %llu",&v)!=1) break;
            s->cnt[ti][c]=v;
            /* advance past this number */
            while(*p==' ') p++;
            while(*p&&*p!=' ') p++;
        }
    }
    fclose(f);
}
/* Compute per-second delta between two snapshots */
static unsigned long long sirq_delta_total(const SoftirqStat *a, const SoftirqStat *b, int ti)
{
    unsigned long long d=0;
    int nc=b->ncpu<MAX_SIRQ_CPUS?b->ncpu:MAX_SIRQ_CPUS;
    for(int c=0;c<nc;c++)
        d+=(b->cnt[ti][c]>a->cnt[ti][c])?(b->cnt[ti][c]-a->cnt[ti][c]):0;
    return d;
}
/* ── Schedstat ───────────────────────────────────────────────── */
typedef struct {
    unsigned long long run_ns;   /* cpu<n> field 7: time running on cpu */
    unsigned long long wait_ns;  /* cpu<n> field 8: time waiting in runqueue */
    unsigned long long slices;   /* cpu<n> field 9: # timeslices run */
} SchedstatCPU;
typedef struct {
    SchedstatCPU cpu[MAX_CPUS];
    int ncpu;
} SchedstatAll;
static void read_schedstat(SchedstatAll *s)
{
    memset(s,0,sizeof(*s));
    FILE *f=fopen("/proc/schedstat","r"); if(!f) return;
    char line[512];
    while(fgets(line,sizeof(line),f)){
        if(strncmp(line,"cpu",3)!=0||!isdigit((unsigned char)line[3])) continue;
        int idx=atoi(line+3);
        if(idx<0||idx>=MAX_CPUS) continue;
        /* skip "cpuN " prefix, then read 9 space-separated fields */
        char *p=line;
        while(*p&&*p!=' ') p++;  /* skip "cpuN" */
        unsigned long long f1,f2,f3,f4,f5,f6,rn,wn,sl;
        if(sscanf(p,"%llu %llu %llu %llu %llu %llu %llu %llu %llu",
                  &f1,&f2,&f3,&f4,&f5,&f6,&rn,&wn,&sl)==9){
            s->cpu[idx].run_ns=rn;
            s->cpu[idx].wait_ns=wn;
            s->cpu[idx].slices=sl;
            if(idx>=s->ncpu) s->ncpu=idx+1;
        }
    }
    fclose(f);
}

typedef struct {
    int ready;
    int ncpu;
    unsigned long long softirq[SIRQ_TYPES][8];
    int run_pct[8];
    int wait_pct[8];
    unsigned long long slices[8];
    SoftirqStat prev_sirq;
    SchedstatAll prev_sched;
} SideStatsState;

static void side_stats_init(SideStatsState *state)
{
    memset(state, 0, sizeof(*state));
    read_softirqs(&state->prev_sirq);
    read_schedstat(&state->prev_sched);
    state->ready = 1;
}

static void side_stats_update(SideStatsState *state, int logical_cpus, int append_sample)
{
    SoftirqStat sirq_now;
    SchedstatAll sched_now;
    int ncpu;

    if (!state->ready) side_stats_init(state);
    if (!append_sample) return;

    read_softirqs(&sirq_now);
    read_schedstat(&sched_now);
    ncpu = logical_cpus > 0 ? logical_cpus : sirq_now.ncpu;
    if (ncpu > 8) ncpu = 8;
    state->ncpu = ncpu;

    for (int ti = 0; ti < SIRQ_TYPES; ti++) {
        for (int c = 0; c < ncpu; c++) {
            unsigned long long now = sirq_now.cnt[ti][c];
            unsigned long long prev = state->prev_sirq.cnt[ti][c];
            state->softirq[ti][c] = now > prev ? now - prev : 0;
        }
    }
    for (int c = 0; c < ncpu; c++) {
        unsigned long long dr = 0, dw = 0, dsl = 0, tot;
        if (sched_now.cpu[c].run_ns > state->prev_sched.cpu[c].run_ns) {
            dr = sched_now.cpu[c].run_ns - state->prev_sched.cpu[c].run_ns;
        }
        if (sched_now.cpu[c].wait_ns > state->prev_sched.cpu[c].wait_ns) {
            dw = sched_now.cpu[c].wait_ns - state->prev_sched.cpu[c].wait_ns;
        }
        if (sched_now.cpu[c].slices > state->prev_sched.cpu[c].slices) {
            dsl = sched_now.cpu[c].slices - state->prev_sched.cpu[c].slices;
        }
        tot = dr + dw;
        state->run_pct[c] = tot > 0 ? (int)(dr * 100 / tot) : 0;
        state->wait_pct[c] = tot > 0 ? (int)(dw * 100 / tot) : 0;
        state->slices[c] = dsl;
    }
    state->prev_sirq = sirq_now;
    state->prev_sched = sched_now;
}
/* ── Tunables ────────────────────────────────────────────────── */
typedef struct { char path[128],value[64],desc[80]; } Tunable;
static int gather_tunables(Tunable *t, int max)
{
    static const struct { const char *path,*desc; } items[] = {
        /* ── CFS scheduler ── */
        {"/proc/sys/kernel/sched_latency_ns",              "CFS target scheduling latency (ns)"},
        {"/proc/sys/kernel/sched_min_granularity_ns",      "min task runtime before preempt (ns)"},
        {"/proc/sys/kernel/sched_wakeup_granularity_ns",   "wakeup preemption granularity (ns)"},
        {"/proc/sys/kernel/sched_migration_cost_ns",       "task migration cost estimate (ns)"},
        {"/proc/sys/kernel/sched_nr_migrate",              "max tasks migrated per softirq"},
        {"/proc/sys/kernel/sched_child_runs_first",        "child runs before parent on fork"},
        {"/proc/sys/kernel/sched_tunable_scaling",         "latency scaling (0=none,1=log,2=linear)"},
        {"/proc/sys/kernel/sched_schedstats",              "enable scheduler statistics"},
        {"/proc/sys/kernel/sched_energy_aware",            "energy-aware scheduling (EAS)"},
        {"/proc/sys/kernel/sched_autogroup_enabled",       "autogroup tasks by session"},
        {"/proc/sys/kernel/sched_cfs_bandwidth_slice_us",  "CFS bandwidth slice size (us)"},
        {"/proc/sys/kernel/sched_util_clamp_min",          "util clamp floor (0–1024)"},
        {"/proc/sys/kernel/sched_util_clamp_max",          "util clamp ceiling (0–1024)"},
        {"/proc/sys/kernel/sched_util_clamp_min_rt_default","RT task util clamp floor"},
        /* ── RT scheduler ── */
        {"/proc/sys/kernel/sched_rt_runtime_us",           "RT tasks CPU budget per period (us)"},
        {"/proc/sys/kernel/sched_rt_period_us",            "RT scheduling period (us)"},
        {"/proc/sys/kernel/sched_rr_timeslice_ms",         "SCHED_RR timeslice (ms)"},
        /* ── deadline scheduler ── */
        {"/proc/sys/kernel/sched_deadline_period_max_us",  "SCHED_DEADLINE max period (us)"},
        {"/proc/sys/kernel/sched_deadline_period_min_us",  "SCHED_DEADLINE min period (us)"},
        /* ── NUMA & topology ── */
        {"/proc/sys/kernel/numa_balancing",                "automatic NUMA page migration"},
        {"/proc/sys/kernel/numa_balancing_scan_delay_ms",  "NUMA balancing initial scan delay (ms)"},
        {"/proc/sys/kernel/timer_migration",               "migrate timers to idle CPUs"},
        /* ── watchdog / safety ── */
        {"/proc/sys/kernel/nmi_watchdog",                  "NMI lockup detector enabled"},
        {"/proc/sys/kernel/watchdog_thresh",               "watchdog lockup threshold (seconds)"},
        {"/proc/sys/kernel/perf_event_paranoid",           "perf_event access (0=all,1=unpriv,2=root,-1=no)"},
        {"/proc/sys/kernel/latencytop",                    "latency tracking per task enabled"},
        {"/proc/sys/kernel/task_delayacct",                "per-task delay accounting enabled"},
        /* ── cpufreq / governor ── */
        {"/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",              "active scaling governor"},
        {"/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq",              "min frequency (kHz)"},
        {"/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq",              "max frequency (kHz)"},
        {"/sys/devices/system/cpu/cpufreq/policy0/energy_performance_preference", "EPP hint (policy0)"},
        /* ── vm / memory pressure ── */
        {"/proc/sys/vm/swappiness",                        "swap tendency (0=avoid,100=aggressive)"},
        {"/proc/sys/vm/dirty_ratio",                       "% RAM dirty before sync write"},
        {"/proc/sys/vm/dirty_background_ratio",            "% RAM dirty before background writeback"},
        {"/proc/sys/vm/dirty_expire_centisecs",            "dirty page age before writeback (cs)"},
        {"/proc/sys/vm/dirty_writeback_centisecs",         "writeback thread wakeup interval (cs)"},
        {"/proc/sys/vm/vfs_cache_pressure",                "inode/dentry cache reclaim pressure"},
        {"/proc/sys/vm/overcommit_memory",                 "overcommit (0=heuristic,1=always,2=limit)"},
        {"/proc/sys/vm/overcommit_ratio",                  "% RAM allowed when policy=2"},
        {"/proc/sys/vm/min_free_kbytes",                   "minimum free memory reserved (kB)"},
        {"/proc/sys/vm/watermark_boost_factor",            "watermark boost on fragmentation (%)"},
        {"/proc/sys/vm/watermark_scale_factor",            "distance between low/high watermarks"},
        {"/proc/sys/vm/zone_reclaim_mode",                 "NUMA zone reclaim aggressiveness"},
        {"/proc/sys/vm/page-cluster",                      "pages read ahead on swap (2^n)"},
        {"/proc/sys/vm/workingset_protection",             "protect active workingset from reclaim"},
        {"/proc/sys/vm/numa_zonelist_order",               "NUMA zone list order (Default/Node/Zone)"},
        {NULL,NULL}
    };
    int n=0;
    for(int i=0;items[i].path&&n<max;i++){
        char buf[64];
        if(read_file_str(items[i].path,buf,sizeof(buf))<0) continue;
        snprintf(t[n].path,sizeof(t[n].path),"%s",items[i].path);
        snprintf(t[n].value,sizeof(t[n].value),"%s",buf);
        snprintf(t[n].desc,sizeof(t[n].desc),"%s",items[i].desc);
        n++;
    }
    return n;
}
/* ── THP ─────────────────────────────────────────────────────── */
typedef struct { char path[128],value[128],desc[80]; } ThpEntry;
static int gather_thp(ThpEntry *t, int max)
{
    static const struct { const char *path,*desc; } items[] = {
        {"/sys/kernel/mm/transparent_hugepage/enabled",          "THP mode (always/madvise/never)"},
        {"/sys/kernel/mm/transparent_hugepage/defrag",           "THP defragmentation policy"},
        {"/sys/kernel/mm/transparent_hugepage/use_zero_page",    "use zero page for THP allocation"},
        {"/sys/kernel/mm/transparent_hugepage/khugepaged/scan_sleep_millisecs","khugepaged scan interval (ms)"},
        {"/sys/kernel/mm/transparent_hugepage/khugepaged/alloc_sleep_millisecs","khugepaged alloc retry delay (ms)"},
        {"/sys/kernel/mm/transparent_hugepage/khugepaged/pages_to_scan","pages scanned per khugepaged pass"},
        {"/sys/kernel/mm/transparent_hugepage/khugepaged/max_ptes_none","max unmapped PTEs per collapse"},
        {NULL,NULL}
    };
    int n=0;
    for(int i=0;items[i].path&&n<max;i++){
        char buf[128];
        if(read_file_str(items[i].path,buf,sizeof(buf))<0) continue;
        snprintf(t[n].path,sizeof(t[n].path),"%s",items[i].path);
        char *lb=strchr(buf,'['),*rb=lb?strchr(lb,']'):NULL;
        if(lb&&rb){int len=(int)(rb-lb-1);if(len>=64)len=63;char act[64];memcpy(act,lb+1,len);act[len]='\0';snprintf(t[n].value,sizeof(t[n].value),"%s",act);}
        else snprintf(t[n].value,sizeof(t[n].value),"%s",buf);
        snprintf(t[n].desc,sizeof(t[n].desc),"%s",items[i].desc);
        n++;
    }
    return n;
}
/* ── bar row ─────────────────────────────────────────────────── */
typedef struct { char prefix[256]; int pct, prev_pct, term_row; } BarRow;
static void print_bar_initial(int pct, int width)
{
    printf("%s",C->bg_free); for(int i=0;i<width;i++) putchar(' '); printf("%s",C->reset);
    printf(" %s%s%3d%%%s",pct_color(pct),C->bold,pct,C->reset);
}
static void repaint_used(const BarRow *r, int u, int bar_width)
{
    printf("\033[%d;1H",r->term_row);
    printf("%s",r->prefix);
    if(u>0){printf("%s%s",bar_bg(r->pct),C->white);for(int i=0;i<u;i++) putchar(' ');printf("%s",C->reset);}
    int fc=bar_width-u; if(fc>0) printf("\033[%dC",fc);
}
static double smooth_bar_value(double prev, int target)
{
    double alpha;
    if (target >= prev) {
        double rise = target - prev;
        alpha = (rise > 30.0) ? 0.76 : (rise > 12.0) ? 0.64 : 0.52;
    } else {
        double drop = prev - target;
        alpha = (drop > 25.0) ? 0.97 : (drop > 10.0) ? 0.91 : 0.82;
    }
    double next = prev + (target - prev) * alpha;
    if (target > 0 && next < 1.0) next = 1.0;
    if (target == 0 && next < 1.2) next = 0.0;
    if (fabs(target - next) < 0.60) next = target;
    if (next < 0.0) next = 0.0;
    if (next > 100.0) next = 100.0;
    return next;
}
static void render_bar_row_full(const char *prefix, int row, double pct_d, int target_pct, int bar_width)
{
    int pct = (int)(pct_d + 0.5);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int used = pct * bar_width / 100;
    int free = bar_width - used;
    printf("\033[%d;1H", row);
    printf("%s", prefix);
    if (used > 0) {
        printf("%s%s", bar_bg(target_pct), C->white);
        for (int i = 0; i < used; i++) putchar(' ');
        printf("%s", C->reset);
    }
    if (free > 0) {
        printf("%s", C->bg_free);
        for (int i = 0; i < free; i++) putchar(' ');
        printf("%s", C->reset);
    }
    printf(" %s%s%3d%%%s", pct_color(target_pct), C->bold, target_pct, C->reset);
}
/* ── live update: repaint only the dynamic rows ─────────────── */
typedef struct {
    int row_clock, row_load;
    char model_short[64];
    char hostname[128];
    int sample_ms;
    int row_bars[MAX_CPUS+1];
    int row_mem_bar, row_mem_stats, row_mem_stats2, row_swap_stats;
    int nbars, bar_width, ucol, suffix_col, mem_col, mem_bw;
    double cpu_temps[64]; int ncpu_temp;
    int row_sirq_start;
    int nsirq_rows;
    SoftirqStat sirq_prev;
    SchedstatAll sched_prev;
    unsigned char timeline[MAX_CPUS+1][MAX_TIMELINE];
    int timeline_width;
} LiveCtx;

static void live_update(LiveCtx *lc,
                        const CpuStat *sa, const CpuStat *sb,
                        const CpuFreq *freqs, int nfreq,
                        const MemInfo *mi, const LoadInfo *li,
                        int final_row)
{
    int bar_width = lc->bar_width;
    int ucol      = lc->ucol;
    /* ── clock ── */
    {
        draw_header_line(lc->row_clock, lc->hostname, lc->model_short, lc->sample_ms);
    }
    /* ── load ── */
    {
        int nc=sb->ncpu>0?sb->ncpu:1;
        const char *lc1=li->load1>nc*0.9?C->red:li->load1>nc*0.7?C->orange:li->load1>nc*0.5?C->yellow:C->green;
        const char *lc5=li->load5>nc*0.9?C->red:li->load5>nc*0.7?C->orange:li->load5>nc*0.5?C->yellow:C->green;
        const char *lc15=li->load15>nc*0.9?C->red:li->load15>nc*0.7?C->orange:li->load15>nc*0.5?C->yellow:C->green;
        const char *bc=li->procs_blocked>0?C->orange:C->white;
        printf("\033[%d;1H",lc->row_load);
        printf("  %sload%s %s%.2f%s %s%.2f%s %s%.2f%s  "
               "%srun%s %s%d%s/%s%d%s  %sblk%s %s%d%s  %sctxsw%s %s%ld%s\033[K",
               C->white,C->reset,
               lc1,li->load1,C->reset,lc5,li->load5,C->reset,lc15,li->load15,C->reset,
               C->white,C->reset,C->cyan,li->running,C->reset,C->white,li->total,C->reset,
               C->white,C->reset,bc,li->procs_blocked,C->reset,
               C->white,C->reset,C->white,li->ctxt,C->reset);
    }
    /* suffix + mem bars rendered below in merged CPU loop */
    /* ── softirq + schedstat columnar update ── */
    if(lc->row_sirq_start > 0){
        SoftirqStat sirq_now;   read_softirqs(&sirq_now);
        SchedstatAll sched_now; read_schedstat(&sched_now);
        int sirq_ncpu = sirq_now.ncpu > 0 ? sirq_now.ncpu : lc->nbars-1;
        if(sirq_ncpu > 8) sirq_ncpu = 8;
        int vc = 8;   /* value column width */
        /* softirq delta rows */
        for(int ti=0;ti<SIRQ_TYPES;ti++){
            printf("\033[%d;1H", lc->row_sirq_start+ti);
            const char *tc=(!strcmp(sirq_names[ti],"SCHED"))   ?C->cyan  :
                           (!strcmp(sirq_names[ti],"TIMER"))   ?C->yellow:
                           (!strcmp(sirq_names[ti],"HRTIMER")) ?C->orange:
                           (!strcmp(sirq_names[ti],"NET_RX")||
                            !strcmp(sirq_names[ti],"NET_TX"))  ?C->green :
                           (!strcmp(sirq_names[ti],"HI"))      ?C->white :
                           (!strcmp(sirq_names[ti],"TASKLET")) ?C->white :
                           (!strcmp(sirq_names[ti],"IRQ_POLL"))?C->white :
                           (!strcmp(sirq_names[ti],"RCU"))     ?C->white :
                           (!strcmp(sirq_names[ti],"BLOCK"))   ?C->white :C->white;
            printf("  %s%s%-10s%s", C->bold, tc, sirq_names[ti], C->reset);
            for(int c=0;c<sirq_ncpu;c++){
                unsigned long long d=(sirq_now.cnt[ti][c]>lc->sirq_prev.cnt[ti][c])
                                    ? sirq_now.cnt[ti][c]-lc->sirq_prev.cnt[ti][c] : 0;
                char vs[10];
                if     (d>=1000000) snprintf(vs,sizeof(vs),"%.1fM",d/1000000.0);
                else if(d>=1000)    snprintf(vs,sizeof(vs),"%.1fk",d/1000.0);
                else                snprintf(vs,sizeof(vs),"%llu",d);
                const char *vc2=d>100000?C->orange:d>10000?C->yellow:C->white;
                printf("%s%*s%s", vc2, vc, vs, C->reset);
            }
            printf("\033[K");
        }
        /* separator row — static, skip */
        /* schedstat rows: run%, wait%, slices/s */
        int sched_rows[3]; /* run%, wait%, slices/s */
        sched_rows[0] = lc->row_sirq_start + SIRQ_TYPES + 1;
        sched_rows[1] = lc->row_sirq_start + SIRQ_TYPES + 2;
        sched_rows[2] = lc->row_sirq_start + SIRQ_TYPES + 3;
        static const char *slabels[3]={"run%","wait%","slices/s"};
        for(int row=0;row<3;row++){
            printf("\033[%d;1H", sched_rows[row]);
            printf("  %s%-10s%s", C->white, slabels[row], C->reset);
            for(int c=0;c<sirq_ncpu;c++){
                unsigned long long dr=0,dw=0,dsl=0;
                if(sched_now.cpu[c].run_ns  > lc->sched_prev.cpu[c].run_ns)
                    dr =sched_now.cpu[c].run_ns  - lc->sched_prev.cpu[c].run_ns;
                if(sched_now.cpu[c].wait_ns > lc->sched_prev.cpu[c].wait_ns)
                    dw =sched_now.cpu[c].wait_ns - lc->sched_prev.cpu[c].wait_ns;
                if(sched_now.cpu[c].slices  > lc->sched_prev.cpu[c].slices)
                    dsl=sched_now.cpu[c].slices  - lc->sched_prev.cpu[c].slices;
                unsigned long long tot=dr+dw;
                char vs[10];
                if(row==0){
                    int rp=tot>0?(int)(dr*100/tot):0;
                    const char *rc=rp>80?C->blue:rp>50?C->cyan:rp>20?C->white:C->gray;
                    snprintf(vs,sizeof(vs),"%d%%",rp);
                    printf("%s%*s%s", rc, vc, vs, C->reset);
                } else if(row==1){
                    int wp=tot>0?(int)(dw*100/tot):0;
                    const char *wc=wp>50?C->red:wp>20?C->orange:wp>5?C->yellow:C->white;
                    snprintf(vs,sizeof(vs),"%d%%",wp);
                    printf("%s%*s%s", wc, vc, vs, C->reset);
                } else {
                    if(dsl>=1000) snprintf(vs,sizeof(vs),"%.1fk",dsl/1000.0);
                    else          snprintf(vs,sizeof(vs),"%llu",dsl);
                    printf("%s%*s%s", C->white, vc, vs, C->reset);
                }
            }
            printf("\033[K");
        }
        lc->sirq_prev =sirq_now;
        lc->sched_prev=sched_now;
    }
    /* ── CPU timelines + suffix + memory panel (single pass per row) ── */
    {
        char prefix[256];
        for (int i = 1; i < lc->nbars; i++) {
            char label[12];
            int pct = cpu_pct(&sa->cpu[i], &sb->cpu[i]);
            snprintf(label, sizeof(label), "cpu%d", i - 1);
            snprintf(prefix, sizeof(prefix), "  %s%-*s%s  ", cpu_row_color(i), ucol, label, C->reset);
            timeline_append(lc->timeline[i], lc->timeline_width, pct);
            render_timeline_row_full(prefix, lc->row_bars[i], lc->timeline[i], lc->timeline_width, pct, i);
            /* suffix: MHz, governor, temp */
            long mhz=(i-1<nfreq)?freqs[i-1].cur_khz/1000:0;
            const char *gov=(i-1<nfreq)?freqs[i-1].governor:"-";
            printf("\033[%d;%dH", lc->row_bars[i], lc->suffix_col);
            if(mhz>0) printf("%s%-6ld%s  %s%-12s%s",cpu_pct_color(pct),mhz,C->reset,C->green,gov,C->reset);
            if(lc->ncpu_temp>0){
                double t;
                if(lc->ncpu_temp==1){ t=lc->cpu_temps[0]; }
                else {
                    int ncores=lc->ncpu_temp-1, nlogical=lc->nbars-1;
                    int tpc=(ncores>0)?nlogical/ncores:2; if(tpc<1)tpc=1;
                    int tidx=(i-1)/tpc+1;
                    t=(tidx<lc->ncpu_temp)?lc->cpu_temps[tidx]:lc->cpu_temps[0];
                }
                printf("  %s%.1f\302\260C%s",temp_color(t),t,C->reset);
            }
            /* memory panel */
            render_mem_row(i-1, mi, lc->row_bars[i], lc->mem_col, lc->mem_bw);
        }
    }
    printf("[%d;1H",final_row);
    fflush(stdout);
}

typedef struct {
    Rect header;
    Rect top;
    Rect cpu;
    Rect mem;
    Rect proc;
    Rect side_stats;
    Rect load;
    int show_mem;
    int mem_stacked;
    int mem_rows;
    int mem_bar_w;
    int show_proc;
    int proc_rows;
    int show_side_stats;
    int side_cpu_cols;
    int show_governor;
    int show_temp;
    int cpu_label_w;
    int cpu_hist_w;
    int cpu_gov_w;
    int cpu_rows;
    int hidden_cpus;
    int load_row;
    int hint_row;
} TopLayout;

typedef struct {
    char model_short[64];
    char hostname[128];
    int sample_ms;
    HeatMode heat_mode;
    unsigned char timeline[HEAT_MODE_COUNT][MAX_CPUS + 1][MAX_TIMELINE];
} DashboardState;

static int read_cpu_temps(double *cpu_temps, int max)
{
    int ncpu_temp = 0;
    DIR *hd = opendir("/sys/class/hwmon");
    if (hd) {
        struct dirent *hde;
        while ((hde = readdir(hd))) {
            if (hde->d_name[0] == '.') continue;
            char namepath[128], name[32];
            snprintf(namepath, sizeof(namepath), "/sys/class/hwmon/%s/name", hde->d_name);
            if (read_file_str(namepath, name, sizeof(name)) < 0) continue;
            if (strcmp(name, "coretemp") != 0) continue;
            char base[128];
            snprintf(base, sizeof(base), "/sys/class/hwmon/%s", hde->d_name);
            for (int t = 1; t <= 64 && ncpu_temp < max; t++) {
                char tpath[192], tbuf[32];
                snprintf(tpath, sizeof(tpath), "%s/temp%d_input", base, t);
                if (read_file_str(tpath, tbuf, sizeof(tbuf)) < 0) break;
                int mc = atoi(tbuf);
                if (mc > 0) cpu_temps[ncpu_temp++] = mc / 1000.0;
            }
            break;
        }
        closedir(hd);
    }
    if (ncpu_temp == 0 && max > 0) {
        for (int pass = 0; pass < 2 && !ncpu_temp; pass++) {
            for (int z = 0; z < 32; z++) {
                char path[128], buf[32], type[64];
                snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", z);
                if (read_file_str(path, type, sizeof(type)) < 0) break;
                int match = (pass == 0 && strstr(type, "x86_pkg")) ||
                            (pass == 1 && strstr(type, "pkg"));
                if (!match) continue;
                snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", z);
                if (read_file_str(path, buf, sizeof(buf)) == 0) {
                    int v = atoi(buf);
                    if (v > 0) {
                        cpu_temps[0] = v / 1000.0;
                        ncpu_temp = 1;
                        break;
                    }
                }
            }
        }
    }
    return ncpu_temp;
}

static double cpu_temp_for_row(const CpuInfo *ci, const double *cpu_temps, int ncpu_temp,
                               int cpu_idx, int logical_cpus)
{
    if (ncpu_temp <= 0) return 0.0;
    if (ncpu_temp == 1) return cpu_temps[0];
    int phys_cores = ncpu_temp - 1;
    int tpc = ci->threads_per_core > 0 ? ci->threads_per_core :
              (phys_cores > 0 ? (logical_cpus + phys_cores - 1) / phys_cores : 1);
    if (tpc < 1) tpc = 1;
    int temp_idx = cpu_idx / tpc + 1;
    if (temp_idx >= ncpu_temp) temp_idx = 0;
    return cpu_temps[temp_idx];
}

static TopLayout build_top_layout(int tw, int th, int ncpu, int has_temp)
{
    TopLayout lo;
    int content_col, content_w, content_h;
    int band_row, body_rows, top_band_h, max_cpu_rows;

    memset(&lo, 0, sizeof(lo));
    lo.header.row = 1;
    lo.header.col = 1;
    lo.header.width = tw > 2 ? tw : 2;
    lo.header.height = th > 6 ? 3 : 2;
    lo.hint_row = th > 0 ? th : 1;

    lo.top.row = lo.header.row + lo.header.height + 1;
    lo.top.col = 1;
    lo.top.width = lo.header.width;
    lo.top.height = th - lo.top.row;
    if (lo.top.height < 4) lo.top.height = 4;

    content_col = lo.top.col + 2;
    content_w = lo.top.width - 4;
    content_h = lo.top.height - 2;
    if (content_w < 18) content_w = lo.top.width - 2;
    if (content_h < 3) content_h = lo.top.height - 2;

    lo.show_mem = content_w >= 28;
    lo.mem_stacked = 0;
    if (lo.show_mem && content_w >= 96) {
        lo.mem.width = clamp_int(content_w / 3 + 1, 34, 42);
        lo.cpu.width = content_w - lo.mem.width - 2;
        if (lo.cpu.width < 44) {
            lo.mem_stacked = 1;
        } else {
            lo.cpu.col = content_col;
            lo.mem.col = lo.cpu.col + lo.cpu.width + 2;
        }
    } else {
        lo.mem_stacked = lo.show_mem;
    }
    if (!lo.show_mem || lo.mem_stacked) {
        lo.cpu.col = content_col;
        lo.cpu.width = content_w;
        lo.mem.col = content_col;
        lo.mem.width = content_w;
    }

    lo.cpu.row = lo.top.row + 2;
    lo.cpu.height = content_h;
    lo.cpu_label_w = clamp_int(3 + digits_int(ncpu > 0 ? ncpu - 1 : 0), 4, 8);
    lo.show_governor = lo.cpu.width >= 50;
    lo.cpu_gov_w = lo.cpu.width >= 64 ? 12 : 8;
    lo.show_temp = has_temp && lo.cpu.width >= 58;

    int fixed_w = lo.cpu_label_w + 2 + 1 + 4 + 2 + 6;
    if (lo.show_governor) fixed_w += 2 + lo.cpu_gov_w;
    if (lo.show_temp) fixed_w += 2 + 6;
    lo.cpu_hist_w = lo.cpu.width - fixed_w;
    if (lo.cpu_hist_w < 8 && lo.show_temp) {
        lo.show_temp = 0;
        lo.cpu_hist_w += 8;
    }
    if (lo.cpu_hist_w < 8 && lo.show_governor) {
        lo.show_governor = 0;
        lo.cpu_hist_w += 2 + lo.cpu_gov_w;
        lo.cpu_gov_w = 0;
    }
    lo.cpu_hist_w = clamp_int(lo.cpu_hist_w, 6, MAX_TIMELINE);

    lo.load_row = lo.top.row + lo.top.height - 2;
    band_row = lo.top.row + 1;
    body_rows = lo.load_row - band_row;
    if (body_rows < 6) body_rows = 6;

    lo.mem_rows = lo.show_mem ? 7 : 0;
    if (lo.mem_stacked && lo.mem_rows > 5) lo.mem_rows = 5;
    lo.mem.height = lo.mem_rows > 0 ? lo.mem_rows + 2 : 0;
    lo.cpu.row = band_row + 1;
    lo.mem.row = lo.mem_stacked ? lo.cpu.row : band_row;

    max_cpu_rows = body_rows - 6;
    if (max_cpu_rows < 1) max_cpu_rows = 1;
    lo.cpu_rows = ncpu;
    if (lo.cpu_rows > max_cpu_rows) lo.cpu_rows = max_cpu_rows;
    if (lo.cpu_rows < 1) lo.cpu_rows = 1;
    if (lo.cpu_rows > ncpu) lo.cpu_rows = ncpu;

    for (;;) {
        top_band_h = 1 + lo.cpu_rows;
        if (lo.show_mem && lo.mem.height > 0) {
            if (lo.mem_stacked) top_band_h += lo.mem.height;
            else if (lo.mem.height > top_band_h) top_band_h = lo.mem.height;
        }
        if (body_rows - top_band_h >= 5) break;
        if (lo.show_mem && lo.mem_rows > 0) {
            lo.mem_rows--;
            lo.mem.height = lo.mem_rows > 0 ? lo.mem_rows + 2 : 0;
            if (lo.mem_rows == 0) lo.show_mem = 0;
            continue;
        }
        if (lo.cpu_rows > 1) {
            lo.cpu_rows--;
            continue;
        }
        break;
    }

    top_band_h = 1 + lo.cpu_rows;
    if (lo.show_mem && lo.mem.height > 0) {
        if (lo.mem_stacked) {
            lo.mem.row = lo.cpu.row + lo.cpu_rows;
            top_band_h += lo.mem.height;
        } else {
            lo.mem.row = band_row;
            if (lo.mem.height > top_band_h) top_band_h = lo.mem.height;
        }
    } else {
        lo.mem.height = 0;
    }

    lo.hidden_cpus = ncpu > lo.cpu_rows ? ncpu - lo.cpu_rows : 0;
    lo.mem_bar_w = clamp_int(lo.mem.width - 26, 6, 18);
    lo.proc.row = band_row + top_band_h;
    lo.proc.col = lo.top.col + 1;
    lo.proc.width = lo.top.width - 2;
    lo.proc.height = lo.load_row - lo.proc.row;
    lo.show_proc = lo.proc.height >= 5;
    lo.proc_rows = lo.show_proc ? lo.proc.height - 3 : 0;
    lo.side_stats.row = 0;
    lo.side_stats.col = 0;
    lo.side_stats.width = 0;
    lo.side_stats.height = 0;
    lo.show_side_stats = 0;
    lo.side_cpu_cols = 0;
    lo.load.row = lo.load_row;
    lo.load.col = lo.top.col + 1;
    lo.load.height = 1;
    lo.load.width = lo.top.width - 2;
    return lo;
}

static void render_header_widget(const TopLayout *lo, const DashboardState *state)
{
    char tb[32], sample_buf[24], heat_buf[24];
    strftime(tb, sizeof(tb), "%a %d %b %Y  %H:%M:%S", localtime(&(time_t){time(NULL)}));
    snprintf(sample_buf, sizeof(sample_buf), "%dms", state->sample_ms);
    snprintf(heat_buf, sizeof(heat_buf), "heat %s", heat_mode_name(state->heat_mode));
    draw_box(&lo->header, C->cyan);
    clear_inner_rect(&lo->header);
    if (lo->header.height < 3) return;
    int row = lo->header.row + 1;
    int col = lo->header.col + 2;
    int remaining = lo->header.width - 4;
    if (remaining < 1) return;
    move_to(row, col);
    remaining -= print_segment_fit(remaining, C->cyan, "LULO");
    remaining -= print_segment_fit(remaining, C->gray, "  ");
    remaining -= print_segment_fit(remaining, C->white,
                                   state->model_short[0] ? state->model_short : "CPU");
    remaining -= print_segment_fit(remaining, C->gray, "  ·  ");
    remaining -= print_segment_fit(remaining, C->white, state->hostname);
    remaining -= print_segment_fit(remaining, C->gray, "  ·  ");
    remaining -= print_segment_fit(remaining, C->cyan, sample_buf);
    remaining -= print_segment_fit(remaining, C->gray, "  ·  ");
    remaining -= print_segment_fit(remaining, heat_mode_accent(state->heat_mode), heat_buf);
    remaining -= print_segment_fit(remaining, C->gray, "  ·  ");
    remaining -= print_segment_fit(remaining, C->white, tb);
    if (remaining > 0) print_spaces(remaining);
}

static void render_top_frame(const TopLayout *lo)
{
    draw_box(&lo->top, C->blue);
}

static void render_top_headers(const TopLayout *lo, const DashboardState *state)
{
    int row = lo->top.row + 1;
    move_to(row, lo->cpu.col);
    printf("%s%-*s%s  ", C->bold, lo->cpu_label_w, "CPU", C->reset);
    printf("%s%s%-*s%s", heat_mode_accent(state->heat_mode), C->bold, lo->cpu_hist_w + 4, "HISTORY", C->reset);
    printf("  %s%-6s%s", C->bold, "MHz", C->reset);
    if (lo->show_governor) printf("  %s%-*s%s", C->bold, lo->cpu_gov_w, "GOV", C->reset);
    if (lo->show_temp) printf("  %s%-6s%s", C->bold, "TEMP", C->reset);
    if (lo->cpu.width > lo->cpu_label_w + lo->cpu_hist_w + 14) {
        int used = lo->cpu_label_w + lo->cpu_hist_w + 14 +
                   (lo->show_governor ? lo->cpu_gov_w + 2 : 0) +
                   (lo->show_temp ? 8 : 0);
        int fill = lo->cpu.width - used;
        if (fill > 0) print_spaces(fill);
    }
}

static void render_heatmap_cells(HeatMode mode, const unsigned char *hist, int width, int idx)
{
    for (int i = 0; i < width; i++) {
        int v = hist[i];
        if (v <= 0) {
            putchar(' ');
            continue;
        }
        printf("%s█%s", cpu_hist_color(mode, idx, v), C->reset);
    }
}

static void render_cpu_widget_row(const TopLayout *lo, const CpuInfo *ci, DashboardState *state,
                                  int term_row, int cpu_idx, int logical_cpus, int pct,
                                  int heat_raw_pct, int heat_task_pct,
                                  const CpuFreq *freqs, int nfreq,
                                  const double *cpu_temps, int ncpu_temp,
                                  int append_sample)
{
    char label[12], mhz_buf[16], temp_buf[16];
    const unsigned char *hist;
    long mhz = cpu_idx < nfreq ? freqs[cpu_idx].cur_khz / 1000 : 0;
    const char *gov = cpu_idx < nfreq ? freqs[cpu_idx].governor : "-";

    snprintf(label, sizeof(label), "cpu%d", cpu_idx);
    if (append_sample) {
        timeline_append(state->timeline[HEAT_MODE_RAW][cpu_idx + 1], MAX_TIMELINE, heat_raw_pct);
        timeline_append(state->timeline[HEAT_MODE_TASK][cpu_idx + 1], MAX_TIMELINE, heat_task_pct);
    }
    hist = state->timeline[state->heat_mode][cpu_idx + 1] + MAX_TIMELINE - lo->cpu_hist_w;

    move_to(term_row, lo->cpu.col);
    printf("%s%-*s%s  ", cpu_row_color(cpu_idx + 1), lo->cpu_label_w, label, C->reset);
    render_heatmap_cells(state->heat_mode, hist, lo->cpu_hist_w, cpu_idx);
    printf(" %s%s%3d%%%s", cpu_pct_color(pct), C->bold, pct, C->reset);

    if (mhz > 0) snprintf(mhz_buf, sizeof(mhz_buf), "%ld", mhz);
    else snprintf(mhz_buf, sizeof(mhz_buf), "-");
    printf("  %s%-6s%s", cpu_pct_color(pct), mhz_buf, C->reset);

    if (lo->show_governor) {
        printf("  %s%-*.*s%s", C->green, lo->cpu_gov_w, lo->cpu_gov_w, gov, C->reset);
    }
    if (lo->show_temp) {
        double t = cpu_temp_for_row(ci, cpu_temps, ncpu_temp, cpu_idx, logical_cpus);
        snprintf(temp_buf, sizeof(temp_buf), "%.1f°C", t);
        printf("  %s%-6s%s", temp_color(t), temp_buf, C->reset);
    }
}

static void render_memory_widget(const TopLayout *lo, const MemInfo *mi)
{
    if (!lo->show_mem || lo->mem_rows <= 0 || lo->mem.height < 3) return;
    draw_box_title(&lo->mem, C->blue, " Memory ", C->bold);
    clear_inner_rect(&lo->mem);
    for (int i = 0; i < lo->mem_rows; i++) {
        render_mem_row(i, mi, lo->mem.row + 1 + i, lo->mem.col + 1, lo->mem_bar_w);
    }
}

static void format_rate_value(char *buf, size_t len, unsigned long long value)
{
    if (value >= 1000000ULL) snprintf(buf, len, "%.1fM", value / 1000000.0);
    else if (value >= 1000ULL) snprintf(buf, len, "%.1fk", value / 1000.0);
    else snprintf(buf, len, "%llu", value);
}

static void render_side_stats_widget(const TopLayout *lo, const SideStatsState *state)
{
    static const int full_types[SIRQ_TYPES] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    static const int short_types[6] = { 0, 1, 2, 3, 7, 8 };
    char buf[32];
    int cols, val_w, row, detail_rows, show_sched, softirq_budget;
    const int *type_order;
    int type_count;

    if (!lo->show_side_stats || !state || state->ncpu <= 0) return;
    cols = lo->side_cpu_cols;
    if (cols > state->ncpu) cols = state->ncpu;
    val_w = (lo->side_stats.width - 8) / cols;
    while (cols > 1 && val_w < 5) {
        cols--;
        val_w = (lo->side_stats.width - 8) / cols;
    }
    if (cols < 1 || val_w < 4) return;

    row = lo->side_stats.row;
    move_to(row, lo->side_stats.col);
    printf("%-8s", "");
    for (int c = 0; c < cols; c++) {
        snprintf(buf, sizeof(buf), "cpu%d", c);
        printf("%*s", val_w, buf);
    }

    detail_rows = lo->side_stats.height - 1;
    show_sched = detail_rows >= 5;
    softirq_budget = detail_rows - (show_sched ? 4 : 0);
    if (softirq_budget >= SIRQ_TYPES) {
        type_order = full_types;
        type_count = SIRQ_TYPES;
    } else {
        type_order = short_types;
        type_count = softirq_budget < 6 ? softirq_budget : 6;
    }

    for (int i = 0; i < type_count; i++) {
        int ti = type_order[i];
        const char *tc = (!strcmp(sirq_names[ti], "SCHED"))   ? C->cyan :
                         (!strcmp(sirq_names[ti], "TIMER"))   ? C->yellow :
                         (!strcmp(sirq_names[ti], "HRTIMER")) ? C->orange :
                         (!strcmp(sirq_names[ti], "NET_RX") ||
                          !strcmp(sirq_names[ti], "NET_TX"))  ? C->green :
                         C->white;
        move_to(row + 1 + i, lo->side_stats.col);
        printf("%s%-8.8s%s", tc, sirq_names[ti], C->reset);
        for (int c = 0; c < cols; c++) {
            format_rate_value(buf, sizeof(buf), state->softirq[ti][c]);
            printf("%*s", val_w, buf);
        }
    }

    if (show_sched) {
        int base = row + 1 + type_count;
        move_to(base, lo->side_stats.col);
        printf("%s", C->gray);
        for (int i = 0; i < lo->side_stats.width; i++) putchar('-');
        printf("%s", C->reset);

        move_to(base + 1, lo->side_stats.col);
        printf("%-8s", "run%");
        for (int c = 0; c < cols; c++) {
            snprintf(buf, sizeof(buf), "%d%%", state->run_pct[c]);
            printf("%*s", val_w, buf);
        }

        move_to(base + 2, lo->side_stats.col);
        printf("%-8s", "wait%");
        for (int c = 0; c < cols; c++) {
            snprintf(buf, sizeof(buf), "%d%%", state->wait_pct[c]);
            printf("%*s", val_w, buf);
        }

        move_to(base + 3, lo->side_stats.col);
        printf("%-8s", "slices/s");
        for (int c = 0; c < cols; c++) {
            format_rate_value(buf, sizeof(buf), state->slices[c]);
            printf("%*s", val_w, buf);
        }
    }
}

static void format_proc_pct(char *buf, size_t len, int tenths)
{
    if (tenths < 1000) snprintf(buf, len, "%d.%d", tenths / 10, tenths % 10);
    else snprintf(buf, len, "%d", tenths / 10);
}

static void format_proc_policy(char *buf, size_t len, int policy)
{
    switch (policy) {
    case 0: snprintf(buf, len, "TS"); break;
    case 1: snprintf(buf, len, "FF"); break;
    case 2: snprintf(buf, len, "RR"); break;
    case 3: snprintf(buf, len, "B"); break;
    case 5: snprintf(buf, len, "IDL"); break;
    case 6: snprintf(buf, len, "DLN"); break;
    case 7: snprintf(buf, len, "EXT"); break;
    default: snprintf(buf, len, "?"); break;
    }
}

static void format_proc_priority(char *buf, size_t len, const LuloProcRow *row)
{
    if (row->rt_priority > 0) snprintf(buf, len, "%d", row->rt_priority);
    else if (row->priority < 0) snprintf(buf, len, "%d", -row->priority - 1);
    else snprintf(buf, len, "%d", row->priority);
}

static void format_proc_time(char *buf, size_t len, unsigned long long time_cs)
{
    unsigned long long total_sec = time_cs / 100;
    unsigned long long cs = time_cs % 100;
    unsigned long long minutes = total_sec / 60;
    unsigned long long seconds = total_sec % 60;
    if (minutes >= 100) {
        unsigned long long hours = minutes / 60;
        minutes %= 60;
        snprintf(buf, len, "%lluh%02llum", hours, minutes);
    } else {
        snprintf(buf, len, "%02llu:%02llu.%02llu", minutes, seconds, cs);
    }
}

static const char *proc_state_color(char state)
{
    switch (state) {
    case 'R': return C->green;
    case 'D': return C->orange;
    case 'Z': return C->red;
    case 'T': return C->yellow;
    case 'I': return C->blue;
    case 'S': return C->white;
    default:  return C->gray;
    }
}

static const char *proc_pid_color(const LuloProcRow *row)
{
    if (row->state == 'Z') return C->red;
    if (row->is_kernel) return C->cyan;
    if (row->is_thread) return C->green;
    return proc_state_color(row->state);
}

static const char *proc_user_color(const LuloProcRow *row)
{
    if (row->is_kernel) return C->gray;
    if (!strcmp(row->user, "root")) return C->red;
    return row->is_thread ? C->green : C->cyan;
}

static const char *proc_cpu_color(int tenths)
{
    if (tenths >= 500) return C->red;
    if (tenths >= 200) return C->orange;
    if (tenths >= 50) return C->yellow;
    if (tenths > 0) return C->green;
    return C->gray;
}

static const char *proc_mem_color(int tenths)
{
    if (tenths >= 100) return C->red;
    if (tenths >= 50) return C->orange;
    if (tenths >= 20) return C->yellow;
    if (tenths > 0) return C->blue;
    return C->gray;
}

static const char *proc_prio_color(const LuloProcRow *row)
{
    if (row->policy == 6) return C->red;
    if (row->rt_priority > 0 || row->priority < 0) return C->orange;
    if (row->priority <= 19) return C->green;
    if (row->priority <= 39) return C->white;
    return C->gray;
}

static const char *proc_policy_color(int policy)
{
    switch (policy) {
    case 1:
    case 2:
        return C->orange;
    case 6:
        return C->red;
    case 7:
        return C->cyan;
    case 3:
        return C->yellow;
    case 5:
        return C->gray;
    default:
        return C->white;
    }
}

static const char *proc_nice_color(int nice_value)
{
    if (nice_value < 0) return C->cyan;
    if (nice_value > 0) return C->yellow;
    return C->white;
}

static const char *proc_time_color(unsigned long long time_cs)
{
    if (time_cs >= 6000) return C->orange;
    if (time_cs > 0) return C->green;
    return C->gray;
}

static const char *proc_command_color(const LuloProcRow *row)
{
    if (row->state == 'Z') return C->red;
    if (row->is_kernel) return C->cyan;
    if (row->is_thread) return C->green;
    return proc_state_color(row->state);
}

static const char *proc_branch_color(void)
{
    return C == &color_off ? C->reset : "\033[38;2;82;108;92m";
}

static void render_process_widget(const TopLayout *lo, const LuloProcSnapshot *snap, const LuloProcState *state)
{
    char line[1024], cpu_buf[16], mem_buf[16], time_buf[16], policy_buf[8], prio_buf[16], rows_buf[32];
    int inner_w, pid_w = 5, user_w = 0, policy_w = 4, prio_w = 4, nice_w = 3, cpu_w = 5, mem_w = 0, time_w = 0;
    int show_user, show_mem, show_time, cmd_w, start, visible;

    if (!lo->show_proc || lo->proc_rows <= 0) return;
    draw_box(&lo->proc, C->blue);
    clear_inner_rect(&lo->proc);

    inner_w = lo->proc.width - 2;
    show_user = inner_w >= 58;
    show_mem = inner_w >= 72;
    show_time = inner_w >= 86;
    user_w = show_user ? (inner_w >= 68 ? 8 : 6) : 0;
    mem_w = show_mem ? 5 : 0;
    time_w = show_time ? 8 : 0;
    cmd_w = inner_w - pid_w - policy_w - prio_w - nice_w - cpu_w - 6;
    if (show_user) cmd_w -= user_w + 1;
    if (show_mem) cmd_w -= mem_w + 1;
    if (show_time) cmd_w -= time_w + 1;
    if (cmd_w < 10 && show_time) {
        show_time = 0;
        cmd_w += time_w + 1;
        time_w = 0;
    }
    if (cmd_w < 10 && show_mem) {
        show_mem = 0;
        cmd_w += mem_w + 1;
        mem_w = 0;
    }
    if (cmd_w < 10 && show_user) {
        show_user = 0;
        cmd_w += user_w + 1;
        user_w = 0;
    }
    if (cmd_w < 8) cmd_w = 8;

    move_to(lo->proc.row, lo->proc.col + 2);
    printf("%s Process Tree %s", C->bold, C->reset);
    if (snap && snap->count > 0) {
        snprintf(rows_buf, sizeof(rows_buf), " %d/%d ", state->selected + 1, snap->count);
        move_to(lo->proc.row, lo->proc.col + lo->proc.width - (int)strlen(rows_buf) - 1);
        printf("%s%s%s", C->gray, rows_buf, C->reset);
    }

    move_to(lo->proc.row + 1, lo->proc.col + 1);
    printf("%s%*s%s ", C->gray, pid_w, "PID", C->reset);
    if (show_user) printf("%s%-*s%s ", C->orange, user_w, "USER", C->reset);
    printf("%s%-*s%s ", C->yellow, policy_w, "POL", C->reset);
    printf("%s%*s%s ", C->orange, prio_w, "PRI", C->reset);
    printf("%s%*s%s ", C->cyan, nice_w, "NI", C->reset);
    printf("%s%*s%s ", C->green, cpu_w, "CPU%", C->reset);
    if (show_mem) printf("%s%*s%s ", C->blue, mem_w, "MEM%", C->reset);
    if (show_time) printf("%s%-*s%s ", C->cyan, time_w, "TIME+", C->reset);
    printf("%s%-*s%s", C->white, cmd_w, "COMMAND", C->reset);

    start = state ? state->scroll : 0;
    if (start < 0) start = 0;
    visible = lo->proc_rows;
    for (int i = 0; i < visible; i++) {
        int row_index = start + i;
        move_to(lo->proc.row + 2 + i, lo->proc.col + 1);
        if (!snap || row_index >= snap->count) {
            printf("%-*s", inner_w, "");
            continue;
        }

        const LuloProcRow *row = &snap->rows[row_index];
        format_proc_pct(cpu_buf, sizeof(cpu_buf), row->cpu_tenths);
        format_proc_pct(mem_buf, sizeof(mem_buf), row->mem_tenths);
        format_proc_time(time_buf, sizeof(time_buf), row->time_cs);
        format_proc_policy(policy_buf, sizeof(policy_buf), row->policy);
        format_proc_priority(prio_buf, sizeof(prio_buf), row);

        snprintf(line, sizeof(line), "%*d ", pid_w, row->pid);
        if (show_user) snprintf(line + strlen(line), sizeof(line) - strlen(line), "%-*.*s ", user_w, user_w, row->user);
        snprintf(line + strlen(line), sizeof(line) - strlen(line), "%-*s ", policy_w, policy_buf);
        snprintf(line + strlen(line), sizeof(line) - strlen(line), "%*s ", prio_w, prio_buf);
        snprintf(line + strlen(line), sizeof(line) - strlen(line), "%*d ", nice_w, row->nice);
        snprintf(line + strlen(line), sizeof(line) - strlen(line), "%*s ", cpu_w, cpu_buf);
        if (show_mem) snprintf(line + strlen(line), sizeof(line) - strlen(line), "%*s ", mem_w, mem_buf);
        if (show_time) snprintf(line + strlen(line), sizeof(line) - strlen(line), "%-*s ", time_w, time_buf);
        snprintf(line + strlen(line), sizeof(line) - strlen(line), "%-*.*s", cmd_w, cmd_w, row->label);
        if (state && row_index == state->selected && C != &color_off) {
            printf("\033[7m%-*.*s%s", inner_w, inner_w, line, C->reset);
            continue;
        }

        printf("%s%*d%s ", proc_pid_color(row), pid_w, row->pid, C->reset);
        if (show_user) printf("%s%-*.*s%s ", proc_user_color(row), user_w, user_w, row->user, C->reset);
        printf("%s%-*s%s ", proc_policy_color(row->policy), policy_w, policy_buf, C->reset);
        printf("%s%*s%s ", proc_prio_color(row), prio_w, prio_buf, C->reset);
        printf("%s%*d%s ", proc_nice_color(row->nice), nice_w, row->nice, C->reset);
        printf("%s%*s%s ", proc_cpu_color(row->cpu_tenths), cpu_w, cpu_buf, C->reset);
        if (show_mem) printf("%s%*s%s ", proc_mem_color(row->mem_tenths), mem_w, mem_buf, C->reset);
        if (show_time) printf("%s%-*s%s ", proc_time_color(row->time_cs), time_w, time_buf, C->reset);
        if (row->label_prefix_len > 0 && row->label_prefix_len < (int)sizeof(row->label) &&
            row->label_prefix_cols < cmd_w) {
            int cmd_only_w = cmd_w - row->label_prefix_cols;
            printf("%s", proc_branch_color());
            fwrite(row->label, 1, (size_t)row->label_prefix_len, stdout);
            printf("%s", C->reset);
            printf("%s%-*.*s%s", proc_command_color(row), cmd_only_w, cmd_only_w,
                   row->label + row->label_prefix_len, C->reset);
        } else {
            printf("%s%-*.*s%s", proc_command_color(row), cmd_w, cmd_w, row->label, C->reset);
        }
    }
}

static void render_load_widget(const TopLayout *lo, const LoadInfo *li, int logical_cpus)
{
    char buf[96];
    int nc = logical_cpus > 0 ? logical_cpus : 1;
    int remaining = lo->load.width - 2;
    if (remaining < 1) return;

    clear_line_span(lo->load.row, lo->load.col + 1, lo->load.width - 2);
    move_to(lo->load.row, lo->load.col + 1);
    snprintf(buf, sizeof(buf), "load ");
    remaining -= print_segment_fit(remaining, C->white, buf);
    snprintf(buf, sizeof(buf), "%.2f %.2f %.2f", li->load1, li->load5, li->load15);
    remaining -= print_segment_fit(remaining,
                                   li->load1 > nc * 0.9 ? C->red :
                                   li->load1 > nc * 0.7 ? C->orange :
                                   li->load1 > nc * 0.5 ? C->yellow : C->green,
                                   buf);
    snprintf(buf, sizeof(buf), "  run ");
    remaining -= print_segment_fit(remaining, C->white, buf);
    snprintf(buf, sizeof(buf), "%d/%d", li->running, li->total);
    remaining -= print_segment_fit(remaining, C->cyan, buf);
    snprintf(buf, sizeof(buf), "  blk ");
    remaining -= print_segment_fit(remaining, C->white, buf);
    snprintf(buf, sizeof(buf), "%d", li->procs_blocked);
    remaining -= print_segment_fit(remaining, li->procs_blocked > 0 ? C->orange : C->white, buf);
    snprintf(buf, sizeof(buf), "  ctxsw ");
    remaining -= print_segment_fit(remaining, C->white, buf);
    snprintf(buf, sizeof(buf), "%ld", li->ctxt);
    remaining -= print_segment_fit(remaining, C->white, buf);
    if (lo->hidden_cpus > 0) {
        snprintf(buf, sizeof(buf), "  +%d cpu%s hidden", lo->hidden_cpus,
                 lo->hidden_cpus == 1 ? "" : "s");
        remaining -= print_segment_fit(remaining, C->gray, buf);
    }
    if (remaining > 0) print_spaces(remaining);
}

static void render_hint_row(int row, int width)
{
    if (row <= 0 || width <= 0) return;
    move_to(row, 1);
    print_segment_fit(width, C->white, "  q / ESC exit   +/- sample   t heat   j/k or arrows scroll   PgUp/PgDn jump");
}

static void finish_render(const TopLayout *lo, int animate)
{
    move_to(animate ? lo->hint_row : (lo->load.row + 1), 1);
    fflush(stdout);
}

static void render_top_dashboard(const TopLayout *lo, const CpuInfo *ci, DashboardState *state,
                                 const CpuStat *sa, const CpuStat *sb,
                                 const CpuFreq *freqs, int nfreq,
                                 const MemInfo *mi, const LoadInfo *li,
                                 const SideStatsState *side_stats,
                                 const LuloProcSnapshot *proc_snap, const LuloProcState *proc_state,
                                 const double *cpu_temps, int ncpu_temp,
                                 int animate, int append_sample, int full_refresh)
{
    if (full_refresh) {
        clear_screen();
        render_top_frame(lo);
        render_top_headers(lo, state);
        if (animate) render_hint_row(lo->hint_row, lo->top.width);
    }
    render_header_widget(lo, state);
    for (int i = 0; i < lo->cpu_rows; i++) {
        int pct = cpu_pct(&sa->cpu[i + 1], &sb->cpu[i + 1]);
        int heat_raw_pct = cpu_heat_pct(&sa->cpu[i + 1], &sb->cpu[i + 1], HEAT_MODE_RAW);
        int heat_task_pct = cpu_heat_pct(&sa->cpu[i + 1], &sb->cpu[i + 1], HEAT_MODE_TASK);
        render_cpu_widget_row(lo, ci, state, lo->cpu.row + i, i, sb->ncpu, pct,
                              heat_raw_pct, heat_task_pct,
                              freqs, nfreq, cpu_temps, ncpu_temp, append_sample);
    }
    render_memory_widget(lo, mi);
    render_side_stats_widget(lo, side_stats);
    render_process_widget(lo, proc_snap, proc_state);
    render_load_widget(lo, li, sb->ncpu);
    finish_render(lo, animate);
}

static void render_process_only(const TopLayout *lo, const LuloProcSnapshot *proc_snap,
                                const LuloProcState *proc_state, int animate)
{
    render_process_widget(lo, proc_snap, proc_state);
    finish_render(lo, animate);
}

static void render_header_only(const TopLayout *lo, const DashboardState *state, int animate)
{
    render_header_widget(lo, state);
    finish_render(lo, animate);
}

typedef enum {
    INPUT_NONE = 0,
    INPUT_QUIT,
    INPUT_SAMPLE_FASTER,
    INPUT_SAMPLE_SLOWER,
    INPUT_TOGGLE_HEAT,
    INPUT_SCROLL_UP,
    INPUT_SCROLL_DOWN,
    INPUT_PAGE_UP,
    INPUT_PAGE_DOWN,
    INPUT_HOME,
    INPUT_END
} InputAction;

static InputAction decode_input_action(char c)
{
    char seq1, seq2, seq3;

    switch (c) {
    case 'q':
    case 'Q':
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
        break;
    }

    if (c != 27) return INPUT_NONE;
    if (!stdin_read_byte_timeout(&seq1, 25)) return INPUT_QUIT;
    if (seq1 == '[') {
        if (!stdin_read_byte_timeout(&seq2, 25)) return INPUT_NONE;
        if (seq2 >= '0' && seq2 <= '9') {
            if (!stdin_read_byte_timeout(&seq3, 25)) return INPUT_NONE;
            if (seq3 == '~') {
                if (seq2 == '1' || seq2 == '7') return INPUT_HOME;
                if (seq2 == '4' || seq2 == '8') return INPUT_END;
                if (seq2 == '5') return INPUT_PAGE_UP;
                if (seq2 == '6') return INPUT_PAGE_DOWN;
            }
            return INPUT_NONE;
        }
        switch (seq2) {
        case 'A': return INPUT_SCROLL_UP;
        case 'B': return INPUT_SCROLL_DOWN;
        case 'H': return INPUT_HOME;
        case 'F': return INPUT_END;
        default: return INPUT_NONE;
        }
    }
    if (seq1 == 'O') {
        if (!stdin_read_byte_timeout(&seq2, 25)) return INPUT_NONE;
        if (seq2 == 'H') return INPUT_HOME;
        if (seq2 == 'F') return INPUT_END;
    }
    stdin_drain_pending_bytes();
    return INPUT_NONE;
}
/* ── main ────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    int no_color = 0, opt;
    int animate, has_tty = 0, exit_requested = 0;
    int sample_ms = 1000;
    int last_tw = -1, last_th = -1;
    int have_layout = 0;
    int append_sample = 1;
    struct termios old_tc, raw_tc;
    CpuInfo ci;
    CpuStat stat_a, stat_b;
    DashboardState dash;
    LuloProcState proc_state;
    SideStatsState side_stats;
    TopLayout last_lo;

    signal(SIGINT, sig_exit);
    signal(SIGTERM, sig_exit);
    while ((opt = getopt(argc, argv, "nh")) != -1) switch (opt) {
        case 'n': no_color = 1; break;
        case 'h':
            fprintf(stderr, "Usage: lulo [-n] [-h]\n  -n  no color\n  -h  this help\n");
            return 0;
    }

    animate = isatty(STDOUT_FILENO) && isatty(STDIN_FILENO);
    if (no_color || !isatty(STDOUT_FILENO)) C = &color_off;

    gather_cpu_info(&ci);
    memset(&dash, 0, sizeof(dash));
    lulo_proc_state_init(&proc_state);
    side_stats_init(&side_stats);
    abbrev_cpu_model(ci.model, dash.model_short, sizeof(dash.model_short));
    gethostname(dash.hostname, sizeof(dash.hostname));
    dash.sample_ms = sample_ms;
    dash.heat_mode = HEAT_MODE_RAW;

    read_cpu_stat(&stat_a);
    sleep_ms(200);
    read_cpu_stat(&stat_b);

    if (animate) {
        printf("\033[?1049h\033[?25l");
        fflush(stdout);
    }

    if (animate && tcgetattr(STDIN_FILENO, &old_tc) == 0) {
        has_tty = 1;
        raw_tc = old_tc;
        cfmakeraw(&raw_tc);
        raw_tc.c_cc[VMIN] = 0;
        raw_tc.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw_tc);
    }

    /* Lower widgets remain in the file for later work; the active UI is top-only for now. */
    for (;;) {
        CpuFreq freqs[MAX_CPUS];
        MemInfo mi;
        LoadInfo li;
        LuloProcSnapshot proc_snap = {0};
        double cpu_temps[64] = {0};
        int tw = term_width();
        int th = term_height();
        int nfreq = gather_cpu_freq(freqs, MAX_CPUS);
        int ncpu_temp = read_cpu_temps(cpu_temps, 64);
        unsigned long long total_delta = cpu_total_delta(&stat_a, &stat_b);
        TopLayout lo;
        int full_refresh;

        gather_meminfo(&mi);
        gather_loadavg(&li);
        side_stats_update(&side_stats, stat_b.ncpu, append_sample);
        lulo_proc_snapshot_gather(&proc_snap, &proc_state, total_delta, mi.total, stat_b.ncpu);
        dash.sample_ms = sample_ms;
        lo = build_top_layout(tw, th, stat_b.ncpu, ncpu_temp > 0);
        lulo_proc_view_sync(&proc_state, &proc_snap, lo.proc_rows);
        full_refresh = !have_layout || tw != last_tw || th != last_th ||
                       memcmp(&lo, &last_lo, sizeof(lo)) != 0;
        render_top_dashboard(&lo, &ci, &dash, &stat_a, &stat_b, freqs, nfreq,
                             &mi, &li, &side_stats, &proc_snap, &proc_state,
                             cpu_temps, ncpu_temp, animate, append_sample, full_refresh);
        append_sample = 0;
        last_tw = tw;
        last_th = th;
        last_lo = lo;
        have_layout = 1;

        if (!animate) {
            lulo_proc_snapshot_free(&proc_snap);
            break;
        }

        long long deadline = mono_ms_now() + sample_ms;
        int need_resample = 0;
        while (!need_resample) {
            int timeout_ms = ms_until_deadline(deadline);
            if (timeout_ms > 100) timeout_ms = 100;
            if (timeout_ms < 0) timeout_ms = 0;
            int pr = 0;
            if (has_tty) {
                struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
                pr = poll(&pfd, 1, timeout_ms);
            } else if (timeout_ms > 0) {
                sleep_ms(timeout_ms);
            }

            int cur_tw = term_width();
            int cur_th = term_height();
            if (cur_tw != last_tw || cur_th != last_th) goto redraw;

            if (has_tty && pr > 0) {
                char c;
                while (read(STDIN_FILENO, &c, 1) == 1) {
                    InputAction action = decode_input_action(c);
                    if (action == INPUT_QUIT) {
                        exit_requested = 1;
                        break;
                    }
                    if (action == INPUT_SAMPLE_FASTER) {
                        sample_ms = adjust_sample_ms(sample_ms, -1);
                        dash.sample_ms = sample_ms;
                        deadline = mono_ms_now() + sample_ms;
                        render_header_only(&lo, &dash, animate);
                        continue;
                    }
                    if (action == INPUT_SAMPLE_SLOWER) {
                        sample_ms = adjust_sample_ms(sample_ms, +1);
                        dash.sample_ms = sample_ms;
                        deadline = mono_ms_now() + sample_ms;
                        render_header_only(&lo, &dash, animate);
                        continue;
                    }
                    if (action == INPUT_TOGGLE_HEAT) {
                        dash.heat_mode = next_heat_mode(dash.heat_mode);
                        render_top_dashboard(&lo, &ci, &dash, &stat_a, &stat_b, freqs, nfreq,
                                             &mi, &li, &side_stats, &proc_snap, &proc_state,
                                             cpu_temps, ncpu_temp, animate, 0, 1);
                        continue;
                    }
                    if (action == INPUT_SCROLL_UP) {
                        lulo_proc_view_move(&proc_state, &proc_snap, lo.proc_rows, -1);
                        render_process_only(&lo, &proc_snap, &proc_state, animate);
                        continue;
                    }
                    if (action == INPUT_SCROLL_DOWN) {
                        lulo_proc_view_move(&proc_state, &proc_snap, lo.proc_rows, +1);
                        render_process_only(&lo, &proc_snap, &proc_state, animate);
                        continue;
                    }
                    if (action == INPUT_PAGE_UP) {
                        lulo_proc_view_page(&proc_state, &proc_snap, lo.proc_rows, -1);
                        render_process_only(&lo, &proc_snap, &proc_state, animate);
                        continue;
                    }
                    if (action == INPUT_PAGE_DOWN) {
                        lulo_proc_view_page(&proc_state, &proc_snap, lo.proc_rows, +1);
                        render_process_only(&lo, &proc_snap, &proc_state, animate);
                        continue;
                    }
                    if (action == INPUT_HOME) {
                        lulo_proc_view_home(&proc_state, &proc_snap, lo.proc_rows);
                        render_process_only(&lo, &proc_snap, &proc_state, animate);
                        continue;
                    }
                    if (action == INPUT_END) {
                        lulo_proc_view_end(&proc_state, &proc_snap, lo.proc_rows);
                        render_process_only(&lo, &proc_snap, &proc_state, animate);
                        continue;
                    }
                }
            }

            if (exit_requested) break;
            if (ms_until_deadline(deadline) == 0) need_resample = 1;
        }

        if (exit_requested) {
            lulo_proc_snapshot_free(&proc_snap);
            break;
        }
        lulo_proc_snapshot_free(&proc_snap);
        stat_a = stat_b;
        read_cpu_stat(&stat_b);
        append_sample = 1;
        continue;

redraw:
        lulo_proc_snapshot_free(&proc_snap);
        append_sample = 0;
    }

    if (animate) {
        printf("\033[?25h");
        if (has_tty) tcsetattr(STDIN_FILENO, TCSANOW, &old_tc);
        printf("\033[?1049l");
        fflush(stdout);
    }
    lulo_proc_state_cleanup(&proc_state);
    return 0;
}
