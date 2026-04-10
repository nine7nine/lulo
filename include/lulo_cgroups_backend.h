#ifndef LULO_CGROUPS_BACKEND_H
#define LULO_CGROUPS_BACKEND_H

#include <pthread.h>

#include "lulo_cgroups.h"

typedef struct {
    int busy;
    int have_snapshot;
    int loading_full;
    int loading_active;
    unsigned generation;
    char error[160];
} LuloCgroupsBackendStatus;

typedef struct LuloCgroupsBackend {
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
    LuloCgroupsState requested_state;
    LuloCgroupsSnapshot published;
} LuloCgroupsBackend;

int lulo_cgroups_backend_start(LuloCgroupsBackend *backend);
void lulo_cgroups_backend_stop(LuloCgroupsBackend *backend);
void lulo_cgroups_backend_request_full(LuloCgroupsBackend *backend, const LuloCgroupsState *state);
void lulo_cgroups_backend_request_active(LuloCgroupsBackend *backend, const LuloCgroupsState *state);
int lulo_cgroups_backend_consume(LuloCgroupsBackend *backend, LuloCgroupsSnapshot *dst,
                                 unsigned *generation, LuloCgroupsBackendStatus *status);
void lulo_cgroups_backend_status(LuloCgroupsBackend *backend, LuloCgroupsBackendStatus *status);

#endif
