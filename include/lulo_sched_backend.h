#ifndef LULO_SCHED_BACKEND_H
#define LULO_SCHED_BACKEND_H

#include <pthread.h>

#include "lulo_sched.h"

typedef struct {
    int busy;
    int have_snapshot;
    int loading_full;
    int loading_active;
    int reloading;
    unsigned generation;
    char error[160];
} LuloSchedBackendStatus;

typedef struct LuloSchedBackend {
    pthread_t tid;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int running;
    int started;
    int refresh_full;
    int refresh_active;
    int reload_config;
    unsigned generation;
    int busy;
    int loading_full;
    int loading_active;
    int reloading;
    int have_snapshot;
    char error[160];
    LuloSchedState requested_state;
    LuloSchedSnapshot published;
} LuloSchedBackend;

int lulo_sched_backend_start(LuloSchedBackend *backend);
void lulo_sched_backend_stop(LuloSchedBackend *backend);
void lulo_sched_backend_request_full(LuloSchedBackend *backend, const LuloSchedState *state);
void lulo_sched_backend_request_active(LuloSchedBackend *backend, const LuloSchedState *state);
void lulo_sched_backend_request_reload(LuloSchedBackend *backend, const LuloSchedState *state);
int lulo_sched_backend_consume(LuloSchedBackend *backend, LuloSchedSnapshot *dst,
                               unsigned *generation, LuloSchedBackendStatus *status);
void lulo_sched_backend_status(LuloSchedBackend *backend, LuloSchedBackendStatus *status);

#endif
