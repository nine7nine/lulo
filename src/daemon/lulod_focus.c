#define _GNU_SOURCE

#include "lulod_focus.h"
#include "lulo_proc_meta.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef LULO_HELPERDIR
#define LULO_HELPERDIR ""
#endif

static const char *focus_provider_from_env(void)
{
    const char *override = getenv("LULOD_FOCUS_PROVIDER");
    const char *session_type;
    const char *wayland_display;
    const char *desktop_vars[] = {
        "XDG_CURRENT_DESKTOP",
        "XDG_SESSION_DESKTOP",
        "DESKTOP_SESSION",
        "KDE_FULL_SESSION",
        "KDE_SESSION_VERSION",
    };

    if (override && *override) {
        if (!strcmp(override, "none")) return NULL;
        if (!strcmp(override, "kde")) return "kde";
        if (strcmp(override, "auto") != 0) return NULL;
    }

    session_type = getenv("XDG_SESSION_TYPE");
    wayland_display = getenv("WAYLAND_DISPLAY");
    if ((!session_type || strcmp(session_type, "wayland") != 0) &&
        (!wayland_display || !*wayland_display)) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(desktop_vars) / sizeof(desktop_vars[0]); i++) {
        const char *value = getenv(desktop_vars[i]);

        if (!value || !*value) continue;
        if (strcasestr(value, "kde") || strcasestr(value, "plasma")) return "kde";
    }
    return NULL;
}

static int sibling_binary_path(const char *name, char *buf, size_t len)
{
    char exe[PATH_MAX];
    char *slash;
    ssize_t n;

    if (!name || !buf || len == 0) return -1;
    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n < 0) return -1;
    exe[n] = '\0';
    slash = strrchr(exe, '/');
    if (!slash) return -1;
    *slash = '\0';
    if (snprintf(buf, len, "%s/%s", exe, name) >= (int)len) return -1;
    return 0;
}

static int installed_prefix_helper_path(const char *name, char *buf, size_t len)
{
    char exe[PATH_MAX];
    char dirbuf[PATH_MAX];
    char *dir;
    ssize_t n;

    if (!name || !buf || len == 0) return -1;
    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n < 0) return -1;
    exe[n] = '\0';
    snprintf(dirbuf, sizeof(dirbuf), "%s", exe);
    dir = strrchr(dirbuf, '/');
    if (!dir) return -1;
    *dir = '\0';
    dir = strrchr(dirbuf, '/');
    if (!dir) return -1;
    if (strcmp(dir + 1, "bin") != 0) return -1;
    *dir = '\0';
    if (dirbuf[0] == '\0') {
        if (snprintf(buf, len, "/libexec/lulo/%s", name) >= (int)len) return -1;
    } else if (snprintf(buf, len, "%s/libexec/lulo/%s", dirbuf, name) >= (int)len) {
        return -1;
    }
    return 0;
}

static int resolve_helper_binary_path(const char *name, char *buf, size_t len)
{
    if (sibling_binary_path(name, buf, len) == 0 && access(buf, X_OK) == 0) return 0;
    if (installed_prefix_helper_path(name, buf, len) == 0 && access(buf, X_OK) == 0) return 0;
    if (LULO_HELPERDIR[0] != '\0') {
        if (snprintf(buf, len, "%s/%s", LULO_HELPERDIR, name) >= (int)len) return -1;
        if (access(buf, X_OK) == 0) return 0;
    }
    if (snprintf(buf, len, "%s/%s", "/usr/libexec/lulo", name) < (int)len &&
        access(buf, X_OK) == 0) return 0;
    errno = ENOENT;
    return -1;
}

static void close_monitor_fd(LulodFocusMonitor *monitor)
{
    if (monitor->fd >= 0) {
        close(monitor->fd);
        monitor->fd = -1;
    }
}

static void reap_child(pid_t pid)
{
    int status;

    if (pid > 0) {
        while (waitpid(pid, &status, WNOHANG) > 0) {
        }
    }
}

static void schedule_restart(LulodFocusMonitor *monitor, long long now_ms)
{
    if (!monitor) return;
    close_monitor_fd(monitor);
    reap_child(monitor->child_pid);
    monitor->child_pid = -1;
    monitor->restart_due_ms = now_ms + 2000;
    monitor->line_len = 0;
}

static int start_focus_helper(LulodFocusMonitor *monitor, long long now_ms,
                              char *err, size_t errlen)
{
    char helper_path[PATH_MAX];
    int pipes[2] = {-1, -1};
    pid_t child;

    if (!monitor || !monitor->enabled || !monitor->provider[0]) return -1;
    if (strcmp(monitor->provider, "kde") != 0) return -1;
    if (resolve_helper_binary_path("lulod-focus-kde", helper_path, sizeof(helper_path)) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to resolve lulod-focus-kde");
        monitor->enabled = 0;
        return -1;
    }
    if (pipe2(pipes, O_CLOEXEC | O_NONBLOCK) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "pipe2: %s", strerror(errno));
        return -1;
    }
    child = fork();
    if (child < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "fork: %s", strerror(errno));
        close(pipes[0]);
        close(pipes[1]);
        return -1;
    }
    if (child == 0) {
        dup2(pipes[1], STDOUT_FILENO);
        close(pipes[0]);
        close(pipes[1]);
        execl(helper_path, helper_path, (char *)NULL);
        _exit(127);
    }
    close(pipes[1]);
    monitor->fd = pipes[0];
    monitor->child_pid = child;
    monitor->restart_due_ms = 0;
    monitor->line_len = 0;
    (void)now_ms;
    return 0;
}

static int parse_focus_pid_line(const char *line, pid_t *pid_out)
{
    char *end = NULL;
    long parsed;

    if (!line || !pid_out) return -1;
    while (*line && isspace((unsigned char)*line)) line++;
    errno = 0;
    parsed = strtol(line, &end, 10);
    if (errno != 0 || end == line) return -1;
    while (end && *end) {
        if (!isspace((unsigned char)*end)) return -1;
        end++;
    }
    if (parsed < 0) parsed = 0;
    *pid_out = (pid_t)parsed;
    return 0;
}

static int commit_focus_pid(LulodFocusMonitor *monitor, pid_t pid,
                            pid_t *pid_out, unsigned long long *start_time_out,
                            const char **provider_out)
{
    unsigned long long start_time = 0;
    char comm[96];

    if (!monitor || !pid_out || !start_time_out || !provider_out) return -1;
    if (pid > 0 &&
        lulo_proc_meta_read_basic(pid, comm, sizeof(comm), &start_time) < 0) {
        return 0;
    }
    if (monitor->last_pid == pid && monitor->last_start_time == start_time) return 0;
    monitor->last_pid = pid;
    monitor->last_start_time = start_time;
    *pid_out = pid;
    *start_time_out = start_time;
    *provider_out = monitor->provider;
    return 1;
}

void lulod_focus_init(LulodFocusMonitor *monitor)
{
    const char *provider;

    if (!monitor) return;
    memset(monitor, 0, sizeof(*monitor));
    monitor->fd = -1;
    monitor->child_pid = -1;
    monitor->last_pid = -1;
    provider = focus_provider_from_env();
    if (!provider) return;
    monitor->enabled = 1;
    snprintf(monitor->provider, sizeof(monitor->provider), "%s", provider);
}

void lulod_focus_stop(LulodFocusMonitor *monitor)
{
    if (!monitor) return;
    close_monitor_fd(monitor);
    if (monitor->child_pid > 0) {
        kill(monitor->child_pid, SIGTERM);
        waitpid(monitor->child_pid, NULL, 0);
    }
    memset(monitor, 0, sizeof(*monitor));
    monitor->fd = -1;
    monitor->child_pid = -1;
    monitor->last_pid = -1;
}

int lulod_focus_poll_fd(LulodFocusMonitor *monitor, long long now_ms)
{
    char err[128];

    if (!monitor || !monitor->enabled) return -1;
    reap_child(monitor->child_pid);
    if (monitor->fd >= 0) return monitor->fd;
    if (monitor->restart_due_ms > 0 && now_ms < monitor->restart_due_ms) return -1;
    err[0] = '\0';
    if (start_focus_helper(monitor, now_ms, err, sizeof(err)) < 0) {
        if (monitor->enabled) monitor->restart_due_ms = now_ms + 5000;
        return -1;
    }
    return monitor->fd;
}

int lulod_focus_handle(LulodFocusMonitor *monitor, short revents, long long now_ms,
                       pid_t *pid_out, unsigned long long *start_time_out,
                       const char **provider_out, char *err, size_t errlen)
{
    char readbuf[256];
    int changed = 0;

    if (err && errlen > 0) err[0] = '\0';
    if (!monitor || !monitor->enabled || monitor->fd < 0) return 0;

    if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
        schedule_restart(monitor, now_ms);
        return commit_focus_pid(monitor, 0, pid_out, start_time_out, provider_out);
    }
    if (!(revents & POLLIN)) return 0;

    for (;;) {
        ssize_t n = read(monitor->fd, readbuf, sizeof(readbuf));

        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (err && errlen > 0) snprintf(err, errlen, "focus helper read: %s", strerror(errno));
            schedule_restart(monitor, now_ms);
            return -1;
        }
        if (n == 0) {
            schedule_restart(monitor, now_ms);
            return commit_focus_pid(monitor, 0, pid_out, start_time_out, provider_out);
        }
        for (ssize_t i = 0; i < n; i++) {
            char ch = readbuf[i];

            if (ch == '\n') {
                pid_t pid = 0;

                monitor->linebuf[monitor->line_len] = '\0';
                if (parse_focus_pid_line(monitor->linebuf, &pid) == 0) {
                    int rc = commit_focus_pid(monitor, pid, pid_out, start_time_out, provider_out);
                    if (rc < 0) return -1;
                    if (rc > 0) changed = 1;
                }
                monitor->line_len = 0;
            } else if (monitor->line_len + 1 < sizeof(monitor->linebuf)) {
                monitor->linebuf[monitor->line_len++] = ch;
            }
        }
    }

    return changed;
}
