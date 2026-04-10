/* dizk.c — clean disk usage display
 *
 * Build:  gcc -O2 -Wno-format-truncation -o dizk dizk.c
 * Usage:  ./dizk [-a] [-w WIDTH] [-n] [-m] [-h]
 *           -a          show all filesystems (incl. tmpfs, snap, etc.)
 *           -w WIDTH    bar width in columns (default: auto)
 *           -n          no color (plain text)
 *           -m          show mount options
 *           -h          help
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <mntent.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <termios.h>
#include <signal.h>
#include <poll.h>

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
static const Colors color_off = {
    "", "", "", "", "", "", "", "", "", "", "", "", "", "", ""
};
static const Colors *C = &color_on;

/* ── thresholds ──────────────────────────────────────────────── */

#define THRESH_WARN  75
#define THRESH_CRIT  90

/* ── helpers ─────────────────────────────────────────────────── */

static int term_width(void)
{
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
        return w.ws_col;
    const char *cols = getenv("COLUMNS");
    return cols ? atoi(cols) : 120;
}

static void fmt_size(char *buf, size_t len, unsigned long long bytes)
{
    const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    double val = (double)bytes;
    int i = 0;
    while (val >= 1024.0 && i < 5) { val /= 1024.0; i++; }
    if      (i == 0)       snprintf(buf, len, "%llu B",  bytes);
    else if (val >= 100.0) snprintf(buf, len, "%.0f %s", val, units[i]);
    else if (val >= 10.0)  snprintf(buf, len, "%.1f %s", val, units[i]);
    else                   snprintf(buf, len, "%.2f %s", val, units[i]);
}

static void hr(const char *ch, const char *color, int width)
{
    printf("%s", color);
    for (int i = 0; i < width; i++) printf("%s", ch);
    printf("%s\n", C->reset);
}

static void trunc_str(const char *src, char *dst, int maxw)
{
    int slen = (int)strlen(src);
    if (slen <= maxw) snprintf(dst, maxw + 1, "%s", src);
    else              snprintf(dst, maxw + 1, "…%s", src + slen - (maxw - 1));
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static const char *bar_bg(int pct)
{
    return (pct >= THRESH_CRIT) ? C->bg_crit :
           (pct >= THRESH_WARN) ? C->bg_warn : C->bg_ok;
}
static const char *pct_color(int pct)
{
    return (pct >= THRESH_CRIT) ? C->red    :
           (pct >= THRESH_WARN) ? C->orange :
           (pct >= 50)          ? C->yellow : C->green;
}

/* ── filesystem filtering ────────────────────────────────────── */

static const char *skip_types[] = {
    "sysfs","proc","devtmpfs","devpts","securityfs","cgroup","cgroup2",
    "pstore","efivarfs","bpf","debugfs","tracefs","hugetlbfs","mqueue",
    "configfs","fusectl","ramfs","binfmt_misc","nsfs","overlay","squashfs",
    "fuse.snapfuse","fuse.portal","autofs","rpc_pipefs","nfsd", NULL
};
static const char *skip_prefixes[] = {
    "/sys","/proc","/dev/pts","/run/user","/run/credentials",
    "/run/snapd","/snap", NULL
};

static int should_skip(const struct mntent *me, int show_all)
{
    if (show_all) return 0;
    for (int i = 0; skip_types[i]; i++)
        if (!strcmp(me->mnt_type, skip_types[i])) return 1;
    if (!strcmp(me->mnt_type, "tmpfs") &&
        strcmp(me->mnt_dir, "/tmp") && strcmp(me->mnt_dir, "/dev/shm"))
        return 1;
    for (int i = 0; skip_prefixes[i]; i++)
        if (!strncmp(me->mnt_dir, skip_prefixes[i], strlen(skip_prefixes[i])))
            return 1;
    return 0;
}

/* ── mount options: keep only interesting ones ───────────────── */

static void extract_mount_opts(const char *raw, char *out, size_t len)
{
    static const char *keep[] = {
        "ro","rw","noatime","relatime","nosuid","nodev","noexec","discard",
        "compress","subvol","space_cache","ssd","errors=","data=","commit=",
        "barrier=","user_xattr","acl","noacl","quota","usrquota","grpquota",
        "nofail","defaults","x-systemd", NULL
    };
    out[0] = '\0';
    size_t pos = 0;
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", raw);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        for (int i = 0; keep[i]; i++) {
            if (!strncmp(tok, keep[i], strlen(keep[i]))) {
                if (pos > 0 && pos + 1 < len) out[pos++] = ',';
                size_t tl = strlen(tok);
                if (pos + tl < len) { memcpy(out + pos, tok, tl); pos += tl; }
                break;
            }
        }
    }
    out[pos] = '\0';
}

/* ── resolve UUID=/LABEL= → /dev/sdX ────────────────────────── */

static void resolve_fstab_dev(const char *spec, char *out, size_t outlen)
{
    /* If it's already a /dev/ path, use as-is */
    if (!strncmp(spec, "/dev/", 5)) {
        snprintf(out, outlen, "%s", spec);
        return;
    }

    const char *prefix = NULL;
    const char *val    = NULL;
    char dir[64];

    if (!strncmp(spec, "UUID=", 5)) {
        prefix = "/dev/disk/by-uuid/";
        val    = spec + 5;
    } else if (!strncmp(spec, "LABEL=", 6)) {
        prefix = "/dev/disk/by-label/";
        val    = spec + 6;
    } else if (!strncmp(spec, "PARTLABEL=", 10)) {
        prefix = "/dev/disk/by-partlabel/";
        val    = spec + 10;
    } else {
        snprintf(out, outlen, "%s", spec);
        return;
    }

    char link[512];
    snprintf(link, sizeof(link), "%s%s", prefix, val);

    char target[512];
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n <= 0) {
        /* fallback: just show last 12 chars of UUID or full label */
        snprintf(out, outlen, "%s", spec);
        return;
    }
    target[n] = '\0';

    /* target is like ../../sda1 — get basename then resolve to /dev/ */
    char *base = strrchr(target, '/');
    snprintf(out, outlen, "/dev/%.*s",
             (int)(outlen - 6), base ? base + 1 : target);
    (void)dir;
}

/* ── data types ──────────────────────────────────────────────── */

typedef struct {
    char device[256], mount[256], fstype[64], opts[512];
    unsigned long long total, used, avail;
    int pct;
} FsEntry;

static int cmp_fs(const void *a, const void *b)
{ return strcmp(((FsEntry *)a)->mount, ((FsEntry *)b)->mount); }

typedef struct {
    char name[64], size[32], model[128], transport[32];
    int  rotational;
} BlkDev;

typedef struct {
    char   name[64];
    /* raw counters from /proc/diskstats */
    unsigned long long rd_ios, rd_sectors, rd_ms;
    unsigned long long wr_ios, wr_sectors, wr_ms;
    unsigned long long io_ms;          /* time doing I/O (ms) */
    unsigned long long discard_ios;
    unsigned long long flush_ios;
    /* derived */
    char   rd_bytes[32], wr_bytes[32]; /* human-readable totals */
    double util_pct;                   /* io_ms / uptime_ms * 100 */
} DiskStat;

typedef struct {
    char spec[256];     /* original fstab spec (UUID=..., LABEL=..., /dev/...) */
    char device[256];   /* resolved /dev/ path */
    char mount[256];
    char fstype[64];
    char opts[256];     /* filtered options */
    int  dump, pass;
} FstabEntry;

/* ── bar row ─────────────────────────────────────────────────── */

typedef struct {
    char prefix[512];
    char suffix[128];
    int  pct;
    int  term_row;      /* 1-based row after screen clear */
} BarRow;

/* ── /proc/diskstats ─────────────────────────────────────────── */

static int gather_diskstats(DiskStat *stats, int max)
{
    /* uptime in ms for utilisation calculation */
    double uptime_ms = 0;
    {
        FILE *f = fopen("/proc/uptime", "r");
        if (f) { double up; if (fscanf(f, "%lf", &up) == 1) uptime_ms = up * 1000.0; fclose(f); }
    }

    FILE *f = fopen("/proc/diskstats", "r");
    if (!f) return 0;

    int n = 0;
    char line[512];
    while (fgets(line, sizeof(line), f) && n < max) {
        unsigned int maj, min;
        char name[64];
        unsigned long long rd_ios, rd_merges, rd_sectors, rd_ms;
        unsigned long long wr_ios, wr_merges, wr_sectors, wr_ms;
        unsigned long long cur_ios, io_ms, wt_ms;
        unsigned long long dis_ios, dis_merges, dis_sectors, dis_ms;
        unsigned long long flush_ios, flush_ms;

        int r = sscanf(line,
            "%u %u %63s "
            "%llu %llu %llu %llu "
            "%llu %llu %llu %llu "
            "%llu %llu %llu "
            "%llu %llu %llu %llu "
            "%llu %llu",
            &maj, &min, name,
            &rd_ios, &rd_merges, &rd_sectors, &rd_ms,
            &wr_ios, &wr_merges, &wr_sectors, &wr_ms,
            &cur_ios, &io_ms, &wt_ms,
            &dis_ios, &dis_merges, &dis_sectors, &dis_ms,
            &flush_ios, &flush_ms);

        if (r < 14) continue;

        /* Skip partitions and unwanted devices.
         * NVMe uses nvmeXnY (disk) vs nvmeXnYpZ (partition) — don't
         * filter on trailing digit alone; check for nvme + 'p' suffix. */
        int nlen = strlen(name);
        if (!strncmp(name, "loop", 4) || !strncmp(name, "ram",  3) ||
            !strncmp(name, "dm-",  3) || !strncmp(name, "zram", 4)) continue;

        /* For nvme: partition names contain 'p' before trailing digits */
        if (!strncmp(name, "nvme", 4)) {
            /* find last 'p' — if it's followed only by digits, it's a partition */
            char *p = strrchr(name, 'p');
            if (p && p > name) {
                int all_digits = 1;
                for (char *q = p + 1; *q; q++)
                    if (!isdigit((unsigned char)*q)) { all_digits = 0; break; }
                if (all_digits && *(p+1) != '\0') continue;
            }
        } else {
            /* For sd/vd/mmcblk etc: trailing digit = partition */
            if (nlen > 0 && isdigit((unsigned char)name[nlen-1])) continue;
        }

        DiskStat *ds = &stats[n];
        snprintf(ds->name, sizeof(ds->name), "%s", name);
        ds->rd_ios     = rd_ios;
        ds->rd_sectors = rd_sectors;
        ds->rd_ms      = rd_ms;
        ds->wr_ios     = wr_ios;
        ds->wr_sectors = wr_sectors;
        ds->wr_ms      = wr_ms;
        ds->io_ms      = io_ms;
        ds->discard_ios = (r >= 15) ? dis_ios   : 0;
        ds->flush_ios   = (r >= 19) ? flush_ios : 0;

        fmt_size(ds->rd_bytes, sizeof(ds->rd_bytes), rd_sectors  * 512ULL);
        fmt_size(ds->wr_bytes, sizeof(ds->wr_bytes), wr_sectors  * 512ULL);
        ds->util_pct = (uptime_ms > 0)
                       ? ((double)io_ms / uptime_ms) * 100.0 : 0.0;
        if (ds->util_pct > 100.0) ds->util_pct = 100.0;
        n++;
    }
    fclose(f);
    return n;
}

/* ── block devices ───────────────────────────────────────────── */

static int read_sysfs_str(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = (fgets(buf, (int)len, f) != NULL);
    fclose(f);
    if (!ok) return -1;
    char *end = buf + strlen(buf) - 1;
    while (end >= buf && isspace((unsigned char)*end)) *end-- = '\0';
    return 0;
}

static int gather_block_devices(BlkDev *devs, int max)
{
    DIR *d = opendir("/sys/block");
    if (!d) return 0;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) && n < max) {
        if (de->d_name[0] == '.') continue;
        if (!strncmp(de->d_name, "loop", 4) || !strncmp(de->d_name, "ram",  3) ||
            !strncmp(de->d_name, "dm-",  3) || !strncmp(de->d_name, "zram", 4))
            continue;

        BlkDev *bd = &devs[n];
        snprintf(bd->name, sizeof(bd->name), "%.*s",
                 (int)(sizeof(bd->name) - 1), de->d_name);

        char path[512], sbuf[32];
        snprintf(path, sizeof(path), "/sys/block/%s/size", de->d_name);
        if (read_sysfs_str(path, sbuf, sizeof(sbuf)) == 0)
            fmt_size(bd->size, sizeof(bd->size), strtoull(sbuf, NULL, 10) * 512ULL);
        else
            snprintf(bd->size, sizeof(bd->size), "?");

        snprintf(path, sizeof(path), "/sys/block/%s/queue/rotational", de->d_name);
        char rbuf[8] = "0";
        read_sysfs_str(path, rbuf, sizeof(rbuf));
        bd->rotational = atoi(rbuf);

        snprintf(path, sizeof(path), "/sys/block/%s/device/model", de->d_name);
        if (read_sysfs_str(path, bd->model, sizeof(bd->model)) < 0)
            bd->model[0] = '\0';

        bd->transport[0] = '\0';
        if      (!strncmp(de->d_name, "nvme",  4)) strcpy(bd->transport, "nvme");
        else if (!strncmp(de->d_name, "sd",    2)) strcpy(bd->transport, "sata");
        else if (!strncmp(de->d_name, "mmcblk",6)) strcpy(bd->transport, "mmc");
        else if (!strncmp(de->d_name, "vd",    2)) strcpy(bd->transport, "virtio");
        n++;
    }
    closedir(d);
    return n;
}

/* ── fstab parsing ───────────────────────────────────────────── */

static int gather_fstab(FstabEntry *entries, int max)
{
    FILE *f = setmntent("/etc/fstab", "r");
    if (!f) return 0;
    int n = 0;
    struct mntent *me;
    while ((me = getmntent(f)) && n < max) {
        /* skip pseudo/virtual */
        if (!strcmp(me->mnt_type, "proc")    ||
            !strcmp(me->mnt_type, "sysfs")   ||
            !strcmp(me->mnt_type, "devpts")  ||
            !strcmp(me->mnt_type, "tmpfs")   ||
            !strcmp(me->mnt_type, "hugetlbfs")) {
            /* keep tmpfs only if it's /tmp or /dev/shm */
            if (!strcmp(me->mnt_type, "tmpfs") &&
                (!strcmp(me->mnt_dir, "/tmp") || !strcmp(me->mnt_dir, "/dev/shm")))
                goto keep;
            continue;
        }
    keep:;
        FstabEntry *e = &entries[n];
        snprintf(e->spec,   sizeof(e->spec),   "%s", me->mnt_fsname);
        snprintf(e->mount,  sizeof(e->mount),  "%s", me->mnt_dir);
        snprintf(e->fstype, sizeof(e->fstype), "%s", me->mnt_type);
        e->dump = me->mnt_freq;
        e->pass = me->mnt_passno;
        resolve_fstab_dev(me->mnt_fsname, e->device, sizeof(e->device));
        extract_mount_opts(me->mnt_opts, e->opts, sizeof(e->opts));
        n++;
    }
    endmntent(f);
    return n;
}

/* ── kernel queue tunables ───────────────────────────────────── */

typedef struct {
    char name[64];
    char scheduler[64];     /* /sys/block/X/queue/scheduler  */
    int  read_ahead_kb;     /* /sys/block/X/queue/read_ahead_kb */
    int  nr_requests;       /* /sys/block/X/queue/nr_requests */
    int  max_sectors_kb;    /* /sys/block/X/queue/max_sectors_kb */
    int  rotational;        /* 0=SSD 1=HDD */
    int  write_cache;       /* /sys/block/X/queue/write_cache: 1=enabled */
    int  nomerges;          /* /sys/block/X/queue/nomerges */
    int  rq_affinity;       /* /sys/block/X/queue/rq_affinity */
    char wbt_lat[32];       /* /sys/block/X/queue/wbt_lat_usec */
    /* NVMe-specific (may be absent) */
    char power_state[32];   /* /sys/block/X/device/power_state or /sys/class/nvme/nvme0/power_state */
    char numa_node[8];      /* /sys/block/X/device/numa_node */
} KTunable;

static int gather_ktunables(KTunable *kt, int max)
{
    DIR *d = opendir("/sys/block");
    if (!d) return 0;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) && n < max) {
        if (de->d_name[0] == '.') continue;
        if (!strncmp(de->d_name, "loop", 4) || !strncmp(de->d_name, "ram",  3) ||
            !strncmp(de->d_name, "dm-",  3) || !strncmp(de->d_name, "zram", 4))
            continue;

        /* skip nvme partitions */
        const char *dn = de->d_name;
        int nlen = strlen(dn);
        if (!strncmp(dn, "nvme", 4)) {
            char *p = strrchr((char*)dn, 'p');
            if (p && p > dn) {
                int ad = 1;
                for (char *q = p+1; *q; q++) if (!isdigit((unsigned char)*q)) { ad=0; break; }
                if (ad && *(p+1)) continue;
            }
        } else if (nlen > 0 && isdigit((unsigned char)dn[nlen-1])) continue;

        KTunable *t = &kt[n];
        snprintf(t->name, sizeof(t->name), "%.*s", (int)(sizeof(t->name)-1), dn);

        char path[512], buf[256];

        /* scheduler: extract [active] from list */
        snprintf(path, sizeof(path), "/sys/block/%s/queue/scheduler", dn);
        if (read_sysfs_str(path, buf, sizeof(buf)) == 0) {
            /* find [xxx] */
            char *lb = strchr(buf, '['), *rb = lb ? strchr(lb, ']') : NULL;
            if (lb && rb) {
                int len = (int)(rb - lb - 1);
                if (len >= (int)sizeof(t->scheduler)) len = sizeof(t->scheduler)-1;
                memcpy(t->scheduler, lb+1, len);
                t->scheduler[len] = '\0';
            } else snprintf(t->scheduler, sizeof(t->scheduler), "%s", buf);
        } else snprintf(t->scheduler, sizeof(t->scheduler), "-");

        #define RD_INT(field, file) do { \
            snprintf(path, sizeof(path), "/sys/block/%s/queue/" file, dn); \
            t->field = (read_sysfs_str(path, buf, sizeof(buf)) == 0) ? atoi(buf) : -1; \
        } while(0)

        RD_INT(read_ahead_kb,  "read_ahead_kb");
        RD_INT(nr_requests,    "nr_requests");
        RD_INT(max_sectors_kb, "max_sectors_kb");
        RD_INT(rotational,     "rotational");
        RD_INT(nomerges,       "nomerges");
        RD_INT(rq_affinity,    "rq_affinity");

        /* write_cache: value is "write back" or "write through" */
        snprintf(path, sizeof(path), "/sys/block/%s/queue/write_cache", dn);
        t->write_cache = -1;
        if (read_sysfs_str(path, buf, sizeof(buf)) == 0)
            t->write_cache = !strncmp(buf, "write back", 10) ? 1 : 0;

        /* wbt_lat_usec */
        snprintf(path, sizeof(path), "/sys/block/%s/queue/wbt_lat_usec", dn);
        t->wbt_lat[0] = '\0';
        if (read_sysfs_str(path, buf, sizeof(buf)) == 0)
            snprintf(t->wbt_lat, sizeof(t->wbt_lat), "%s", buf);

        /* NVMe: power state from /sys/class/nvme/nvmeX/power_state */
        t->power_state[0] = '\0';
        if (!strncmp(dn, "nvme", 4)) {
            snprintf(path, sizeof(path), "/sys/class/nvme/%s/power_state", dn);
            if (read_sysfs_str(path, buf, sizeof(buf)) == 0)
                snprintf(t->power_state, sizeof(t->power_state), "%s", buf);
        }

        /* numa_node */
        snprintf(path, sizeof(path), "/sys/block/%s/device/numa_node", dn);
        t->numa_node[0] = '\0';
        if (read_sysfs_str(path, buf, sizeof(buf)) == 0)
            snprintf(t->numa_node, sizeof(t->numa_node), "%s", buf);

        #undef RD_INT
        n++;
    }
    closedir(d);
    return n;
}

/* ── swap info ───────────────────────────────────────────────── */

typedef struct {
    char filename[256];
    char type[16];
    unsigned long long size, used;
    int  priority;
} SwapEntry;

static int gather_swap(SwapEntry *sw, int max)
{
    FILE *f = fopen("/proc/swaps", "r");
    if (!f) return 0;
    char line[512];
    /* skip header */
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }
    int n = 0;
    while (fgets(line, sizeof(line), f) && n < max) {
        SwapEntry *e = &sw[n];
        unsigned long long sz, used;
        int prio;
        char type[32];
        if (sscanf(line, "%255s %31s %llu %llu %d",
                   e->filename, type, &sz, &used, &prio) == 5) {
            snprintf(e->type, sizeof(e->type), "%s", type);
            e->size     = sz * 1024ULL;
            e->used     = used * 1024ULL;
            e->priority = prio;
            n++;
        }
    }
    fclose(f);
    return n;
}

/* ── bar drawing ─────────────────────────────────────────────── */

static void print_bar_initial(int pct, int bar_width)
{
    /* full free/gray bar; used portion animated in later */
    printf("%s", C->bg_free);
    for (int i = 0; i < bar_width; i++) putchar(' ');
    printf("%s", C->reset);
    printf(" %s%s%3d%%%s", pct_color(pct), C->bold, pct, C->reset);
}

static void repaint_used(const BarRow *r, int u, int bar_width)
{
    printf("\033[%d;1H", r->term_row);
    printf("%s", r->prefix);
    if (u > 0) {
        printf("%s%s", bar_bg(r->pct), C->white);
        for (int i = 0; i < u; i++) putchar(' ');
        printf("%s", C->reset);
    }
    int free_cells = bar_width - u;
    if (free_cells > 0)
        printf("\033[%dC", free_cells);
}

static int read_key_with_escape_handling(void)
{
    unsigned char c;
    for (;;) {
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n != 1) return -1;

        if (c == 3) return 3; /* Ctrl+C */
        if (c == 'q' || c == 'Q' || c == '\r' || c == '\n') return c;

        if (c != 27) continue; /* ignore random keys */

        /* ESC may be standalone or the start of an escape sequence. */
        struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
        int pr = poll(&pfd, 1, 35);
        if (pr <= 0 || !(pfd.revents & POLLIN)) return 27; /* standalone ESC */

        /* Drain the rest of the queued escape sequence and ignore it. */
        do {
            unsigned char junk[64];
            ssize_t r = read(STDIN_FILENO, junk, sizeof(junk));
            if (r <= 0) break;
        } while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN));
    }
}

/* ── main ────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    int show_all = 0, bar_width = 0, no_color = 0, show_opts = 0;
    int opt;
    signal(SIGINT,  sig_exit);
    signal(SIGTERM, sig_exit);
    while ((opt = getopt(argc, argv, "aw:nmh")) != -1) {
        switch (opt) {
        case 'a': show_all  = 1;           break;
        case 'w': bar_width = atoi(optarg); break;
        case 'n': no_color  = 1;           break;
        case 'm': show_opts = 1;           break;
        case 'h':
            fprintf(stderr,
                "Usage: dizk [-a] [-w WIDTH] [-n] [-m] [-h]\n"
                "  -a  show all filesystems\n"
                "  -w  bar width (default: auto)\n"
                "  -n  no color\n"
                "  -m  show mount options\n"
                "  -h  this help\n");
            return 0;
        }
    }

    int animate = isatty(STDOUT_FILENO) && isatty(STDIN_FILENO);

    if (no_color || !isatty(STDOUT_FILENO))
        C = &color_off;

    int tw = term_width();
    if (bar_width <= 0) bar_width = tw - 86;
    if (bar_width < 10) bar_width = 10;
    if (bar_width > 80) bar_width = 80;

    /* Enter alternate screen buffer — clean slate, no scroll issues.
     * Cursor starts at (1,1) guaranteed. */
    if (animate) {
        printf("\033[?1049h\033[H");
        fflush(stdout);
    }

    int cur_row = 1;

    /* ── header ── */
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0)
        snprintf(hostname, sizeof(hostname), "localhost");
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%a %d %b %Y  %H:%M:%S",
             localtime(&(time_t){time(NULL)}));

    hr("═", C->cyan, tw);                  cur_row++;
    printf("  %s%sDIZK%s  %s  ·  %s%s\n",
           C->cyan, C->bold, C->reset,
           hostname, timebuf, C->reset);              cur_row++;
    hr("═", C->cyan, tw);                  cur_row++;

    printf("  %s%s%-18s %-18s %-6s  %-*s   %-8s %-8s %-8s%s\n",
           C->bold, C->white,
           "DEVICE", "MOUNT", "TYPE",
           bar_width + 5, "USED / FREE",
           "USED", "FREE", "TOTAL",
           C->reset);                       cur_row++;
    printf("\n");                           cur_row++;

    /* ── gather filesystems ── */
    FsEntry entries[128];
    int count = 0;
    FILE *mtab = setmntent("/etc/mtab", "r");
    if (!mtab) mtab = setmntent("/proc/mounts", "r");
    if (!mtab) { perror("setmntent"); return 1; }

    char seen_dev[128][256];
    int seen_count = 0;
    struct mntent *me;
    while ((me = getmntent(mtab)) && count < 128) {
        if (should_skip(me, show_all)) continue;
        int dup = 0;
        for (int i = 0; i < seen_count; i++)
            if (!strcmp(seen_dev[i], me->mnt_fsname)) { dup = 1; break; }
        if (dup) continue;
        if (seen_count < 128)
            snprintf(seen_dev[seen_count++], 256, "%s", me->mnt_fsname);

        struct statvfs st;
        if (statvfs(me->mnt_dir, &st) != 0 || st.f_blocks == 0) continue;

        FsEntry *e = &entries[count];
        snprintf(e->device, sizeof(e->device), "%s", me->mnt_fsname);
        snprintf(e->mount,  sizeof(e->mount),  "%s", me->mnt_dir);
        snprintf(e->fstype, sizeof(e->fstype), "%s", me->mnt_type);
        extract_mount_opts(me->mnt_opts, e->opts, sizeof(e->opts));

        unsigned long long frag = st.f_frsize;
        e->total = st.f_blocks * frag;
        e->avail = st.f_bavail * frag;
        e->used  = e->total - st.f_bfree * frag;
        e->pct   = e->total > 0 ? (int)((e->used * 100ULL) / e->total) : 0;
        if (e->pct > 100) e->pct = 100;
        count++;
    }
    endmntent(mtab);
    qsort(entries, count, sizeof(FsEntry), cmp_fs);

    /* ── print fs rows ── */
    BarRow *rows = calloc(count + 1, sizeof(BarRow));
    int anim_count = 0;
    unsigned long long grand_total = 0, grand_used = 0, grand_avail = 0;

    for (int i = 0; i < count; i++) {
        FsEntry *e = &entries[i];
        char s_total[32], s_used[32], s_free[32];
        fmt_size(s_total, sizeof(s_total), e->total);
        fmt_size(s_used,  sizeof(s_used),  e->used);
        fmt_size(s_free,  sizeof(s_free),  e->avail);

        const char *mc = (e->pct >= THRESH_CRIT) ? C->red :
                         (e->pct >= THRESH_WARN) ? C->orange : C->cyan;
        char dev_disp[20], mnt_disp[20];
        trunc_str(e->device, dev_disp, 18);
        trunc_str(e->mount,  mnt_disp, 18);

        BarRow *r = &rows[i];
        snprintf(r->prefix, sizeof(r->prefix),
                 "  %s%-18s%s %s%-18s%s %s%-6s%s  ",
                 C->green, dev_disp, C->reset,
                 mc, mnt_disp, C->reset,
                 C->white, e->fstype, C->reset);
        snprintf(r->suffix, sizeof(r->suffix),
                 "   %s%-8s%s %s%-8s%s %s%-8s%s",
                 C->white, s_used, C->reset,
                 C->green, s_free, C->reset,
                 C->white, s_total, C->reset);
        r->pct      = e->pct;
        r->term_row = cur_row;              /* record before printing */

        printf("%s", r->prefix);
        print_bar_initial(e->pct, bar_width);
        printf("%s\n", r->suffix);          cur_row++;

        if (show_opts && e->opts[0]) {
            printf("  %s└ %s%s\n", C->white, e->opts, C->reset);
            cur_row++;
        }

        grand_total += e->total;
        grand_used  += e->used;
        grand_avail += e->avail;
    }
    anim_count = count;

    /* ── totals row ── */
    if (count > 1) {
        printf("\n");                       cur_row++;
        hr("─", C->gray, tw);              cur_row++;

        char s_total[32], s_used[32], s_free[32];
        fmt_size(s_total, sizeof(s_total), grand_total);
        fmt_size(s_used,  sizeof(s_used),  grand_used);
        fmt_size(s_free,  sizeof(s_free),  grand_avail);
        int grand_pct = grand_total > 0
                        ? (int)((grand_used * 100ULL) / grand_total) : 0;

        BarRow *r = &rows[anim_count];
        snprintf(r->prefix, sizeof(r->prefix),
                 "  %-18s %s%s%-18s%s %-6s  ",
                 "",
                 C->white, C->bold, "TOTAL", C->reset,
                 "");
        snprintf(r->suffix, sizeof(r->suffix),
                 "   %s%s%-8s%s %s%-8s%s %s%-8s%s",
                 C->bold, C->white, s_used, C->reset,
                 C->green, s_free, C->reset,
                 C->white, s_total, C->reset);
        r->pct      = grand_pct;
        r->term_row = cur_row;

        printf("%s", r->prefix);
        print_bar_initial(grand_pct, bar_width);
        printf("%s\n", r->suffix);         cur_row++;
        anim_count++;
    }

    printf("\n");                           cur_row++;

    /* ── block devices ── */
    hr("─", C->gray, tw);                  cur_row++;
    printf("\n  %s%sBLOCK DEVICES%s\n\n",
           C->bold, C->white, C->reset);   cur_row += 3;

    BlkDev devs[32];
    int ndev = gather_block_devices(devs, 32);
    if (ndev == 0) {
        printf("  %s(no block devices found)%s\n", C->white, C->reset);
        cur_row++;
    } else {
        for (int i = 0; i < ndev; i++) {
            BlkDev *bd = &devs[i];
            const char *dtype_c = bd->rotational ? C->yellow : C->cyan;
            const char *dtype   = bd->rotational ? "HDD" : "SSD";
            const char *tran_c  = !strcmp(bd->transport, "nvme") ? C->cyan : C->white;
            printf("  %s/dev/%-12s%s  %s%-8s%s  %s%s%s  %s%-6s%s",
                   C->green, bd->name, C->reset,
                   C->white, bd->size, C->reset,
                   dtype_c, dtype, C->reset,
                   tran_c, bd->transport[0] ? bd->transport : "-", C->reset);
            if (bd->model[0]) printf("  %s%s%s", C->white, bd->model, C->reset);
            printf("\n");                   cur_row++;
        }
    }

    /* ── fstab section ── */
    printf("\n");                           cur_row++;
    hr("─", C->gray, tw);                  cur_row++;
    printf("\n  %s%sFSTAB%s\n\n",
           C->bold, C->white, C->reset);   cur_row += 3;

    FstabEntry ftab[64];
    int nftab = gather_fstab(ftab, 64);

    if (nftab == 0) {
        printf("  %s(could not read /etc/fstab)%s\n", C->white, C->reset);
        cur_row++;
    } else {
        /*
         * Columns: DEVICE(20) MOUNT(20) TYPE(6) OPTIONS(fill) d p
         * Options width = tw - 2(indent) - 20 - 1 - 20 - 1 - 6 - 2 - 4(dp) = tw - 56
         */
        int opts_w = tw - 56;
        if (opts_w < 20) opts_w = 20;
        if (opts_w > 60) opts_w = 60;

        printf("  %s%s%-20s %-20s %-6s  %-*s  d p%s\n",
               C->bold, C->white,
               "DEVICE", "MOUNT", "TYPE", opts_w, "OPTIONS", C->reset);
        printf("\n");                       cur_row += 2;

        for (int i = 0; i < nftab; i++) {
            FstabEntry *e = &ftab[i];

            int resolved = !strncmp(e->device, "/dev/", 5);
            char dev_disp[22], mnt_disp[22];
            char opts_disp[64];
            trunc_str(resolved ? e->device : e->spec, dev_disp, 20);
            trunc_str(e->mount,                       mnt_disp, 20);
            trunc_str(e->opts[0] ? e->opts : "-",     opts_disp, opts_w);

            const char *fc = !strcmp(e->fstype, "swap")  ? C->yellow :
                             !strcmp(e->fstype, "ext4")  ? C->cyan   :
                             !strcmp(e->fstype, "vfat")  ? C->orange :
                             !strcmp(e->fstype, "btrfs") ? C->green  : C->white;
            const char *dc = C->green;

            printf("  %s%-20s%s %s%-20s%s %s%-6s%s  %s%-*s%s  %s%d %d%s\n",
                   dc, dev_disp,      C->reset,
                   C->white, mnt_disp, C->reset,
                   fc, e->fstype,     C->reset,
                   C->white, opts_w, opts_disp, C->reset,
                   C->white, e->dump, e->pass, C->reset);
            cur_row++;

            if (!resolved) {
                const char *eq  = strchr(e->spec, '=');
                const char *val = eq ? eq + 1 : e->spec;
                char vt[22]; trunc_str(val, vt, 20);
                printf("  %s  └ %-18s%s\n", C->white, vt, C->reset);
                cur_row++;
            }
        }
    }

    /* ── I/O stats ── */
    DiskStat dstats[32];
    int ndstat = gather_diskstats(dstats, 32);

    if (ndstat > 0) {
        printf("\n");                           cur_row++;
        hr("─", C->gray, tw);                  cur_row++;
        printf("\n  %s%sI/O STATS%s  (cumulative since boot)\n\n",
               C->bold, C->white, C->reset);
        cur_row += 3;

        printf("  %s%s%-12s  %-10s %-10s  %-8s %-8s  %-8s %-8s  %s%-6s%s%s\n",
               C->bold, C->white,
               "DEVICE", "READS", "WRITES",
               "RD DATA", "WR DATA",
               "RD TIME", "WR TIME",
               C->white, "UTIL", C->reset, C->reset);
        printf("\n");                           cur_row += 2;

        for (int i = 0; i < ndstat; i++) {
            DiskStat *ds = &dstats[i];

            /* format rd/wr time as s or ms */
            char rd_t[16], wr_t[16];
            if (ds->rd_ms >= 1000)
                snprintf(rd_t, sizeof(rd_t), "%.1fs", ds->rd_ms / 1000.0);
            else
                snprintf(rd_t, sizeof(rd_t), "%llums", ds->rd_ms);
            if (ds->wr_ms >= 1000)
                snprintf(wr_t, sizeof(wr_t), "%.1fs", ds->wr_ms / 1000.0);
            else
                snprintf(wr_t, sizeof(wr_t), "%llums", ds->wr_ms);

            /* util bar: 8 chars wide */
            int ub = (int)(ds->util_pct / 100.0 * 8.0);
            const char *uc = ds->util_pct >= 80 ? C->red    :
                             ds->util_pct >= 40 ? C->yellow : C->green;

            char util_bar[64];
            int bp = 0;
            /* colored filled portion */
            if (ub > 0) {
                bp += snprintf(util_bar + bp, sizeof(util_bar) - bp,
                               "%s%s", bar_bg((int)ds->util_pct), C->white);
                for (int j = 0; j < ub; j++)
                    util_bar[bp++] = ' ';
                bp += snprintf(util_bar + bp, sizeof(util_bar) - bp,
                               "%s", C->reset);
            }
            /* dim remainder */
            if (ub < 8) {
                bp += snprintf(util_bar + bp, sizeof(util_bar) - bp,
                               "%s", C->bg_free);
                for (int j = ub; j < 8; j++)
                    util_bar[bp++] = ' ';
                bp += snprintf(util_bar + bp, sizeof(util_bar) - bp,
                               "%s", C->reset);
            }
            util_bar[bp] = '\0';

            printf("  %s%-12s%s  %s%-10llu%s %s%-10llu%s  "
                   "%s%-8s%s %s%-8s%s  "
                   "%s%-8s%s %s%-8s%s  "
                   "%s %s%.1f%%%s\n",
                   C->green, ds->name, C->reset,
                   C->white, ds->rd_ios, C->reset,
                   C->white, ds->wr_ios, C->reset,
                   C->cyan,  ds->rd_bytes, C->reset,
                   C->green, ds->wr_bytes, C->reset,
                   C->white, rd_t, C->reset,
                   C->white, wr_t, C->reset,
                   util_bar,
                   uc, ds->util_pct, C->reset);
            cur_row++;
        }
    }

    /* ── kernel queue tunables ── */
    KTunable ktunables[32];
    int nkt = gather_ktunables(ktunables, 32);

    if (nkt > 0) {
        printf("\n");                           cur_row++;
        hr("─", C->gray, tw);                  cur_row++;
        printf("\n  %s%sKERNEL TUNABLES%s\n\n",
               C->bold, C->white, C->reset);
        cur_row += 3;

        for (int i = 0; i < nkt; i++) {
            KTunable *t = &ktunables[i];

            /* Device header */
            printf("  %s%s/dev/%s%s\n",
                   C->bold, C->green, t->name, C->reset);
            cur_row++;

            /*
             * Fixed-column layout:
             *   col  2: indent
             *   col  4: path starts
             *   col 50: '=' (fixed, pad path to 46 visible chars)
             *   col 54: value (18 chars)
             *   col 73: description
             *
             * We build the full path string first so we can measure it,
             * then pad with spaces to reach col 50.
             */
            #define KT_LINE(fullpath, val_str, vc, desc) do { \
                int _plen = (int)strlen(fullpath); \
                int _pad  = 46 - _plen; if (_pad < 1) _pad = 1; \
                printf("  %s  %s%*s%s  =  %s%-16s%s  %s%s%s\n", \
                       C->white, fullpath, _pad, "", C->reset, \
                       vc, val_str, C->reset, \
                       C->white, desc, C->reset); \
                cur_row++; \
            } while(0)

            #define KT_Q(suffix, val_str, vc, desc) do { \
                char _fp[256]; \
                snprintf(_fp, sizeof(_fp), "/sys/block/%s/queue/%s", t->name, suffix); \
                KT_LINE(_fp, val_str, vc, desc); \
            } while(0)

            /* scheduler */
            {
                const char *sc = !strcmp(t->scheduler, "bfq")        ? C->cyan  :
                                 !strcmp(t->scheduler, "kyber")       ? C->green :
                                 !strcmp(t->scheduler, "mq-deadline") ? C->white : C->white;
                KT_Q("scheduler", t->scheduler, sc, "I/O scheduler");
            }

            if (t->read_ahead_kb >= 0) {
                char v[16]; snprintf(v, sizeof(v), "%d kB", t->read_ahead_kb);
                KT_Q("read_ahead_kb", v, C->white, "sequential read-ahead window");
            }
            if (t->nr_requests >= 0) {
                char v[16]; snprintf(v, sizeof(v), "%d", t->nr_requests);
                KT_Q("nr_requests", v, C->white, "max queued requests per queue");
            }
            if (t->max_sectors_kb >= 0) {
                char v[16]; snprintf(v, sizeof(v), "%d kB", t->max_sectors_kb);
                KT_Q("max_sectors_kb", v, C->white, "max single I/O size");
            }
            if (t->nomerges >= 0) {
                const char *v  = t->nomerges == 0 ? "enabled (0)"  :
                                 t->nomerges == 1 ? "partial (1)"  : "disabled (2)";
                const char *vc = t->nomerges == 0 ? C->green : C->yellow;
                KT_Q("nomerges", v, vc, "adjacent request merging");
            }
            if (t->write_cache >= 0) {
                const char *v  = t->write_cache ? "write back" : "write through";
                const char *vc = t->write_cache ? C->cyan : C->white;
                KT_Q("write_cache", v, vc, "drive write cache policy");
            }
            if (t->rq_affinity >= 0) {
                const char *v = t->rq_affinity == 0 ? "off (0)"     :
                                t->rq_affinity == 1 ? "on (1)"      : "migrate (2)";
                KT_Q("rq_affinity", v, C->white, "completion CPU affinity");
            }
            if (t->wbt_lat[0]) {
                char v[40]; snprintf(v, sizeof(v), "%s us", t->wbt_lat);
                KT_Q("wbt_lat_usec", v, C->white, "writeback throttle latency target");
            }

            /* NVMe-specific extras */
            if (t->power_state[0]) {
                char fp[256];
                snprintf(fp, sizeof(fp), "/sys/class/nvme/%s/power_state", t->name);
                KT_LINE(fp, t->power_state, C->cyan, "NVMe power management state");
            }
            if (t->numa_node[0] && strcmp(t->numa_node, "-1") != 0) {
                char fp[256];
                snprintf(fp, sizeof(fp), "/sys/block/%s/device/numa_node", t->name);
                KT_LINE(fp, t->numa_node, C->white, "NUMA node affinity");
            }

            #undef KT_Q
            #undef KT_LINE

            if (i < nkt - 1) {
                printf("\n");                   cur_row++;
            }
        }
    }

    /* ── swap ── */
    SwapEntry swaps[16];
    int nswap = gather_swap(swaps, 16);

    if (nswap > 0) {
        printf("\n");                           cur_row++;
        hr("─", C->gray, tw);                  cur_row++;
        printf("\n  %s%sSWAP%s\n\n",
               C->bold, C->white, C->reset);   cur_row += 3;

        printf("  %s%s%-32s  %-9s  %-10s  %-10s  %-4s  %-4s%s\n",
               C->bold, C->white,
               "DEVICE", "TYPE", "SIZE", "USED", "PRIO", "UTIL", C->reset);
        printf("\n");                           cur_row += 2;

        for (int i = 0; i < nswap; i++) {
            SwapEntry *e = &swaps[i];
            char s_size[20], s_used[20];
            fmt_size(s_size, sizeof(s_size), e->size);
            fmt_size(s_used, sizeof(s_used), e->used);

            int pct = e->size > 0 ? (int)(e->used * 100ULL / e->size) : 0;
            const char *uc = pct >= THRESH_CRIT ? C->red :
                             pct >= THRESH_WARN ? C->orange :
                             pct > 0            ? C->yellow : C->green;

            int ub = pct * 12 / 100;
            /* Print columns without color inside width specifiers */
            printf("  %s%-32s%s  %s%-9s%s  %s%-10s%s  %s%-10s%s  %s%-4d%s  ",
                   C->green, e->filename, C->reset,
                   C->white, e->type,     C->reset,
                   C->white, s_size,      C->reset,
                   uc,       s_used,      C->reset,
                   C->white, e->priority, C->reset);

            /* inline mini bar */
            printf("%s", C->bg_ok);
            for (int j = 0; j < ub; j++) putchar(' ');
            printf("%s%s", C->reset, C->bg_free);
            for (int j = ub; j < 12; j++) putchar(' ');
            printf("%s %s%d%%%s\n", C->reset, uc, pct, C->reset);
            cur_row++;
        }
    }

    printf("\n");                           cur_row++;
    hr("═", C->cyan, tw);                  cur_row++;
    printf("\n");                           cur_row++;

    int final_row = cur_row;
    if (!animate || anim_count == 0) {
        free(rows);
        return 0;
    }

    int used_max = 0;
    for (int i = 0; i < anim_count; i++) {
        int u = rows[i].pct * bar_width / 100;
        if (u > used_max) used_max = u;
    }

    printf("\033[?25l");   /* hide cursor during animation */
    fflush(stdout);

    for (int f = 1; f <= used_max; f++) {
        for (int i = 0; i < anim_count; i++) {
            int u = rows[i].pct * bar_width / 100;
            if (f > u) continue;
            repaint_used(&rows[i], f, bar_width);
        }
        printf("\033[%d;1H", final_row);
        fflush(stdout);
        sleep_ms(5);
    }

    printf("\033[?25h");   /* restore cursor */
    printf("\033[%d;1H", final_row);
    fflush(stdout);

    /* ── wait for q / Q / ESC / Ctrl+C to exit alternate screen ── */
    struct termios old_tc, raw_tc;
    if (tcgetattr(STDIN_FILENO, &old_tc) == 0) {
        raw_tc = old_tc;
        cfmakeraw(&raw_tc);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw_tc);

        /* Show a dim prompt at the bottom-right corner */
        int th = 0;
        struct winsize wsize;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &wsize) == 0)
            th = wsize.ws_row;
        if (th > 0) {
            printf("\033[%d;1H", th);
            printf("%s  q · ESC · Enter to exit %s", C->white, C->reset);
            fflush(stdout);
        }

        for (;;) {
            int key = read_key_with_escape_handling();
            if (key < 0) break;
            if (key == 'q' || key == 'Q' || key == 27 || key == 3 ||
                key == '\r' || key == '\n')
                break;
        }
        tcsetattr(STDIN_FILENO, TCSANOW, &old_tc);
    }

    /* Leave alternate screen — terminal restored to prior state */
    printf("\033[?1049l");
    fflush(stdout);

    free(rows);
    return 0;
}
