#define _GNU_SOURCE

#include "lulo_tune_backend.h"

#include "lulod_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long long mono_ms_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void sleep_ms(int ms)
{
    struct timespec ts;

    if (ms <= 0) return;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}

static int lulod_binary_path(char *buf, size_t len)
{
    char exe[PATH_MAX];
    ssize_t n;
    char *slash;
    size_t exe_len;

    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n < 0) return -1;
    exe[n] = '\0';
    slash = strrchr(exe, '/');
    if (!slash) return -1;
    slash[1] = '\0';
    exe_len = strlen(exe);
    if (exe_len + sizeof("lulod") > len) return -1;
    memcpy(buf, exe, exe_len);
    memcpy(buf + exe_len, "lulod", sizeof("lulod"));
    return 0;
}

static int spawn_lulod(LuloTuneBackend *backend)
{
    char path[PATH_MAX];
    pid_t pid;
    int devnull = -1;
    long long now = mono_ms_now();

    if (backend && backend->last_spawn_ms > 0 && now - backend->last_spawn_ms < 1000) return 0;
    if (backend) backend->last_spawn_ms = now;
    if (lulod_binary_path(path, sizeof(path)) < 0) return -1;

    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setsid();
        signal(SIGPIPE, SIG_IGN);
        devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        execl(path, "lulod", (char *)NULL);
        _exit(127);
    }
    return 0;
}

static int start_lulod_service(void)
{
    pid_t pid;
    int status = 0;
    int devnull = -1;

    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        execlp("systemctl", "systemctl", "--user", "start", "lulod.service", (char *)NULL);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int request_via_lulod(LuloTuneBackend *backend, uint32_t type, const LuloTuneState *state,
                             LuloTuneSnapshot *snap, char *err, size_t errlen)
{
    char socket_path[108];
    int fd = -1;

    if (err && errlen > 0) err[0] = '\0';
    if (lulod_socket_path(socket_path, sizeof(socket_path)) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "bad lulod socket path");
        return -1;
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        fd = lulod_connect_socket(socket_path);
        if (fd >= 0) break;
        if (errno != ENOENT && errno != ECONNREFUSED) break;
        if (start_lulod_service() < 0 && spawn_lulod(backend) < 0) break;
        sleep_ms(150);
    }
    if (fd < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to connect to lulod");
        return -1;
    }
    if (lulod_send_tune_request(fd, type, state) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to send request to lulod");
        close(fd);
        return -1;
    }
    if (lulod_recv_tune_response(fd, snap, err, errlen) < 0) {
        if (err && errlen > 0 && !err[0]) snprintf(err, errlen, "failed to read lulod response");
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static void *lulo_tune_backend_main(void *opaque)
{
    LuloTuneBackend *backend = opaque;

    for (;;) {
        int do_full = 0;
        int do_active = 0;
        int do_save_snapshot = 0;
        int do_save_preset = 0;
        int do_apply_selected = 0;
        uint32_t type = LULOD_REQ_TUNE_FULL;
        LuloTuneState state = {0};
        LuloTuneSnapshot work = {0};
        char err[160] = {0};
        int rc = 0;

        pthread_mutex_lock(&backend->lock);
        while (backend->running &&
               !backend->refresh_full && !backend->refresh_active &&
               !backend->save_snapshot && !backend->save_preset &&
               !backend->applying_selected) {
            pthread_cond_wait(&backend->cond, &backend->lock);
        }
        if (!backend->running) {
            pthread_mutex_unlock(&backend->lock);
            break;
        }

        do_save_snapshot = backend->save_snapshot;
        do_save_preset = !do_save_snapshot && backend->save_preset;
        do_apply_selected = !do_save_snapshot && !do_save_preset && backend->applying_selected;
        do_full = !do_save_snapshot && !do_save_preset && !do_apply_selected &&
                  (backend->refresh_full || !backend->have_snapshot);
        do_active = !do_save_snapshot && !do_save_preset && !do_apply_selected &&
                    !do_full && backend->refresh_active;
        backend->refresh_full = 0;
        backend->refresh_active = 0;
        backend->save_snapshot = 0;
        backend->save_preset = 0;
        backend->applying_selected = 0;
        backend->busy = 1;
        backend->loading_full = do_full;
        backend->loading_active = do_active;
        backend->saving_snapshot = do_save_snapshot;
        backend->saving_preset = do_save_preset;
        backend->applying_selected = do_apply_selected;
        state = backend->requested_state;
        pthread_mutex_unlock(&backend->lock);

        if (do_save_snapshot) type = LULOD_REQ_TUNE_SAVE_SNAPSHOT;
        else if (do_save_preset) type = LULOD_REQ_TUNE_SAVE_PRESET;
        else if (do_apply_selected) type = LULOD_REQ_TUNE_APPLY_SELECTED;
        else if (do_active) type = LULOD_REQ_TUNE_ACTIVE;
        else type = LULOD_REQ_TUNE_FULL;
        rc = request_via_lulod(backend, type, &state, &work, err, sizeof(err));

        pthread_mutex_lock(&backend->lock);
        backend->busy = 0;
        backend->loading_full = 0;
        backend->loading_active = 0;
        backend->saving_snapshot = 0;
        backend->saving_preset = 0;
        backend->applying_selected = 0;
        if (rc < 0) {
            snprintf(backend->error, sizeof(backend->error), "%s", err[0] ? err : "lulod request failed");
            lulo_tune_snapshot_free(&work);
        } else {
            backend->error[0] = '\0';
            lulo_tune_snapshot_free(&backend->published);
            backend->published = work;
            memset(&work, 0, sizeof(work));
            backend->have_snapshot = 1;
            backend->generation++;
        }
        pthread_mutex_unlock(&backend->lock);
    }

    return NULL;
}

int lulo_tune_backend_start(LuloTuneBackend *backend)
{
    memset(backend, 0, sizeof(*backend));
    if (pthread_mutex_init(&backend->lock, NULL) != 0) return -1;
    if (pthread_cond_init(&backend->cond, NULL) != 0) {
        pthread_mutex_destroy(&backend->lock);
        return -1;
    }
    backend->running = 1;
    if (pthread_create(&backend->tid, NULL, lulo_tune_backend_main, backend) != 0) {
        backend->running = 0;
        pthread_cond_destroy(&backend->cond);
        pthread_mutex_destroy(&backend->lock);
        return -1;
    }
    backend->started = 1;
    return 0;
}

void lulo_tune_backend_stop(LuloTuneBackend *backend)
{
    if (!backend || !backend->started) return;

    pthread_mutex_lock(&backend->lock);
    backend->running = 0;
    pthread_cond_broadcast(&backend->cond);
    pthread_mutex_unlock(&backend->lock);

    pthread_join(backend->tid, NULL);
    pthread_cond_destroy(&backend->cond);
    pthread_mutex_destroy(&backend->lock);
    lulo_tune_snapshot_free(&backend->published);
    memset(backend, 0, sizeof(*backend));
}

static void lulo_tune_backend_request(LuloTuneBackend *backend, const LuloTuneState *state,
                                      int full, int active, int save_snapshot, int save_preset,
                                      int apply_selected)
{
    if (!backend || !backend->started) return;

    pthread_mutex_lock(&backend->lock);
    if (state) backend->requested_state = *state;
    if (save_snapshot) {
        backend->save_snapshot = 1;
        backend->save_preset = 0;
        backend->refresh_full = 0;
        backend->refresh_active = 0;
        backend->applying_selected = 0;
    } else if (save_preset) {
        backend->save_preset = 1;
        backend->save_snapshot = 0;
        backend->refresh_full = 0;
        backend->refresh_active = 0;
        backend->applying_selected = 0;
    } else if (apply_selected) {
        backend->applying_selected = 1;
        backend->save_snapshot = 0;
        backend->save_preset = 0;
        backend->refresh_full = 0;
        backend->refresh_active = 0;
    } else if (full) {
        backend->refresh_full = 1;
        backend->refresh_active = 0;
        backend->applying_selected = 0;
    } else if (active && !backend->refresh_full) {
        backend->refresh_active = 1;
        backend->applying_selected = 0;
    }
    pthread_cond_signal(&backend->cond);
    pthread_mutex_unlock(&backend->lock);
}

void lulo_tune_backend_request_full(LuloTuneBackend *backend, const LuloTuneState *state)
{
    lulo_tune_backend_request(backend, state, 1, 0, 0, 0, 0);
}

void lulo_tune_backend_request_active(LuloTuneBackend *backend, const LuloTuneState *state)
{
    lulo_tune_backend_request(backend, state, 0, 1, 0, 0, 0);
}

void lulo_tune_backend_request_save_snapshot(LuloTuneBackend *backend, const LuloTuneState *state)
{
    lulo_tune_backend_request(backend, state, 0, 0, 1, 0, 0);
}

void lulo_tune_backend_request_save_preset(LuloTuneBackend *backend, const LuloTuneState *state)
{
    lulo_tune_backend_request(backend, state, 0, 0, 0, 1, 0);
}

void lulo_tune_backend_request_apply_selected(LuloTuneBackend *backend, const LuloTuneState *state)
{
    lulo_tune_backend_request(backend, state, 0, 0, 0, 0, 1);
}

int lulo_tune_backend_consume(LuloTuneBackend *backend, LuloTuneSnapshot *dst,
                              unsigned *generation, LuloTuneBackendStatus *status)
{
    unsigned next_generation = 0;
    int changed = 0;
    int rc = 0;
    LuloTuneSnapshot fresh = {0};

    if (!backend) return -1;

    pthread_mutex_lock(&backend->lock);
    next_generation = backend->generation;
    if (status) {
        status->busy = backend->busy;
        status->have_snapshot = backend->have_snapshot;
        status->loading_full = backend->loading_full;
        status->loading_active = backend->loading_active;
        status->saving_snapshot = backend->saving_snapshot;
        status->saving_preset = backend->saving_preset;
        status->applying_selected = backend->applying_selected;
        status->generation = backend->generation;
        snprintf(status->error, sizeof(status->error), "%s", backend->error);
    }
    if (dst && generation && backend->have_snapshot && *generation != backend->generation) {
        rc = lulo_tune_snapshot_clone(&fresh, &backend->published);
        if (rc == 0) {
            lulo_tune_snapshot_free(dst);
            *dst = fresh;
            memset(&fresh, 0, sizeof(fresh));
            *generation = backend->generation;
            changed = 1;
        }
    } else if (generation) {
        *generation = next_generation;
    }
    pthread_mutex_unlock(&backend->lock);

    lulo_tune_snapshot_free(&fresh);
    return rc < 0 ? -1 : changed;
}

void lulo_tune_backend_status(LuloTuneBackend *backend, LuloTuneBackendStatus *status)
{
    if (!backend || !status) return;
    pthread_mutex_lock(&backend->lock);
    status->busy = backend->busy;
    status->have_snapshot = backend->have_snapshot;
    status->loading_full = backend->loading_full;
    status->loading_active = backend->loading_active;
    status->saving_snapshot = backend->saving_snapshot;
    status->saving_preset = backend->saving_preset;
    status->applying_selected = backend->applying_selected;
    status->generation = backend->generation;
    snprintf(status->error, sizeof(status->error), "%s", backend->error);
    pthread_mutex_unlock(&backend->lock);
}
