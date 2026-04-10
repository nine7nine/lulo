#ifndef LULOD_SYSTEM_TRACE_H
#define LULOD_SYSTEM_TRACE_H

#include <stddef.h>
#include <sys/types.h>

int lulod_system_trace_begin(pid_t target_pid, uid_t uid, gid_t gid,
                             char *session_id, size_t session_id_len,
                             char *output_path, size_t output_path_len,
                             char *err, size_t errlen);
int lulod_system_trace_end(const char *session_id, uid_t uid,
                           char *err, size_t errlen);

#endif
