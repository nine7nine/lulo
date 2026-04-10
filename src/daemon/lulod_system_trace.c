#define _GNU_SOURCE

#include "lulod_system_trace.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    uid_t uid;
    gid_t gid;
    pid_t child_pid;
    pid_t target_pid;
    char output_path[PATH_MAX];
} TraceMeta;

static const char *k_trace_meta_root = "/run/lulo-trace-meta";

static int ensure_dir_mode(const char *path, mode_t mode)
{
    struct stat st;

    if (mkdir(path, mode) == 0) return 0;
    if (errno != EEXIST) return -1;
    if (stat(path, &st) < 0) return -1;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

static int trace_runtime_dir(uid_t uid, char *buf, size_t len)
{
    if (snprintf(buf, len, "/run/user/%lu/lulo-trace", (unsigned long)uid) >= (int)len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int ensure_user_trace_dir(uid_t uid, gid_t gid, char *path, size_t path_len)
{
    struct stat st;

    if (trace_runtime_dir(uid, path, path_len) < 0) return -1;
    if (mkdir(path, 0700) < 0 && errno != EEXIST) return -1;
    if (stat(path, &st) < 0) return -1;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    if (chown(path, uid, gid) < 0) return -1;
    if (chmod(path, 0700) < 0) return -1;
    return 0;
}

static int validate_session_id(const char *session_id)
{
    const unsigned char *p = (const unsigned char *)session_id;

    if (!session_id || !*session_id) return -1;
    while (*p) {
        if (!(isalnum(*p) || *p == '-' || *p == '_' || *p == '.')) return -1;
        p++;
    }
    return 0;
}

static int session_paths(const char *session_id, uid_t uid,
                         char *output_path, size_t output_path_len,
                         char *meta_path, size_t meta_path_len)
{
    char trace_root[PATH_MAX];

    if (validate_session_id(session_id) < 0) {
        errno = EINVAL;
        return -1;
    }
    if (trace_runtime_dir(uid, trace_root, sizeof(trace_root)) < 0) return -1;
    if (snprintf(output_path, output_path_len, "%s/%s.log", trace_root, session_id) >= (int)output_path_len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (snprintf(meta_path, meta_path_len, "%s/%s.meta", k_trace_meta_root, session_id) >= (int)meta_path_len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int write_meta_file(const char *meta_path, const TraceMeta *meta)
{
    FILE *fp = fopen(meta_path, "w");

    if (!fp) return -1;
    if (fprintf(fp, "uid=%lu\ngid=%lu\nchild_pid=%ld\ntarget_pid=%ld\noutput_path=%s\n",
                (unsigned long)meta->uid, (unsigned long)meta->gid,
                (long)meta->child_pid, (long)meta->target_pid, meta->output_path) < 0) {
        fclose(fp);
        return -1;
    }
    if (fchmod(fileno(fp), 0600) < 0) {
        fclose(fp);
        return -1;
    }
    if (fclose(fp) < 0) return -1;
    return 0;
}

static int read_meta_file(const char *meta_path, TraceMeta *meta)
{
    FILE *fp;
    char line[PATH_MAX + 64];

    memset(meta, 0, sizeof(*meta));
    fp = fopen(meta_path, "r");
    if (!fp) return -1;
    while (fgets(line, sizeof(line), fp)) {
        char *eq = strchr(line, '=');
        char *key;
        char *value;

        if (!eq) continue;
        *eq = '\0';
        key = line;
        value = eq + 1;
        value[strcspn(value, "\r\n")] = '\0';
        if (strcmp(key, "uid") == 0) meta->uid = (uid_t)strtoul(value, NULL, 10);
        else if (strcmp(key, "gid") == 0) meta->gid = (gid_t)strtoul(value, NULL, 10);
        else if (strcmp(key, "child_pid") == 0) meta->child_pid = (pid_t)strtol(value, NULL, 10);
        else if (strcmp(key, "target_pid") == 0) meta->target_pid = (pid_t)strtol(value, NULL, 10);
        else if (strcmp(key, "output_path") == 0) snprintf(meta->output_path, sizeof(meta->output_path), "%s", value);
    }
    if (fclose(fp) < 0) return -1;
    if (meta->uid == 0 || meta->child_pid <= 0 || !meta->output_path[0]) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int read_text_file(const char *path, char *buf, size_t len)
{
    FILE *fp;

    if (!buf || len == 0) {
        errno = EINVAL;
        return -1;
    }
    buf[0] = '\0';
    fp = fopen(path, "r");
    if (!fp) return -1;
    if (!fgets(buf, (int)len, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}

static int process_is_traceable(pid_t pid, char *err, size_t errlen)
{
    char exe_path[64];
    char linkbuf[PATH_MAX];

    if (pid <= 0) {
        if (err && errlen > 0) snprintf(err, errlen, "invalid pid");
        errno = EINVAL;
        return -1;
    }
    if (snprintf(exe_path, sizeof(exe_path), "/proc/%ld/exe", (long)pid) >= (int)sizeof(exe_path)) {
        if (err && errlen > 0) snprintf(err, errlen, "invalid pid");
        errno = EINVAL;
        return -1;
    }
    if (kill(pid, 0) < 0 && errno != EPERM) {
        if (err && errlen > 0) snprintf(err, errlen, "pid %ld not found", (long)pid);
        return -1;
    }
    if (readlink(exe_path, linkbuf, sizeof(linkbuf) - 1) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "kernel threads are not supported");
        errno = EPERM;
        return -1;
    }
    if (access("/usr/bin/strace", X_OK) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "strace is not installed");
        return -1;
    }
    return 0;
}

static int next_session_id(char *buf, size_t len, pid_t target_pid)
{
    struct timespec ts;
    unsigned long mix;

    if (!buf || len == 0) return -1;
    clock_gettime(CLOCK_REALTIME, &ts);
    mix = (unsigned long)(ts.tv_nsec ^ (long)target_pid ^ getpid());
    if (snprintf(buf, len, "trace-%ld-%ld-%lx",
                 (long)target_pid, (long)ts.tv_sec, mix & 0xfffffUL) >= (int)len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int read_first_error_line(const char *path, char *err, size_t errlen)
{
    char line[256];

    if (read_text_file(path, line, sizeof(line)) < 0) return -1;
    if (err && errlen > 0) snprintf(err, errlen, "%s", line);
    return 0;
}

int lulod_system_trace_begin(pid_t target_pid, uid_t uid, gid_t gid,
                             char *session_id, size_t session_id_len,
                             char *output_path, size_t output_path_len,
                             char *err, size_t errlen)
{
    char trace_root[PATH_MAX];
    char meta_path[PATH_MAX];
    TraceMeta meta;
    int fd = -1;
    pid_t child;

    if (err && errlen > 0) err[0] = '\0';
    if (!session_id || !output_path) {
        errno = EINVAL;
        return -1;
    }
    if (ensure_dir_mode(k_trace_meta_root, 0700) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to create trace runtime");
        return -1;
    }
    if (ensure_user_trace_dir(uid, gid, trace_root, sizeof(trace_root)) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to create user trace dir");
        return -1;
    }
    if (process_is_traceable(target_pid, err, errlen) < 0) return -1;
    if (next_session_id(session_id, session_id_len, target_pid) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to create trace session");
        return -1;
    }
    if (session_paths(session_id, uid, output_path, output_path_len, meta_path, sizeof(meta_path)) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to create trace paths");
        return -1;
    }

    fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to open trace log");
        return -1;
    }
    if (fchown(fd, uid, gid) < 0) {
        close(fd);
        unlink(output_path);
        if (err && errlen > 0) snprintf(err, errlen, "failed to chown trace log");
        return -1;
    }
    child = fork();
    if (child < 0) {
        close(fd);
        unlink(output_path);
        if (err && errlen > 0) snprintf(err, errlen, "failed to fork strace");
        return -1;
    }
    if (child == 0) {
        char pid_buf[32];
        int null_fd;

        null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            close(null_fd);
        }
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
        snprintf(pid_buf, sizeof(pid_buf), "%ld", (long)target_pid);
        execl("/usr/bin/strace", "strace", "-f", "-s", "256", "-yy", "-ttt", "-p", pid_buf, (char *)NULL);
        _exit(127);
    }
    close(fd);

    memset(&meta, 0, sizeof(meta));
    meta.uid = uid;
    meta.gid = gid;
    meta.child_pid = child;
    meta.target_pid = target_pid;
    snprintf(meta.output_path, sizeof(meta.output_path), "%s", output_path);
    if (write_meta_file(meta_path, &meta) < 0) {
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
        unlink(output_path);
        if (err && errlen > 0) snprintf(err, errlen, "failed to write trace session");
        return -1;
    }

    usleep(150000);
    if (waitpid(child, NULL, WNOHANG) == child) {
        read_first_error_line(output_path, err, errlen);
        unlink(meta_path);
        unlink(output_path);
        errno = EIO;
        return -1;
    }
    return 0;
}

int lulod_system_trace_end(const char *session_id, uid_t uid,
                           char *err, size_t errlen)
{
    char output_path[PATH_MAX];
    char meta_path[PATH_MAX];
    TraceMeta meta;
    int status = 0;

    if (err && errlen > 0) err[0] = '\0';
    if (session_paths(session_id, uid, output_path, sizeof(output_path), meta_path, sizeof(meta_path)) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "invalid trace session");
        return -1;
    }
    if (read_meta_file(meta_path, &meta) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "trace session not found");
        return -1;
    }
    if (meta.uid != uid) {
        if (err && errlen > 0) snprintf(err, errlen, "trace session ownership mismatch");
        errno = EPERM;
        return -1;
    }
    if (meta.child_pid > 0) {
        kill(meta.child_pid, SIGTERM);
        usleep(100000);
        if (waitpid(meta.child_pid, &status, WNOHANG) == 0) {
            kill(meta.child_pid, SIGKILL);
            waitpid(meta.child_pid, &status, 0);
        }
    }
    unlink(meta.output_path);
    unlink(meta_path);
    return 0;
}
