#ifndef LULOD_IPC_H
#define LULOD_IPC_H

#include <stddef.h>
#include <stdint.h>

#include "lulo_systemd.h"
#include "lulo_tune.h"

enum {
    LULOD_REQ_SYSTEMD_FULL = 1,
    LULOD_REQ_SYSTEMD_ACTIVE = 2,
    LULOD_REQ_TUNE_FULL = 3,
    LULOD_REQ_TUNE_ACTIVE = 4,
    LULOD_REQ_TUNE_SAVE_SNAPSHOT = 5,
    LULOD_REQ_TUNE_SAVE_PRESET = 6,
    LULOD_REQ_TUNE_APPLY_SELECTED = 7,
};

int lulod_socket_path(char *buf, size_t len);
int lulod_create_server_socket(const char *path);
int lulod_connect_socket(const char *path);

int lulod_recv_request_header(int fd, uint32_t *type);
int lulod_send_systemd_request(int fd, uint32_t type, const LuloSystemdState *state);
int lulod_recv_systemd_state(int fd, LuloSystemdState *state);
int lulod_recv_systemd_request(int fd, uint32_t *type, LuloSystemdState *state);
int lulod_send_systemd_response(int fd, int status, const char *err, const LuloSystemdSnapshot *snap);
int lulod_recv_systemd_response(int fd, LuloSystemdSnapshot *snap, char *err, size_t errlen);

int lulod_send_tune_request(int fd, uint32_t type, const LuloTuneState *state);
int lulod_recv_tune_state(int fd, LuloTuneState *state);
int lulod_recv_tune_request(int fd, uint32_t *type, LuloTuneState *state);
int lulod_send_tune_response(int fd, int status, const char *err, const LuloTuneSnapshot *snap);
int lulod_recv_tune_response(int fd, LuloTuneSnapshot *snap, char *err, size_t errlen);

#endif
