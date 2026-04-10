#ifndef LULO_EDIT_H
#define LULO_EDIT_H

#include <limits.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct {
    int privileged;
    char original_path[PATH_MAX];
    char edit_path[PATH_MAX];
    char session_id[128];
} LuloEditSession;

typedef struct {
    char session_id[128];
    char output_path[PATH_MAX];
} LuloTraceSession;

void lulo_edit_session_init(LuloEditSession *session);
void lulo_edit_session_clear(LuloEditSession *session);
int lulo_edit_session_begin(const char *path, LuloEditSession *session,
                            char *err, size_t errlen);
int lulo_edit_session_commit(LuloEditSession *session, char *err, size_t errlen);
int lulo_edit_session_cancel(LuloEditSession *session, char *err, size_t errlen);
int lulo_edit_write_file(const char *path, const char *content,
                         char *err, size_t errlen);
int lulo_edit_delete_file(const char *path, char *err, size_t errlen);
void lulo_trace_session_init(LuloTraceSession *session);
void lulo_trace_session_clear(LuloTraceSession *session);
int lulo_trace_session_begin(pid_t target_pid, LuloTraceSession *session,
                             char *err, size_t errlen);
int lulo_trace_session_end(LuloTraceSession *session, char *err, size_t errlen);

#endif
