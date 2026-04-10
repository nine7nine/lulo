#define _GNU_SOURCE

#include "lulo_dizk.h"

#include <ctype.h>
#include <dirent.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>

typedef struct {
    char device[256];
    char mount[256];
    char fstype[64];
    char opts[512];
    unsigned long long total;
    unsigned long long used;
    unsigned long long avail;
    int pct;
} FsEntry;

typedef struct {
    char name[64];
    char size[32];
    char model[128];
    char transport[32];
    int rotational;
} BlkDev;

typedef struct {
    char name[64];
    unsigned long long rd_ios;
    unsigned long long rd_sectors;
    unsigned long long rd_ms;
    unsigned long long wr_ios;
    unsigned long long wr_sectors;
    unsigned long long wr_ms;
    unsigned long long io_ms;
    unsigned long long discard_ios;
    unsigned long long flush_ios;
    char rd_bytes[32];
    char wr_bytes[32];
    double util_pct;
} DiskStat;

typedef struct {
    char spec[256];
    char device[256];
    char mount[256];
    char fstype[64];
    char opts[256];
    int dump;
    int pass;
} FstabEntry;

typedef struct {
    char name[64];
    char scheduler[64];
    int read_ahead_kb;
    int nr_requests;
    int max_sectors_kb;
    int rotational;
    int write_cache;
    int nomerges;
    int rq_affinity;
    char wbt_lat[32];
    char power_state[32];
    char numa_node[8];
} KTunable;

typedef struct {
    char filename[256];
    char type[16];
    unsigned long long size;
    unsigned long long used;
    int priority;
} SwapEntry;

static const char *skip_types[] = {
    "sysfs", "proc", "devtmpfs", "devpts", "securityfs", "cgroup", "cgroup2",
    "pstore", "efivarfs", "bpf", "debugfs", "tracefs", "hugetlbfs", "mqueue",
    "configfs", "fusectl", "ramfs", "binfmt_misc", "nsfs", "overlay", "squashfs",
    "fuse.snapfuse", "fuse.portal", "autofs", "rpc_pipefs", "nfsd", NULL
};

static const char *skip_prefixes[] = {
    "/sys", "/proc", "/dev/pts", "/run/user", "/run/credentials",
    "/run/snapd", "/snap", NULL
};

static int clamp_int(int value, int lo, int hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static void trunc_str(const char *src, char *dst, int maxw)
{
    int slen;

    if (maxw <= 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    slen = (int)strlen(src);
    if (slen <= maxw) snprintf(dst, (size_t)maxw + 1, "%s", src);
    else if (maxw >= 3) snprintf(dst, (size_t)maxw + 1, "...%s", src + slen - (maxw - 3));
    else snprintf(dst, (size_t)maxw + 1, "%.*s", maxw, src);
}

static void fmt_size(char *buf, size_t len, unsigned long long bytes)
{
    const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    double val = (double)bytes;
    int i = 0;

    while (val >= 1024.0 && i < 5) {
        val /= 1024.0;
        i++;
    }
    if (i == 0) snprintf(buf, len, "%llu B", bytes);
    else if (val >= 100.0) snprintf(buf, len, "%.0f %s", val, units[i]);
    else if (val >= 10.0) snprintf(buf, len, "%.1f %s", val, units[i]);
    else snprintf(buf, len, "%.2f %s", val, units[i]);
}

static int read_sysfs_str(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    int ok;
    char *end;

    if (!f) return -1;
    ok = fgets(buf, (int)len, f) != NULL;
    fclose(f);
    if (!ok) return -1;
    end = buf + strlen(buf) - 1;
    while (end >= buf && isspace((unsigned char)*end)) *end-- = '\0';
    return 0;
}

static int should_skip(const struct mntent *me, int show_all)
{
    if (show_all) return 0;
    for (int i = 0; skip_types[i]; i++) {
        if (!strcmp(me->mnt_type, skip_types[i])) return 1;
    }
    if (!strcmp(me->mnt_type, "tmpfs") &&
        strcmp(me->mnt_dir, "/tmp") && strcmp(me->mnt_dir, "/dev/shm")) {
        return 1;
    }
    for (int i = 0; skip_prefixes[i]; i++) {
        if (!strncmp(me->mnt_dir, skip_prefixes[i], strlen(skip_prefixes[i]))) return 1;
    }
    return 0;
}

static void extract_mount_opts(const char *raw, char *out, size_t len)
{
    static const char *keep[] = {
        "ro", "rw", "noatime", "relatime", "nosuid", "nodev", "noexec", "discard",
        "compress", "subvol", "space_cache", "ssd", "errors=", "data=", "commit=",
        "barrier=", "user_xattr", "acl", "noacl", "quota", "usrquota", "grpquota",
        "nofail", "defaults", "x-systemd", NULL
    };
    size_t pos = 0;
    char buf[2048];

    out[0] = '\0';
    snprintf(buf, sizeof(buf), "%s", raw ? raw : "");
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        for (int i = 0; keep[i]; i++) {
            if (!strncmp(tok, keep[i], strlen(keep[i]))) {
                size_t tl = strlen(tok);
                if (pos > 0 && pos + 1 < len) out[pos++] = ',';
                if (pos + tl < len) {
                    memcpy(out + pos, tok, tl);
                    pos += tl;
                }
                break;
            }
        }
    }
    out[pos] = '\0';
}

static void resolve_fstab_dev(const char *spec, char *out, size_t outlen)
{
    const char *prefix = NULL;
    const char *val = NULL;
    char link[512];
    char target[512];
    ssize_t n;
    char *base;

    if (!strncmp(spec, "/dev/", 5)) {
        snprintf(out, outlen, "%s", spec);
        return;
    }
    if (!strncmp(spec, "UUID=", 5)) {
        prefix = "/dev/disk/by-uuid/";
        val = spec + 5;
    } else if (!strncmp(spec, "LABEL=", 6)) {
        prefix = "/dev/disk/by-label/";
        val = spec + 6;
    } else if (!strncmp(spec, "PARTLABEL=", 10)) {
        prefix = "/dev/disk/by-partlabel/";
        val = spec + 10;
    } else {
        snprintf(out, outlen, "%s", spec);
        return;
    }

    snprintf(link, sizeof(link), "%s%s", prefix, val);
    n = readlink(link, target, sizeof(target) - 1);
    if (n <= 0) {
        snprintf(out, outlen, "%s", spec);
        return;
    }
    target[n] = '\0';
    base = strrchr(target, '/');
    snprintf(out, outlen, "/dev/%.*s", (int)(outlen - 6), base ? base + 1 : target);
}

static int cmp_fs(const void *a, const void *b)
{
    const FsEntry *fa = a;
    const FsEntry *fb = b;
    return strcmp(fa->mount, fb->mount);
}

static int cmp_blkdev(const void *a, const void *b)
{
    const BlkDev *da = a;
    const BlkDev *db = b;
    return strcmp(da->name, db->name);
}

static int cmp_iostat(const void *a, const void *b)
{
    const DiskStat *da = a;
    const DiskStat *db = b;

    if (da->util_pct < db->util_pct) return 1;
    if (da->util_pct > db->util_pct) return -1;
    return strcmp(da->name, db->name);
}

static int cmp_tunable(const void *a, const void *b)
{
    const KTunable *ta = a;
    const KTunable *tb = b;
    return strcmp(ta->name, tb->name);
}

static int cmp_swap(const void *a, const void *b)
{
    const SwapEntry *sa = a;
    const SwapEntry *sb = b;
    unsigned long long sa_pct = sa->size ? sa->used * 100ULL / sa->size : 0;
    unsigned long long sb_pct = sb->size ? sb->used * 100ULL / sb->size : 0;

    if (sa_pct < sb_pct) return 1;
    if (sa_pct > sb_pct) return -1;
    if (sa->priority < sb->priority) return 1;
    if (sa->priority > sb->priority) return -1;
    return strcmp(sa->filename, sb->filename);
}

static int gather_filesystems(FsEntry *entries, int max, int show_all)
{
    FILE *mtab = setmntent("/etc/mtab", "r");
    char seen_dev[128][256];
    int seen_count = 0;
    struct mntent *me;
    int count = 0;

    if (!mtab) mtab = setmntent("/proc/mounts", "r");
    if (!mtab) return 0;

    while ((me = getmntent(mtab)) && count < max) {
        struct statvfs st;
        int dup = 0;

        if (should_skip(me, show_all)) continue;
        for (int i = 0; i < seen_count; i++) {
            if (!strcmp(seen_dev[i], me->mnt_fsname)) {
                dup = 1;
                break;
            }
        }
        if (dup) continue;
        if (seen_count < 128) snprintf(seen_dev[seen_count++], 256, "%s", me->mnt_fsname);
        if (statvfs(me->mnt_dir, &st) != 0 || st.f_blocks == 0) continue;

        FsEntry *e = &entries[count++];
        memset(e, 0, sizeof(*e));
        snprintf(e->device, sizeof(e->device), "%s", me->mnt_fsname);
        snprintf(e->mount, sizeof(e->mount), "%s", me->mnt_dir);
        snprintf(e->fstype, sizeof(e->fstype), "%s", me->mnt_type);
        extract_mount_opts(me->mnt_opts, e->opts, sizeof(e->opts));
        e->total = st.f_blocks * (unsigned long long)st.f_frsize;
        e->avail = st.f_bavail * (unsigned long long)st.f_frsize;
        e->used = e->total - st.f_bfree * (unsigned long long)st.f_frsize;
        e->pct = e->total > 0 ? (int)(e->used * 100ULL / e->total) : 0;
        if (e->pct > 100) e->pct = 100;
    }
    endmntent(mtab);
    qsort(entries, (size_t)count, sizeof(*entries), cmp_fs);
    return count;
}

static int gather_block_devices(BlkDev *devs, int max)
{
    DIR *d = opendir("/sys/block");
    struct dirent *de;
    int count = 0;

    if (!d) return 0;
    while ((de = readdir(d)) && count < max) {
        char path[512];
        char sbuf[32];
        char rbuf[8] = "0";
        BlkDev *bd;

        if (de->d_name[0] == '.') continue;
        if (!strncmp(de->d_name, "loop", 4) || !strncmp(de->d_name, "ram", 3) ||
            !strncmp(de->d_name, "dm-", 3) || !strncmp(de->d_name, "zram", 4)) {
            continue;
        }

        bd = &devs[count++];
        memset(bd, 0, sizeof(*bd));
        snprintf(bd->name, sizeof(bd->name), "%.*s", (int)sizeof(bd->name) - 1, de->d_name);
        snprintf(path, sizeof(path), "/sys/block/%s/size", de->d_name);
        if (read_sysfs_str(path, sbuf, sizeof(sbuf)) == 0) {
            fmt_size(bd->size, sizeof(bd->size), strtoull(sbuf, NULL, 10) * 512ULL);
        } else {
            snprintf(bd->size, sizeof(bd->size), "?");
        }
        snprintf(path, sizeof(path), "/sys/block/%s/queue/rotational", de->d_name);
        read_sysfs_str(path, rbuf, sizeof(rbuf));
        bd->rotational = atoi(rbuf);
        snprintf(path, sizeof(path), "/sys/block/%s/device/model", de->d_name);
        if (read_sysfs_str(path, bd->model, sizeof(bd->model)) < 0) bd->model[0] = '\0';
        if (!strncmp(de->d_name, "nvme", 4)) snprintf(bd->transport, sizeof(bd->transport), "nvme");
        else if (!strncmp(de->d_name, "sd", 2)) snprintf(bd->transport, sizeof(bd->transport), "sata");
        else if (!strncmp(de->d_name, "mmcblk", 6)) snprintf(bd->transport, sizeof(bd->transport), "mmc");
        else if (!strncmp(de->d_name, "vd", 2)) snprintf(bd->transport, sizeof(bd->transport), "virtio");
        else snprintf(bd->transport, sizeof(bd->transport), "-");
    }
    closedir(d);
    qsort(devs, (size_t)count, sizeof(*devs), cmp_blkdev);
    return count;
}

static int gather_fstab(FstabEntry *entries, int max)
{
    FILE *f = setmntent("/etc/fstab", "r");
    struct mntent *me;
    int count = 0;

    if (!f) return 0;
    while ((me = getmntent(f)) && count < max) {
        FstabEntry *e;

        if (!strcmp(me->mnt_type, "proc") || !strcmp(me->mnt_type, "sysfs") ||
            !strcmp(me->mnt_type, "devpts") || !strcmp(me->mnt_type, "hugetlbfs")) {
            continue;
        }
        if (!strcmp(me->mnt_type, "tmpfs") &&
            strcmp(me->mnt_dir, "/tmp") && strcmp(me->mnt_dir, "/dev/shm")) {
            continue;
        }

        e = &entries[count++];
        memset(e, 0, sizeof(*e));
        snprintf(e->spec, sizeof(e->spec), "%s", me->mnt_fsname);
        snprintf(e->mount, sizeof(e->mount), "%s", me->mnt_dir);
        snprintf(e->fstype, sizeof(e->fstype), "%s", me->mnt_type);
        e->dump = me->mnt_freq;
        e->pass = me->mnt_passno;
        resolve_fstab_dev(me->mnt_fsname, e->device, sizeof(e->device));
        extract_mount_opts(me->mnt_opts, e->opts, sizeof(e->opts));
    }
    endmntent(f);
    return count;
}

static int gather_diskstats(DiskStat *stats, int max)
{
    FILE *uptime = fopen("/proc/uptime", "r");
    FILE *f = fopen("/proc/diskstats", "r");
    double uptime_ms = 0.0;
    char line[512];
    int count = 0;

    if (uptime) {
        double up;
        if (fscanf(uptime, "%lf", &up) == 1) uptime_ms = up * 1000.0;
        fclose(uptime);
    }
    if (!f) return 0;

    while (fgets(line, sizeof(line), f) && count < max) {
        unsigned int maj, min;
        char name[64];
        unsigned long long rd_ios, rd_merges, rd_sectors, rd_ms;
        unsigned long long wr_ios, wr_merges, wr_sectors, wr_ms;
        unsigned long long cur_ios, io_ms, wt_ms;
        unsigned long long dis_ios, dis_merges, dis_sectors, dis_ms;
        unsigned long long flush_ios, flush_ms;
        int rc;
        int nlen;

        rc = sscanf(line,
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
        if (rc < 14) continue;
        if (!strncmp(name, "loop", 4) || !strncmp(name, "ram", 3) ||
            !strncmp(name, "dm-", 3) || !strncmp(name, "zram", 4)) {
            continue;
        }
        nlen = (int)strlen(name);
        if (!strncmp(name, "nvme", 4)) {
            char *p = strrchr(name, 'p');
            if (p && p > name) {
                int all_digits = 1;
                for (char *q = p + 1; *q; q++) {
                    if (!isdigit((unsigned char)*q)) {
                        all_digits = 0;
                        break;
                    }
                }
                if (all_digits && *(p + 1)) continue;
            }
        } else if (nlen > 0 && isdigit((unsigned char)name[nlen - 1])) {
            continue;
        }

        DiskStat *ds = &stats[count++];
        memset(ds, 0, sizeof(*ds));
        snprintf(ds->name, sizeof(ds->name), "%s", name);
        ds->rd_ios = rd_ios;
        ds->rd_sectors = rd_sectors;
        ds->rd_ms = rd_ms;
        ds->wr_ios = wr_ios;
        ds->wr_sectors = wr_sectors;
        ds->wr_ms = wr_ms;
        ds->io_ms = io_ms;
        ds->discard_ios = (rc >= 15) ? dis_ios : 0;
        ds->flush_ios = (rc >= 19) ? flush_ios : 0;
        fmt_size(ds->rd_bytes, sizeof(ds->rd_bytes), rd_sectors * 512ULL);
        fmt_size(ds->wr_bytes, sizeof(ds->wr_bytes), wr_sectors * 512ULL);
        ds->util_pct = uptime_ms > 0.0 ? ((double)io_ms / uptime_ms) * 100.0 : 0.0;
        if (ds->util_pct > 100.0) ds->util_pct = 100.0;
    }
    fclose(f);
    qsort(stats, (size_t)count, sizeof(*stats), cmp_iostat);
    return count;
}

static int gather_ktunables(KTunable *tunables, int max)
{
    DIR *d = opendir("/sys/block");
    struct dirent *de;
    int count = 0;

    if (!d) return 0;
    while ((de = readdir(d)) && count < max) {
        const char *dn = de->d_name;
        int nlen;
        char path[512];
        char buf[256];
        KTunable *t;

        if (dn[0] == '.') continue;
        if (!strncmp(dn, "loop", 4) || !strncmp(dn, "ram", 3) ||
            !strncmp(dn, "dm-", 3) || !strncmp(dn, "zram", 4)) {
            continue;
        }
        nlen = (int)strlen(dn);
        if (!strncmp(dn, "nvme", 4)) {
            char *p = strrchr((char *)dn, 'p');
            if (p && p > dn) {
                int all_digits = 1;
                for (char *q = p + 1; *q; q++) {
                    if (!isdigit((unsigned char)*q)) {
                        all_digits = 0;
                        break;
                    }
                }
                if (all_digits && *(p + 1)) continue;
            }
        } else if (nlen > 0 && isdigit((unsigned char)dn[nlen - 1])) {
            continue;
        }

        t = &tunables[count++];
        memset(t, 0, sizeof(*t));
        snprintf(t->name, sizeof(t->name), "%s", dn);

        snprintf(path, sizeof(path), "/sys/block/%s/queue/scheduler", dn);
        if (read_sysfs_str(path, buf, sizeof(buf)) == 0) {
            char *lb = strchr(buf, '[');
            char *rb = lb ? strchr(lb, ']') : NULL;
            if (lb && rb) {
                int len = (int)(rb - lb - 1);
                if (len >= (int)sizeof(t->scheduler)) len = (int)sizeof(t->scheduler) - 1;
                memcpy(t->scheduler, lb + 1, (size_t)len);
                t->scheduler[len] = '\0';
            } else {
                snprintf(t->scheduler, sizeof(t->scheduler), "%.*s",
                         (int)sizeof(t->scheduler) - 1, buf);
            }
        } else {
            snprintf(t->scheduler, sizeof(t->scheduler), "-");
        }

#define RD_INT(field, file) do { \
        snprintf(path, sizeof(path), "/sys/block/%s/queue/" file, dn); \
        t->field = (read_sysfs_str(path, buf, sizeof(buf)) == 0) ? atoi(buf) : -1; \
    } while (0)

        RD_INT(read_ahead_kb, "read_ahead_kb");
        RD_INT(nr_requests, "nr_requests");
        RD_INT(max_sectors_kb, "max_sectors_kb");
        RD_INT(rotational, "rotational");
        RD_INT(nomerges, "nomerges");
        RD_INT(rq_affinity, "rq_affinity");

        snprintf(path, sizeof(path), "/sys/block/%s/queue/write_cache", dn);
        t->write_cache = -1;
        if (read_sysfs_str(path, buf, sizeof(buf)) == 0) {
            t->write_cache = !strncmp(buf, "write back", 10) ? 1 : 0;
        }
        snprintf(path, sizeof(path), "/sys/block/%s/queue/wbt_lat_usec", dn);
        t->wbt_lat[0] = '\0';
        if (read_sysfs_str(path, buf, sizeof(buf)) == 0) {
            snprintf(t->wbt_lat, sizeof(t->wbt_lat), "%.*s", (int)sizeof(t->wbt_lat) - 1, buf);
        }
        t->power_state[0] = '\0';
        if (!strncmp(dn, "nvme", 4)) {
            snprintf(path, sizeof(path), "/sys/class/nvme/%s/power_state", dn);
            if (read_sysfs_str(path, buf, sizeof(buf)) == 0) {
                snprintf(t->power_state, sizeof(t->power_state), "%.*s",
                         (int)sizeof(t->power_state) - 1, buf);
            }
        }
        t->numa_node[0] = '\0';
        snprintf(path, sizeof(path), "/sys/block/%s/device/numa_node", dn);
        if (read_sysfs_str(path, buf, sizeof(buf)) == 0) {
            snprintf(t->numa_node, sizeof(t->numa_node), "%.*s",
                     (int)sizeof(t->numa_node) - 1, buf);
        }

#undef RD_INT
    }
    closedir(d);
    qsort(tunables, (size_t)count, sizeof(*tunables), cmp_tunable);
    return count;
}

static int gather_swap(SwapEntry *swaps, int max)
{
    FILE *f = fopen("/proc/swaps", "r");
    char line[512];
    int count = 0;

    if (!f) return 0;
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return 0;
    }
    while (fgets(line, sizeof(line), f) && count < max) {
        SwapEntry *e = &swaps[count];
        unsigned long long sz;
        unsigned long long used;
        int prio;
        char type[32];

        if (sscanf(line, "%255s %31s %llu %llu %d", e->filename, type, &sz, &used, &prio) == 5) {
            snprintf(e->type, sizeof(e->type), "%.*s", (int)sizeof(e->type) - 1, type);
            e->size = sz * 1024ULL;
            e->used = used * 1024ULL;
            e->priority = prio;
            count++;
        }
    }
    fclose(f);
    qsort(swaps, (size_t)count, sizeof(*swaps), cmp_swap);
    return count;
}

static void fill_fs_rows(LuloDizkSnapshot *snap, const FsEntry *entries, int count)
{
    snap->fs_count = clamp_int(count, 0, LULO_DIZK_MAX_FS);
    for (int i = 0; i < snap->fs_count; i++) {
        LuloDizkFsRow *row = &snap->filesystems[i];

        memset(row, 0, sizeof(*row));
        trunc_str(entries[i].device, row->device, (int)sizeof(row->device) - 1);
        trunc_str(entries[i].mount, row->mount, (int)sizeof(row->mount) - 1);
        trunc_str(entries[i].fstype, row->fstype, (int)sizeof(row->fstype) - 1);
        fmt_size(row->used, sizeof(row->used), entries[i].used);
        fmt_size(row->avail, sizeof(row->avail), entries[i].avail);
        fmt_size(row->total, sizeof(row->total), entries[i].total);
        row->pct = entries[i].pct;
    }
}

static void fill_block_rows(LuloDizkSnapshot *snap, const BlkDev *devs, int count)
{
    snap->blockdev_count = clamp_int(count, 0, LULO_DIZK_MAX_BLOCKDEVS);
    for (int i = 0; i < snap->blockdev_count; i++) {
        LuloDizkBlockRow *row = &snap->blockdevs[i];

        memset(row, 0, sizeof(*row));
        trunc_str(devs[i].name, row->name, (int)sizeof(row->name) - 1);
        trunc_str(devs[i].size, row->size, (int)sizeof(row->size) - 1);
        snprintf(row->type, sizeof(row->type), "%s", devs[i].rotational ? "HDD" : "SSD");
        trunc_str(devs[i].transport, row->transport, (int)sizeof(row->transport) - 1);
        trunc_str(devs[i].model[0] ? devs[i].model : "-", row->model, (int)sizeof(row->model) - 1);
    }
}

static void fill_iostat_rows(LuloDizkSnapshot *snap, const DiskStat *stats, int count)
{
    snap->iostat_count = clamp_int(count, 0, LULO_DIZK_MAX_IOSTATS);
    for (int i = 0; i < snap->iostat_count; i++) {
        LuloDizkIoRow *row = &snap->iostats[i];

        memset(row, 0, sizeof(*row));
        trunc_str(stats[i].name, row->name, (int)sizeof(row->name) - 1);
        trunc_str(stats[i].rd_bytes, row->rd_bytes, (int)sizeof(row->rd_bytes) - 1);
        trunc_str(stats[i].wr_bytes, row->wr_bytes, (int)sizeof(row->wr_bytes) - 1);
        row->rd_ios = stats[i].rd_ios;
        row->wr_ios = stats[i].wr_ios;
        row->util_pct = clamp_int((int)(stats[i].util_pct + 0.5), 0, 100);
    }
}

static void fill_tunable_rows(LuloDizkSnapshot *snap, const KTunable *tunables, int count)
{
    snap->tunable_count = clamp_int(count, 0, LULO_DIZK_MAX_TUNABLES);
    for (int i = 0; i < snap->tunable_count; i++) {
        LuloDizkTunableRow *row = &snap->tunables[i];

        memset(row, 0, sizeof(*row));
        trunc_str(tunables[i].name, row->name, (int)sizeof(row->name) - 1);
        trunc_str(tunables[i].scheduler, row->scheduler, (int)sizeof(row->scheduler) - 1);
        snprintf(row->cache, sizeof(row->cache), "%s",
                 tunables[i].write_cache > 0 ? "wb" :
                 tunables[i].write_cache == 0 ? "wt" : "-");
        if (tunables[i].power_state[0]) {
            trunc_str(tunables[i].power_state, row->state, (int)sizeof(row->state) - 1);
        } else if (tunables[i].wbt_lat[0]) {
            snprintf(row->state, sizeof(row->state), "wbt=%sus", tunables[i].wbt_lat);
        } else {
            snprintf(row->state, sizeof(row->state), "-");
        }
        trunc_str(tunables[i].numa_node[0] ? tunables[i].numa_node : "-", row->numa_node,
                  (int)sizeof(row->numa_node) - 1);
        row->read_ahead_kb = tunables[i].read_ahead_kb;
        row->nr_requests = tunables[i].nr_requests;
        row->max_sectors_kb = tunables[i].max_sectors_kb;
    }
}

static void fill_swap_rows(LuloDizkSnapshot *snap, const SwapEntry *swaps, int count)
{
    snap->swap_count = clamp_int(count, 0, LULO_DIZK_MAX_SWAP);
    for (int i = 0; i < snap->swap_count; i++) {
        LuloDizkSwapRow *row = &snap->swaps[i];
        int pct = swaps[i].size > 0 ? (int)(swaps[i].used * 100ULL / swaps[i].size) : 0;

        memset(row, 0, sizeof(*row));
        trunc_str(swaps[i].filename, row->name, (int)sizeof(row->name) - 1);
        trunc_str(swaps[i].type, row->type, (int)sizeof(row->type) - 1);
        fmt_size(row->size, sizeof(row->size), swaps[i].size);
        fmt_size(row->used, sizeof(row->used), swaps[i].used);
        row->priority = swaps[i].priority;
        row->pct = pct;
    }
}

void lulo_dizk_state_init(LuloDizkState *state)
{
    memset(state, 0, sizeof(*state));
}

void lulo_dizk_state_cleanup(LuloDizkState *state)
{
    memset(state, 0, sizeof(*state));
}

int lulo_dizk_snapshot_gather(LuloDizkSnapshot *snap, const LuloDizkState *state, int width)
{
    FsEntry fs_entries[LULO_DIZK_MAX_FS];
    BlkDev devs[LULO_DIZK_MAX_BLOCKDEVS];
    DiskStat stats[LULO_DIZK_MAX_IOSTATS];
    KTunable tunables[LULO_DIZK_MAX_TUNABLES];
    SwapEntry swaps[LULO_DIZK_MAX_SWAP];
    FstabEntry fstab[32];
    int fs_count;
    int dev_count;
    int io_count;
    int tunable_count;
    int swap_count;

    (void)width;
    memset(snap, 0, sizeof(*snap));

    fs_count = gather_filesystems(fs_entries, LULO_DIZK_MAX_FS, state ? state->show_all : 0);
    dev_count = gather_block_devices(devs, LULO_DIZK_MAX_BLOCKDEVS);
    io_count = gather_diskstats(stats, LULO_DIZK_MAX_IOSTATS);
    tunable_count = gather_ktunables(tunables, LULO_DIZK_MAX_TUNABLES);
    swap_count = gather_swap(swaps, LULO_DIZK_MAX_SWAP);

    fill_fs_rows(snap, fs_entries, fs_count);
    fill_block_rows(snap, devs, dev_count);
    fill_iostat_rows(snap, stats, io_count);
    fill_tunable_rows(snap, tunables, tunable_count);
    fill_swap_rows(snap, swaps, swap_count);

    snap->fstab_count = gather_fstab(fstab, (int)(sizeof(fstab) / sizeof(fstab[0])));
    snap->count = snap->fs_count;
    return 0;
}

void lulo_dizk_snapshot_free(LuloDizkSnapshot *snap)
{
    memset(snap, 0, sizeof(*snap));
}

void lulo_dizk_view_sync(LuloDizkState *state, const LuloDizkSnapshot *snap, int visible_rows)
{
    int max_scroll;

    if (!state || !snap || snap->fs_count <= 0 || visible_rows <= 0) {
        if (state) state->scroll = 0;
        return;
    }
    max_scroll = snap->fs_count > visible_rows ? snap->fs_count - visible_rows : 0;
    state->scroll = clamp_int(state->scroll, 0, max_scroll);
}

void lulo_dizk_view_move(LuloDizkState *state, const LuloDizkSnapshot *snap, int visible_rows, int delta)
{
    lulo_dizk_view_sync(state, snap, visible_rows);
    state->scroll += delta;
    lulo_dizk_view_sync(state, snap, visible_rows);
}

void lulo_dizk_view_page(LuloDizkState *state, const LuloDizkSnapshot *snap, int visible_rows, int pages)
{
    int delta = visible_rows > 0 ? visible_rows * pages : pages;
    lulo_dizk_view_move(state, snap, visible_rows, delta);
}

void lulo_dizk_view_home(LuloDizkState *state)
{
    if (!state) return;
    state->scroll = 0;
}

void lulo_dizk_view_end(LuloDizkState *state, const LuloDizkSnapshot *snap, int visible_rows)
{
    if (!state || !snap || snap->fs_count <= 0) {
        if (state) state->scroll = 0;
        return;
    }
    state->scroll = snap->fs_count > visible_rows ? snap->fs_count - visible_rows : 0;
}
