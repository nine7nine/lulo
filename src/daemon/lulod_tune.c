#define _GNU_SOURCE

#include "lulod_tune.h"
#include "lulod_admin.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TUNE_DETAIL_LIMIT 256

typedef struct {
    LuloTuneSource source;
    const char *label;
    const char *path;
} ExplorerRoot;

static const ExplorerRoot tune_roots[] = {
    { LULO_TUNE_SOURCE_PROC, "proc/sys", "/proc/sys" },
    { LULO_TUNE_SOURCE_SYS, "sys", "/sys" },
    { LULO_TUNE_SOURCE_CGROUP, "cgroup", "/sys/fs/cgroup" },
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

static int append_linef(char ***lines, int *count, const char *fmt, ...)
{
    char buf[768];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return append_snapshot_line(lines, count, buf);
}

static void clear_lines(char ***lines, int *count)
{
    if (!lines || !count) return;
    for (int i = 0; i < *count; i++) free((*lines)[i]);
    free(*lines);
    *lines = NULL;
    *count = 0;
}

static void trim_right(char *buf)
{
    size_t len;

    if (!buf) return;
    len = strlen(buf);
    while (len > 0 && isspace((unsigned char)buf[len - 1])) buf[--len] = '\0';
}

static void sanitize_inline_text(char *buf)
{
    if (!buf) return;
    for (char *p = buf; *p; ++p) {
        if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
    }
    trim_right(buf);
}

static void copy_capped(char *dst, size_t len, const char *src)
{
    size_t n;

    if (!dst || len == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= len) n = len - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int join_path2(char *buf, size_t len, const char *a, const char *b, const char *suffix)
{
    size_t alen;
    size_t blen;
    size_t slen;

    if (!buf || len == 0 || !a || !b) return -1;
    alen = strlen(a);
    blen = strlen(b);
    slen = suffix ? strlen(suffix) : 0;
    if (alen + 1 + blen + slen + 1 > len) return -1;
    memcpy(buf, a, alen);
    buf[alen] = '/';
    memcpy(buf + alen + 1, b, blen);
    if (slen > 0) memcpy(buf + alen + 1 + blen, suffix, slen);
    buf[alen + 1 + blen + slen] = '\0';
    return 0;
}

static const ExplorerRoot *root_from_path(const char *path)
{
    if (!path || !*path) return NULL;
    for (size_t i = 0; i < sizeof(tune_roots) / sizeof(tune_roots[0]); i++) {
        size_t len = strlen(tune_roots[i].path);
        if (!strncmp(path, tune_roots[i].path, len) &&
            (path[len] == '\0' || path[len] == '/')) {
            return &tune_roots[i];
        }
    }
    return NULL;
}

static int ensure_parent_dirs(const char *path)
{
    char tmp[PATH_MAX];
    size_t len;

    if (!path) return -1;
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len == 0 || len >= sizeof(tmp)) return -1;
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = '\0';
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
        tmp[i] = '/';
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
    return 0;
}

static int tune_data_root(char *buf, size_t len)
{
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");

    if (!buf || len == 0) return -1;
    if (xdg && *xdg) {
        if (join_path2(buf, len, xdg, "lulod", "/tunables") < 0) return -1;
    } else if (home && *home) {
        char base[PATH_MAX];
        if (join_path2(base, sizeof(base), home, ".local", "/share") < 0) return -1;
        if (join_path2(buf, len, base, "lulod", "/tunables") < 0) return -1;
    }
    else return -1;
    return 0;
}

static int tune_bundle_dir(char *buf, size_t len, int preset)
{
    char root[PATH_MAX];

    if (tune_data_root(root, sizeof(root)) < 0) return -1;
    return join_path2(buf, len, root, preset ? "presets" : "snapshots", NULL);
}

static int append_bundle_meta(LuloTuneBundleMeta **items, int *count,
                              const char *id, const char *name,
                              const char *created, int item_count)
{
    LuloTuneBundleMeta *next;
    LuloTuneBundleMeta *item;

    next = realloc(*items, (size_t)(*count + 1) * sizeof(*next));
    if (!next) return -1;
    *items = next;
    item = &next[(*count)++];
    memset(item, 0, sizeof(*item));
    snprintf(item->id, sizeof(item->id), "%s", id ? id : "");
    snprintf(item->name, sizeof(item->name), "%s", name ? name : item->id);
    snprintf(item->created, sizeof(item->created), "%s", created ? created : "");
    item->item_count = item_count;
    return 0;
}

static int read_text_file_line(const char *path, char *buf, size_t len)
{
    FILE *fp;

    if (!path || !buf || len == 0) return -1;
    buf[0] = '\0';
    fp = fopen(path, "r");
    if (!fp) return -1;
    if (!fgets(buf, (int)len, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    sanitize_inline_text(buf);
    return 0;
}

static int append_tune_row(LuloTuneSnapshot *snap, const char *path, const char *name,
                           const char *group, const char *value,
                           int writable, int is_dir, LuloTuneSource source)
{
    LuloTuneRow *next;
    LuloTuneRow *row;

    next = realloc(snap->rows, (size_t)(snap->count + 1) * sizeof(*next));
    if (!next) return -1;
    snap->rows = next;
    row = &snap->rows[snap->count++];
    memset(row, 0, sizeof(*row));
    snprintf(row->path, sizeof(row->path), "%s", path ? path : "");
    snprintf(row->name, sizeof(row->name), "%s", name ? name : "");
    snprintf(row->group, sizeof(row->group), "%s", group ? group : "");
    snprintf(row->value, sizeof(row->value), "%s", value ? value : "");
    row->writable = writable;
    row->is_dir = is_dir;
    row->source = source;
    return 0;
}

static int tune_row_cmp(const void *a, const void *b)
{
    const LuloTuneRow *ra = a;
    const LuloTuneRow *rb = b;

    if (!strcmp(ra->name, "..")) return -1;
    if (!strcmp(rb->name, "..")) return 1;
    if (ra->is_dir != rb->is_dir) return rb->is_dir - ra->is_dir;
    return strcmp(ra->name, rb->name);
}

static int bundle_meta_cmp(const void *a, const void *b)
{
    const LuloTuneBundleMeta *ma = a;
    const LuloTuneBundleMeta *mb = b;
    int rc = strcmp(mb->created, ma->created);

    if (rc != 0) return rc;
    return strcmp(mb->id, ma->id);
}

static const char *tune_note_for_path(const char *path)
{
    if (!path) return NULL;
    if (strstr(path, "/proc/sys/vm/swappiness")) return "Balance between reclaiming page cache and swapping anonymous memory.";
    if (strstr(path, "/proc/sys/vm/dirty_ratio")) return "Upper dirty-memory threshold before processes start writeback throttling.";
    if (strstr(path, "/proc/sys/vm/dirty_background_ratio")) return "Background flusher threshold for dirty memory.";
    if (strstr(path, "/proc/sys/kernel/sched_autogroup_enabled")) return "Desktop scheduling grouping for interactive workloads.";
    if (strstr(path, "/proc/sys/kernel/nmi_watchdog")) return "Kernel hard-lockup watchdog overhead and detection control.";
    if (strstr(path, "/transparent_hugepage/enabled")) return "Anonymous Transparent Huge Page policy.";
    if (strstr(path, "/transparent_hugepage/defrag")) return "THP compaction/defragmentation aggressiveness.";
    if (strstr(path, "/cpufreq/") && strstr(path, "scaling_governor")) return "CPU frequency governor for the policy domain.";
    if (strstr(path, "/cpufreq/") && strstr(path, "energy_performance_preference")) return "Energy-vs-performance preference hint for the policy.";
    if (strstr(path, "/sys/block/") && strstr(path, "/queue/scheduler")) return "Block I/O scheduler for this device queue.";
    if (strstr(path, "/sys/block/") && strstr(path, "/queue/read_ahead_kb")) return "Kernel read-ahead window in KiB.";
    if (strstr(path, "/sys/block/") && strstr(path, "/queue/nr_requests")) return "Maximum queued block requests per hardware queue.";
    if (strstr(path, "/sys/fs/cgroup/cpu.max")) return "Cgroup-wide CPU bandwidth quota and period.";
    if (strstr(path, "/sys/fs/cgroup/cpu.weight")) return "Relative CPU weight for the cgroup.";
    if (strstr(path, "/sys/fs/cgroup/memory.high")) return "Soft memory pressure threshold for the cgroup.";
    if (strstr(path, "/sys/fs/cgroup/memory.max")) return "Hard memory limit for the cgroup.";
    return NULL;
}

static int append_file_preview_lines(char ***lines, int *count, const char *path)
{
    FILE *fp;
    char *line = NULL;
    size_t cap = 0;
    int lines_added = 0;

    if (!path || !*path) return 0;
    fp = fopen(path, "r");
    if (!fp) return append_linef(lines, count, "read error: %s", strerror(errno));
    while (getline(&line, &cap, fp) > 0) {
        trim_right(line);
        if (append_snapshot_line(lines, count, line) < 0) {
            free(line);
            fclose(fp);
            return -1;
        }
        lines_added++;
        if (*count >= TUNE_DETAIL_LIMIT) break;
    }
    free(line);
    fclose(fp);
    if (lines_added == 0) return append_snapshot_line(lines, count, "(empty)");
    return 0;
}

static int group_label_for_entry(char *buf, size_t len, const ExplorerRoot *root, const char *dirpath)
{
    const char *rel;

    if (!buf || len == 0 || !root || !dirpath) return -1;
    if (!strcmp(dirpath, root->path)) {
        snprintf(buf, len, "%s", root->label);
        return 0;
    }
    rel = dirpath + strlen(root->path);
    while (*rel == '/') rel++;
    if (!*rel) snprintf(buf, len, "%s", root->label);
    else snprintf(buf, len, "%s/%s", root->label, rel);
    return 0;
}

static int gather_root_rows(LuloTuneSnapshot *snap)
{
    for (size_t i = 0; i < sizeof(tune_roots) / sizeof(tune_roots[0]); i++) {
        if (append_tune_row(snap, tune_roots[i].path, tune_roots[i].label,
                            "root", tune_roots[i].path, 0, 1, tune_roots[i].source) < 0) {
            return -1;
        }
    }
    return 0;
}

static int gather_directory_rows(LuloTuneSnapshot *snap, const char *dirpath)
{
    DIR *dir;
    struct dirent *ent;
    const ExplorerRoot *root = root_from_path(dirpath);
    char group[160];

    if (!root) return -1;
    dir = opendir(dirpath);
    if (!dir) return -1;
    group_label_for_entry(group, sizeof(group), root, dirpath);
    {
        char parent[PATH_MAX];
        char *slash;

        if (strcmp(dirpath, root->path) == 0) {
            parent[0] = '\0';
        } else {
            snprintf(parent, sizeof(parent), "%s", dirpath);
            slash = strrchr(parent, '/');
            if (slash && slash != parent) *slash = '\0';
            else snprintf(parent, sizeof(parent), "%s", root->path);
        }
        if (append_tune_row(snap, parent, "..", group, "<up>", 0, 1, root->source) < 0) {
            closedir(dir);
            return -1;
        }
    }

    while ((ent = readdir(dir)) != NULL) {
        char full[PATH_MAX];
        struct stat st;

        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        snprintf(full, sizeof(full), "%s/%s", dirpath, ent->d_name);
        if (stat(full, &st) < 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (append_tune_row(snap, full, ent->d_name, group, "<dir>", 0, 1, root->source) < 0) {
                closedir(dir);
                return -1;
            }
        } else if (S_ISREG(st.st_mode)) {
            char value[192];
            if (read_text_file_line(full, value, sizeof(value)) < 0) snprintf(value, sizeof(value), "%s", "(unreadable)");
            if (append_tune_row(snap, full, ent->d_name, group, value,
                                access(full, W_OK) == 0, 0, root->source) < 0) {
                closedir(dir);
                return -1;
            }
        }
    }
    closedir(dir);
    if (snap->count > 1) qsort(snap->rows, (size_t)snap->count, sizeof(*snap->rows), tune_row_cmp);
    return 0;
}

static int load_bundle_meta_dir(LuloTuneBundleMeta **items, int *count, int preset)
{
    char dirpath[PATH_MAX];
    DIR *dir;
    struct dirent *ent;

    if (tune_bundle_dir(dirpath, sizeof(dirpath), preset) < 0) return -1;
    if (ensure_parent_dirs(dirpath) < 0) return -1;
    dir = opendir(dirpath);
    if (!dir) return 0;
    while ((ent = readdir(dir)) != NULL) {
        char path[PATH_MAX];
        FILE *fp;
        char line[512];
        char id[96] = {0};
        char name[96] = {0};
        char created[32] = {0};
        int item_count = 0;
        size_t nlen;

        if (ent->d_name[0] == '.') continue;
        nlen = strlen(ent->d_name);
        if (nlen < 7 || strcmp(ent->d_name + nlen - 6, ".ltune") != 0) continue;
        if (join_path2(path, sizeof(path), dirpath, ent->d_name, NULL) < 0) continue;
        fp = fopen(path, "r");
        if (!fp) continue;
        while (fgets(line, sizeof(line), fp)) {
            trim_right(line);
            if (!strncmp(line, "id=", 3)) copy_capped(id, sizeof(id), line + 3);
            else if (!strncmp(line, "name=", 5)) copy_capped(name, sizeof(name), line + 5);
            else if (!strncmp(line, "created=", 8)) copy_capped(created, sizeof(created), line + 8);
            else if (!strncmp(line, "count=", 6)) item_count = atoi(line + 6);
            if (id[0] && name[0] && created[0] && item_count > 0) break;
        }
        fclose(fp);
        if (!id[0]) snprintf(id, sizeof(id), "%.*s", (int)(nlen - 6), ent->d_name);
        if (!name[0]) snprintf(name, sizeof(name), "%s", id);
        if (append_bundle_meta(items, count, id, name, created, item_count) < 0) {
            closedir(dir);
            return -1;
        }
    }
    closedir(dir);
    if (*count > 1) qsort(*items, (size_t)*count, sizeof(**items), bundle_meta_cmp);
    return 0;
}

static int bundle_path_from_id(char *buf, size_t len, int preset, const char *id)
{
    char dirpath[PATH_MAX];

    if (!id || !*id) return -1;
    if (tune_bundle_dir(dirpath, sizeof(dirpath), preset) < 0) return -1;
    return join_path2(buf, len, dirpath, id, ".ltune");
}

static int active_explore_row(const LuloTuneSnapshot *snap, const LuloTuneState *state)
{
    if (!snap || !state || snap->count <= 0) return -1;
    if (state->selected_path[0]) {
        for (int i = 0; i < snap->count; i++) {
            if (strcmp(snap->rows[i].path, state->selected_path) == 0) return i;
        }
    }
    if (state->selected >= 0 && state->selected < snap->count) return state->selected;
    if (state->cursor >= 0 && state->cursor < snap->count) return state->cursor;
    return -1;
}

static int selected_bundle_index(const LuloTuneBundleMeta *items, int count,
                                 const char *selected_id, int selected_index)
{
    if (!items || count <= 0) return -1;
    if (selected_id && *selected_id) {
        for (int i = 0; i < count; i++) {
            if (strcmp(items[i].id, selected_id) == 0) return i;
        }
    }
    if (selected_index >= 0 && selected_index < count) return selected_index;
    return -1;
}

static int write_current_bundle(const LuloTuneSnapshot *snap, const LuloTuneState *state, int preset)
{
    char dirpath[PATH_MAX];
    char path[PATH_MAX];
    char id[96];
    char created[32];
    char value[192];
    time_t now;
    struct tm tm;
    FILE *fp;
    int idx;
    const LuloTuneRow *row;

    if (!snap || !state) return -1;
    idx = active_explore_row(snap, state);
    if (idx < 0 || snap->rows[idx].is_dir) return -1;
    row = &snap->rows[idx];
    if (tune_bundle_dir(dirpath, sizeof(dirpath), preset) < 0) return -1;
    if (ensure_parent_dirs(dirpath) < 0) return -1;
    now = time(NULL);
    localtime_r(&now, &tm);
    strftime(id, sizeof(id), "%Y%m%d-%H%M%S", &tm);
    strftime(created, sizeof(created), "%Y-%m-%d %H:%M:%S", &tm);
    if (join_path2(path, sizeof(path), dirpath, id, ".ltune") < 0) return -1;
    fp = fopen(path, "w");
    if (!fp) return -1;
    if (state->staged_path[0] && strcmp(state->staged_path, row->path) == 0) {
        snprintf(value, sizeof(value), "%s", state->staged_value);
    } else if (read_text_file_line(row->path, value, sizeof(value)) < 0) {
        snprintf(value, sizeof(value), "%s", row->value);
    }
    sanitize_inline_text(value);
    fprintf(fp, "type=%s\n", preset ? "preset" : "snapshot");
    fprintf(fp, "id=%s\n", id);
    fprintf(fp, "name=%s-%s\n", preset ? "preset" : "snapshot", id);
    fprintf(fp, "created=%s\n", created);
    fprintf(fp, "count=1\n\n");
    fprintf(fp, "%d\t%d\t%s\t%s\t%s\t%s\n",
            (int)row->source, row->writable, row->path, row->name, row->group, value);
    fclose(fp);
    return 0;
}

static int load_bundle_preview(LuloTuneSnapshot *snap, int preset, const char *id)
{
    char path[PATH_MAX];
    FILE *fp;
    char line[768];
    char name[96] = {0};
    char created[32] = {0};
    char type_name[32] = {0};
    int after_header = 0;

    if (!snap || bundle_path_from_id(path, sizeof(path), preset, id) < 0) return -1;
    fp = fopen(path, "r");
    if (!fp) {
        snprintf(snap->detail_title, sizeof(snap->detail_title), "%s", preset ? "preset" : "snapshot");
        snprintf(snap->detail_status, sizeof(snap->detail_status), "failed to open");
        return append_linef(&snap->detail_lines, &snap->detail_line_count, "open error: %s", strerror(errno));
    }
    while (fgets(line, sizeof(line), fp)) {
        trim_right(line);
        if (!line[0]) {
            after_header = 1;
            break;
        }
        if (!strncmp(line, "type=", 5)) copy_capped(type_name, sizeof(type_name), line + 5);
        else if (!strncmp(line, "name=", 5)) copy_capped(name, sizeof(name), line + 5);
        else if (!strncmp(line, "created=", 8)) copy_capped(created, sizeof(created), line + 8);
    }

    snprintf(snap->detail_title, sizeof(snap->detail_title), "%s", name[0] ? name : id);
    snprintf(snap->detail_status, sizeof(snap->detail_status), "%s  %s",
             type_name[0] ? type_name : (preset ? "preset" : "snapshot"),
             created[0] ? created : "");
    if (append_linef(&snap->detail_lines, &snap->detail_line_count, "id: %s", id) < 0 ||
        append_linef(&snap->detail_lines, &snap->detail_line_count, "created: %s",
                     created[0] ? created : "(unknown)") < 0 ||
        append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, "") < 0) {
        fclose(fp);
        return -1;
    }
    if (!after_header) {
        fclose(fp);
        return append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, "(empty bundle)");
    }
    while (fgets(line, sizeof(line), fp)) {
        char rendered[768];
        char *save = NULL;
        char *field;
        int source = 0;
        int writable = 0;
        const char *path_field = "";
        const char *group_field = "";
        const char *value_field = "";

        trim_right(line);
        if (!line[0]) continue;
        field = strtok_r(line, "\t", &save);
        if (field) source = atoi(field);
        field = strtok_r(NULL, "\t", &save);
        if (field) writable = atoi(field);
        field = strtok_r(NULL, "\t", &save);
        if (field) path_field = field;
        field = strtok_r(NULL, "\t", &save);
        field = strtok_r(NULL, "\t", &save);
        if (field) group_field = field;
        field = strtok_r(NULL, "\t", &save);
        if (field) value_field = field;
        snprintf(rendered, sizeof(rendered), "[%s] %-3s %-20s %s = %s",
                 lulo_tune_source_name((LuloTuneSource)source),
                 writable ? "rw" : "ro", group_field, path_field, value_field);
        if (append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, rendered) < 0) {
            fclose(fp);
            return -1;
        }
        if (snap->detail_line_count >= TUNE_DETAIL_LIMIT) break;
    }
    fclose(fp);
    return 0;
}

static const char *selected_bundle_id(const LuloTuneSnapshot *snap, const LuloTuneState *state, int *preset_out)
{
    if (!state) return NULL;
    if (state->view == LULO_TUNE_VIEW_SNAPSHOTS) {
        if (preset_out) *preset_out = 0;
        if (state->selected_snapshot_id[0]) return state->selected_snapshot_id;
        if (snap && state->snapshot_cursor >= 0 && state->snapshot_cursor < snap->snapshot_count) {
            return snap->snapshots[state->snapshot_cursor].id;
        }
        return NULL;
    }
    if (state->view == LULO_TUNE_VIEW_PRESETS) {
        if (preset_out) *preset_out = 1;
        if (state->selected_preset_id[0]) return state->selected_preset_id;
        if (snap && state->preset_cursor >= 0 && state->preset_cursor < snap->preset_count) {
            return snap->presets[state->preset_cursor].id;
        }
        return NULL;
    }
    return NULL;
}

static int load_selected_bundle_plan(const LuloTuneSnapshot *snap, const LuloTuneState *state,
                                     LuloAdminTunePlan *plan,
                                     char *err, size_t errlen)
{
    char path[PATH_MAX];
    char line[768];
    const char *id;
    FILE *fp;
    int preset = 0;
    int after_header = 0;

    if (err && errlen > 0) err[0] = '\0';
    id = selected_bundle_id(snap, state, &preset);
    if (!id || !*id) {
        if (err && errlen > 0) snprintf(err, errlen, "select a snapshot or preset to apply");
        return -1;
    }
    if (bundle_path_from_id(path, sizeof(path), preset, id) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to resolve bundle path");
        return -1;
    }
    fp = fopen(path, "r");
    if (!fp) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to open %s: %s", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof(line), fp)) {
        trim_right(line);
        if (!line[0]) {
            after_header = 1;
            break;
        }
    }
    if (!after_header) {
        fclose(fp);
        if (err && errlen > 0) snprintf(err, errlen, "%s is empty", path);
        return -1;
    }
    while (fgets(line, sizeof(line), fp)) {
        char *save = NULL;
        char *field;
        const char *path_field = NULL;
        const char *value_field = NULL;

        trim_right(line);
        if (!line[0]) continue;
        field = strtok_r(line, "\t", &save);
        if (!field) continue;
        field = strtok_r(NULL, "\t", &save);
        if (!field) continue;
        field = strtok_r(NULL, "\t", &save);
        if (field) path_field = field;
        field = strtok_r(NULL, "\t", &save);
        field = strtok_r(NULL, "\t", &save);
        field = strtok_r(NULL, "\t", &save);
        if (field) value_field = field;
        if (!path_field || !value_field) continue;
        if (lulo_admin_tune_plan_append(plan, path_field, value_field) < 0) {
            fclose(fp);
            if (err && errlen > 0) snprintf(err, errlen, "failed to build apply plan");
            return -1;
        }
    }
    fclose(fp);
    if (plan->count <= 0) {
        if (err && errlen > 0) snprintf(err, errlen, "%s does not contain any tunables", path);
        return -1;
    }
    return 0;
}

static int refresh_explore_detail(LuloTuneSnapshot *snap, const LuloTuneState *state)
{
    int idx;
    const LuloTuneRow *row;
    char value[192];
    const char *note;
    int have_value = 0;

    idx = active_explore_row(snap, state);
    if (idx < 0) {
        clear_lines(&snap->detail_lines, &snap->detail_line_count);
        snprintf(snap->detail_title, sizeof(snap->detail_title), "tunable");
        snprintf(snap->detail_status, sizeof(snap->detail_status), "select a file");
        return 0;
    }
    row = &snap->rows[idx];
    clear_lines(&snap->detail_lines, &snap->detail_line_count);
    snprintf(snap->detail_title, sizeof(snap->detail_title), "%s", row->name);
    snprintf(snap->detail_status, sizeof(snap->detail_status), "%s  %s",
             row->writable ? "writable" : "read-only", row->group);
    if (row->is_dir) {
        return append_linef(&snap->detail_lines, &snap->detail_line_count, "directory: %s", row->path);
    }
    if (append_linef(&snap->detail_lines, &snap->detail_line_count, "path: %s", row->path) < 0 ||
        append_linef(&snap->detail_lines, &snap->detail_line_count, "source: %s", lulo_tune_source_name(row->source)) < 0 ||
        append_linef(&snap->detail_lines, &snap->detail_line_count, "group: %s", row->group) < 0 ||
        append_linef(&snap->detail_lines, &snap->detail_line_count, "access: %s",
                     row->writable ? "writable" : "read-only") < 0) {
        return -1;
    }
    if (state->staged_path[0] && strcmp(state->staged_path, row->path) == 0) {
        if (append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, "") < 0 ||
            append_linef(&snap->detail_lines, &snap->detail_line_count, "staged value: %s",
                         state->staged_value[0] ? state->staged_value : "(empty)") < 0) {
            return -1;
        }
    }
    note = tune_note_for_path(row->path);
    if (note && *note) {
        if (append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, "") < 0 ||
            append_linef(&snap->detail_lines, &snap->detail_line_count, "note: %s", note) < 0) {
            return -1;
        }
    }
    if (append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, "") < 0 ||
        append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, "current value:") < 0) {
        return -1;
    }
    have_value = read_text_file_line(row->path, value, sizeof(value)) == 0;
    if (have_value) snprintf(snap->rows[idx].value, sizeof(snap->rows[idx].value), "%s", value);
    else snprintf(value, sizeof(value), "%s", "(unreadable)");
    if (append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, value) < 0) {
        return -1;
    }
    if (append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, "") < 0 ||
        append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, "raw file:") < 0) {
        return -1;
    }
    return append_file_preview_lines(&snap->detail_lines, &snap->detail_line_count, row->path);
}

static int refresh_bundle_detail(LuloTuneSnapshot *snap, const LuloTuneState *state, int preset)
{
    int idx;
    const LuloTuneBundleMeta *items = preset ? snap->presets : snap->snapshots;
    int count = preset ? snap->preset_count : snap->snapshot_count;
    const char *selected_id = preset ? state->selected_preset_id : state->selected_snapshot_id;
    int selected_index = preset ? state->preset_selected : state->snapshot_selected;

    idx = selected_bundle_index(items, count, selected_id, selected_index);
    clear_lines(&snap->detail_lines, &snap->detail_line_count);
    if (idx < 0) {
        snprintf(snap->detail_title, sizeof(snap->detail_title), "%s", preset ? "preset" : "snapshot");
        snprintf(snap->detail_status, sizeof(snap->detail_status), "select a saved config");
        return 0;
    }
    return load_bundle_preview(snap, preset, items[idx].id);
}

int lulod_tune_snapshot_refresh_active(LuloTuneSnapshot *snap, const LuloTuneState *state)
{
    if (!snap || !state) return -1;
    switch (state->view) {
    case LULO_TUNE_VIEW_SNAPSHOTS:
        return refresh_bundle_detail(snap, state, 0);
    case LULO_TUNE_VIEW_PRESETS:
        return refresh_bundle_detail(snap, state, 1);
    case LULO_TUNE_VIEW_EXPLORE:
    default:
        return refresh_explore_detail(snap, state);
    }
}

int lulod_tune_snapshot_gather(LuloTuneSnapshot *snap, const LuloTuneState *state)
{
    const char *browse = state ? state->browse_path : "";
    const ExplorerRoot *root = root_from_path(browse);

    if (!snap) return -1;
    lulo_tune_snapshot_free(snap);
    if (!browse || !*browse) {
        if (gather_root_rows(snap) < 0) goto fail;
    } else if (!root || gather_directory_rows(snap, browse) < 0) {
        if (gather_root_rows(snap) < 0) goto fail;
    }
    if (load_bundle_meta_dir(&snap->snapshots, &snap->snapshot_count, 0) < 0) goto fail;
    if (load_bundle_meta_dir(&snap->presets, &snap->preset_count, 1) < 0) goto fail;
    return lulod_tune_snapshot_refresh_active(snap, state);

fail:
    lulo_tune_snapshot_free(snap);
    return -1;
}

int lulod_tune_snapshot_save_current(const LuloTuneSnapshot *snap, const LuloTuneState *state, int preset)
{
    return write_current_bundle(snap, state, preset);
}

int lulod_tune_snapshot_apply_current(const LuloTuneSnapshot *snap, const LuloTuneState *state,
                                      char *err, size_t errlen)
{
    LuloAdminTunePlan plan;
    int rc;
    int idx = -1;

    lulo_admin_tune_plan_init(&plan);
    if (state && snap && state->view == LULO_TUNE_VIEW_EXPLORE) idx = active_explore_row(snap, state);
    if (idx >= 0 && state->staged_path[0] &&
        strcmp(state->staged_path, snap->rows[idx].path) == 0) {
        rc = lulo_admin_tune_plan_append(&plan, state->staged_path, state->staged_value);
        if (rc < 0 && err && errlen > 0) snprintf(err, errlen, "failed to build staged apply plan");
    } else {
        rc = load_selected_bundle_plan(snap, state, &plan, err, errlen);
    }
    if (rc == 0) rc = lulod_admin_apply_tune_plan(&plan, err, errlen);
    lulo_admin_tune_plan_free(&plan);
    return rc;
}
