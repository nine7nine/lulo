#ifndef LULO_MODEL_H
#define LULO_MODEL_H

#include <stddef.h>

#define LULO_MAX_TIMELINE 128
#define LULO_MAX_CPUS 256

typedef struct {
    int row;
    int col;
    int width;
    int height;
} LuloRect;

typedef enum {
    HEAT_MODE_RAW = 0,
    HEAT_MODE_TASK,
    HEAT_MODE_COUNT
} HeatMode;

typedef struct {
    char model[128];
    char vendor[32];
    char flags[4096];
    char microcode[32];
    char stepping[8];
    char family[8];
    int physical_cores;
    int logical_cores;
    int sockets;
    int cores_per_socket;
    int threads_per_core;
    int cache_l1d_kb;
    int cache_l2_kb;
    int cache_l3_kb;
    double base_mhz;
} CpuInfo;

typedef struct {
    unsigned long long user;
    unsigned long long nice;
    unsigned long long sys;
    unsigned long long idle;
    unsigned long long iowait;
    unsigned long long irq;
    unsigned long long softirq;
    unsigned long long steal;
} CpuTick;

typedef struct {
    CpuTick cpu[LULO_MAX_CPUS + 1];
    int ncpu;
} CpuStat;

typedef struct {
    int cpu;
    long cur_khz;
    long min_khz;
    long max_khz;
    char governor[32];
    char driver[32];
} CpuFreq;

typedef struct {
    unsigned long long total;
    unsigned long long free;
    unsigned long long available;
    unsigned long long buffers;
    unsigned long long cached;
    unsigned long long shmem;
    unsigned long long slab;
    unsigned long long sreclaimable;
    unsigned long long swap_total;
    unsigned long long swap_free;
    unsigned long long swap_cached;
    unsigned long long dirty;
    unsigned long long writeback;
    unsigned long long hugepages_total;
    unsigned long long hugepages_free;
    unsigned long long hugepage_size_kb;
} MemInfo;

typedef struct {
    double load1;
    double load5;
    double load15;
    int running;
    int total;
    int procs_blocked;
    long ctxt;
} LoadInfo;

typedef struct {
    LuloRect header;
    LuloRect top;
    LuloRect cpu;
    LuloRect mem;
    LuloRect proc;
    LuloRect load;
    int show_mem;
    int mem_stacked;
    int mem_rows;
    int mem_bar_w;
    int show_proc;
    int proc_rows;
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
    unsigned char timeline[HEAT_MODE_COUNT][LULO_MAX_CPUS + 1][LULO_MAX_TIMELINE];
} DashboardState;

void lulo_dashboard_init(DashboardState *dash, const CpuInfo *ci, int sample_ms);
void lulo_dashboard_append_heat(DashboardState *dash, int cpu_idx, int heat_raw_pct, int heat_task_pct);
const unsigned char *lulo_dashboard_history(const DashboardState *dash, int cpu_idx, int width);

const char *lulo_heat_mode_name(HeatMode mode);
HeatMode lulo_next_heat_mode(HeatMode mode);

int lulo_adjust_sample_ms(int current, int delta);

void lulo_gather_cpu_info(CpuInfo *ci);
int lulo_read_cpu_stat(CpuStat *s);
int lulo_cpu_pct(const CpuTick *a, const CpuTick *b);
int lulo_cpu_heat_pct(const CpuTick *a, const CpuTick *b, HeatMode mode);
unsigned long long lulo_cpu_total_delta(const CpuStat *a, const CpuStat *b);
int lulo_gather_cpu_freq(CpuFreq *freqs, int max);
void lulo_gather_meminfo(MemInfo *m);
void lulo_gather_loadavg(LoadInfo *li);
int lulo_read_cpu_temps(double *cpu_temps, int max);
double lulo_cpu_temp_for_row(const CpuInfo *ci, const double *cpu_temps, int ncpu_temp,
                             int cpu_idx, int logical_cpus);
TopLayout lulo_build_layout(int tw, int th, int ncpu, int has_temp);

void lulo_format_size(char *buf, size_t len, unsigned long long bytes);
void lulo_format_proc_pct(char *buf, size_t len, int tenths);
void lulo_format_proc_policy(char *buf, size_t len, int policy);
void lulo_format_proc_priority(char *buf, size_t len, int rt_priority, int priority);
void lulo_format_proc_time(char *buf, size_t len, unsigned long long time_cs);

#endif
