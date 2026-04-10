#ifndef LULO_UDEV_BACKEND_H
#define LULO_UDEV_BACKEND_H

#include <pthread.h>

#include "lulo_udev.h"

typedef struct {
    int busy;
    int have_snapshot;
    int loading_full;
    int loading_active;
    unsigned generation;
    char error[160];
} LuloUdevBackendStatus;

typedef struct LuloUdevBackend {
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
    LuloUdevState requested_state;
    LuloUdevSnapshot published;
} LuloUdevBackend;

int lulo_udev_backend_start(LuloUdevBackend *backend);
void lulo_udev_backend_stop(LuloUdevBackend *backend);
void lulo_udev_backend_request_full(LuloUdevBackend *backend, const LuloUdevState *state);
void lulo_udev_backend_request_active(LuloUdevBackend *backend, const LuloUdevState *state);
int lulo_udev_backend_consume(LuloUdevBackend *backend, LuloUdevSnapshot *dst,
                              unsigned *generation, LuloUdevBackendStatus *status);
void lulo_udev_backend_status(LuloUdevBackend *backend, LuloUdevBackendStatus *status);

#endif
