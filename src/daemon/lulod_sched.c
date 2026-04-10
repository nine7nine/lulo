#define _GNU_SOURCE

#include "lulod_sched.h"

#include "lulod_system_ipc.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int lulod_sched_snapshot_gather(LuloSchedSnapshot *snap, int reload,
                                char *err, size_t errlen)
{
    char socket_path[108];
    uint32_t type = reload ? LULOD_SYSTEM_REQ_SCHED_RELOAD : LULOD_SYSTEM_REQ_SCHED_FULL;
    int fd = -1;

    if (err && errlen > 0) err[0] = '\0';
    if (lulod_system_socket_path(socket_path, sizeof(socket_path)) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "bad lulod-system socket path");
        return -1;
    }
    fd = lulod_system_connect_socket(socket_path);
    if (fd < 0) {
        if (err && errlen > 0) {
            snprintf(err, errlen, "failed to connect to lulod-system: %s",
                     strerror(errno));
        }
        return -1;
    }
    if (lulod_system_send_sched_request(fd, type) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to send request to lulod-system");
        close(fd);
        return -1;
    }
    if (lulod_system_recv_sched_response(fd, snap, err, errlen) < 0) {
        if (err && errlen > 0 && !err[0]) {
            snprintf(err, errlen, "failed to read lulod-system response");
        }
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

int lulod_sched_focus_update(pid_t pid, unsigned long long start_time,
                             const char *provider, char *err, size_t errlen)
{
    char socket_path[108];
    int fd = -1;

    if (err && errlen > 0) err[0] = '\0';
    if (lulod_system_socket_path(socket_path, sizeof(socket_path)) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "bad lulod-system socket path");
        return -1;
    }
    fd = lulod_system_connect_socket(socket_path);
    if (fd < 0) {
        if (err && errlen > 0) {
            snprintf(err, errlen, "failed to connect to lulod-system: %s",
                     strerror(errno));
        }
        return -1;
    }
    if (lulod_system_send_focus_update_request(fd, pid, start_time, provider) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to send focus update");
        close(fd);
        return -1;
    }
    if (lulod_system_recv_status_response(fd, err, errlen) < 0) {
        if (err && errlen > 0 && !err[0]) {
            snprintf(err, errlen, "failed to read focus update response");
        }
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}
