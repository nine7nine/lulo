#define _GNU_SOURCE

#include "lulod_udev.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define UDEV_DETAIL_LIMIT 256
#define UDEV_DATA_ROOT "/run/udev/data"

typedef struct {
    const char *root;
    const char *source;
} ConfigRoot;

static const ConfigRoot rule_roots[] = {
    { "/etc/udev/rules.d", "etc" },
    { "/usr/lib/udev/rules.d", "vendor" },
    { "/lib/udev/rules.d", "vendor" },
};

static const ConfigRoot hwdb_roots[] = {
    { "/etc/udev/hwdb.d", "etc" },
    { "/usr/lib/udev/hwdb.d", "vendor" },
    { "/lib/udev/hwdb.d", "vendor" },
};

static int append_snapshot_line(char ***lines, int *count, const char *text)
{
    char **next;
    char *copy;

    next = realloc(*lines, (size_t)(*count + 1) * sizeof(*next));
    if (!next) return -1;
    *lines = next;
    copy = strdup(text ? text : "");
    if (!copy) return -1;
    (*lines)[(*count)++] = copy;
    return 0;
}

static void clear_lines(char ***lines, int *count)
{
    if (!lines || !count) return;
    for (int i = 0; i < *count; i++) free((*lines)[i]);
    free(*lines);
    *lines = NULL;
    *count = 0;
}

static void copy_trunc(char *dst, size_t len, const char *src)
{
    size_t n;

    if (!dst || len == 0) return;
    if (!src) src = "";
    n = strlen(src);
    if (n >= len) n = len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int append_linef(char ***lines, int *count, const char *fmt, ...)
{
    char buf[768];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return append_snapshot_line(lines, count, buf);
}

static int path_join(char *buf, size_t len, const char *a, const char *b)
{
    size_t alen;
    size_t blen;

    if (!buf || len == 0 || !a || !b) return -1;
    alen = strlen(a);
    blen = strlen(b);
    if (alen + 1 + blen + 1 > len) return -1;
    memcpy(buf, a, alen);
    buf[alen] = '/';
    memcpy(buf + alen + 1, b, blen);
    buf[alen + 1 + blen] = '\0';
    return 0;
}

static int append_config_row(LuloUdevConfigRow **rows, int *count,
                             const char *path, const char *name, const char *source)
{
    LuloUdevConfigRow *next;
    LuloUdevConfigRow *row;

    next = realloc(*rows, (size_t)(*count + 1) * sizeof(*next));
    if (!next) return -1;
    *rows = next;
    row = &next[(*count)++];
    memset(row, 0, sizeof(*row));
    copy_trunc(row->path, sizeof(row->path), path);
    copy_trunc(row->name, sizeof(row->name), name);
    copy_trunc(row->source, sizeof(row->source), source);
    return 0;
}

static int append_device_row(LuloUdevSnapshot *snap, const char *path, const char *name,
                             const char *subsystem, const char *devnode, const char *devpath)
{
    LuloUdevDeviceRow *next;
    LuloUdevDeviceRow *row;

    next = realloc(snap->devices, (size_t)(snap->device_count + 1) * sizeof(*next));
    if (!next) return -1;
    snap->devices = next;
    row = &next[snap->device_count++];
    memset(row, 0, sizeof(*row));
    copy_trunc(row->path, sizeof(row->path), path);
    copy_trunc(row->name, sizeof(row->name), name);
    copy_trunc(row->subsystem, sizeof(row->subsystem), subsystem);
    copy_trunc(row->devnode, sizeof(row->devnode), devnode);
    copy_trunc(row->devpath, sizeof(row->devpath), devpath);
    return 0;
}

static int config_source_rank(const char *source)
{
    if (!source) return 2;
    if (strcmp(source, "etc") == 0) return 0;
    if (strcmp(source, "vendor") == 0) return 1;
    return 2;
}

static int config_row_cmp(const void *a, const void *b)
{
    const LuloUdevConfigRow *ra = a;
    const LuloUdevConfigRow *rb = b;
    int sa = config_source_rank(ra->source);
    int sb = config_source_rank(rb->source);

    if (sa != sb) return sa - sb;
    return strcmp(ra->name, rb->name);
}

static int device_row_cmp(const void *a, const void *b)
{
    const LuloUdevDeviceRow *ra = a;
    const LuloUdevDeviceRow *rb = b;
    int cmp = strcmp(ra->subsystem, rb->subsystem);

    if (cmp != 0) return cmp;
    cmp = strcmp(ra->name, rb->name);
    if (cmp != 0) return cmp;
    return strcmp(ra->path, rb->path);
}

static int read_text_file_lines(const char *path, char ***lines, int *count)
{
    FILE *fp;
    char buf[1024];

    if (!path || !lines || !count) return -1;
    fp = fopen(path, "r");
    if (!fp) return -1;
    while (fgets(buf, sizeof(buf), fp) && *count < UDEV_DETAIL_LIMIT) {
        buf[strcspn(buf, "\r\n")] = '\0';
        if (append_snapshot_line(lines, count, buf) < 0) {
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);
    return 0;
}

static int scan_config_dir(LuloUdevConfigRow **rows, int *count,
                           const char *root, const char *source,
                           const char *suffix)
{
    DIR *dir;
    struct dirent *ent;

    dir = opendir(root);
    if (!dir) return errno == ENOENT ? 0 : -1;
    while ((ent = readdir(dir)) != NULL) {
        char full[PATH_MAX];
        struct stat st;
        size_t name_len;
        size_t suffix_len;

        if (ent->d_name[0] == '.') continue;
        name_len = strlen(ent->d_name);
        suffix_len = strlen(suffix);
        if (name_len < suffix_len || strcmp(ent->d_name + name_len - suffix_len, suffix) != 0) continue;
        if (path_join(full, sizeof(full), root, ent->d_name) < 0) continue;
        if (lstat(full, &st) < 0 || !S_ISREG(st.st_mode)) continue;
        if (append_config_row(rows, count, full, ent->d_name, source) < 0) {
            closedir(dir);
            return -1;
        }
    }
    closedir(dir);
    return 0;
}

static void parse_udev_data_line(const char *line,
                                 char *subsystem, size_t subsystem_len,
                                 char *devnode, size_t devnode_len,
                                 char *devpath, size_t devpath_len)
{
    if (!line) return;
    if (strncmp(line, "E:SUBSYSTEM=", 13) == 0) {
        copy_trunc(subsystem, subsystem_len, line + 13);
    } else if (strncmp(line, "E:DEVNAME=", 10) == 0) {
        copy_trunc(devnode, devnode_len, line + 10);
    } else if (strncmp(line, "N:", 2) == 0 && !devnode[0]) {
        copy_trunc(devnode, devnode_len, line + 2);
    } else if (strncmp(line, "P:", 2) == 0) {
        copy_trunc(devpath, devpath_len, line + 2);
    }
}

static void derive_device_name(const char *path, const char *devnode, const char *devpath,
                               char *name, size_t name_len)
{
    const char *base = NULL;

    if (devnode && *devnode) {
        base = strrchr(devnode, '/');
        copy_trunc(name, name_len, base ? base + 1 : devnode);
        return;
    }
    if (devpath && *devpath) {
        base = strrchr(devpath, '/');
        copy_trunc(name, name_len, base ? base + 1 : devpath);
        return;
    }
    base = path ? strrchr(path, '/') : NULL;
    copy_trunc(name, name_len, base ? base + 1 : (path ? path : ""));
}

static int scan_udev_devices(LuloUdevSnapshot *snap)
{
    DIR *dir;
    struct dirent *ent;

    dir = opendir(UDEV_DATA_ROOT);
    if (!dir) return errno == ENOENT ? 0 : -1;
    while ((ent = readdir(dir)) != NULL) {
        char full[PATH_MAX];
        struct stat st;
        FILE *fp;
        char line[1024];
        char subsystem[64] = "";
        char devnode[160] = "";
        char devpath[320] = "";
        char name[128] = "";

        if (ent->d_name[0] == '.') continue;
        if (path_join(full, sizeof(full), UDEV_DATA_ROOT, ent->d_name) < 0) continue;
        if (lstat(full, &st) < 0 || !S_ISREG(st.st_mode)) continue;
        fp = fopen(full, "r");
        if (!fp) continue;
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            parse_udev_data_line(line, subsystem, sizeof(subsystem), devnode, sizeof(devnode),
                                 devpath, sizeof(devpath));
            if (subsystem[0] && devnode[0] && devpath[0]) break;
        }
        fclose(fp);
        derive_device_name(full, devnode, devpath, name, sizeof(name));
        if (append_device_row(snap, full, name,
                              subsystem[0] ? subsystem : "-",
                              devnode[0] ? devnode : "-",
                              devpath[0] ? devpath : "-") < 0) {
            closedir(dir);
            return -1;
        }
    }
    closedir(dir);
    return 0;
}

static int gather_config_rows(LuloUdevSnapshot *snap)
{
    for (size_t i = 0; i < sizeof(rule_roots) / sizeof(rule_roots[0]); i++) {
        if (scan_config_dir(&snap->rules, &snap->rule_count,
                            rule_roots[i].root, rule_roots[i].source, ".rules") < 0) {
            return -1;
        }
    }
    for (size_t i = 0; i < sizeof(hwdb_roots) / sizeof(hwdb_roots[0]); i++) {
        if (scan_config_dir(&snap->hwdb, &snap->hwdb_count,
                            hwdb_roots[i].root, hwdb_roots[i].source, ".hwdb") < 0) {
            return -1;
        }
    }
    if (scan_udev_devices(snap) < 0) return -1;
    if (snap->rule_count > 1) qsort(snap->rules, (size_t)snap->rule_count, sizeof(*snap->rules), config_row_cmp);
    if (snap->hwdb_count > 1) qsort(snap->hwdb, (size_t)snap->hwdb_count, sizeof(*snap->hwdb), config_row_cmp);
    if (snap->device_count > 1) qsort(snap->devices, (size_t)snap->device_count, sizeof(*snap->devices), device_row_cmp);
    return 0;
}

static const LuloUdevConfigRow *selected_rule(const LuloUdevSnapshot *snap, const LuloUdevState *state)
{
    if (!snap || !state || state->rule_selected < 0 || state->rule_selected >= snap->rule_count) return NULL;
    return &snap->rules[state->rule_selected];
}

static const LuloUdevConfigRow *selected_hwdb(const LuloUdevSnapshot *snap, const LuloUdevState *state)
{
    if (!snap || !state || state->hwdb_selected < 0 || state->hwdb_selected >= snap->hwdb_count) return NULL;
    return &snap->hwdb[state->hwdb_selected];
}

static const LuloUdevDeviceRow *selected_device(const LuloUdevSnapshot *snap, const LuloUdevState *state)
{
    if (!snap || !state || state->device_selected < 0 || state->device_selected >= snap->device_count) return NULL;
    return &snap->devices[state->device_selected];
}

static int fill_rule_preview(LuloUdevSnapshot *snap, const LuloUdevConfigRow *row)
{
    if (!snap || !row) return -1;
    clear_lines(&snap->detail_lines, &snap->detail_line_count);
    copy_trunc(snap->detail_title, sizeof(snap->detail_title), row->name);
    copy_trunc(snap->detail_status, sizeof(snap->detail_status), row->path);
    if (append_linef(&snap->detail_lines, &snap->detail_line_count, "path: %s", row->path) < 0 ||
        append_linef(&snap->detail_lines, &snap->detail_line_count, "source: %s", row->source) < 0 ||
        append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, "") < 0) {
        return -1;
    }
    return read_text_file_lines(row->path, &snap->detail_lines, &snap->detail_line_count);
}

static int fill_device_preview(LuloUdevSnapshot *snap, const LuloUdevDeviceRow *row)
{
    FILE *fp;
    char line[1024];
    size_t status_off = 0;

    if (!snap || !row) return -1;
    clear_lines(&snap->detail_lines, &snap->detail_line_count);
    copy_trunc(snap->detail_title, sizeof(snap->detail_title), row->name);
    copy_trunc(snap->detail_status, sizeof(snap->detail_status), row->subsystem);
    status_off = strlen(snap->detail_status);
    if (status_off + 1 < sizeof(snap->detail_status) && row->devnode[0]) {
        snap->detail_status[status_off++] = ' ';
        copy_trunc(snap->detail_status + status_off,
                   sizeof(snap->detail_status) - status_off,
                   row->devnode);
    }
    if (append_linef(&snap->detail_lines, &snap->detail_line_count, "path: %s", row->path) < 0 ||
        append_linef(&snap->detail_lines, &snap->detail_line_count, "subsystem: %s", row->subsystem) < 0 ||
        append_linef(&snap->detail_lines, &snap->detail_line_count, "devnode: %s", row->devnode) < 0 ||
        append_linef(&snap->detail_lines, &snap->detail_line_count, "syspath: %s", row->devpath) < 0 ||
        append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, "") < 0 ||
        append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, "raw udev db:") < 0) {
        return -1;
    }
    fp = fopen(row->path, "r");
    if (!fp) return -1;
    while (fgets(line, sizeof(line), fp) && snap->detail_line_count < UDEV_DETAIL_LIMIT) {
        line[strcspn(line, "\r\n")] = '\0';
        if (append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, line) < 0) {
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);
    return 0;
}

int lulod_udev_snapshot_gather(LuloUdevSnapshot *snap, const LuloUdevState *state)
{
    if (!snap) return -1;
    memset(snap, 0, sizeof(*snap));
    if (gather_config_rows(snap) < 0) {
        lulo_udev_snapshot_free(snap);
        return -1;
    }
    lulo_udev_snapshot_mark_loading(snap, state);
    return 0;
}

int lulod_udev_snapshot_refresh_active(LuloUdevSnapshot *snap, const LuloUdevState *state)
{
    if (!snap || !state) return -1;

    switch (state->view) {
    case LULO_UDEV_VIEW_HWDB: {
        const LuloUdevConfigRow *row = selected_hwdb(snap, state);

        if (!row) {
            lulo_udev_snapshot_mark_loading(snap, state);
            return 0;
        }
        return fill_rule_preview(snap, row);
    }
    case LULO_UDEV_VIEW_DEVICES: {
        const LuloUdevDeviceRow *row = selected_device(snap, state);

        if (!row) {
            lulo_udev_snapshot_mark_loading(snap, state);
            return 0;
        }
        return fill_device_preview(snap, row);
    }
    case LULO_UDEV_VIEW_RULES:
    default: {
        const LuloUdevConfigRow *row = selected_rule(snap, state);

        if (!row) {
            lulo_udev_snapshot_mark_loading(snap, state);
            return 0;
        }
        return fill_rule_preview(snap, row);
    }
    }
}
