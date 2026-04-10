#ifndef LULOD_SYSTEM_EDIT_H
#define LULOD_SYSTEM_EDIT_H

#include <stddef.h>
#include <sys/types.h>

int lulod_system_edit_begin(const char *path, uid_t uid, gid_t gid,
                            char *session_id, size_t session_id_len,
                            char *edit_path, size_t edit_path_len,
                            int *reload_sched,
                            char *err, size_t errlen);
int lulod_system_edit_commit(const char *session_id, uid_t uid, int *reload_sched,
                             char *err, size_t errlen);
int lulod_system_edit_cancel(const char *session_id, uid_t uid,
                             char *err, size_t errlen);
int lulod_system_write_file(const char *path, const char *content, int *reload_sched,
                            char *err, size_t errlen);
int lulod_system_delete_file(const char *path, int *reload_sched,
                             char *err, size_t errlen);

#endif
