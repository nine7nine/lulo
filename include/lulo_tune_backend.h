#ifndef LULO_TUNE_BACKEND_H
#define LULO_TUNE_BACKEND_H

#include <pthread.h>

#include "lulo_tune.h"

typedef struct {
    int busy;
    int have_snapshot;
    int loading_full;
    int loading_active;
    int saving_snapshot;
    int saving_preset;
    int applying_selected;
    unsigned generation;
    char error[160];
} LuloTuneBackendStatus;

typedef struct LuloTuneBackend {
    pthread_t tid;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int running;
    int started;
    int refresh_full;
    int refresh_active;
    int save_snapshot;
    int save_preset;
    unsigned generation;
    int busy;
    int loading_full;
    int loading_active;
    int saving_snapshot;
    int saving_preset;
    int applying_selected;
    int have_snapshot;
    long long last_spawn_ms;
    char error[160];
    LuloTuneState requested_state;
    LuloTuneSnapshot published;
} LuloTuneBackend;

int lulo_tune_backend_start(LuloTuneBackend *backend);
void lulo_tune_backend_stop(LuloTuneBackend *backend);
void lulo_tune_backend_request_full(LuloTuneBackend *backend, const LuloTuneState *state);
void lulo_tune_backend_request_active(LuloTuneBackend *backend, const LuloTuneState *state);
void lulo_tune_backend_request_save_snapshot(LuloTuneBackend *backend, const LuloTuneState *state);
void lulo_tune_backend_request_save_preset(LuloTuneBackend *backend, const LuloTuneState *state);
void lulo_tune_backend_request_apply_selected(LuloTuneBackend *backend, const LuloTuneState *state);
int lulo_tune_backend_consume(LuloTuneBackend *backend, LuloTuneSnapshot *dst,
                              unsigned *generation, LuloTuneBackendStatus *status);
void lulo_tune_backend_status(LuloTuneBackend *backend, LuloTuneBackendStatus *status);

#endif
