#define _GNU_SOURCE

#include "lulo_sched_backend.h"

#include "lulod_ipc.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int request_via_lulod(uint32_t type, const LuloSchedState *state,
                             LuloSchedSnapshot *snap, char *err, size_t errlen)
{
    char socket_path[108];
    int fd = -1;

    if (err && errlen > 0) err[0] = '\0';
    if (lulod_socket_path(socket_path, sizeof(socket_path)) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "bad lulod socket path");
        return -1;
    }
    fd = lulod_connect_socket(socket_path);
    if (fd < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to connect to lulod");
        return -1;
    }
    if (lulod_send_sched_request(fd, type, state) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to send request to lulod");
        close(fd);
        return -1;
    }
    if (lulod_recv_sched_response(fd, snap, err, errlen) < 0) {
        if (err && errlen > 0 && !err[0]) snprintf(err, errlen, "failed to read lulod response");
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static void *lulo_sched_backend_main(void *opaque)
{
    LuloSchedBackend *backend = opaque;

    for (;;) {
        int do_full = 0;
        int do_active = 0;
        int do_reload = 0;
        uint32_t type = LULOD_REQ_SCHED_FULL;
        LuloSchedState state = {0};
        LuloSchedSnapshot work = {0};
        char err[160] = {0};
        int rc;

        pthread_mutex_lock(&backend->lock);
        while (backend->running &&
               !backend->refresh_full && !backend->refresh_active && !backend->reload_config) {
            pthread_cond_wait(&backend->cond, &backend->lock);
        }
        if (!backend->running) {
            pthread_mutex_unlock(&backend->lock);
            break;
        }

        do_reload = backend->reload_config;
        do_full = !do_reload && (backend->refresh_full || !backend->have_snapshot);
        do_active = !do_reload && !do_full && backend->refresh_active;
        backend->refresh_full = 0;
        backend->refresh_active = 0;
        backend->reload_config = 0;
        backend->busy = 1;
        backend->loading_full = do_full;
        backend->loading_active = do_active;
        backend->reloading = do_reload;
        state = backend->requested_state;
        pthread_mutex_unlock(&backend->lock);

        if (do_reload) type = LULOD_REQ_SCHED_RELOAD;
        else if (do_active) type = LULOD_REQ_SCHED_ACTIVE;
        else type = LULOD_REQ_SCHED_FULL;
        rc = request_via_lulod(type, &state, &work, err, sizeof(err));

        pthread_mutex_lock(&backend->lock);
        backend->busy = 0;
        backend->loading_full = 0;
        backend->loading_active = 0;
        backend->reloading = 0;
        if (rc < 0) {
            snprintf(backend->error, sizeof(backend->error), "%s",
                     err[0] ? err : "lulod request failed");
            lulo_sched_snapshot_free(&work);
        } else {
            backend->error[0] = '\0';
            lulo_sched_snapshot_free(&backend->published);
            backend->published = work;
            memset(&work, 0, sizeof(work));
            backend->have_snapshot = 1;
            backend->generation++;
        }
        pthread_mutex_unlock(&backend->lock);
    }

    return NULL;
}

int lulo_sched_backend_start(LuloSchedBackend *backend)
{
    memset(backend, 0, sizeof(*backend));
    if (pthread_mutex_init(&backend->lock, NULL) != 0) return -1;
    if (pthread_cond_init(&backend->cond, NULL) != 0) {
        pthread_mutex_destroy(&backend->lock);
        return -1;
    }
    backend->running = 1;
    if (pthread_create(&backend->tid, NULL, lulo_sched_backend_main, backend) != 0) {
        backend->running = 0;
        pthread_cond_destroy(&backend->cond);
        pthread_mutex_destroy(&backend->lock);
        return -1;
    }
    backend->started = 1;
    return 0;
}

void lulo_sched_backend_stop(LuloSchedBackend *backend)
{
    if (!backend || !backend->started) return;

    pthread_mutex_lock(&backend->lock);
    backend->running = 0;
    pthread_cond_broadcast(&backend->cond);
    pthread_mutex_unlock(&backend->lock);

    pthread_join(backend->tid, NULL);
    pthread_cond_destroy(&backend->cond);
    pthread_mutex_destroy(&backend->lock);
    lulo_sched_snapshot_free(&backend->published);
    memset(backend, 0, sizeof(*backend));
}

static void lulo_sched_backend_request(LuloSchedBackend *backend, const LuloSchedState *state,
                                       int full, int active, int reload)
{
    if (!backend || !backend->started) return;

    pthread_mutex_lock(&backend->lock);
    if (state) backend->requested_state = *state;
    if (reload) {
        backend->reload_config = 1;
        backend->refresh_full = 0;
        backend->refresh_active = 0;
    } else if (full) {
        backend->refresh_full = 1;
        backend->refresh_active = 0;
    } else if (active && !backend->refresh_full) {
        backend->refresh_active = 1;
    }
    pthread_cond_signal(&backend->cond);
    pthread_mutex_unlock(&backend->lock);
}

void lulo_sched_backend_request_full(LuloSchedBackend *backend, const LuloSchedState *state)
{
    lulo_sched_backend_request(backend, state, 1, 0, 0);
}

void lulo_sched_backend_request_active(LuloSchedBackend *backend, const LuloSchedState *state)
{
    lulo_sched_backend_request(backend, state, 0, 1, 0);
}

void lulo_sched_backend_request_reload(LuloSchedBackend *backend, const LuloSchedState *state)
{
    lulo_sched_backend_request(backend, state, 0, 0, 1);
}

int lulo_sched_backend_consume(LuloSchedBackend *backend, LuloSchedSnapshot *dst,
                               unsigned *generation, LuloSchedBackendStatus *status)
{
    unsigned next_generation = 0;
    int changed = 0;
    int rc = 0;
    LuloSchedSnapshot fresh = {0};

    if (!backend) return -1;

    pthread_mutex_lock(&backend->lock);
    next_generation = backend->generation;
    if (status) {
        status->busy = backend->busy;
        status->have_snapshot = backend->have_snapshot;
        status->loading_full = backend->loading_full;
        status->loading_active = backend->loading_active;
        status->reloading = backend->reloading;
        status->generation = backend->generation;
        snprintf(status->error, sizeof(status->error), "%s", backend->error);
    }
    if (dst && generation && backend->have_snapshot && *generation != backend->generation) {
        rc = lulo_sched_snapshot_clone(&fresh, &backend->published);
        if (rc == 0) {
            lulo_sched_snapshot_free(dst);
            *dst = fresh;
            memset(&fresh, 0, sizeof(fresh));
            *generation = backend->generation;
            changed = 1;
        }
    } else if (generation) {
        *generation = next_generation;
    }
    pthread_mutex_unlock(&backend->lock);

    lulo_sched_snapshot_free(&fresh);
    return rc < 0 ? -1 : changed;
}

void lulo_sched_backend_status(LuloSchedBackend *backend, LuloSchedBackendStatus *status)
{
    if (!backend || !status) return;
    pthread_mutex_lock(&backend->lock);
    status->busy = backend->busy;
    status->have_snapshot = backend->have_snapshot;
    status->loading_full = backend->loading_full;
    status->loading_active = backend->loading_active;
    status->reloading = backend->reloading;
    status->generation = backend->generation;
    snprintf(status->error, sizeof(status->error), "%s", backend->error);
    pthread_mutex_unlock(&backend->lock);
}
