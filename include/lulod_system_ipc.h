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
    LULOD_SYSTEM_REQ_EDIT_BEGIN = 4,
    LULOD_SYSTEM_REQ_EDIT_COMMIT = 5,
    LULOD_SYSTEM_REQ_EDIT_CANCEL = 6,
    LULOD_SYSTEM_REQ_FILE_WRITE = 7,
    LULOD_SYSTEM_REQ_FILE_DELETE = 8,
    LULOD_SYSTEM_REQ_SCHED_APPLY_PRESET = 9,
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
int lulod_system_send_sched_apply_preset_request(int fd, const char *preset_id);
int lulod_system_recv_sched_apply_preset_request(int fd, char *preset_id, size_t preset_id_len);
int lulod_system_send_edit_begin_request(int fd, const char *path);
int lulod_system_recv_edit_begin_request(int fd, char *path, size_t path_len);
int lulod_system_send_edit_session_request(int fd, uint32_t type, const char *session_id);
int lulod_system_recv_edit_session_request(int fd, char *session_id, size_t session_id_len);
int lulod_system_send_edit_begin_response(int fd, int status, const char *err,
                                          const char *session_id, const char *edit_path);
int lulod_system_recv_edit_begin_response(int fd, char *session_id, size_t session_id_len,
                                          char *edit_path, size_t edit_path_len,
                                          char *err, size_t errlen);
int lulod_system_send_file_write_request(int fd, const char *path, const char *content);
int lulod_system_recv_file_write_request(int fd, char *path, size_t path_len,
                                         char **content);
int lulod_system_send_file_delete_request(int fd, const char *path);
int lulod_system_recv_file_delete_request(int fd, char *path, size_t path_len);
int lulod_system_send_status_response(int fd, int status, const char *err);
int lulod_system_recv_status_response(int fd, char *err, size_t errlen);
int lulod_system_send_sched_response(int fd, int status, const char *err, const LuloSchedSnapshot *snap);
int lulod_system_recv_sched_response(int fd, LuloSchedSnapshot *snap, char *err, size_t errlen);

#endif
