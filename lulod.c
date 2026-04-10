#define _GNU_SOURCE

#include "lulo_systemd.h"
#include "lulod_systemd.h"
#include "lulod_ipc.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t lulod_running = 1;

static void handle_signal(int signo)
{
    (void)signo;
    lulod_running = 0;
}

static long long mono_ms_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int refresh_full_cache(LuloSystemdSnapshot *cache, LuloSystemdState *state, long long *due_ms)
{
    LuloSystemdSnapshot fresh = {0};

    if (lulod_systemd_snapshot_gather(&fresh, state) < 0) return -1;
    lulo_systemd_snapshot_free(cache);
    *cache = fresh;
    *due_ms = mono_ms_now() + 5000;
    return 0;
}

static int state_has_active_preview(const LuloSystemdState *state)
{
    if (!state) return 0;
    if (state->view == LULO_SYSTEMD_VIEW_CONFIG) {
        return state->config_selected >= 0 && state->selected_config[0];
    }
    return state->selected >= 0 && state->selected_unit[0];
}

static int reply_for_request(uint32_t type, const LuloSystemdState *state,
                             LuloSystemdSnapshot *cache, int *have_cache, long long *due_ms,
                             LuloSystemdSnapshot *reply)
{
    int need_full;

    memset(reply, 0, sizeof(*reply));
    need_full = !*have_cache || mono_ms_now() >= *due_ms ||
                (state && state->view == LULO_SYSTEMD_VIEW_CONFIG && !cache->configs_loaded);
    if (need_full) {
        LuloSystemdState gather_state = state ? *state : (LuloSystemdState){0};
        if (refresh_full_cache(cache, &gather_state, due_ms) < 0) return -1;
        *have_cache = 1;
    }
    if (lulo_systemd_snapshot_clone(reply, cache) < 0) return -1;
    if ((type == LULOD_REQ_SYSTEMD_ACTIVE ||
         (type == LULOD_REQ_SYSTEMD_FULL && state_has_active_preview(state))) &&
        lulod_systemd_snapshot_refresh_active(reply, state) < 0) {
        lulo_systemd_snapshot_free(reply);
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    char socket_path[108];
    int listen_fd;
    LuloSystemdSnapshot cache = {0};
    int have_cache = 0;
    long long due_ms = 0;

    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        fprintf(stderr, "Usage: lulod\n");
        return 0;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    if (lulod_socket_path(socket_path, sizeof(socket_path)) < 0) {
        fprintf(stderr, "failed to determine lulod socket path\n");
        return 1;
    }
    listen_fd = lulod_create_server_socket(socket_path);
    if (listen_fd < 0) {
        fprintf(stderr, "failed to create lulod socket at %s: %s\n", socket_path, strerror(errno));
        return 1;
    }

    while (lulod_running) {
        int client_fd;
        uint32_t type = 0;
        LuloSystemdState state = {0};
        LuloSystemdSnapshot reply = {0};
        struct pollfd pfd = {
            .fd = listen_fd,
            .events = POLLIN
        };
        int pr;

        pr = poll(&pfd, 1, 250);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        if (!(pfd.revents & POLLIN)) {
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
            continue;
        }

        client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        if (lulod_recv_systemd_request(client_fd, &type, &state) < 0) {
            lulod_send_systemd_response(client_fd, -1, "bad request", NULL);
            close(client_fd);
            continue;
        }
        if (type != LULOD_REQ_SYSTEMD_FULL && type != LULOD_REQ_SYSTEMD_ACTIVE) {
            lulod_send_systemd_response(client_fd, -1, "unknown request", NULL);
            close(client_fd);
            continue;
        }
        if (reply_for_request(type, &state, &cache, &have_cache, &due_ms, &reply) < 0) {
            lulod_send_systemd_response(client_fd, -1, "systemd request failed", NULL);
            close(client_fd);
            continue;
        }
        lulod_send_systemd_response(client_fd, 0, NULL, &reply);
        lulo_systemd_snapshot_free(&reply);
        close(client_fd);
    }

    close(listen_fd);
    unlink(socket_path);
    lulo_systemd_snapshot_free(&cache);
    return 0;
}
