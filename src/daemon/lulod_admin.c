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

static int helper_binary_path(char *buf, size_t len)
{
    char exe[PATH_MAX];
    ssize_t n;
    char *slash;

    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n < 0) return -1;
    exe[n] = '\0';
    slash = strrchr(exe, '/');
    if (!slash) return -1;
    slash[1] = '\0';
    if (strlen(exe) + strlen("lulo-admin") + 1 > len) return -1;
    snprintf(buf, len, "%slulo-admin", exe);
    return 0;
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

int lulod_admin_apply_tune_plan(const LuloAdminTunePlan *plan, char *err, size_t errlen)
{
    char helper[PATH_MAX];
    int stdin_pipe[2] = { -1, -1 };
    int stderr_pipe[2] = { -1, -1 };
    pid_t pid;
    int status = 0;
    FILE *fp = NULL;

    if (err && errlen > 0) err[0] = '\0';
    if (!plan || plan->count <= 0) {
        if (err && errlen > 0) snprintf(err, errlen, "empty apply plan");
        return -1;
    }
    if (helper_binary_path(helper, sizeof(helper)) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to resolve lulo-admin path");
        return -1;
    }
    if (pipe2(stdin_pipe, O_CLOEXEC) < 0 || pipe2(stderr_pipe, O_CLOEXEC) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to create helper pipes");
        goto fail;
    }

    pid = fork();
    if (pid < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to fork helper");
        goto fail;
    }
    if (pid == 0) {
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        dup2(stderr_pipe[1], STDOUT_FILENO);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        execlp("pkexec", "pkexec", helper, "apply-tune", (char *)NULL);
        _exit(127);
    }

    close(stdin_pipe[0]);
    stdin_pipe[0] = -1;
    close(stderr_pipe[1]);
    stderr_pipe[1] = -1;

    fp = fdopen(stdin_pipe[1], "w");
    if (!fp) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to open helper stdin");
        close(stdin_pipe[1]);
        stdin_pipe[1] = -1;
        goto wait_child;
    }
    if (lulo_admin_tune_plan_write_stream(fp, plan, err, errlen) < 0) {
        fclose(fp);
        fp = NULL;
        stdin_pipe[1] = -1;
        goto wait_child;
    }
    fclose(fp);
    fp = NULL;
    stdin_pipe[1] = -1;

wait_child:
    read_error_pipe(stderr_pipe[0], err, errlen);
    close(stderr_pipe[0]);
    stderr_pipe[0] = -1;
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
    if (stdin_pipe[0] >= 0) close(stdin_pipe[0]);
    if (stdin_pipe[1] >= 0) close(stdin_pipe[1]);
    if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
    if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);
    return -1;
}
