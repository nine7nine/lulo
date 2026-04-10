#define _GNU_SOURCE

#include "lulod_cgroups.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CGROUP_ROOT "/sys/fs/cgroup"
#define CGROUP_DETAIL_LIMIT 256

typedef struct {
    const char *root;
    const char *source;
} ConfigRoot;

static const ConfigRoot config_roots[] = {
    { "/etc/systemd/system", "etc" },
    { "/etc/systemd/user", "etc" },
    { "/etc/systemd/system.control", "etc" },
    { "/etc/systemd/user.control", "etc" },
    { "/usr/lib/systemd/system", "vendor" },
    { "/usr/lib/systemd/user", "vendor" },
    { "/lib/systemd/system", "vendor" },
    { "/lib/systemd/user", "vendor" },
};

static const char *cgroup_directives[] = {
    "AllowedCPUs=",
    "AllowedMemoryNodes=",
    "CPUAccounting=",
    "CPUAffinity=",
    "CPUQuota=",
    "CPUQuotaPeriodSec=",
    "CPUSchedulingPolicy=",
    "CPUSchedulingPriority=",
    "CPUSchedulingResetOnFork=",
    "CPUShares=",
    "CPUWeight=",
    "DefaultCPUAccounting=",
    "DefaultMemoryAccounting=",
    "DefaultTasksAccounting=",
    "Delegate=",
    "IOAccounting=",
    "IODeviceLatencyTargetSec=",
    "IODeviceWeight=",
    "IOReadBandwidthMax=",
    "IOWriteBandwidthMax=",
    "IOWeight=",
    "ManagedOOMMemoryPressure=",
    "ManagedOOMMemoryPressureLimit=",
    "ManagedOOMPreference=",
    "ManagedOOMSwap=",
    "MemoryAccounting=",
    "MemoryHigh=",
    "MemoryLow=",
    "MemoryMax=",
    "MemoryMin=",
    "MemorySwapMax=",
    "StartupCPUWeight=",
    "StartupIOWeight=",
    "StartupMemoryHigh=",
    "StartupMemoryLow=",
    "StartupMemoryMax=",
    "TasksAccounting=",
    "TasksMax=",
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

static void trim_right(char *buf)
{
    size_t len;

    if (!buf) return;
    len = strlen(buf);
    while (len > 0 && isspace((unsigned char)buf[len - 1])) buf[--len] = '\0';
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

static void sanitize_inline_text(char *buf)
{
    if (!buf) return;
    for (char *p = buf; *p; ++p) {
        if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
    }
    trim_right(buf);
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

static int count_file_lines(const char *path)
{
    FILE *fp;
    char buf[512];
    int count = 0;

    fp = fopen(path, "r");
    if (!fp) return -1;
    while (fgets(buf, sizeof(buf), fp)) count++;
    fclose(fp);
    return count;
}

static int count_child_dirs(const char *path)
{
    DIR *dir;
    struct dirent *ent;
    int count = 0;
    char full[PATH_MAX];
    struct stat st;

    dir = opendir(path);
    if (!dir) return -1;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (path_join(full, sizeof(full), path, ent->d_name) < 0) continue;
        if (lstat(full, &st) < 0) continue;
        if (S_ISDIR(st.st_mode)) count++;
    }
    closedir(dir);
    return count;
}

static void last_path_component(const char *path, char *buf, size_t len)
{
    const char *base;

    if (!buf || len == 0) return;
    buf[0] = '\0';
    if (!path || !*path) return;
    base = strrchr(path, '/');
    if (!base || !base[1]) {
        copy_trunc(buf, len, strcmp(path, CGROUP_ROOT) == 0 ? "/" : path);
        return;
    }
    copy_trunc(buf, len, base + 1);
}

static int normalize_browse_path(const LuloCgroupsState *state, char *buf, size_t len)
{
    char resolved[PATH_MAX];

    if (!buf || len == 0) return -1;
    copy_trunc(buf, len, CGROUP_ROOT);
    if (!state || !state->browse_path[0]) return 0;
    if (!realpath(state->browse_path, resolved)) return 0;
    if (strncmp(resolved, CGROUP_ROOT, strlen(CGROUP_ROOT)) != 0) return 0;
    if (strlen(resolved) >= len) return -1;
    copy_trunc(buf, len, resolved);
    return 0;
}

static int append_tree_row(LuloCgroupsSnapshot *snap, const char *path, const char *name,
                           const char *type, const char *controllers,
                           int proc_count, int thread_count, int subgroup_count,
                           int is_parent)
{
    LuloCgroupTreeRow *next;
    LuloCgroupTreeRow *row;

    next = realloc(snap->tree_rows, (size_t)(snap->tree_count + 1) * sizeof(*next));
    if (!next) return -1;
    snap->tree_rows = next;
    row = &next[snap->tree_count++];
    memset(row, 0, sizeof(*row));
    copy_trunc(row->path, sizeof(row->path), path);
    copy_trunc(row->name, sizeof(row->name), name);
    copy_trunc(row->type, sizeof(row->type), type ? type : "-");
    copy_trunc(row->controllers, sizeof(row->controllers), controllers ? controllers : "-");
    row->process_count = proc_count;
    row->thread_count = thread_count;
    row->subgroup_count = subgroup_count;
    row->is_parent = is_parent;
    return 0;
}

static int append_file_row(LuloCgroupsSnapshot *snap, const char *path, const char *name,
                           const char *value, int writable)
{
    LuloCgroupFileRow *next;
    LuloCgroupFileRow *row;

    next = realloc(snap->file_rows, (size_t)(snap->file_count + 1) * sizeof(*next));
    if (!next) return -1;
    snap->file_rows = next;
    row = &next[snap->file_count++];
    memset(row, 0, sizeof(*row));
    copy_trunc(row->path, sizeof(row->path), path);
    copy_trunc(row->name, sizeof(row->name), name);
    copy_trunc(row->value, sizeof(row->value), value);
    row->writable = writable;
    return 0;
}

static int append_config_row(LuloCgroupsSnapshot *snap, const char *path, const char *name,
                             const char *source, const char *kind)
{
    LuloCgroupConfigRow *next;
    LuloCgroupConfigRow *row;

    next = realloc(snap->configs, (size_t)(snap->config_count + 1) * sizeof(*next));
    if (!next) return -1;
    snap->configs = next;
    row = &next[snap->config_count++];
    memset(row, 0, sizeof(*row));
    copy_trunc(row->path, sizeof(row->path), path);
    copy_trunc(row->name, sizeof(row->name), name);
    copy_trunc(row->source, sizeof(row->source), source);
    copy_trunc(row->kind, sizeof(row->kind), kind);
    return 0;
}

static int tree_row_cmp(const void *a, const void *b)
{
    const LuloCgroupTreeRow *ra = a;
    const LuloCgroupTreeRow *rb = b;

    if (ra->is_parent != rb->is_parent) return rb->is_parent - ra->is_parent;
    return strcmp(ra->name, rb->name);
}

static int file_row_cmp(const void *a, const void *b)
{
    const LuloCgroupFileRow *ra = a;
    const LuloCgroupFileRow *rb = b;

    if (ra->writable != rb->writable) return rb->writable - ra->writable;
    return strcmp(ra->name, rb->name);
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
    const LuloCgroupConfigRow *ra = a;
    const LuloCgroupConfigRow *rb = b;
    int sa = config_source_rank(ra->source);
    int sb = config_source_rank(rb->source);

    if (sa != sb) return sa - sb;
    if (strcmp(ra->kind, rb->kind) != 0) return strcmp(ra->kind, rb->kind);
    return strcmp(ra->name, rb->name);
}

static int gather_tree_rows(LuloCgroupsSnapshot *snap, const char *browse_path)
{
    DIR *dir;
    struct dirent *ent;
    char parent_path[PATH_MAX];

    if (!snap || !browse_path) return -1;
    if (strcmp(browse_path, CGROUP_ROOT) != 0) {
        copy_trunc(parent_path, sizeof(parent_path), browse_path);
        {
            char *slash = strrchr(parent_path, '/');

            if (slash && slash > parent_path) *slash = '\0';
            else copy_trunc(parent_path, sizeof(parent_path), CGROUP_ROOT);
        }
        if (append_tree_row(snap, parent_path, "..", "parent", "", -1, -1, -1, 1) < 0) return -1;
    }

    dir = opendir(browse_path);
    if (!dir) return -1;
    while ((ent = readdir(dir)) != NULL) {
        char full[PATH_MAX];
        char type[64] = "-";
        char controllers[192] = "-";
        struct stat st;
        int proc_count;
        int thread_count;
        int subgroup_count;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (path_join(full, sizeof(full), browse_path, ent->d_name) < 0) continue;
        if (lstat(full, &st) < 0 || !S_ISDIR(st.st_mode)) continue;

        {
            char path[PATH_MAX];

            if (path_join(path, sizeof(path), full, "cgroup.type") == 0) {
                read_text_file_line(path, type, sizeof(type));
            }
            if (path_join(path, sizeof(path), full, "cgroup.controllers") == 0) {
                read_text_file_line(path, controllers, sizeof(controllers));
            }
            if (path_join(path, sizeof(path), full, "cgroup.procs") == 0) {
                proc_count = count_file_lines(path);
            } else {
                proc_count = -1;
            }
            if (path_join(path, sizeof(path), full, "cgroup.threads") == 0) {
                thread_count = count_file_lines(path);
            } else {
                thread_count = -1;
            }
        }
        subgroup_count = count_child_dirs(full);
        if (append_tree_row(snap, full, ent->d_name, type, controllers,
                            proc_count, thread_count, subgroup_count, 0) < 0) {
            closedir(dir);
            return -1;
        }
    }
    closedir(dir);
    if (snap->tree_count > 1) {
        qsort(snap->tree_rows, (size_t)snap->tree_count, sizeof(*snap->tree_rows), tree_row_cmp);
    }
    return 0;
}

static int gather_file_rows(LuloCgroupsSnapshot *snap, const char *browse_path)
{
    DIR *dir;
    struct dirent *ent;

    dir = opendir(browse_path);
    if (!dir) return -1;
    while ((ent = readdir(dir)) != NULL) {
        char full[PATH_MAX];
        char value[192] = "";
        struct stat st;
        int writable;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (path_join(full, sizeof(full), browse_path, ent->d_name) < 0) continue;
        if (lstat(full, &st) < 0 || !S_ISREG(st.st_mode)) continue;
        writable = access(full, W_OK) == 0;
        if (read_text_file_line(full, value, sizeof(value)) < 0) {
            snprintf(value, sizeof(value), "%s", writable ? "(write-only)" : "(unreadable)");
        }
        if (append_file_row(snap, full, ent->d_name, value, writable) < 0) {
            closedir(dir);
            return -1;
        }
    }
    closedir(dir);
    if (snap->file_count > 1) {
        qsort(snap->file_rows, (size_t)snap->file_count, sizeof(*snap->file_rows), file_row_cmp);
    }
    return 0;
}

static int file_has_cgroup_directive(const char *path)
{
    FILE *fp;
    char line[768];

    fp = fopen(path, "r");
    if (!fp) return 0;
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;

        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == ';' || *p == '\0' || *p == '\n') continue;
        for (size_t i = 0; i < sizeof(cgroup_directives) / sizeof(cgroup_directives[0]); i++) {
            size_t len = strlen(cgroup_directives[i]);

            if (strncmp(p, cgroup_directives[i], len) == 0) {
                fclose(fp);
                return 1;
            }
        }
    }
    fclose(fp);
    return 0;
}

static const char *config_kind_for_name(const char *path, const char *name)
{
    size_t len = name ? strlen(name) : 0;

    if (path && strstr(path, ".d/")) return "dropin";
    if (len >= 6 && strcmp(name + len - 6, ".slice") == 0) return "slice";
    if (len >= 8 && strcmp(name + len - 8, ".service") == 0) return "service";
    if (len >= 6 && strcmp(name + len - 6, ".scope") == 0) return "scope";
    if (len >= 5 && strcmp(name + len - 5, ".conf") == 0) return "conf";
    return "unit";
}

static int should_include_config_file(const char *root, const char *full, const char *name)
{
    size_t len;

    (void)root;
    if (!name) return 0;
    len = strlen(name);
    if (len >= 6 && strcmp(name + len - 6, ".slice") == 0) return 1;
    if (len >= 5 && strcmp(name + len - 5, ".conf") == 0 && strstr(full, ".d/")) {
        return file_has_cgroup_directive(full);
    }
    if (len >= 8 && strcmp(name + len - 8, ".service") == 0) return file_has_cgroup_directive(full);
    if (len >= 6 && strcmp(name + len - 6, ".scope") == 0) return file_has_cgroup_directive(full);
    return 0;
}

static int scan_config_dir(LuloCgroupsSnapshot *snap, const char *root, const char *source,
                           const char *path, int depth)
{
    DIR *dir;
    struct dirent *ent;

    if (depth > 8) return 0;
    dir = opendir(path);
    if (!dir) return 0;
    while ((ent = readdir(dir)) != NULL) {
        char full[PATH_MAX];
        char rel[320];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (path_join(full, sizeof(full), path, ent->d_name) < 0) continue;
        if (lstat(full, &st) < 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (scan_config_dir(snap, root, source, full, depth + 1) < 0) {
                closedir(dir);
                return -1;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;
        if (!should_include_config_file(root, full, ent->d_name)) continue;
        if (strncmp(full, root, strlen(root)) == 0 && full[strlen(root)] == '/') {
            copy_trunc(rel, sizeof(rel), full + strlen(root) + 1);
        } else {
            copy_trunc(rel, sizeof(rel), ent->d_name);
        }
        if (append_config_row(snap, full, rel, source, config_kind_for_name(full, ent->d_name)) < 0) {
            closedir(dir);
            return -1;
        }
    }
    closedir(dir);
    return 0;
}

static int ensure_configs_loaded(LuloCgroupsSnapshot *snap)
{
    if (!snap) return -1;
    if (snap->configs_loaded) return 0;
    for (size_t i = 0; i < sizeof(config_roots) / sizeof(config_roots[0]); i++) {
        scan_config_dir(snap, config_roots[i].root, config_roots[i].source, config_roots[i].root, 0);
    }
    if (snap->config_count > 1) {
        qsort(snap->configs, (size_t)snap->config_count, sizeof(*snap->configs), config_row_cmp);
    }
    snap->configs_loaded = 1;
    return 0;
}

static int selected_tree_index(const LuloCgroupsSnapshot *snap, const LuloCgroupsState *state)
{
    if (!snap || !state || snap->tree_count <= 0) return -1;
    if (state->selected_tree_path[0]) {
        for (int i = 0; i < snap->tree_count; i++) {
            if (strcmp(snap->tree_rows[i].path, state->selected_tree_path) == 0) return i;
        }
    }
    if (state->tree_cursor >= 0 && state->tree_cursor < snap->tree_count) return state->tree_cursor;
    return 0;
}

static int selected_file_index(const LuloCgroupsSnapshot *snap, const LuloCgroupsState *state)
{
    if (!snap || !state || snap->file_count <= 0) return -1;
    if (state->selected_file_path[0]) {
        for (int i = 0; i < snap->file_count; i++) {
            if (strcmp(snap->file_rows[i].path, state->selected_file_path) == 0) return i;
        }
    }
    if (state->file_cursor >= 0 && state->file_cursor < snap->file_count) return state->file_cursor;
    return 0;
}

static int selected_config_index(const LuloCgroupsSnapshot *snap, const LuloCgroupsState *state)
{
    if (!snap || !state || snap->config_count <= 0) return -1;
    if (state->selected_config[0]) {
        for (int i = 0; i < snap->config_count; i++) {
            if (strcmp(snap->configs[i].path, state->selected_config) == 0) return i;
        }
    }
    if (state->config_cursor >= 0 && state->config_cursor < snap->config_count) return state->config_cursor;
    return 0;
}

static int append_file_preview_lines(char ***lines, int *count, const char *path)
{
    FILE *fp;
    char *line = NULL;
    size_t cap = 0;
    int added = 0;

    fp = fopen(path, "r");
    if (!fp) return append_linef(lines, count, "read error: %s", strerror(errno));
    while (getline(&line, &cap, fp) > 0) {
        trim_right(line);
        if (append_snapshot_line(lines, count, line) < 0) {
            free(line);
            fclose(fp);
            return -1;
        }
        added++;
        if (*count >= CGROUP_DETAIL_LIMIT) break;
    }
    free(line);
    fclose(fp);
    if (added == 0) return append_snapshot_line(lines, count, "(empty)");
    return 0;
}

static int append_cgroup_members(char ***lines, int *count, const char *path)
{
    FILE *fp;
    char procs_path[PATH_MAX];
    char buf[128];
    int shown = 0;

    if (path_join(procs_path, sizeof(procs_path), path, "cgroup.procs") < 0) return 0;
    fp = fopen(procs_path, "r");
    if (!fp) return 0;
    if (append_snapshot_line(lines, count, "") < 0) {
        fclose(fp);
        return -1;
    }
    if (append_snapshot_line(lines, count, "members:") < 0) {
        fclose(fp);
        return -1;
    }
    while (fgets(buf, sizeof(buf), fp) && shown < 12) {
        char line[192];
        char comm_path[PATH_MAX];
        char comm[96] = "";
        pid_t pid;

        trim_right(buf);
        pid = (pid_t)strtol(buf, NULL, 10);
        snprintf(comm_path, sizeof(comm_path), "/proc/%ld/comm", (long)pid);
        if (read_text_file_line(comm_path, comm, sizeof(comm)) < 0) comm[0] = '\0';
        if (comm[0]) snprintf(line, sizeof(line), "%ld  %.160s", (long)pid, comm);
        else snprintf(line, sizeof(line), "%ld", (long)pid);
        if (append_snapshot_line(lines, count, line) < 0) {
            fclose(fp);
            return -1;
        }
        shown++;
    }
    fclose(fp);
    return 0;
}

static void set_detail_idle(LuloCgroupsSnapshot *snap, const LuloCgroupsState *state)
{
    clear_lines(&snap->detail_lines, &snap->detail_line_count);
    switch (state ? state->view : LULO_CGROUPS_VIEW_TREE) {
    case LULO_CGROUPS_VIEW_FILES:
        snprintf(snap->detail_title, sizeof(snap->detail_title), "cgroup file");
        snprintf(snap->detail_status, sizeof(snap->detail_status), "select a file");
        break;
    case LULO_CGROUPS_VIEW_CONFIG:
        snprintf(snap->detail_title, sizeof(snap->detail_title), "cgroup config");
        snprintf(snap->detail_status, sizeof(snap->detail_status), "select a config");
        break;
    case LULO_CGROUPS_VIEW_TREE:
    default:
        snprintf(snap->detail_title, sizeof(snap->detail_title), "cgroup");
        snprintf(snap->detail_status, sizeof(snap->detail_status), "select a cgroup");
        break;
    }
}

static int refresh_tree_preview(LuloCgroupsSnapshot *snap, const LuloCgroupsState *state)
{
    char path[PATH_MAX];
    char name[128];
    char type[64] = "-";
    char controllers[192] = "-";
    char subtree[192] = "-";
    char file[PATH_MAX];
    int proc_count = -1;
    int thread_count = -1;
    int subgroup_count = -1;
    int idx;

    if (!snap) return -1;
    clear_lines(&snap->detail_lines, &snap->detail_line_count);
    idx = selected_tree_index(snap, state);
    if (idx >= 0 && idx < snap->tree_count) {
        copy_trunc(path, sizeof(path), snap->tree_rows[idx].path);
        copy_trunc(name, sizeof(name), snap->tree_rows[idx].name);
        copy_trunc(type, sizeof(type), snap->tree_rows[idx].type);
        copy_trunc(controllers, sizeof(controllers), snap->tree_rows[idx].controllers);
        proc_count = snap->tree_rows[idx].process_count;
        thread_count = snap->tree_rows[idx].thread_count;
        subgroup_count = snap->tree_rows[idx].subgroup_count;
    } else {
        copy_trunc(path, sizeof(path), snap->browse_path[0] ? snap->browse_path : CGROUP_ROOT);
        last_path_component(path, name, sizeof(name));
    }
    if (path_join(file, sizeof(file), path, "cgroup.type") == 0) read_text_file_line(file, type, sizeof(type));
    if (path_join(file, sizeof(file), path, "cgroup.controllers") == 0) {
        read_text_file_line(file, controllers, sizeof(controllers));
    }
    if (path_join(file, sizeof(file), path, "cgroup.subtree_control") == 0) {
        read_text_file_line(file, subtree, sizeof(subtree));
    }
    if (proc_count < 0 && path_join(file, sizeof(file), path, "cgroup.procs") == 0) {
        proc_count = count_file_lines(file);
    }
    if (thread_count < 0 && path_join(file, sizeof(file), path, "cgroup.threads") == 0) {
        thread_count = count_file_lines(file);
    }
    if (subgroup_count < 0) subgroup_count = count_child_dirs(path);

    copy_trunc(snap->detail_title, sizeof(snap->detail_title), name[0] ? name : "/");
    copy_trunc(snap->detail_status, sizeof(snap->detail_status), path);
    if (append_linef(&snap->detail_lines, &snap->detail_line_count, "path: %s", path) < 0) return -1;
    if (append_linef(&snap->detail_lines, &snap->detail_line_count, "type: %s", type) < 0) return -1;
    if (append_linef(&snap->detail_lines, &snap->detail_line_count, "controllers: %s",
                     controllers[0] ? controllers : "-") < 0) return -1;
    if (append_linef(&snap->detail_lines, &snap->detail_line_count, "subtree_control: %s",
                     subtree[0] ? subtree : "-") < 0) return -1;
    if (append_linef(&snap->detail_lines, &snap->detail_line_count, "processes: %d",
                     proc_count >= 0 ? proc_count : 0) < 0) return -1;
    if (append_linef(&snap->detail_lines, &snap->detail_line_count, "threads: %d",
                     thread_count >= 0 ? thread_count : 0) < 0) return -1;
    if (append_linef(&snap->detail_lines, &snap->detail_line_count, "children: %d",
                     subgroup_count >= 0 ? subgroup_count : 0) < 0) return -1;
    if (path_join(file, sizeof(file), path, "cgroup.events") == 0 && access(file, R_OK) == 0) {
        if (append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, "") < 0) return -1;
        if (append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, "events:") < 0) return -1;
        if (append_file_preview_lines(&snap->detail_lines, &snap->detail_line_count, file) < 0) return -1;
    }
    if (append_cgroup_members(&snap->detail_lines, &snap->detail_line_count, path) < 0) return -1;
    return 0;
}

static int refresh_file_preview(LuloCgroupsSnapshot *snap, const LuloCgroupsState *state)
{
    int idx;
    const LuloCgroupFileRow *row;

    if (!snap || !state || snap->file_count <= 0) {
        set_detail_idle(snap, state);
        return 0;
    }
    idx = selected_file_index(snap, state);
    if (idx < 0 || idx >= snap->file_count) {
        set_detail_idle(snap, state);
        return 0;
    }
    row = &snap->file_rows[idx];
    clear_lines(&snap->detail_lines, &snap->detail_line_count);
    copy_trunc(snap->detail_title, sizeof(snap->detail_title), row->name);
    copy_trunc(snap->detail_status, sizeof(snap->detail_status), row->path);
    if (append_linef(&snap->detail_lines, &snap->detail_line_count, "path: %s", row->path) < 0) return -1;
    if (append_linef(&snap->detail_lines, &snap->detail_line_count, "access: %s",
                     row->writable ? "read-write" : "read-only") < 0) return -1;
    if (append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, "") < 0) return -1;
    if (append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, "current value:") < 0) return -1;
    if (append_snapshot_line(&snap->detail_lines, &snap->detail_line_count,
                             row->value[0] ? row->value : "(empty)") < 0) return -1;
    if (append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, "") < 0) return -1;
    if (append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, "raw file:") < 0) return -1;
    return append_file_preview_lines(&snap->detail_lines, &snap->detail_line_count, row->path);
}

static int refresh_config_preview(LuloCgroupsSnapshot *snap, const LuloCgroupsState *state)
{
    int idx;
    const LuloCgroupConfigRow *row;

    if (!snap || !state || snap->config_count <= 0) {
        set_detail_idle(snap, state);
        return 0;
    }
    idx = selected_config_index(snap, state);
    if (idx < 0 || idx >= snap->config_count) {
        set_detail_idle(snap, state);
        return 0;
    }
    row = &snap->configs[idx];
    clear_lines(&snap->detail_lines, &snap->detail_line_count);
    copy_trunc(snap->detail_title, sizeof(snap->detail_title), row->name);
    copy_trunc(snap->detail_status, sizeof(snap->detail_status), row->path);
    if (append_linef(&snap->detail_lines, &snap->detail_line_count, "path: %s", row->path) < 0) return -1;
    if (append_linef(&snap->detail_lines, &snap->detail_line_count, "source: %s", row->source) < 0) return -1;
    if (append_linef(&snap->detail_lines, &snap->detail_line_count, "kind: %s", row->kind) < 0) return -1;
    if (append_snapshot_line(&snap->detail_lines, &snap->detail_line_count, "") < 0) return -1;
    return append_file_preview_lines(&snap->detail_lines, &snap->detail_line_count, row->path);
}

int lulod_cgroups_snapshot_gather(LuloCgroupsSnapshot *snap, const LuloCgroupsState *state)
{
    char browse_path[PATH_MAX];

    if (!snap) return -1;
    memset(snap, 0, sizeof(*snap));
    if (normalize_browse_path(state, browse_path, sizeof(browse_path)) < 0) return -1;
    copy_trunc(snap->browse_path, sizeof(snap->browse_path), browse_path);
    if (gather_tree_rows(snap, browse_path) < 0) goto fail;
    if (gather_file_rows(snap, browse_path) < 0) goto fail;
    if (ensure_configs_loaded(snap) < 0) goto fail;
    if (lulod_cgroups_snapshot_refresh_active(snap, state) < 0) goto fail;
    return 0;

fail:
    lulo_cgroups_snapshot_free(snap);
    return -1;
}

int lulod_cgroups_snapshot_refresh_active(LuloCgroupsSnapshot *snap, const LuloCgroupsState *state)
{
    if (!snap) return -1;
    switch (state ? state->view : LULO_CGROUPS_VIEW_TREE) {
    case LULO_CGROUPS_VIEW_FILES:
        return refresh_file_preview(snap, state);
    case LULO_CGROUPS_VIEW_CONFIG:
        return refresh_config_preview(snap, state);
    case LULO_CGROUPS_VIEW_TREE:
    default:
        return refresh_tree_preview(snap, state);
    }
}
