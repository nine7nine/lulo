#define _GNU_SOURCE

#include "lulo_proc_meta.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int read_text_file(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    char *end;

    if (!f) return -1;
    if (!fgets(buf, (int)len, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    end = buf + strlen(buf);
    while (end > buf && (end[-1] == '\n' || end[-1] == '\r' ||
                         end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }
    return 0;
}

static int read_cmdline_joined(const char *path, char *buf, size_t len)
{
    FILE *f;
    size_t nread;

    if (len == 0) return -1;
    f = fopen(path, "r");
    if (!f) return -1;
    nread = fread(buf, 1, len - 1, f);
    fclose(f);
    if (nread == 0) {
        buf[0] = '\0';
        return -1;
    }
    buf[nread] = '\0';
    for (size_t i = 0; i < nread; i++) {
        if (buf[i] == '\0') buf[i] = ' ';
    }
    while (nread > 0 && buf[nread - 1] == ' ') buf[--nread] = '\0';
    return nread > 0 ? 0 : -1;
}

static int parse_proc_stat_identity(const char *line, char *comm, size_t comm_len,
                                    unsigned long long *start_time)
{
    const char *open = strchr(line, '(');
    const char *close = strrchr(line, ')');
    char tail[2048];
    char *saveptr = NULL;
    char *tok;
    int field = 0;

    if (!open || !close || close <= open || !comm || comm_len == 0) return -1;
    if (start_time) *start_time = 0;
    {
        size_t n = (size_t)(close - open - 1);
        if (n >= comm_len) n = comm_len - 1;
        memcpy(comm, open + 1, n);
        comm[n] = '\0';
    }
    snprintf(tail, sizeof(tail), "%s", close + 2);
    tok = strtok_r(tail, " ", &saveptr);
    while (tok) {
        if (field == 19 && start_time) {
            *start_time = strtoull(tok, NULL, 10);
            break;
        }
        field++;
        tok = strtok_r(NULL, " ", &saveptr);
    }
    return 0;
}

int lulo_proc_meta_read_basic(pid_t pid, char *comm, size_t comm_len,
                              unsigned long long *start_time)
{
    char path[64];
    char line[4096];

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    if (read_text_file(path, line, sizeof(line)) < 0) return -1;
    return parse_proc_stat_identity(line, comm, comm_len, start_time);
}

int lulo_proc_meta_read_exe(pid_t pid, char *buf, size_t len)
{
    char path[64];
    ssize_t n;

    if (!buf || len == 0) return -1;
    snprintf(path, sizeof(path), "/proc/%d/exe", pid);
    n = readlink(path, buf, len - 1);
    if (n < 0) {
        buf[0] = '\0';
        return -1;
    }
    buf[n] = '\0';
    return 0;
}

int lulo_proc_meta_read_cmdline(pid_t pid, char *buf, size_t len)
{
    char path[64];

    if (!buf || len == 0) return -1;
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    return read_cmdline_joined(path, buf, len);
}

int lulo_proc_meta_read_cgroup(pid_t pid, char *buf, size_t len)
{
    char path[64];
    FILE *f;
    char line[512];

    if (!buf || len == 0) return -1;
    buf[0] = '\0';
    snprintf(path, sizeof(path), "/proc/%d/cgroup", pid);
    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        char *first = strchr(line, ':');
        char *second;
        char *value;
        char *end;

        if (!first) continue;
        second = strchr(first + 1, ':');
        if (!second) continue;
        value = second + 1;
        end = value + strlen(value);
        while (end > value && (end[-1] == '\n' || end[-1] == '\r')) *--end = '\0';
        if (line[0] == '0' && first == line + 1) {
            snprintf(buf, len, "%s", value);
            fclose(f);
            return 0;
        }
        if (!buf[0]) snprintf(buf, len, "%s", value);
    }
    fclose(f);
    return buf[0] ? 0 : -1;
}

void lulo_proc_meta_derive_unit(const char *cgroup_path, char *buf, size_t len)
{
    const char *start;
    const char *end;

    if (!buf || len == 0) return;
    buf[0] = '\0';
    if (!cgroup_path || !*cgroup_path) return;
    end = cgroup_path + strlen(cgroup_path);
    while (end > cgroup_path) {
        start = end;
        while (start > cgroup_path && start[-1] != '/') start--;
        if (start < end &&
            (strncmp(end - 8, ".service", 8) == 0 || strncmp(end - 6, ".scope", 6) == 0)) {
            size_t n = (size_t)(end - start);
            if (n >= len) n = len - 1;
            memcpy(buf, start, n);
            buf[n] = '\0';
            return;
        }
        if (start == cgroup_path) break;
        end = start - 1;
    }
}

void lulo_proc_meta_derive_slice(const char *cgroup_path, char *buf, size_t len)
{
    const char *start;
    const char *end;

    if (!buf || len == 0) return;
    buf[0] = '\0';
    if (!cgroup_path || !*cgroup_path) return;
    end = cgroup_path + strlen(cgroup_path);
    while (end > cgroup_path) {
        start = end;
        while (start > cgroup_path && start[-1] != '/') start--;
        if (start < end && end - start >= 6 && strncmp(end - 6, ".slice", 6) == 0) {
            size_t n = (size_t)(end - start);
            if (n >= len) n = len - 1;
            memcpy(buf, start, n);
            buf[n] = '\0';
            return;
        }
        if (start == cgroup_path) break;
        end = start - 1;
    }
}

int lulo_proc_meta_collect(pid_t pid, LuloProcMeta *meta)
{
    if (!meta) return -1;
    memset(meta, 0, sizeof(*meta));
    meta->pid = pid;
    if (lulo_proc_meta_read_basic(pid, meta->comm, sizeof(meta->comm), &meta->start_time) < 0) {
        return -1;
    }
    lulo_proc_meta_read_exe(pid, meta->exe, sizeof(meta->exe));
    lulo_proc_meta_read_cmdline(pid, meta->cmdline, sizeof(meta->cmdline));
    lulo_proc_meta_read_cgroup(pid, meta->cgroup, sizeof(meta->cgroup));
    lulo_proc_meta_derive_unit(meta->cgroup, meta->unit, sizeof(meta->unit));
    lulo_proc_meta_derive_slice(meta->cgroup, meta->slice, sizeof(meta->slice));
    return 0;
}
