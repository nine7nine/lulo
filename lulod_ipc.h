#ifndef LULOD_IPC_H
#define LULOD_IPC_H

#include <stddef.h>
#include <stdint.h>

#include "lulo_systemd.h"

enum {
    LULOD_REQ_SYSTEMD_FULL = 1,
    LULOD_REQ_SYSTEMD_ACTIVE = 2,
};

int lulod_socket_path(char *buf, size_t len);
int lulod_create_server_socket(const char *path);
int lulod_connect_socket(const char *path);

int lulod_send_systemd_request(int fd, uint32_t type, const LuloSystemdState *state);
int lulod_recv_systemd_request(int fd, uint32_t *type, LuloSystemdState *state);
int lulod_send_systemd_response(int fd, int status, const char *err, const LuloSystemdSnapshot *snap);
int lulod_recv_systemd_response(int fd, LuloSystemdSnapshot *snap, char *err, size_t errlen);

#endif
