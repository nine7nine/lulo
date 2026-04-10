#define _GNU_SOURCE

#include "lulo_admin.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LULO_ADMIN_TUNE_PLAN_MAGIC "lulo-admin-tune-v1"

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;

    if (!err || errlen == 0) return;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static void trim_right(char *buf)
{
    size_t len;

    if (!buf) return;
    len = strlen(buf);
    while (len > 0 && isspace((unsigned char)buf[len - 1])) buf[--len] = '\0';
}

static int path_has_prefix(const char *path, const char *prefix)
{
    size_t len;

    if (!path || !prefix) return 0;
    len = strlen(prefix);
    return strncmp(path, prefix, len) == 0 &&
           (path[len] == '\0' || path[len] == '/');
}

static int write_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;

    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

void lulo_admin_tune_plan_init(LuloAdminTunePlan *plan)
{
    if (!plan) return;
    memset(plan, 0, sizeof(*plan));
}

void lulo_admin_tune_plan_free(LuloAdminTunePlan *plan)
{
    if (!plan) return;
    free(plan->items);
    memset(plan, 0, sizeof(*plan));
}

int lulo_admin_tune_plan_append(LuloAdminTunePlan *plan, const char *path, const char *value)
{
    LuloAdminTuneItem *next;
    LuloAdminTuneItem *item;

    if (!plan || !path || !*path || !value) return -1;
    next = realloc(plan->items, (size_t)(plan->count + 1) * sizeof(*next));
    if (!next) return -1;
    plan->items = next;
    item = &next[plan->count++];
    memset(item, 0, sizeof(*item));
    snprintf(item->path, sizeof(item->path), "%s", path);
    snprintf(item->value, sizeof(item->value), "%s", value);
    return 0;
}

int lulo_admin_tune_plan_write_stream(FILE *fp, const LuloAdminTunePlan *plan,
                                      char *err, size_t errlen)
{
    if (!fp || !plan || plan->count <= 0) {
        set_err(err, errlen, "empty apply plan");
        return -1;
    }
    if (fprintf(fp, "%s\t%d\n", LULO_ADMIN_TUNE_PLAN_MAGIC, plan->count) < 0) {
        set_err(err, errlen, "failed to write apply plan header");
        return -1;
    }
    for (int i = 0; i < plan->count; i++) {
        if (strchr(plan->items[i].path, '\t') || strchr(plan->items[i].value, '\t') ||
            strchr(plan->items[i].path, '\n') || strchr(plan->items[i].value, '\n')) {
            set_err(err, errlen, "apply plan contains unsupported control characters");
            return -1;
        }
        if (fprintf(fp, "%s\t%s\n", plan->items[i].path, plan->items[i].value) < 0) {
            set_err(err, errlen, "failed to write apply plan item");
            return -1;
        }
    }
    if (fflush(fp) < 0) {
        set_err(err, errlen, "failed to flush apply plan");
        return -1;
    }
    return 0;
}

int lulo_admin_tune_plan_read_stream(FILE *fp, LuloAdminTunePlan *plan,
                                     char *err, size_t errlen)
{
    char line[1024];
    int expected = 0;

    if (!fp || !plan) {
        set_err(err, errlen, "bad apply plan stream");
        return -1;
    }
    lulo_admin_tune_plan_free(plan);
    if (!fgets(line, sizeof(line), fp)) {
        set_err(err, errlen, "empty apply plan");
        return -1;
    }
    trim_right(line);
    if (sscanf(line, LULO_ADMIN_TUNE_PLAN_MAGIC "\t%d", &expected) != 1 || expected <= 0) {
        set_err(err, errlen, "invalid apply plan header");
        return -1;
    }
    for (int i = 0; i < expected; i++) {
        char *tab;

        if (!fgets(line, sizeof(line), fp)) {
            set_err(err, errlen, "truncated apply plan");
            lulo_admin_tune_plan_free(plan);
            return -1;
        }
        trim_right(line);
        tab = strchr(line, '\t');
        if (!tab) {
            set_err(err, errlen, "invalid apply plan item");
            lulo_admin_tune_plan_free(plan);
            return -1;
        }
        *tab++ = '\0';
        if (lulo_admin_tune_plan_append(plan, line, tab) < 0) {
            set_err(err, errlen, "failed to append apply item");
            lulo_admin_tune_plan_free(plan);
            return -1;
        }
    }
    return 0;
}

int lulo_admin_tune_path_allowed(const char *path, char *resolved, size_t len,
                                 char *err, size_t errlen)
{
    static const char *allowed_roots[] = {
        "/proc/sys",
        "/sys",
        "/sys/fs/cgroup",
    };
    char real[PATH_MAX];

    if (!path || !*path) {
        set_err(err, errlen, "missing path");
        return -1;
    }
    if (!realpath(path, real)) {
        set_err(err, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }
    for (size_t i = 0; i < sizeof(allowed_roots) / sizeof(allowed_roots[0]); i++) {
        if (path_has_prefix(real, allowed_roots[i])) {
            if (resolved && len > 0) snprintf(resolved, len, "%s", real);
            return 0;
        }
    }
    set_err(err, errlen, "%s is outside the allowed tunable roots", real);
    return -1;
}

int lulo_admin_tune_write_value(const char *path, const char *value,
                                char *err, size_t errlen)
{
    char resolved[PATH_MAX];
    struct stat st;
    int fd = -1;

    if (lulo_admin_tune_path_allowed(path, resolved, sizeof(resolved), err, errlen) < 0) return -1;
    if (stat(resolved, &st) < 0) {
        set_err(err, errlen, "%s: %s", resolved, strerror(errno));
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        set_err(err, errlen, "%s is a directory", resolved);
        return -1;
    }
    fd = open(resolved, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        set_err(err, errlen, "%s: %s", resolved, strerror(errno));
        return -1;
    }
    if (value && *value) {
        if (write_all(fd, value, strlen(value)) < 0) {
            set_err(err, errlen, "%s: %s", resolved, strerror(errno));
            close(fd);
            return -1;
        }
    }
    close(fd);
    return 0;
}
