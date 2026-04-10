#define _GNU_SOURCE

#include "lulod_admin.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef LULO_HELPERDIR
#define LULO_HELPERDIR ""
#endif

static int helper_binary_path(char *buf, size_t len)
{
    char exe[PATH_MAX];
    ssize_t n;
    char *slash;
    size_t exe_len;

    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n >= 0) {
        exe[n] = '\0';
        slash = strrchr(exe, '/');
        if (!slash) return -1;
        slash[1] = '\0';
        exe_len = strlen(exe);
        if (exe_len + sizeof("lulo-admin") <= len) {
            memcpy(buf, exe, exe_len);
            memcpy(buf + exe_len, "lulo-admin", sizeof("lulo-admin"));
            if (access(buf, X_OK) == 0) return 0;
        }
    }
    if (LULO_HELPERDIR[0] == '\0') return -1;
    if (snprintf(buf, len, "%s/lulo-admin", LULO_HELPERDIR) >= (int)len) return -1;
    if (access(buf, X_OK) == 0) return 0;
    return -1;
}

static int read_error_pipe(int fd, char *err, size_t errlen)
{
    char buf[256];
    size_t used = 0;

    if (err && errlen > 0) err[0] = '\0';
    while (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return 0;
        if (err && errlen > 1 && used < errlen - 1) {
            size_t take = (size_t)n;
            if (take > errlen - 1 - used) take = errlen - 1 - used;
            memcpy(err + used, buf, take);
            used += take;
            err[used] = '\0';
        }
    }
    return 0;
}

static void close_if_open(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

int lulod_admin_apply_tune_plan(const LuloAdminTunePlan *plan, char *err, size_t errlen)
{
    char helper[PATH_MAX];
    int stdin_read_fd = -1;
    int stdin_write_fd = -1;
    int stderr_read_fd = -1;
    int stderr_write_fd = -1;
    pid_t pid;
    int status = 0;
    FILE *fp = NULL;
    int fds[2];

    if (err && errlen > 0) err[0] = '\0';
    if (!plan || plan->count <= 0) {
        if (err && errlen > 0) snprintf(err, errlen, "empty apply plan");
        return -1;
    }
    if (helper_binary_path(helper, sizeof(helper)) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to resolve lulo-admin path");
        return -1;
    }
    if (pipe2(fds, O_CLOEXEC) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to create helper pipes");
        goto fail;
    }
    stdin_read_fd = fds[0];
    stdin_write_fd = fds[1];
    if (pipe2(fds, O_CLOEXEC) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to create helper pipes");
        goto fail;
    }
    stderr_read_fd = fds[0];
    stderr_write_fd = fds[1];

    pid = fork();
    if (pid < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to fork helper");
        goto fail;
    }
    if (pid == 0) {
        dup2(stdin_read_fd, STDIN_FILENO);
        dup2(stderr_write_fd, STDERR_FILENO);
        dup2(stderr_write_fd, STDOUT_FILENO);
        close_if_open(&stdin_read_fd);
        close_if_open(&stdin_write_fd);
        close_if_open(&stderr_read_fd);
        close_if_open(&stderr_write_fd);
        execlp("pkexec", "pkexec", helper, "apply-tune", (char *)NULL);
        _exit(127);
    }

    close_if_open(&stdin_read_fd);
    close_if_open(&stderr_write_fd);

    fp = fdopen(stdin_write_fd, "w");
    if (!fp) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to open helper stdin");
        close_if_open(&stdin_write_fd);
        goto wait_child;
    }
    if (lulo_admin_tune_plan_write_stream(fp, plan, err, errlen) < 0) {
        fclose(fp);
        fp = NULL;
        stdin_write_fd = -1;
        goto wait_child;
    }
    fclose(fp);
    fp = NULL;
    stdin_write_fd = -1;

wait_child:
    read_error_pipe(stderr_read_fd, err, errlen);
    close_if_open(&stderr_read_fd);
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) break;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
    if (err && errlen > 0 && !err[0]) {
        if (WIFEXITED(status)) snprintf(err, errlen, "helper exited with status %d", WEXITSTATUS(status));
        else snprintf(err, errlen, "helper failed");
    }
    return -1;

fail:
    close_if_open(&stdin_read_fd);
    close_if_open(&stdin_write_fd);
    close_if_open(&stderr_read_fd);
    close_if_open(&stderr_write_fd);
    return -1;
}
