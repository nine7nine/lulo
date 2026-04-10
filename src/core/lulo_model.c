#define _GNU_SOURCE

#include "lulo_model.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int read_file_str(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    int ok;
    char *end;

    if (!f) return -1;
    ok = (fgets(buf, (int)len, f) != NULL);
    fclose(f);
    if (!ok) return -1;
    end = buf + strlen(buf) - 1;
    while (end >= buf && (*end == '\n' || *end == '\r' || *end == ' ')) *end-- = '\0';
    return 0;
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
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

static void timeline_append(unsigned char *hist, int width, int pct)
{
    if (width <= 0) return;
    memmove(hist, hist + 1, (size_t)(width - 1));
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    hist[width - 1] = (unsigned char)pct;
}

static void abbrev_cpu_model(const char *in, char *out, size_t len)
{
    const char *intel_core;
    const char *ryzen;
    const char *at;
    size_t n;

    if (!in || !*in) {
        snprintf(out, len, "CPU");
        return;
    }
    intel_core = strstr(in, "Intel(R) Core(TM) ");
    if (intel_core) {
        intel_core += strlen("Intel(R) Core(TM) ");
        at = strstr(intel_core, " @ ");
        n = at ? (size_t)(at - intel_core) : strlen(intel_core);
        if (n >= len) n = len - 1;
        memcpy(out, intel_core, n);
        out[n] = '\0';
        return;
    }
    ryzen = strstr(in, "AMD Ryzen ");
    if (ryzen) {
        at = strstr(ryzen, " with ");
        n = at ? (size_t)(at - ryzen) : strlen(ryzen);
        if (n >= len) n = len - 1;
        memcpy(out, ryzen, n);
        out[n] = '\0';
        return;
    }
    snprintf(out, len, "%.*s", (int)len - 1, in);
}

void lulo_dashboard_init(DashboardState *dash, const CpuInfo *ci, int sample_ms)
{
    memset(dash, 0, sizeof(*dash));
    abbrev_cpu_model(ci->model, dash->model_short, sizeof(dash->model_short));
    gethostname(dash->hostname, sizeof(dash->hostname));
    dash->sample_ms = sample_ms;
}

void lulo_dashboard_append_heat(DashboardState *dash, int cpu_idx, int heat_pct)
{
    if (cpu_idx < 0 || cpu_idx >= LULO_MAX_CPUS) return;
    timeline_append(dash->timeline[cpu_idx + 1], LULO_MAX_TIMELINE, heat_pct);
}

const unsigned char *lulo_dashboard_history(const DashboardState *dash, int cpu_idx, int width)
{
    if (cpu_idx < 0 || cpu_idx >= LULO_MAX_CPUS) return NULL;
    if (width < 0) width = 0;
    if (width > LULO_MAX_TIMELINE) width = LULO_MAX_TIMELINE;
    return dash->timeline[cpu_idx + 1] + (LULO_MAX_TIMELINE - width);
}

int lulo_adjust_sample_ms(int current, int delta)
{
    int step = current < 1000 ? 100 : 200;
    return clamp_int(current + delta * step, 100, 5000);
}

void lulo_format_size(char *buf, size_t len, unsigned long long bytes)
{
    const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double val = (double)bytes;
    int i = 0;
    while (val >= 1024.0 && i < 4) {
        val /= 1024.0;
        i++;
    }
    if (i == 0) snprintf(buf, len, "%llu B", bytes);
    else if (val >= 100.0) snprintf(buf, len, "%.0f %s", val, units[i]);
    else if (val >= 10.0) snprintf(buf, len, "%.1f %s", val, units[i]);
    else snprintf(buf, len, "%.2f %s", val, units[i]);
}

void lulo_gather_cpu_info(CpuInfo *ci)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[4096];
    int phys_ids[64];
    int nphys = 0;

    memset(ci, 0, sizeof(*ci));
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *colon = strchr(line, ':');
        char key[64];
        char *k;
        char *ke;
        char *v;
        int klen;
        int vlen;
        const char *val;

        if (!colon) continue;
        klen = (int)(colon - line);
        if (klen >= (int)sizeof(key)) klen = (int)sizeof(key) - 1;
        memcpy(key, line, (size_t)klen);
        key[klen] = '\0';
        k = key;
        while (*k == '\t' || *k == ' ') k++;
        ke = k + strlen(k) - 1;
        while (ke > k && (*ke == ' ' || *ke == '\t')) *ke-- = '\0';
        v = colon + 1;
        while (*v == ' ') v++;
        vlen = (int)strlen(v);
        while (vlen > 0 && (v[vlen - 1] == '\n' || v[vlen - 1] == '\r' || v[vlen - 1] == ' ')) v[--vlen] = '\0';
        val = v;

        if (!strcmp(k, "model name") && !ci->model[0]) snprintf(ci->model, sizeof(ci->model), "%s", val);
        else if (!strcmp(k, "vendor_id") && !ci->vendor[0]) snprintf(ci->vendor, sizeof(ci->vendor), "%s", val);
        else if (!strcmp(k, "cpu MHz") && ci->base_mhz == 0) ci->base_mhz = atof(val);
        else if (!strcmp(k, "cache size") && !ci->cache_l2_kb) ci->cache_l2_kb = atoi(val);
        else if (!strcmp(k, "physical id")) {
            int id = atoi(val);
            int found = 0;
            for (int i = 0; i < nphys; i++) {
                if (phys_ids[i] == id) {
                    found = 1;
                    break;
                }
            }
            if (!found && nphys < 64) phys_ids[nphys++] = id;
        } else if (!strcmp(k, "processor")) ci->logical_cores++;
        else if (!strcmp(k, "cpu cores") && !ci->cores_per_socket) ci->cores_per_socket = atoi(val);
        else if (!strcmp(k, "stepping") && !ci->stepping[0]) snprintf(ci->stepping, sizeof(ci->stepping), "%s", val);
        else if (!strcmp(k, "cpu family") && !ci->family[0]) snprintf(ci->family, sizeof(ci->family), "%s", val);
        else if (!strcmp(k, "microcode") && !ci->microcode[0]) snprintf(ci->microcode, sizeof(ci->microcode), "%s", val);
        else if (!strcmp(k, "flags") && !ci->flags[0]) snprintf(ci->flags, sizeof(ci->flags), "%s", val);
    }
    fclose(f);
    ci->sockets = nphys > 0 ? nphys : 1;
    if (!ci->cores_per_socket && ci->logical_cores) ci->cores_per_socket = ci->logical_cores / ci->sockets;
    ci->physical_cores = ci->sockets * ci->cores_per_socket;
    ci->threads_per_core = ci->physical_cores > 0 ? ci->logical_cores / ci->physical_cores : 1;

    {
        char path[256];
        char buf[64];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index0/size");
        if (read_file_str(path, buf, sizeof(buf)) == 0) ci->cache_l1d_kb = atoi(buf);
        for (int idx = 0; idx <= 4; idx++) {
            char lvl[8];
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%d/level", idx);
            if (read_file_str(path, lvl, sizeof(lvl)) < 0) break;
            if (atoi(lvl) == 3) {
                snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%d/size", idx);
                if (read_file_str(path, buf, sizeof(buf)) == 0) ci->cache_l3_kb = atoi(buf);
            }
        }
    }
}

int lulo_read_cpu_stat(CpuStat *s)
{
    FILE *f = fopen("/proc/stat", "r");
    char line[256];

    if (!f) return -1;
    s->ncpu = 0;
    while (fgets(line, sizeof(line), f)) {
        int idx = -1;
        if (strncmp(line, "cpu", 3) != 0) break;
        if (line[3] == ' ') idx = 0;
        else if (isdigit((unsigned char)line[3])) idx = atoi(line + 3) + 1;
        else continue;
        if (idx > LULO_MAX_CPUS) continue;
        {
            CpuTick *t = &s->cpu[idx];
            sscanf(line + (idx == 0 ? 4 : 3 + (idx < 10 ? 1 : idx < 100 ? 2 : 3) + 1),
                   "%llu %llu %llu %llu %llu %llu %llu %llu",
                   &t->user, &t->nice, &t->sys, &t->idle,
                   &t->iowait, &t->irq, &t->softirq, &t->steal);
        }
        if (idx > 0 && idx > s->ncpu) s->ncpu = idx;
    }
    fclose(f);
    return 0;
}

int lulo_cpu_pct(const CpuTick *a, const CpuTick *b)
{
    unsigned long long ia = a->idle + a->iowait;
    unsigned long long ib = b->idle + b->iowait;
    unsigned long long ta = a->user + a->nice + a->sys + ia + a->irq + a->softirq + a->steal;
    unsigned long long tb = b->user + b->nice + b->sys + ib + b->irq + b->softirq + b->steal;
    unsigned long long dt = tb > ta ? tb - ta : 0;
    unsigned long long di = ib > ia ? ib - ia : 0;
    return dt == 0 ? 0 : (int)(100ULL * (dt - di) / dt);
}

int lulo_cpu_heat_pct(const CpuTick *a, const CpuTick *b)
{
    unsigned long long ia = a->idle + a->iowait;
    unsigned long long ib = b->idle + b->iowait;
    unsigned long long ta = a->user + a->nice + a->sys + ia + a->irq + a->softirq + a->steal;
    unsigned long long tb = b->user + b->nice + b->sys + ib + b->irq + b->softirq + b->steal;
    unsigned long long dt = tb > ta ? tb - ta : 0;
    unsigned long long work_a = a->user + a->nice + a->sys + a->irq + a->softirq + a->steal;
    unsigned long long work_b = b->user + b->nice + b->sys + b->irq + b->softirq + b->steal;
    unsigned long long dw;

    if (dt == 0) return 0;
    dw = work_b > work_a ? work_b - work_a : 0;
    return (int)(100ULL * dw / dt);
}

unsigned long long lulo_cpu_total_delta(const CpuStat *a, const CpuStat *b)
{
    const CpuTick *ta = &a->cpu[0];
    const CpuTick *tb = &b->cpu[0];
    unsigned long long sa = ta->user + ta->nice + ta->sys + ta->idle + ta->iowait +
                            ta->irq + ta->softirq + ta->steal;
    unsigned long long sb = tb->user + tb->nice + tb->sys + tb->idle + tb->iowait +
                            tb->irq + tb->softirq + tb->steal;
    return sb > sa ? sb - sa : 0;
}

int lulo_gather_cpu_freq(CpuFreq *freqs, int max)
{
    int n = 0;
    for (int cpu = 0; cpu < max; cpu++) {
        char path[256];
        char buf[64];

        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
        if (read_file_str(path, buf, sizeof(buf)) < 0) break;
        freqs[n].cpu = cpu;
        freqs[n].cur_khz = atol(buf);

        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq", cpu);
        freqs[n].min_khz = read_file_str(path, buf, sizeof(buf)) == 0 ? atol(buf) : 0;
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", cpu);
        freqs[n].max_khz = read_file_str(path, buf, sizeof(buf)) == 0 ? atol(buf) : 0;
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpu);
        if (read_file_str(path, freqs[n].governor, sizeof(freqs[n].governor)) < 0) strcpy(freqs[n].governor, "-");
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_driver", cpu);
        if (read_file_str(path, freqs[n].driver, sizeof(freqs[n].driver)) < 0) strcpy(freqs[n].driver, "-");
        n++;
    }
    return n;
}

void lulo_gather_meminfo(MemInfo *m)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];

    memset(m, 0, sizeof(*m));
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long v;
        char key[64];
        if (sscanf(line, "%63s %llu", key, &v) != 2) continue;
        v *= 1024;
        if (!strcmp(key, "MemTotal:")) m->total = v;
        else if (!strcmp(key, "MemAvailable:")) m->available = v;
        else if (!strcmp(key, "Buffers:")) m->buffers = v;
        else if (!strcmp(key, "Cached:")) m->cached = v;
        else if (!strcmp(key, "Slab:")) m->slab = v;
        else if (!strcmp(key, "SwapTotal:")) m->swap_total = v;
        else if (!strcmp(key, "SwapFree:")) m->swap_free = v;
        else if (!strcmp(key, "Dirty:")) m->dirty = v;
        else if (!strcmp(key, "Writeback:")) m->writeback = v;
        else if (!strcmp(key, "HugePages_Total:")) m->hugepages_total = v / 1024;
        else if (!strcmp(key, "HugePages_Free:")) m->hugepages_free = v / 1024;
        else if (!strcmp(key, "Hugepagesize:")) m->hugepage_size_kb = v / 1024;
    }
    fclose(f);
}

void lulo_gather_loadavg(LoadInfo *li)
{
    FILE *f;
    char line[256];

    memset(li, 0, sizeof(*li));
    f = fopen("/proc/loadavg", "r");
    if (f) {
        int _r = fscanf(f, "%lf %lf %lf %d/%d", &li->load1, &li->load5, &li->load15, &li->running, &li->total);
        (void)_r;
        fclose(f);
    }
    f = fopen("/proc/stat", "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "ctxt ", 5)) li->ctxt = atol(line + 5);
        else if (!strncmp(line, "procs_blocked ", 14)) li->procs_blocked = atoi(line + 14);
    }
    fclose(f);
}

int lulo_read_cpu_temps(double *cpu_temps, int max)
{
    int ncpu_temp = 0;
    DIR *hd = opendir("/sys/class/hwmon");

    if (hd) {
        struct dirent *hde;
        while ((hde = readdir(hd))) {
            char namepath[128];
            char name[32];
            char base[128];

            if (hde->d_name[0] == '.') continue;
            if (snprintf(namepath, sizeof(namepath), "/sys/class/hwmon/%s/name", hde->d_name) >= (int)sizeof(namepath)) {
                continue;
            }
            if (read_file_str(namepath, name, sizeof(name)) < 0) continue;
            if (strcmp(name, "coretemp") != 0) continue;
            if (snprintf(base, sizeof(base), "/sys/class/hwmon/%s", hde->d_name) >= (int)sizeof(base)) {
                continue;
            }
            for (int t = 1; t <= 64 && ncpu_temp < max; t++) {
                char tpath[192];
                char tbuf[32];
                int mc;
                snprintf(tpath, sizeof(tpath), "%s/temp%d_input", base, t);
                if (read_file_str(tpath, tbuf, sizeof(tbuf)) < 0) break;
                mc = atoi(tbuf);
                if (mc > 0) cpu_temps[ncpu_temp++] = mc / 1000.0;
            }
            break;
        }
        closedir(hd);
    }
    if (ncpu_temp == 0 && max > 0) {
        for (int pass = 0; pass < 2 && !ncpu_temp; pass++) {
            for (int z = 0; z < 32; z++) {
                char path[128];
                char buf[32];
                char type[64];
                int match;

                snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", z);
                if (read_file_str(path, type, sizeof(type)) < 0) break;
                match = (pass == 0 && strstr(type, "x86_pkg")) || (pass == 1 && strstr(type, "pkg"));
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

double lulo_cpu_temp_for_row(const CpuInfo *ci, const double *cpu_temps, int ncpu_temp,
                             int cpu_idx, int logical_cpus)
{
    int phys_cores;
    int tpc;
    int temp_idx;

    if (ncpu_temp <= 0) return 0.0;
    if (ncpu_temp == 1) return cpu_temps[0];
    phys_cores = ncpu_temp - 1;
    tpc = ci->threads_per_core > 0 ? ci->threads_per_core :
          (phys_cores > 0 ? (logical_cpus + phys_cores - 1) / phys_cores : 1);
    if (tpc < 1) tpc = 1;
    temp_idx = cpu_idx / tpc + 1;
    if (temp_idx >= ncpu_temp) temp_idx = 0;
    return cpu_temps[temp_idx];
}

TopLayout lulo_build_layout(int tw, int th, int ncpu, int has_temp)
{
    TopLayout lo;
    int content_col;
    int content_w;
    int content_h;
    int band_row;
    int body_rows;
    int top_band_h;
    int max_cpu_rows;
    int fixed_w;

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

    lo.show_mem = content_w >= 30;
    lo.mem_stacked = 0;
    if (lo.show_mem && content_w >= 86) {
        lo.mem.width = clamp_int(content_w / 3, 30, 36);
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

    fixed_w = lo.cpu_label_w + 2 + 1 + 4 + 2 + 6;
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
    lo.cpu_hist_w = clamp_int(lo.cpu_hist_w, 6, LULO_MAX_TIMELINE);

    lo.load_row = lo.top.row + lo.top.height - 2;
    band_row = lo.top.row + 1;
    body_rows = lo.load_row - band_row;
    if (body_rows < 6) body_rows = 6;

    lo.mem_rows = lo.show_mem ? (lo.mem_stacked ? 5 : 7) : 0;
    lo.mem.height = lo.mem_rows > 0 ? lo.mem_rows + 2 : 0;
    lo.mem.row = lo.mem_stacked ? lo.cpu.row + lo.cpu_rows : band_row;

    max_cpu_rows = body_rows - 6;
    if (max_cpu_rows < 1) max_cpu_rows = 1;
    lo.cpu_rows = ncpu;
    if (lo.cpu_rows > max_cpu_rows) lo.cpu_rows = max_cpu_rows;
    if (lo.cpu_rows < 1) lo.cpu_rows = 1;
    if (lo.cpu_rows > ncpu) lo.cpu_rows = ncpu;

    for (;;) {
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
        if (body_rows - top_band_h >= 5) break;
        if (lo.mem_rows > 0) {
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
        } else if (lo.mem.height > top_band_h) {
            top_band_h = lo.mem.height;
        }
    }

    lo.hidden_cpus = ncpu > lo.cpu_rows ? ncpu - lo.cpu_rows : 0;
    lo.mem_bar_w = clamp_int(lo.mem.width - 26, 6, 18);
    lo.proc.row = band_row + top_band_h;
    lo.proc.col = lo.top.col + 1;
    lo.proc.width = lo.top.width - 2;
    lo.proc.height = lo.load_row - lo.proc.row;
    lo.show_proc = lo.proc.height >= 5;
    lo.proc_rows = lo.show_proc ? lo.proc.height - 3 : 0;
    lo.load.row = lo.load_row;
    lo.load.col = lo.top.col + 1;
    lo.load.height = 1;
    lo.load.width = lo.top.width - 2;
    return lo;
}

void lulo_format_proc_pct(char *buf, size_t len, int tenths)
{
    if (tenths < 1000) snprintf(buf, len, "%d.%d", tenths / 10, tenths % 10);
    else snprintf(buf, len, "%d", tenths / 10);
}

void lulo_format_proc_policy(char *buf, size_t len, int policy)
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

void lulo_format_proc_priority(char *buf, size_t len, int rt_priority, int priority)
{
    if (rt_priority > 0) snprintf(buf, len, "%d", rt_priority);
    else if (priority < 0) snprintf(buf, len, "%d", -priority - 1);
    else snprintf(buf, len, "%d", priority);
}

void lulo_format_proc_time(char *buf, size_t len, unsigned long long time_cs)
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
