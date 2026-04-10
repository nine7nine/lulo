#ifndef LULO_SYSTEMD_BACKEND_H
#define LULO_SYSTEMD_BACKEND_H

#include <pthread.h>

#include "lulo_systemd.h"

typedef struct {
    int busy;
    int have_snapshot;
    int loading_full;
    int loading_active;
    unsigned generation;
    char error[160];
} LuloSystemdBackendStatus;

typedef struct LuloSystemdBackend {
    pthread_t tid;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int running;
    int started;
    int refresh_full;
    int refresh_active;
    unsigned generation;
    int busy;
    int loading_full;
    int loading_active;
    int have_snapshot;
    long long last_spawn_ms;
    char error[160];
    LuloSystemdState requested_state;
    LuloSystemdSnapshot published;
} LuloSystemdBackend;

int lulo_systemd_backend_start(LuloSystemdBackend *backend);
void lulo_systemd_backend_stop(LuloSystemdBackend *backend);
void lulo_systemd_backend_request_full(LuloSystemdBackend *backend, const LuloSystemdState *state);
void lulo_systemd_backend_request_active(LuloSystemdBackend *backend, const LuloSystemdState *state);
int lulo_systemd_backend_consume(LuloSystemdBackend *backend, LuloSystemdSnapshot *dst,
                                 unsigned *generation, LuloSystemdBackendStatus *status);
void lulo_systemd_backend_status(LuloSystemdBackend *backend, LuloSystemdBackendStatus *status);

#endif
