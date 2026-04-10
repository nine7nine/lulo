#define _GNU_SOURCE

#include "lulod_system_sched.h"
#include "lulo_proc_meta.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <linux/ioprio.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef IOPRIO_WHO_PROCESS
#define IOPRIO_WHO_PROCESS 1
#endif

#ifndef IOPRIO_CLASS_NONE
#define IOPRIO_CLASS_NONE 0
#define IOPRIO_CLASS_RT 1
#define IOPRIO_CLASS_BE 2
#define IOPRIO_CLASS_IDLE 3
#endif

#ifndef IOPRIO_PRIO_VALUE
#define IOPRIO_PRIO_VALUE(class, data) (((class) << IOPRIO_CLASS_SHIFT) | (data))
#define IOPRIO_PRIO_CLASS(mask) (((mask) >> IOPRIO_CLASS_SHIFT) & 0x7)
#define IOPRIO_PRIO_DATA(mask) ((mask) & ((1 << IOPRIO_CLASS_SHIFT) - 1))
#endif

static void trim_right(char *text)
{
    size_t len;

    if (!text) return;
    len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[--len] = '\0';
    }
}

static char *trim_left(char *text)
{
    while (text && *text && isspace((unsigned char)*text)) text++;
    return text;
}

static char *trim(char *text)
{
    text = trim_left(text);
    trim_right(text);
    return text;
}

static void file_stem(const char *path, char *buf, size_t len)
{
    const char *base = strrchr(path ? path : "", '/');
    size_t n;

    if (len == 0) return;
    base = base ? base + 1 : (path ? path : "");
    n = strcspn(base, ".");
    if (n >= len) n = len - 1;
    memcpy(buf, base, n);
    buf[n] = '\0';
}

static int parse_bool_value(const char *text, int *value)
{
    if (!text || !value) return -1;
    if (!strcasecmp(text, "1") || !strcasecmp(text, "true") ||
        !strcasecmp(text, "yes") || !strcasecmp(text, "on") ||
        !strcasecmp(text, "enable") || !strcasecmp(text, "enabled")) {
        *value = 1;
        return 0;
    }
    if (!strcasecmp(text, "0") || !strcasecmp(text, "false") ||
        !strcasecmp(text, "no") || !strcasecmp(text, "off") ||
        !strcasecmp(text, "disable") || !strcasecmp(text, "disabled")) {
        *value = 0;
        return 0;
    }
    return -1;
}

static int parse_int_value(const char *text, int min_value, int max_value, int *value)
{
    char *end = NULL;
    long parsed;

    if (!text || !value) return -1;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || !end || *trim_left(end) != '\0') return -1;
    if (parsed < min_value || parsed > max_value) return -1;
    *value = (int)parsed;
    return 0;
}

static int parse_policy_value(const char *text, int *policy)
{
    if (!text || !policy) return -1;
    if (!strcasecmp(text, "other") || !strcasecmp(text, "normal")) {
        *policy = SCHED_OTHER;
        return 0;
    }
    if (!strcasecmp(text, "batch")) {
        *policy = SCHED_BATCH;
        return 0;
    }
    if (!strcasecmp(text, "idle")) {
        *policy = SCHED_IDLE;
        return 0;
    }
    if (!strcasecmp(text, "fifo")) {
        *policy = SCHED_FIFO;
        return 0;
    }
    if (!strcasecmp(text, "rr") || !strcasecmp(text, "round-robin")) {
        *policy = SCHED_RR;
        return 0;
    }
    return parse_int_value(text, 0, 7, policy);
}

static int parse_io_class_value(const char *text, int *io_class)
{
    if (!text || !io_class) return -1;
    if (!strcasecmp(text, "none") || !strcasecmp(text, "unset")) {
        *io_class = IOPRIO_CLASS_NONE;
        return 0;
    }
    if (!strcasecmp(text, "realtime") || !strcasecmp(text, "rt")) {
        *io_class = IOPRIO_CLASS_RT;
        return 0;
    }
    if (!strcasecmp(text, "best-effort") || !strcasecmp(text, "best_effort") ||
        !strcasecmp(text, "besteffort") || !strcasecmp(text, "be")) {
        *io_class = IOPRIO_CLASS_BE;
        return 0;
    }
    if (!strcasecmp(text, "idle")) {
        *io_class = IOPRIO_CLASS_IDLE;
        return 0;
    }
    return parse_int_value(text, 0, 3, io_class);
}

static int parse_match_kind(const char *text, LuloSchedMatchKind *kind)
{
    if (!text || !kind) return -1;
    if (!strcasecmp(text, "comm") || !strcasecmp(text, "name")) {
        *kind = LULO_SCHED_MATCH_COMM;
        return 0;
    }
    if (!strcasecmp(text, "exe") || !strcasecmp(text, "path")) {
        *kind = LULO_SCHED_MATCH_EXE;
        return 0;
    }
    if (!strcasecmp(text, "cmd") || !strcasecmp(text, "cmdline") ||
        !strcasecmp(text, "command")) {
        *kind = LULO_SCHED_MATCH_CMDLINE;
        return 0;
    }
    if (!strcasecmp(text, "unit")) {
        *kind = LULO_SCHED_MATCH_UNIT;
        return 0;
    }
    if (!strcasecmp(text, "slice")) {
        *kind = LULO_SCHED_MATCH_SLICE;
        return 0;
    }
    if (!strcasecmp(text, "cgroup")) {
        *kind = LULO_SCHED_MATCH_CGROUP;
        return 0;
    }
    return -1;
}

static const char *path_basename_ptr(const char *path)
{
    const char *slash = strrchr(path ? path : "", '/');
    return slash ? slash + 1 : (path ? path : "");
}

static int is_conf_name(const char *name)
{
    size_t len;

    if (!name || name[0] == '.') return 0;
    len = strlen(name);
    return len > 5 && strcmp(name + len - 5, ".conf") == 0;
}

static int normalized_policy(int policy)
{
#ifdef SCHED_RESET_ON_FORK
    return policy & ~SCHED_RESET_ON_FORK;
#else
    return policy;
#endif
}

static void free_string_list(char **items, int count)
{
    if (!items) return;
    for (int i = 0; i < count; i++) free(items[i]);
    free(items);
}

static int collect_conf_paths(const char *dirpath, char ***items, int *count)
{
    struct dirent **namelist = NULL;
    int n = -1;
    char **paths = NULL;
    int out_count = 0;

    *items = NULL;
    *count = 0;
    n = scandir(dirpath, &namelist, NULL, alphasort);
    if (n < 0) {
        if (errno == ENOENT || errno == ENOTDIR) return 0;
        return -1;
    }
    for (int i = 0; i < n; i++) {
        char full[PATH_MAX];
        struct stat st;
        char **next;

        if (!is_conf_name(namelist[i]->d_name)) {
            free(namelist[i]);
            continue;
        }
        snprintf(full, sizeof(full), "%s/%s", dirpath, namelist[i]->d_name);
        free(namelist[i]);
        if (stat(full, &st) < 0 || !S_ISREG(st.st_mode)) continue;
        next = realloc(paths, (size_t)(out_count + 1) * sizeof(*paths));
        if (!next) {
            free_string_list(paths, out_count);
            free(namelist);
            return -1;
        }
        paths = next;
        paths[out_count] = strdup(full);
        if (!paths[out_count]) {
            free_string_list(paths, out_count);
            free(namelist);
            return -1;
        }
        out_count++;
    }
    free(namelist);
    *items = paths;
    *count = out_count;
    return 0;
}

static int find_profile_index(const LuloSchedSnapshot *snap, const char *name)
{
    if (!snap || !name || !*name) return -1;
    for (int i = 0; i < snap->profile_count; i++) {
        if (strcmp(snap->profiles[i].name, name) == 0) return i;
    }
    return -1;
}

static int find_enabled_profile_index(const LuloSchedSnapshot *snap, const char *name)
{
    int idx = find_profile_index(snap, name);
    if (idx < 0) return -1;
    return snap->profiles[idx].enabled ? idx : -1;
}

static int find_rule_index(const LuloSchedSnapshot *snap, const char *name)
{
    if (!snap || !name || !*name) return -1;
    for (int i = 0; i < snap->rule_count; i++) {
        if (strcmp(snap->rules[i].name, name) == 0) return i;
    }
    return -1;
}

static int append_or_replace_profile(LuloSchedSnapshot *snap, const LuloSchedProfileRow *row)
{
    int idx;
    LuloSchedProfileRow *next;

    if (!snap || !row) return -1;
    idx = find_profile_index(snap, row->name);
    if (idx >= 0) {
        snap->profiles[idx] = *row;
        return 0;
    }
    next = realloc(snap->profiles, (size_t)(snap->profile_count + 1) * sizeof(*next));
    if (!next) return -1;
    snap->profiles = next;
    snap->profiles[snap->profile_count++] = *row;
    return 0;
}

static int append_or_replace_rule(LuloSchedSnapshot *snap, const LuloSchedRuleRow *row)
{
    int idx;
    LuloSchedRuleRow *next;

    if (!snap || !row) return -1;
    idx = find_rule_index(snap, row->name);
    if (idx >= 0) {
        snap->rules[idx] = *row;
        return 0;
    }
    next = realloc(snap->rules, (size_t)(snap->rule_count + 1) * sizeof(*next));
    if (!next) return -1;
    snap->rules = next;
    snap->rules[snap->rule_count++] = *row;
    return 0;
}

static int sched_policy_rank_for_profile_row(const LuloSchedProfileRow *row)
{
    if (!row || !row->has_policy) return 3;
    switch (row->policy) {
    case SCHED_DEADLINE:
        return 0;
    case SCHED_FIFO:
        return 1;
    case SCHED_RR:
        return 2;
    case SCHED_OTHER:
    default:
        return 3;
    case SCHED_BATCH:
        return 4;
    case SCHED_IDLE:
        return 5;
    }
}

static long sched_profile_priority_score_row(const LuloSchedProfileRow *row)
{
    long score;
    long nice_score = 0;
    long rt_score = 0;
    int rank;

    if (!row) return -1;
    rank = sched_policy_rank_for_profile_row(row);
    score = (long)(600 - rank * 100) * 1000L;
    if (row->has_rt_priority) rt_score = (long)row->rt_priority * 100L;
    if (row->has_nice) nice_score = (long)(20 - row->nice) * 10L;
    if (!row->enabled) score -= 250L;
    return score + rt_score + nice_score;
}

static int profile_row_cmp(const void *a, const void *b)
{
    const LuloSchedProfileRow *ra = a;
    const LuloSchedProfileRow *rb = b;
    long sa;
    long sb;
    int cmp;

    sa = sched_profile_priority_score_row(ra);
    sb = sched_profile_priority_score_row(rb);
    if (sa != sb) return sa > sb ? -1 : 1;
    cmp = strcmp(ra->name, rb->name);
    if (cmp != 0) return cmp;
    return strcmp(ra->path, rb->path);
}

static long sched_rule_target_score(const LuloSchedSnapshot *snap, const LuloSchedRuleRow *row)
{
    int idx;

    if (!row) return -1;
    if (row->exclude) return 1000000000L;
    if (!snap || !row->profile[0]) return -1;
    idx = find_profile_index(snap, row->profile);
    if (idx < 0) return -1;
    return sched_profile_priority_score_row(&snap->profiles[idx]);
}

static int rule_match_kind_rank(LuloSchedMatchKind kind)
{
    switch (kind) {
    case LULO_SCHED_MATCH_DYNAMIC:
        return 0;
    case LULO_SCHED_MATCH_UNIT:
        return 1;
    case LULO_SCHED_MATCH_EXE:
        return 2;
    case LULO_SCHED_MATCH_COMM:
        return 3;
    case LULO_SCHED_MATCH_CMDLINE:
        return 4;
    case LULO_SCHED_MATCH_SLICE:
        return 5;
    case LULO_SCHED_MATCH_CGROUP:
        return 6;
    default:
        return 7;
    }
}

static int rule_row_cmp(const void *a, const void *b, void *ctx)
{
    const LuloSchedSnapshot *snap = ctx;
    const LuloSchedRuleRow *ra = a;
    const LuloSchedRuleRow *rb = b;
    long sa;
    long sb;
    int cmp;
    int ka;
    int kb;

    sa = sched_rule_target_score(snap, ra);
    sb = sched_rule_target_score(snap, rb);
    if (sa != sb) return sa > sb ? -1 : 1;
    cmp = strcmp(ra->profile, rb->profile);
    if (cmp != 0) return cmp;
    ka = rule_match_kind_rank(ra->match_kind);
    kb = rule_match_kind_rank(rb->match_kind);
    if (ka != kb) return ka - kb;
    cmp = strcmp(ra->pattern, rb->pattern);
    if (cmp != 0) return cmp;
    return strcmp(ra->name, rb->name);
}

static int parse_profile_file(const char *path, LuloSchedProfileRow *row, char *err, size_t errlen)
{
    FILE *fp = NULL;
    char *line = NULL;
    size_t cap = 0;
    ssize_t nread;
    unsigned lineno = 0;

    memset(row, 0, sizeof(*row));
    row->enabled = 1;
    snprintf(row->path, sizeof(row->path), "%s", path);
    file_stem(path, row->name, sizeof(row->name));

    fp = fopen(path, "r");
    if (!fp) {
        if (err && errlen > 0) snprintf(err, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }
    while ((nread = getline(&line, &cap, fp)) >= 0) {
        char *work;
        char *eq;
        char *key;
        char *value;

        (void)nread;
        lineno++;
        work = trim(line);
        if (!*work || *work == '#' || *work == ';') continue;
        eq = strchr(work, '=');
        if (!eq) {
            if (err && errlen > 0) snprintf(err, errlen, "%s:%u: expected key=value", path, lineno);
            goto fail;
        }
        *eq = '\0';
        key = trim(work);
        value = trim(eq + 1);

        if (!strcmp(key, "name")) {
            snprintf(row->name, sizeof(row->name), "%s", value);
        } else if (!strcmp(key, "enabled")) {
            if (parse_bool_value(value, &row->enabled) < 0) goto bad_value;
        } else if (!strcmp(key, "nice")) {
            if (parse_int_value(value, -20, 19, &row->nice) < 0) goto bad_value;
            row->has_nice = 1;
        } else if (!strcmp(key, "policy")) {
            if (parse_policy_value(value, &row->policy) < 0) goto bad_value;
            row->has_policy = 1;
        } else if (!strcmp(key, "rt_priority")) {
            if (parse_int_value(value, 1, 99, &row->rt_priority) < 0) goto bad_value;
            row->has_rt_priority = 1;
        } else if (!strcmp(key, "io_class")) {
            if (parse_io_class_value(value, &row->io_class) < 0) goto bad_value;
            row->has_io_class = 1;
        } else if (!strcmp(key, "io_priority")) {
            if (parse_int_value(value, 0, 7, &row->io_priority) < 0) goto bad_value;
            row->has_io_priority = 1;
        } else {
            if (err && errlen > 0) snprintf(err, errlen, "%s:%u: unknown key '%s'", path, lineno, key);
            goto fail;
        }
        continue;

bad_value:
        if (err && errlen > 0) snprintf(err, errlen, "%s:%u: invalid value for '%s'", path, lineno, key);
        goto fail;
    }
    free(line);
    fclose(fp);
    if (!row->name[0]) {
        if (err && errlen > 0) snprintf(err, errlen, "%s: profile name is empty", path);
        return -1;
    }
    return 0;

fail:
    free(line);
    fclose(fp);
    return -1;
}

static int parse_rule_file(const char *path, LuloSchedRuleRow *row, char *err, size_t errlen)
{
    FILE *fp = NULL;
    char *line = NULL;
    size_t cap = 0;
    ssize_t nread;
    unsigned lineno = 0;

    memset(row, 0, sizeof(*row));
    row->enabled = 1;
    row->match_kind = LULO_SCHED_MATCH_COMM;
    snprintf(row->path, sizeof(row->path), "%s", path);
    file_stem(path, row->name, sizeof(row->name));

    fp = fopen(path, "r");
    if (!fp) {
        if (err && errlen > 0) snprintf(err, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }
    while ((nread = getline(&line, &cap, fp)) >= 0) {
        char *work;
        char *eq;
        char *key;
        char *value;

        (void)nread;
        lineno++;
        work = trim(line);
        if (!*work || *work == '#' || *work == ';') continue;
        eq = strchr(work, '=');
        if (!eq) {
            if (err && errlen > 0) snprintf(err, errlen, "%s:%u: expected key=value", path, lineno);
            goto fail;
        }
        *eq = '\0';
        key = trim(work);
        value = trim(eq + 1);

        if (!strcmp(key, "name")) {
            snprintf(row->name, sizeof(row->name), "%s", value);
        } else if (!strcmp(key, "enabled")) {
            if (parse_bool_value(value, &row->enabled) < 0) goto bad_value;
        } else if (!strcmp(key, "exclude")) {
            if (parse_bool_value(value, &row->exclude) < 0) goto bad_value;
        } else if (!strcmp(key, "match")) {
            if (parse_match_kind(value, &row->match_kind) < 0) goto bad_value;
        } else if (!strcmp(key, "pattern")) {
            snprintf(row->pattern, sizeof(row->pattern), "%s", value);
        } else if (!strcmp(key, "profile") || !strcmp(key, "action")) {
            snprintf(row->profile, sizeof(row->profile), "%s", value);
        } else {
            if (err && errlen > 0) snprintf(err, errlen, "%s:%u: unknown key '%s'", path, lineno, key);
            goto fail;
        }
        continue;

bad_value:
        if (err && errlen > 0) snprintf(err, errlen, "%s:%u: invalid value for '%s'", path, lineno, key);
        goto fail;
    }
    free(line);
    fclose(fp);
    if (!row->name[0]) {
        if (err && errlen > 0) snprintf(err, errlen, "%s: rule name is empty", path);
        return -1;
    }
    if (!row->pattern[0]) {
        if (err && errlen > 0) snprintf(err, errlen, "%s: rule pattern is empty", path);
        return -1;
    }
    if (!row->exclude && !row->profile[0]) {
        if (err && errlen > 0) snprintf(err, errlen, "%s: rule profile is empty", path);
        return -1;
    }
    if (row->exclude) row->profile[0] = '\0';
    return 0;

fail:
    free(line);
    fclose(fp);
    return -1;
}

static int load_global_config(LuloSchedSnapshot *snap, const char *config_root, char *err, size_t errlen)
{
    char path[PATH_MAX];
    FILE *fp;
    char *line = NULL;
    size_t cap = 0;
    ssize_t nread;
    unsigned lineno = 0;

    snprintf(path, sizeof(path), "%s/scheduler.conf", config_root);
    fp = fopen(path, "r");
    if (!fp) {
        if (errno == ENOENT) return 0;
        if (err && errlen > 0) snprintf(err, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }
    while ((nread = getline(&line, &cap, fp)) >= 0) {
        char *work;
        char *eq;
        char *key;
        char *value;
        int parsed;

        (void)nread;
        lineno++;
        work = trim(line);
        if (!*work || *work == '#' || *work == ';') continue;
        eq = strchr(work, '=');
        if (!eq) {
            if (err && errlen > 0) snprintf(err, errlen, "%s:%u: expected key=value", path, lineno);
            free(line);
            fclose(fp);
            return -1;
        }
        *eq = '\0';
        key = trim(work);
        value = trim(eq + 1);
        if (!strcmp(key, "watcher_interval_ms") || !strcmp(key, "refresh_ms")) {
            if (parse_int_value(value, 100, 10000, &parsed) < 0) {
                if (err && errlen > 0) snprintf(err, errlen, "%s:%u: invalid watcher interval", path, lineno);
                free(line);
                fclose(fp);
                return -1;
            }
            snap->watcher_interval_ms = parsed;
        } else if (!strcmp(key, "focus_enabled")) {
            if (parse_bool_value(value, &parsed) < 0) {
                if (err && errlen > 0) snprintf(err, errlen, "%s:%u: invalid focus_enabled", path, lineno);
                free(line);
                fclose(fp);
                return -1;
            }
            snap->focus_enabled = parsed;
        } else if (!strcmp(key, "focus_profile")) {
            snprintf(snap->focus_profile, sizeof(snap->focus_profile), "%s", value);
        } else if (!strcmp(key, "background_enabled")) {
            if (parse_bool_value(value, &parsed) < 0) {
                if (err && errlen > 0) snprintf(err, errlen, "%s:%u: invalid background_enabled", path, lineno);
                free(line);
                fclose(fp);
                return -1;
            }
            snap->background_enabled = parsed;
        } else if (!strcmp(key, "background_profile")) {
            snprintf(snap->background_profile, sizeof(snap->background_profile), "%s", value);
        } else if (!strcmp(key, "background_match_app_slice")) {
            if (parse_bool_value(value, &parsed) < 0) {
                if (err && errlen > 0) snprintf(err, errlen, "%s:%u: invalid background_match_app_slice", path, lineno);
                free(line);
                fclose(fp);
                return -1;
            }
            snap->background_match_app_slice = parsed;
        } else if (!strcmp(key, "background_match_background_slice")) {
            if (parse_bool_value(value, &parsed) < 0) {
                if (err && errlen > 0) snprintf(err, errlen, "%s:%u: invalid background_match_background_slice", path, lineno);
                free(line);
                fclose(fp);
                return -1;
            }
            snap->background_match_background_slice = parsed;
        } else if (!strcmp(key, "background_match_app_unit_prefix")) {
            if (parse_bool_value(value, &parsed) < 0) {
                if (err && errlen > 0) snprintf(err, errlen, "%s:%u: invalid background_match_app_unit_prefix", path, lineno);
                free(line);
                fclose(fp);
                return -1;
            }
            snap->background_match_app_unit_prefix = parsed;
        } else {
            if (err && errlen > 0) snprintf(err, errlen, "%s:%u: unknown key '%s'", path, lineno, key);
            free(line);
            fclose(fp);
            return -1;
        }
    }
    free(line);
    fclose(fp);
    return 0;
}

static int load_profiles(LuloSchedSnapshot *snap, const char *config_root, char *err, size_t errlen)
{
    char dirpath[PATH_MAX];
    char **paths = NULL;
    int count = 0;
    int rc = -1;

    snprintf(dirpath, sizeof(dirpath), "%s/profiles.d", config_root);
    if (collect_conf_paths(dirpath, &paths, &count) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "%s: %s", dirpath, strerror(errno));
        return -1;
    }
    for (int i = 0; i < count; i++) {
        LuloSchedProfileRow row;

        if (parse_profile_file(paths[i], &row, err, errlen) < 0) goto out;
        if (append_or_replace_profile(snap, &row) < 0) {
            if (err && errlen > 0) snprintf(err, errlen, "out of memory loading profiles");
            goto out;
        }
    }
    if (snap->profile_count > 1) {
        qsort(snap->profiles, (size_t)snap->profile_count, sizeof(*snap->profiles), profile_row_cmp);
    }
    rc = 0;

out:
    free_string_list(paths, count);
    return rc;
}

static void append_summary_token(char *buf, size_t len, const char *token)
{
    size_t used;

    if (!buf || len == 0 || !token || !*token) return;
    used = strlen(buf);
    if (used > 0 && used + 3 < len) {
        memcpy(buf + used, " + ", 3);
        used += 3;
        buf[used] = '\0';
    }
    if (used < len) snprintf(buf + used, len - used, "%s", token);
}

static void copy_capped_string(char *dst, size_t len, const char *src)
{
    size_t copy_len;

    if (!dst || len == 0) return;
    copy_len = strnlen(src ? src : "", len - 1);
    memcpy(dst, src ? src : "", copy_len);
    dst[copy_len] = '\0';
}

static void build_background_pattern_summary(const LuloSchedSnapshot *snap, char *buf, size_t len)
{
    if (!buf || len == 0) return;
    buf[0] = '\0';
    if (!snap) return;
    if (!snap->background_enabled) {
        snprintf(buf, len, "disabled");
        return;
    }
    if (snap->background_match_app_slice) append_summary_token(buf, len, "app.slice");
    if (snap->background_match_background_slice) append_summary_token(buf, len, "background.slice");
    if (snap->background_match_app_unit_prefix) append_summary_token(buf, len, "unit:app-*");
    if (!buf[0]) snprintf(buf, len, "no classifiers");
}

static int load_rules(LuloSchedSnapshot *snap, const char *config_root, char *err, size_t errlen)
{
    char dirpath[PATH_MAX];
    char **paths = NULL;
    int count = 0;
    int rc = -1;

    snprintf(dirpath, sizeof(dirpath), "%s/rules.d", config_root);
    if (collect_conf_paths(dirpath, &paths, &count) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "%s: %s", dirpath, strerror(errno));
        return -1;
    }
    for (int i = 0; i < count; i++) {
        LuloSchedRuleRow row;

        if (parse_rule_file(paths[i], &row, err, errlen) < 0) goto out;
        if (append_or_replace_rule(snap, &row) < 0) {
            if (err && errlen > 0) snprintf(err, errlen, "out of memory loading rules");
            goto out;
        }
    }
    {
        char config_path[PATH_MAX];
        LuloSchedRuleRow row;

        snprintf(config_path, sizeof(config_path), "%s/scheduler.conf", config_root);

        memset(&row, 0, sizeof(row));
        snprintf(row.name, sizeof(row.name), "(focus)");
        copy_capped_string(row.path, sizeof(row.path), config_path);
        row.enabled = snap->focus_enabled;
        row.match_kind = LULO_SCHED_MATCH_DYNAMIC;
        snprintf(row.pattern, sizeof(row.pattern), "active window");
        snprintf(row.profile, sizeof(row.profile), "%s",
                 snap->focus_profile[0] ? snap->focus_profile : "-");
        if (append_or_replace_rule(snap, &row) < 0) {
            if (err && errlen > 0) snprintf(err, errlen, "out of memory loading builtin focus rule");
            goto out;
        }

        memset(&row, 0, sizeof(row));
        snprintf(row.name, sizeof(row.name), "(background)");
        copy_capped_string(row.path, sizeof(row.path), config_path);
        row.enabled = snap->background_enabled;
        row.match_kind = LULO_SCHED_MATCH_DYNAMIC;
        build_background_pattern_summary(snap, row.pattern, sizeof(row.pattern));
        snprintf(row.profile, sizeof(row.profile), "%s",
                 snap->background_profile[0] ? snap->background_profile : "-");
        if (append_or_replace_rule(snap, &row) < 0) {
            if (err && errlen > 0) snprintf(err, errlen, "out of memory loading builtin background rule");
            goto out;
        }
    }
    if (snap->rule_count > 1) {
        qsort_r(snap->rules, (size_t)snap->rule_count, sizeof(*snap->rules),
                rule_row_cmp, snap);
    }
    rc = 0;

out:
    free_string_list(paths, count);
    return rc;
}

static void clear_focus_target(LuloSchedSnapshot *snap, int keep_provider)
{
    char provider[sizeof(snap->focus_provider)];

    if (!snap) return;
    if (keep_provider) snprintf(provider, sizeof(provider), "%s", snap->focus_provider);
    snap->focused_pid = 0;
    snap->focused_start_time = 0;
    snap->focused_comm[0] = '\0';
    snap->focused_exe[0] = '\0';
    snap->focused_unit[0] = '\0';
    snap->focused_slice[0] = '\0';
    snap->focused_cgroup[0] = '\0';
    if (keep_provider) snprintf(snap->focus_provider, sizeof(snap->focus_provider), "%s", provider);
    else snap->focus_provider[0] = '\0';
}

static void copy_focus_target(LuloSchedSnapshot *snap, const LuloProcMeta *meta)
{
    if (!snap) return;
    if (!meta) {
        clear_focus_target(snap, 1);
        return;
    }
    snap->focused_pid = (int)meta->pid;
    snap->focused_start_time = meta->start_time;
    snprintf(snap->focused_comm, sizeof(snap->focused_comm), "%s", meta->comm);
    snprintf(snap->focused_exe, sizeof(snap->focused_exe), "%s", meta->exe);
    snprintf(snap->focused_unit, sizeof(snap->focused_unit), "%s", meta->unit);
    snprintf(snap->focused_slice, sizeof(snap->focused_slice), "%s", meta->slice);
    snprintf(snap->focused_cgroup, sizeof(snap->focused_cgroup), "%s", meta->cgroup);
}

static void validate_focus_target(LuloSchedSnapshot *snap)
{
    unsigned long long start_time = 0;
    char comm[96];

    if (!snap || snap->focused_pid <= 0) return;
    if (lulo_proc_meta_read_basic((pid_t)snap->focused_pid, comm, sizeof(comm), &start_time) < 0 ||
        start_time != snap->focused_start_time) {
        clear_focus_target(snap, 1);
    }
}

static int query_process_sched(pid_t pid, int *nice_value, int *policy, int *rt_priority)
{
    struct sched_param param;
    int current_policy;

    errno = 0;
    *nice_value = getpriority(PRIO_PROCESS, pid);
    if (*nice_value == -1 && errno != 0) return -1;
    errno = 0;
    current_policy = sched_getscheduler(pid);
    if (current_policy < 0) return -1;
    memset(&param, 0, sizeof(param));
    if (sched_getparam(pid, &param) < 0) return -1;
    *policy = normalized_policy(current_policy);
    *rt_priority = param.sched_priority;
    return 0;
}

static int query_process_io(pid_t pid, int *io_class, int *io_priority)
{
    int value;

    if (!io_class || !io_priority) return -1;
    errno = 0;
    value = (int)syscall(SYS_ioprio_get, IOPRIO_WHO_PROCESS, (int)pid);
    if (value < 0) return -1;
    *io_class = IOPRIO_PRIO_CLASS(value);
    *io_priority = IOPRIO_PRIO_DATA(value);
    return 0;
}

static void desired_io_target(const LuloSchedProfileRow *profile,
                              int current_io_class, int current_io_priority,
                              int *target_io_class, int *target_io_priority)
{
    int target_class;
    int target_priority;

    target_class = current_io_class;
    target_priority = current_io_priority;
    if (profile->has_io_class) {
        target_class = profile->io_class;
    } else if (profile->has_io_priority && target_class == IOPRIO_CLASS_NONE) {
        target_class = IOPRIO_CLASS_BE;
    }

    if (target_class == IOPRIO_CLASS_RT || target_class == IOPRIO_CLASS_BE) {
        if (profile->has_io_priority) target_priority = profile->io_priority;
        else if (profile->has_io_class) target_priority = 4;
        if (target_priority < 0) target_priority = 0;
        if (target_priority > 7) target_priority = 7;
    } else {
        target_priority = 0;
    }

    *target_io_class = target_class;
    *target_io_priority = target_priority;
}

static int match_rule_target(const LuloSchedRuleRow *rule, const LuloProcMeta *meta)
{
    const char *exe_base;

    if (!rule || !meta || !rule->pattern[0]) return 0;
    switch (rule->match_kind) {
    case LULO_SCHED_MATCH_DYNAMIC:
        return 0;
    case LULO_SCHED_MATCH_EXE:
        if (meta->exe[0] && fnmatch(rule->pattern, meta->exe, 0) == 0) return 1;
        exe_base = path_basename_ptr(meta->exe);
        return exe_base && *exe_base && fnmatch(rule->pattern, exe_base, 0) == 0;
    case LULO_SCHED_MATCH_CMDLINE:
        return meta->cmdline[0] && fnmatch(rule->pattern, meta->cmdline, 0) == 0;
    case LULO_SCHED_MATCH_UNIT:
        return meta->unit[0] && fnmatch(rule->pattern, meta->unit, 0) == 0;
    case LULO_SCHED_MATCH_SLICE:
        return meta->slice[0] && fnmatch(rule->pattern, meta->slice, 0) == 0;
    case LULO_SCHED_MATCH_CGROUP:
        return meta->cgroup[0] && fnmatch(rule->pattern, meta->cgroup, 0) == 0;
    case LULO_SCHED_MATCH_COMM:
    default:
        return meta->comm[0] && fnmatch(rule->pattern, meta->comm, 0) == 0;
    }
}

static int find_matching_rule(const LuloSchedSnapshot *snap, const LuloProcMeta *meta,
                              int *rule_idx, int *profile_idx)
{
    if (rule_idx) *rule_idx = -1;
    if (profile_idx) *profile_idx = -1;
    if (!snap) return 0;
    for (int i = 0; i < snap->rule_count; i++) {
        int profile = -1;
        const LuloSchedRuleRow *rule = &snap->rules[i];

        if (!rule->enabled) continue;
        if (rule->match_kind == LULO_SCHED_MATCH_DYNAMIC) continue;
        if (!match_rule_target(rule, meta)) continue;
        if (rule->exclude) {
            if (rule_idx) *rule_idx = i;
            return -1;
        }
        profile = find_enabled_profile_index(snap, rule->profile);
        if (profile < 0) continue;
        if (rule_idx) *rule_idx = i;
        if (profile_idx) *profile_idx = profile;
        return 1;
    }
    return 0;
}

static int looks_like_application_process(const LuloSchedSnapshot *snap, const LuloProcMeta *meta)
{
    if (!snap || !meta || !snap->background_enabled) return 0;
    if (snap->background_match_app_slice &&
        meta->cgroup[0] && strstr(meta->cgroup, "/app.slice/")) return 1;
    if (snap->background_match_background_slice &&
        meta->cgroup[0] && strstr(meta->cgroup, "/background.slice/")) return 1;
    if (snap->background_match_app_unit_prefix &&
        meta->unit[0] && strncmp(meta->unit, "app-", 4) == 0) return 1;
    return 0;
}

static int process_matches_focus(const LuloSchedSnapshot *snap, const LuloProcMeta *meta)
{
    if (!snap || !meta || snap->focused_pid <= 0) return 0;
    if (meta->pid == snap->focused_pid && meta->start_time == snap->focused_start_time) return 1;
    if (snap->focused_unit[0] && meta->unit[0] && strcmp(meta->unit, snap->focused_unit) == 0) return 1;
    if (snap->focused_cgroup[0] && meta->cgroup[0] && strcmp(meta->cgroup, snap->focused_cgroup) == 0) return 1;
    return 0;
}

static int find_dynamic_profile(const LuloSchedSnapshot *snap, const LuloProcMeta *meta,
                                char *rule_name, size_t rule_name_len, int *focused)
{
    int idx = -1;

    if (focused) *focused = 0;
    if (!snap || !meta) return -1;

    if (snap->focus_enabled && snap->focus_profile[0] && process_matches_focus(snap, meta)) {
        idx = find_enabled_profile_index(snap, snap->focus_profile);
        if (idx >= 0) {
            if (rule_name && rule_name_len > 0) snprintf(rule_name, rule_name_len, "(focus)");
            if (focused) *focused = 1;
            return idx;
        }
    }

    if (snap->background_enabled && snap->background_profile[0] &&
        looks_like_application_process(snap, meta)) {
        idx = find_enabled_profile_index(snap, snap->background_profile);
        if (idx >= 0) {
            if (rule_name && rule_name_len > 0) snprintf(rule_name, rule_name_len, "(background)");
            return idx;
        }
    }

    return -1;
}

static int find_focus_profile(const LuloSchedSnapshot *snap, const LuloProcMeta *meta,
                              char *rule_name, size_t rule_name_len, int *focused)
{
    int idx;

    if (focused) *focused = 0;
    if (!snap || !meta) return -1;
    if (!(snap->focus_enabled && snap->focus_profile[0] && process_matches_focus(snap, meta))) return -1;
    idx = find_enabled_profile_index(snap, snap->focus_profile);
    if (idx < 0) return -1;
    if (rule_name && rule_name_len > 0) snprintf(rule_name, rule_name_len, "(focus)");
    if (focused) *focused = 1;
    return idx;
}

static void set_status_error(char *buf, size_t len, const char *prefix)
{
    if (!buf || len == 0 || !prefix) return;
    snprintf(buf, len, "%s: %s", prefix, strerror(errno));
}

static int profile_targets_match(const LuloSchedProfileRow *profile,
                                 int nice_value, int policy, int rt_priority,
                                 int io_class, int io_priority)
{
    int target_policy;
    int target_rt_priority;
    int target_io_class;
    int target_io_priority;

    if (!profile) return 0;
    if (profile->has_nice && nice_value != profile->nice) return 0;

    target_policy = normalized_policy(policy);
    target_rt_priority = rt_priority;
    if (profile->has_policy) target_policy = normalized_policy(profile->policy);
    if (target_policy == SCHED_FIFO || target_policy == SCHED_RR) {
        if (profile->has_rt_priority) target_rt_priority = profile->rt_priority;
        else if (target_rt_priority <= 0) target_rt_priority = 1;
    } else {
        target_rt_priority = 0;
    }

    if (normalized_policy(policy) != target_policy) return 0;
    if ((target_policy == SCHED_FIFO || target_policy == SCHED_RR) &&
        rt_priority != target_rt_priority) {
        return 0;
    }
    if (target_policy != SCHED_FIFO && target_policy != SCHED_RR && rt_priority != 0) return 0;
    desired_io_target(profile, io_class, io_priority, &target_io_class, &target_io_priority);
    if (profile->has_io_class || profile->has_io_priority) {
        if (io_class != target_io_class) return 0;
        if ((target_io_class == IOPRIO_CLASS_RT || target_io_class == IOPRIO_CLASS_BE) &&
            io_priority != target_io_priority) return 0;
        if (target_io_class != IOPRIO_CLASS_RT && target_io_class != IOPRIO_CLASS_BE &&
            io_priority != 0) {
            return 0;
        }
    }
    return 1;
}

static void apply_profile_to_pid(pid_t pid, const LuloSchedProfileRow *profile,
                                 int current_nice, int current_policy, int current_rt_priority,
                                 int current_io_class, int current_io_priority,
                                 int *out_nice, int *out_policy, int *out_rt_priority,
                                 int *out_io_class, int *out_io_priority,
                                 char *status, size_t status_len)
{
    int changed = 0;
    int target_policy = current_policy;
    int target_rt_priority = current_rt_priority;
    int target_io_class = current_io_class;
    int target_io_priority = current_io_priority;
    int sched_attempted = 0;
    int sched_failed = 0;
    int io_attempted = 0;
    int io_failed = 0;
    struct sched_param param;

    if (status_len > 0) status[0] = '\0';
    if (profile->has_nice && current_nice != profile->nice) {
        if (setpriority(PRIO_PROCESS, pid, profile->nice) < 0) {
            set_status_error(status, status_len, "nice");
        } else {
            changed = 1;
        }
    }

    if (profile->has_policy) target_policy = profile->policy;
    if (target_policy == SCHED_FIFO || target_policy == SCHED_RR) {
        if (profile->has_rt_priority) target_rt_priority = profile->rt_priority;
        else if (target_rt_priority <= 0) target_rt_priority = 1;
    } else {
        target_rt_priority = 0;
    }

    if ((profile->has_policy || profile->has_rt_priority) &&
        (normalized_policy(current_policy) != normalized_policy(target_policy) ||
         current_rt_priority != target_rt_priority)) {
        memset(&param, 0, sizeof(param));
        param.sched_priority = target_rt_priority;
        sched_attempted = 1;
        if (sched_setscheduler(pid, target_policy, &param) < 0) {
            sched_failed = 1;
            if (!status[0]) set_status_error(status, status_len, "sched");
        } else {
            changed = 1;
        }
    }

    desired_io_target(profile, current_io_class, current_io_priority,
                      &target_io_class, &target_io_priority);
    if ((profile->has_io_class || profile->has_io_priority) &&
        (current_io_class != target_io_class || current_io_priority != target_io_priority)) {
        io_attempted = 1;
        if (syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, (int)pid,
                    IOPRIO_PRIO_VALUE(target_io_class, target_io_priority)) < 0) {
            io_failed = 1;
            if (!status[0]) set_status_error(status, status_len, "io");
        } else {
            changed = 1;
        }
    }

    if (query_process_sched(pid, out_nice, out_policy, out_rt_priority) < 0) {
        if (!status[0]) set_status_error(status, status_len, "query");
        *out_nice = current_nice;
        *out_policy = current_policy;
        *out_rt_priority = current_rt_priority;
        *out_io_class = current_io_class;
        *out_io_priority = current_io_priority;
    } else {
        if (query_process_io(pid, out_io_class, out_io_priority) < 0) {
            if (!status[0]) set_status_error(status, status_len, "io-query");
            *out_io_class = current_io_class;
            *out_io_priority = current_io_priority;
        }
        if (profile_targets_match(profile, *out_nice, *out_policy, *out_rt_priority,
                                  *out_io_class, *out_io_priority)) {
            snprintf(status, status_len, "%s", changed ? "applied" : "ok");
        } else {
            if (sched_attempted && sched_failed &&
                normalized_policy(*out_policy) == normalized_policy(target_policy) &&
                *out_rt_priority == target_rt_priority) {
                status[0] = '\0';
            }
            if (io_attempted && io_failed &&
                *out_io_class == target_io_class &&
                *out_io_priority == target_io_priority) {
                status[0] = '\0';
            }
            if (!status[0]) {
                snprintf(status, status_len, "%s", changed ? "applied" : "ok");
            }
        }
    }
}

static int live_row_cmp(const void *a, const void *b)
{
    const LuloSchedLiveRow *ra = a;
    const LuloSchedLiveRow *rb = b;
    int pa;
    int pb;
    int ia;
    int ib;
    int cmp;

    pa = normalized_policy(ra->policy);
    pb = normalized_policy(rb->policy);
    if (pa != pb) {
        int ra_rank = pa == SCHED_FIFO ? 0 :
                      pa == SCHED_RR ? 1 :
                      pa == SCHED_OTHER ? 2 :
                      pa == SCHED_BATCH ? 3 :
                      pa == SCHED_IDLE ? 4 : 5;
        int rb_rank = pb == SCHED_FIFO ? 0 :
                      pb == SCHED_RR ? 1 :
                      pb == SCHED_OTHER ? 2 :
                      pb == SCHED_BATCH ? 3 :
                      pb == SCHED_IDLE ? 4 : 5;

        if (ra_rank != rb_rank) return ra_rank - rb_rank;
    }
    if (ra->rt_priority != rb->rt_priority) return rb->rt_priority - ra->rt_priority;
    ia = ra->io_class == IOPRIO_CLASS_RT ? 0 :
         ra->io_class == IOPRIO_CLASS_BE ? 1 :
         ra->io_class == IOPRIO_CLASS_NONE ? 2 :
         ra->io_class == IOPRIO_CLASS_IDLE ? 3 : 4;
    ib = rb->io_class == IOPRIO_CLASS_RT ? 0 :
         rb->io_class == IOPRIO_CLASS_BE ? 1 :
         rb->io_class == IOPRIO_CLASS_NONE ? 2 :
         rb->io_class == IOPRIO_CLASS_IDLE ? 3 : 4;
    if (ia != ib) return ia - ib;
    if ((ra->io_class == IOPRIO_CLASS_RT || ra->io_class == IOPRIO_CLASS_BE) &&
        ra->io_priority != rb->io_priority) {
        return ra->io_priority - rb->io_priority;
    }
    if (ra->nice != rb->nice) return ra->nice - rb->nice;
    if (ra->focused != rb->focused) return rb->focused - ra->focused;
    cmp = strcmp(ra->profile, rb->profile);
    if (cmp != 0) return cmp;
    cmp = strcmp(ra->rule, rb->rule);
    if (cmp != 0) return cmp;
    cmp = strcmp(ra->comm, rb->comm);
    if (cmp != 0) return cmp;
    return ra->pid - rb->pid;
}

int lulod_system_sched_default_config_root(char *buf, size_t len)
{
    const char *override = getenv("LULOD_SYSTEM_CONFIG_DIR");

    if (!buf || len == 0) return -1;
    if (override && *override) {
        snprintf(buf, len, "%s", override);
        return 0;
    }
    snprintf(buf, len, "/etc/lulo/scheduler");
    return 0;
}

int lulod_system_sched_set_focus(LuloSchedSnapshot *snap, pid_t pid, unsigned long long start_time,
                                 const char *provider)
{
    LuloProcMeta meta;

    if (!snap) return -1;
    if (provider) snprintf(snap->focus_provider, sizeof(snap->focus_provider), "%s", provider);
    if (pid <= 0) {
        clear_focus_target(snap, 1);
        return 0;
    }
    if (lulo_proc_meta_collect(pid, &meta) < 0 || meta.start_time != start_time) {
        clear_focus_target(snap, 1);
        return -1;
    }
    copy_focus_target(snap, &meta);
    return 0;
}

int lulod_system_sched_reload(LuloSchedSnapshot *snap, const char *config_root,
                              char *err, size_t errlen)
{
    LuloSchedSnapshot fresh = {0};

    if (err && errlen > 0) err[0] = '\0';
    if (!snap || !config_root || !*config_root) {
        if (err && errlen > 0) snprintf(err, errlen, "invalid scheduler config root");
        return -1;
    }

    snprintf(fresh.config_root, sizeof(fresh.config_root), "%s", config_root);
    fresh.watcher_interval_ms = 1000;
    fresh.focus_enabled = 1;
    snprintf(fresh.focus_profile, sizeof(fresh.focus_profile), "focused");
    fresh.background_enabled = 1;
    snprintf(fresh.background_profile, sizeof(fresh.background_profile), "background");
    fresh.background_match_app_slice = 1;
    fresh.background_match_background_slice = 1;
    fresh.background_match_app_unit_prefix = 1;
    fresh.focused_pid = snap->focused_pid;
    fresh.focused_start_time = snap->focused_start_time;
    snprintf(fresh.focus_provider, sizeof(fresh.focus_provider), "%s", snap->focus_provider);
    snprintf(fresh.focused_comm, sizeof(fresh.focused_comm), "%s", snap->focused_comm);
    snprintf(fresh.focused_exe, sizeof(fresh.focused_exe), "%s", snap->focused_exe);
    snprintf(fresh.focused_unit, sizeof(fresh.focused_unit), "%s", snap->focused_unit);
    snprintf(fresh.focused_slice, sizeof(fresh.focused_slice), "%s", snap->focused_slice);
    snprintf(fresh.focused_cgroup, sizeof(fresh.focused_cgroup), "%s", snap->focused_cgroup);

    if (load_global_config(&fresh, config_root, err, errlen) < 0 ||
        load_profiles(&fresh, config_root, err, errlen) < 0 ||
        load_rules(&fresh, config_root, err, errlen) < 0) {
        lulo_sched_snapshot_free(&fresh);
        return -1;
    }

    lulo_sched_snapshot_free(snap);
    *snap = fresh;
    memset(&fresh, 0, sizeof(fresh));
    return 0;
}

int lulod_system_sched_scan(LuloSchedSnapshot *snap, char *err, size_t errlen)
{
    DIR *dir = NULL;
    struct dirent *ent;
    LuloSchedLiveRow *live = NULL;
    int live_count = 0;
    int rc = -1;

    if (err && errlen > 0) err[0] = '\0';
    if (!snap) {
        if (err && errlen > 0) snprintf(err, errlen, "invalid scheduler state");
        return -1;
    }
    validate_focus_target(snap);

    dir = opendir("/proc");
    if (!dir) {
        if (err && errlen > 0) snprintf(err, errlen, "/proc: %s", strerror(errno));
        return -1;
    }

    while ((ent = readdir(dir)) != NULL) {
        char *end = NULL;
        long parsed_pid;
        pid_t pid;
        LuloProcMeta meta;
        int rule_idx = -1;
        int profile_idx = -1;
        int matched_profile_idx = -1;
        int match_rc;
        int nice_value = 0;
        int policy = 0;
        int rt_priority = 0;
        int io_class = IOPRIO_CLASS_NONE;
        int io_priority = 0;
        int focused = 0;
        LuloSchedLiveRow row;
        char rule_name[64] = "";

        if (!isdigit((unsigned char)ent->d_name[0])) continue;
        errno = 0;
        parsed_pid = strtol(ent->d_name, &end, 10);
        if (errno != 0 || !end || *end != '\0' || parsed_pid <= 0) continue;
        pid = (pid_t)parsed_pid;
        if (lulo_proc_meta_collect(pid, &meta) < 0) continue;
        match_rc = find_matching_rule(snap, &meta, &rule_idx, &profile_idx);
        if (match_rc < 0) continue;
        matched_profile_idx = profile_idx;

        profile_idx = find_focus_profile(snap, &meta, rule_name, sizeof(rule_name), &focused);
        if (profile_idx < 0 && match_rc > 0) {
            profile_idx = matched_profile_idx;
            snprintf(rule_name, sizeof(rule_name), "%s", snap->rules[rule_idx].name);
            focused = process_matches_focus(snap, &meta);
        } else if (profile_idx < 0) {
            profile_idx = find_dynamic_profile(snap, &meta, rule_name, sizeof(rule_name), &focused);
            if (profile_idx < 0) continue;
        }
        if (query_process_sched(pid, &nice_value, &policy, &rt_priority) < 0) continue;
        if (query_process_io(pid, &io_class, &io_priority) < 0) {
            io_class = IOPRIO_CLASS_NONE;
            io_priority = 0;
        }

        memset(&row, 0, sizeof(row));
        row.pid = pid;
        row.start_time = meta.start_time;
        row.focused = focused;
        snprintf(row.comm, sizeof(row.comm), "%s", meta.comm);
        snprintf(row.exe, sizeof(row.exe), "%s", meta.exe);
        snprintf(row.unit, sizeof(row.unit), "%s", meta.unit);
        snprintf(row.slice, sizeof(row.slice), "%s", meta.slice);
        snprintf(row.cgroup, sizeof(row.cgroup), "%s", meta.cgroup);
        snprintf(row.profile, sizeof(row.profile), "%s", snap->profiles[profile_idx].name);
        snprintf(row.rule, sizeof(row.rule), "%s", rule_name);

        apply_profile_to_pid(pid, &snap->profiles[profile_idx],
                             nice_value, policy, rt_priority, io_class, io_priority,
                             &row.nice, &row.policy, &row.rt_priority,
                             &row.io_class, &row.io_priority,
                             row.status, sizeof(row.status));

        {
            LuloSchedLiveRow *next = realloc(live, (size_t)(live_count + 1) * sizeof(*next));
            if (!next) {
                if (err && errlen > 0) snprintf(err, errlen, "out of memory scanning processes");
                goto out;
            }
            live = next;
            live[live_count++] = row;
        }
    }

    if (live_count > 1) qsort(live, (size_t)live_count, sizeof(*live), live_row_cmp);

    free(snap->live);
    snap->live = live;
    snap->live_count = live_count;
    snap->scan_generation++;
    rc = 0;
    live = NULL;

out:
    free(live);
    closedir(dir);
    return rc;
}
