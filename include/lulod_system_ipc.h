#ifndef LULOD_SYSTEM_IPC_H
#define LULOD_SYSTEM_IPC_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "lulo_sched.h"

enum {
    LULOD_SYSTEM_REQ_SCHED_FULL = 1,
    LULOD_SYSTEM_REQ_SCHED_RELOAD = 2,
    LULOD_SYSTEM_REQ_SCHED_FOCUS_UPDATE = 3,
};

int lulod_system_socket_path(char *buf, size_t len);
int lulod_system_create_server_socket(const char *path);
int lulod_system_connect_socket(const char *path);

int lulod_system_recv_request_header(int fd, uint32_t *type);
int lulod_system_send_sched_request(int fd, uint32_t type);
int lulod_system_send_focus_update_request(int fd, pid_t pid, unsigned long long start_time,
                                           const char *provider);
int lulod_system_recv_focus_update_request(int fd, pid_t *pid, unsigned long long *start_time,
                                           char *provider, size_t provider_len);
int lulod_system_send_status_response(int fd, int status, const char *err);
int lulod_system_recv_status_response(int fd, char *err, size_t errlen);
int lulod_system_send_sched_response(int fd, int status, const char *err, const LuloSchedSnapshot *snap);
int lulod_system_recv_sched_response(int fd, LuloSchedSnapshot *snap, char *err, size_t errlen);

#endif
