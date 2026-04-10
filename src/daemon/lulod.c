#define _GNU_SOURCE

#include "lulo_cgroups.h"
#include "lulo_sched.h"
#include "lulo_systemd.h"
#include "lulo_tune.h"
#include "lulo_udev.h"
#include "lulod_cgroups.h"
#include "lulod_focus.h"
#include "lulod_sched.h"
#include "lulod_systemd.h"
#include "lulod_tune.h"
#include "lulod_udev.h"
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

typedef struct {
    pid_t pid;
    unsigned long long start_time;
    char provider[32];
    long long next_sync_ms;
} LulodFocusSyncState;

static void focus_sync_set(LulodFocusSyncState *sync, pid_t pid,
                           unsigned long long start_time,
                           const char *provider, long long now_ms)
{
    if (!sync) return;
    sync->pid = pid;
    sync->start_time = start_time;
    snprintf(sync->provider, sizeof(sync->provider), "%s", provider ? provider : "");
    sync->next_sync_ms = now_ms;
}

static void focus_sync_result(LulodFocusSyncState *sync, int ok, long long now_ms)
{
    if (!sync) return;
    sync->next_sync_ms = now_ms + (ok ? 5000 : 1000);
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

static int refresh_sched_cache(LuloSchedSnapshot *cache, int reload,
                               long long *due_ms, char *err, size_t errlen)
{
    LuloSchedSnapshot fresh = {0};
    int interval_ms = 1000;

    if (lulod_sched_snapshot_gather(&fresh, reload, err, errlen) < 0) return -1;
    lulo_sched_snapshot_free(cache);
    *cache = fresh;
    if (cache->watcher_interval_ms > 0) interval_ms = cache->watcher_interval_ms;
    if (interval_ms < 250) interval_ms = 250;
    *due_ms = mono_ms_now() + interval_ms;
    return 0;
}

static int refresh_tune_cache(LuloTuneSnapshot *cache, const LuloTuneState *state,
                              LuloTuneState *cache_state, long long *due_ms)
{
    LuloTuneSnapshot fresh = {0};
    LuloTuneState gather_state = state ? *state : (LuloTuneState){0};

    if (lulod_tune_snapshot_gather(&fresh, &gather_state) < 0) return -1;
    lulo_tune_snapshot_free(cache);
    *cache = fresh;
    if (cache_state) *cache_state = gather_state;
    *due_ms = mono_ms_now() + 5000;
    return 0;
}

static int refresh_cgroups_cache(LuloCgroupsSnapshot *cache, const LuloCgroupsState *state,
                                 LuloCgroupsState *cache_state, long long *due_ms)
{
    LuloCgroupsSnapshot fresh = {0};
    LuloCgroupsState gather_state = state ? *state : (LuloCgroupsState){0};

    if (lulod_cgroups_snapshot_gather(&fresh, &gather_state) < 0) return -1;
    lulo_cgroups_snapshot_free(cache);
    *cache = fresh;
    if (cache_state) *cache_state = gather_state;
    *due_ms = mono_ms_now() + 5000;
    return 0;
}

static int refresh_udev_cache(LuloUdevSnapshot *cache, long long *due_ms)
{
    LuloUdevSnapshot fresh = {0};

    if (lulod_udev_snapshot_gather(&fresh, NULL) < 0) return -1;
    lulo_udev_snapshot_free(cache);
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

static int tune_state_has_active_preview(const LuloTuneState *state)
{
    if (!state) return 0;
    switch (state->view) {
    case LULO_TUNE_VIEW_SNAPSHOTS:
        return state->snapshot_selected >= 0 && state->selected_snapshot_id[0];
    case LULO_TUNE_VIEW_PRESETS:
        return state->preset_selected >= 0 && state->selected_preset_id[0];
    case LULO_TUNE_VIEW_EXPLORE:
    default:
        return state->selected >= 0 && state->selected_path[0];
    }
}

static int sched_state_has_active_preview(const LuloSchedState *state)
{
    if (!state) return 0;
    switch (state->view) {
    case LULO_SCHED_VIEW_RULES:
        return state->rule_selected >= 0 && state->selected_rule[0];
    case LULO_SCHED_VIEW_LIVE:
        return state->live_selected >= 0 && state->selected_live_pid > 0;
    case LULO_SCHED_VIEW_TUNABLES:
        return state->tunable_selected >= 0 && state->selected_tunable_path[0];
    case LULO_SCHED_VIEW_PRESETS:
        return state->preset_selected >= 0 && state->selected_preset_id[0];
    case LULO_SCHED_VIEW_PROFILES:
    default:
        return state->profile_selected >= 0 && state->selected_profile[0];
    }
}

static int cgroups_state_has_active_preview(const LuloCgroupsState *state)
{
    if (!state) return 0;
    switch (state->view) {
    case LULO_CGROUPS_VIEW_CONFIG:
        return state->config_selected >= 0 && state->selected_config[0];
    case LULO_CGROUPS_VIEW_FILES:
        return state->file_selected >= 0 && state->selected_file_path[0];
    case LULO_CGROUPS_VIEW_TREE:
    default:
        return state->tree_selected >= 0 && state->selected_tree_path[0];
    }
}

static int udev_state_has_active_preview(const LuloUdevState *state)
{
    if (!state) return 0;
    switch (state->view) {
    case LULO_UDEV_VIEW_HWDB:
        return state->hwdb_selected >= 0 && state->selected_hwdb[0];
    case LULO_UDEV_VIEW_DEVICES:
        return state->device_selected >= 0 && state->selected_device[0];
    case LULO_UDEV_VIEW_RULES:
    default:
        return state->rule_selected >= 0 && state->selected_rule[0];
    }
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

static int reply_for_tune_request(uint32_t type, const LuloTuneState *state,
                                  LuloTuneSnapshot *cache, LuloTuneState *cache_state,
                                  int *have_cache, long long *due_ms,
                                  LuloTuneSnapshot *reply,
                                  char *err, size_t errlen)
{
    int need_full;
    int browse_changed = 0;

    if (err && errlen > 0) err[0] = '\0';
    memset(reply, 0, sizeof(*reply));
    if (state && cache_state) browse_changed = strcmp(cache_state->browse_path, state->browse_path) != 0;
    need_full = !*have_cache || mono_ms_now() >= *due_ms || browse_changed;
    if (need_full) {
        if (refresh_tune_cache(cache, state, cache_state, due_ms) < 0) {
            if (err && errlen > 0) snprintf(err, errlen, "failed to refresh tune cache");
            return -1;
        }
        *have_cache = 1;
    }
    if (type == LULOD_REQ_TUNE_SAVE_SNAPSHOT || type == LULOD_REQ_TUNE_SAVE_PRESET) {
        if (lulod_tune_snapshot_save_current(cache, state, type == LULOD_REQ_TUNE_SAVE_PRESET) < 0) {
            if (err && errlen > 0) snprintf(err, errlen, "failed to save selected tunable");
            return -1;
        }
        if (refresh_tune_cache(cache, state, cache_state, due_ms) < 0) {
            if (err && errlen > 0) snprintf(err, errlen, "failed to refresh tune cache");
            return -1;
        }
        *have_cache = 1;
    } else if (type == LULOD_REQ_TUNE_APPLY_SELECTED) {
        if (lulod_tune_snapshot_apply_current(cache, state, err, errlen) < 0) return -1;
        if (refresh_tune_cache(cache, state, cache_state, due_ms) < 0) {
            if (err && errlen > 0) snprintf(err, errlen, "failed to refresh tune cache");
            return -1;
        }
        *have_cache = 1;
    }
    if (lulo_tune_snapshot_clone(reply, cache) < 0) return -1;
    if ((type == LULOD_REQ_TUNE_ACTIVE ||
         type == LULOD_REQ_TUNE_SAVE_SNAPSHOT ||
         type == LULOD_REQ_TUNE_SAVE_PRESET ||
         type == LULOD_REQ_TUNE_APPLY_SELECTED ||
         (type == LULOD_REQ_TUNE_FULL && tune_state_has_active_preview(state))) &&
        lulod_tune_snapshot_refresh_active(reply, state) < 0) {
        lulo_tune_snapshot_free(reply);
        if (err && errlen > 0) snprintf(err, errlen, "failed to refresh tune preview");
        return -1;
    }
    return 0;
}

static int reply_for_sched_request(uint32_t type, const LuloSchedState *state,
                                   LuloSchedSnapshot *cache, int *have_cache, long long *due_ms,
                                   LuloSchedSnapshot *reply,
                                   char *err, size_t errlen)
{
    int need_reload;
    int need_full;
    int apply_preset;

    if (err && errlen > 0) err[0] = '\0';
    memset(reply, 0, sizeof(*reply));
    need_reload = type == LULOD_REQ_SCHED_RELOAD;
    apply_preset = type == LULOD_REQ_SCHED_APPLY_PRESET;
    need_full = need_reload || !*have_cache || mono_ms_now() >= *due_ms;
    if (need_full) {
        if (refresh_sched_cache(cache, need_reload, due_ms, err, errlen) < 0) {
            if (err && errlen > 0 && !err[0]) snprintf(err, errlen, "failed to refresh scheduler cache");
            return -1;
        }
        *have_cache = 1;
    }
    if (apply_preset) {
        if (!state || state->view != LULO_SCHED_VIEW_PRESETS || !state->selected_preset_id[0]) {
            if (err && errlen > 0) snprintf(err, errlen, "select a scheduler tunables preset first");
            return -1;
        }
        if (lulod_sched_apply_tunable_preset(state->selected_preset_id, cache, err, errlen) < 0) {
            if (err && errlen > 0 && !err[0]) snprintf(err, errlen, "failed to apply scheduler preset");
            return -1;
        }
        if (cache->watcher_interval_ms > 0) {
            int interval_ms = cache->watcher_interval_ms < 250 ? 250 : cache->watcher_interval_ms;
            *due_ms = mono_ms_now() + interval_ms;
        }
        *have_cache = 1;
    }
    if (lulo_sched_snapshot_clone(reply, cache) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to clone scheduler snapshot");
        return -1;
    }
    if ((type == LULOD_REQ_SCHED_ACTIVE ||
         type == LULOD_REQ_SCHED_RELOAD ||
         type == LULOD_REQ_SCHED_APPLY_PRESET ||
         (type == LULOD_REQ_SCHED_FULL && sched_state_has_active_preview(state))) &&
        lulo_sched_snapshot_refresh_active(reply, state) < 0) {
        lulo_sched_snapshot_free(reply);
        if (err && errlen > 0) snprintf(err, errlen, "failed to refresh scheduler preview");
        return -1;
    }
    return 0;
}

static int reply_for_cgroups_request(uint32_t type, const LuloCgroupsState *state,
                                     LuloCgroupsSnapshot *cache, LuloCgroupsState *cache_state,
                                     int *have_cache, long long *due_ms,
                                     LuloCgroupsSnapshot *reply,
                                     char *err, size_t errlen)
{
    int need_full;
    int browse_changed = 0;

    if (err && errlen > 0) err[0] = '\0';
    memset(reply, 0, sizeof(*reply));
    if (state && cache_state) browse_changed = strcmp(cache_state->browse_path, state->browse_path) != 0;
    need_full = !*have_cache || mono_ms_now() >= *due_ms || browse_changed;
    if (need_full) {
        if (refresh_cgroups_cache(cache, state, cache_state, due_ms) < 0) {
            if (err && errlen > 0) snprintf(err, errlen, "failed to refresh cgroups cache");
            return -1;
        }
        *have_cache = 1;
    }
    if (lulo_cgroups_snapshot_clone(reply, cache) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to clone cgroups snapshot");
        return -1;
    }
    if ((type == LULOD_REQ_CGROUPS_ACTIVE ||
         (type == LULOD_REQ_CGROUPS_FULL && cgroups_state_has_active_preview(state))) &&
        lulod_cgroups_snapshot_refresh_active(reply, state) < 0) {
        lulo_cgroups_snapshot_free(reply);
        if (err && errlen > 0) snprintf(err, errlen, "failed to refresh cgroups preview");
        return -1;
    }
    return 0;
}

static int reply_for_udev_request(uint32_t type, const LuloUdevState *state,
                                  LuloUdevSnapshot *cache, int *have_cache,
                                  long long *due_ms, LuloUdevSnapshot *reply,
                                  char *err, size_t errlen)
{
    int need_full;

    if (err && errlen > 0) err[0] = '\0';
    memset(reply, 0, sizeof(*reply));
    need_full = !*have_cache || mono_ms_now() >= *due_ms;
    if (need_full) {
        if (refresh_udev_cache(cache, due_ms) < 0) {
            if (err && errlen > 0) snprintf(err, errlen, "failed to refresh udev cache");
            return -1;
        }
        *have_cache = 1;
    }
    if (lulo_udev_snapshot_clone(reply, cache) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "failed to clone udev snapshot");
        return -1;
    }
    if ((type == LULOD_REQ_UDEV_ACTIVE ||
         (type == LULOD_REQ_UDEV_FULL && udev_state_has_active_preview(state))) &&
        lulod_udev_snapshot_refresh_active(reply, state) < 0) {
        lulo_udev_snapshot_free(reply);
        if (err && errlen > 0) snprintf(err, errlen, "failed to refresh udev preview");
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    char socket_path[108];
    int listen_fd;
    LuloSystemdSnapshot cache = {0};
    LuloSchedSnapshot sched_cache = {0};
    LuloTuneSnapshot tune_cache = {0};
    LuloCgroupsSnapshot cgroups_cache = {0};
    LuloUdevSnapshot udev_cache = {0};
    LuloTuneState tune_cache_state = {0};
    LuloCgroupsState cgroups_cache_state = {0};
    int have_cache = 0;
    int have_sched_cache = 0;
    int have_tune_cache = 0;
    int have_cgroups_cache = 0;
    int have_udev_cache = 0;
    long long due_ms = 0;
    long long sched_due_ms = 0;
    long long tune_due_ms = 0;
    long long cgroups_due_ms = 0;
    long long udev_due_ms = 0;
    LulodFocusMonitor focus_monitor;
    LulodFocusSyncState focus_sync = {0};
    char focus_err[160] = {0};

    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        fprintf(stderr, "Usage: lulod\n");
        return 0;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);
    lulod_focus_init(&focus_monitor);

    if (lulod_socket_path(socket_path, sizeof(socket_path)) < 0) {
        fprintf(stderr, "failed to determine lulod socket path\n");
        return 1;
    }
    listen_fd = lulod_create_server_socket(socket_path);
    if (listen_fd < 0) {
        fprintf(stderr, "failed to create lulod socket at %s: %s\n", socket_path, strerror(errno));
        return 1;
    }
    focus_sync_set(&focus_sync, 0, 0,
                   focus_monitor.provider[0] ? focus_monitor.provider : "",
                   mono_ms_now());

    while (lulod_running) {
        int client_fd;
        uint32_t type = 0;
        LuloSystemdState state = {0};
        LuloSchedState sched_state = {0};
        LuloTuneState tune_state = {0};
        LuloCgroupsState cgroups_state = {0};
        LuloUdevState udev_state = {0};
        LuloSystemdSnapshot reply = {0};
        LuloSchedSnapshot sched_reply = {0};
        LuloTuneSnapshot tune_reply = {0};
        LuloCgroupsSnapshot cgroups_reply = {0};
        LuloUdevSnapshot udev_reply = {0};
        char errbuf[160] = {0};
        struct pollfd pfds[2];
        nfds_t nfds = 1;
        int focus_fd;
        int pr;
        long long now_ms;

        memset(pfds, 0, sizeof(pfds));
        pfds[0].fd = listen_fd;
        pfds[0].events = POLLIN;
        now_ms = mono_ms_now();
        focus_fd = lulod_focus_poll_fd(&focus_monitor, now_ms);
        if (focus_fd >= 0) {
            pfds[1].fd = focus_fd;
            pfds[1].events = POLLIN | POLLERR | POLLHUP | POLLNVAL;
            nfds = 2;
        }
        if (now_ms >= focus_sync.next_sync_ms) {
            int ok = lulod_sched_focus_update(focus_sync.pid, focus_sync.start_time,
                                              focus_sync.provider, focus_err,
                                              sizeof(focus_err)) == 0;

            focus_sync_result(&focus_sync, ok, now_ms);
        }

        pr = poll(pfds, nfds, 250);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (nfds > 1 && pfds[1].revents) {
            pid_t focus_pid = 0;
            unsigned long long focus_start_time = 0;
            const char *provider = NULL;
            int focus_rc = lulod_focus_handle(&focus_monitor, pfds[1].revents, mono_ms_now(),
                                              &focus_pid, &focus_start_time, &provider,
                                              errbuf, sizeof(errbuf));

            if (focus_rc > 0) {
                focus_sync_set(&focus_sync, focus_pid, focus_start_time,
                               provider ? provider : "", mono_ms_now());
                have_sched_cache = 0;
                sched_due_ms = 0;
            }
        }
        if (pr == 0) continue;
        if (!(pfds[0].revents & POLLIN)) {
            if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
            continue;
        }

        client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        if (lulod_recv_request_header(client_fd, &type) < 0) {
            lulod_send_systemd_response(client_fd, -1, "bad request", NULL);
            close(client_fd);
            continue;
        }
        if (type == LULOD_REQ_SYSTEMD_FULL || type == LULOD_REQ_SYSTEMD_ACTIVE) {
            if (lulod_recv_systemd_state(client_fd, &state) < 0) {
                lulod_send_systemd_response(client_fd, -1, "bad request", NULL);
                close(client_fd);
                continue;
            }
            if (reply_for_request(type, &state, &cache, &have_cache, &due_ms, &reply) < 0) {
                lulod_send_systemd_response(client_fd, -1, "systemd request failed", NULL);
            } else {
                lulod_send_systemd_response(client_fd, 0, NULL, &reply);
            }
            lulo_systemd_snapshot_free(&reply);
        } else if (type == LULOD_REQ_SCHED_FULL || type == LULOD_REQ_SCHED_ACTIVE ||
                   type == LULOD_REQ_SCHED_RELOAD) {
            if (lulod_recv_sched_state(client_fd, &sched_state) < 0) {
                lulod_send_sched_response(client_fd, -1, "bad request", NULL);
                close(client_fd);
                continue;
            }
            if (reply_for_sched_request(type, &sched_state, &sched_cache,
                                        &have_sched_cache, &sched_due_ms, &sched_reply,
                                        errbuf, sizeof(errbuf)) < 0) {
                lulod_send_sched_response(client_fd, -1,
                                          errbuf[0] ? errbuf : "scheduler request failed", NULL);
            } else {
                lulod_send_sched_response(client_fd, 0, NULL, &sched_reply);
            }
            lulo_sched_snapshot_free(&sched_reply);
        } else if (type == LULOD_REQ_TUNE_FULL || type == LULOD_REQ_TUNE_ACTIVE ||
                   type == LULOD_REQ_TUNE_SAVE_SNAPSHOT || type == LULOD_REQ_TUNE_SAVE_PRESET ||
                   type == LULOD_REQ_TUNE_APPLY_SELECTED) {
            if (lulod_recv_tune_state(client_fd, &tune_state) < 0) {
                lulod_send_tune_response(client_fd, -1, "bad request", NULL);
                close(client_fd);
                continue;
            }
            if (reply_for_tune_request(type, &tune_state, &tune_cache, &tune_cache_state,
                                       &have_tune_cache, &tune_due_ms, &tune_reply,
                                       errbuf, sizeof(errbuf)) < 0) {
                lulod_send_tune_response(client_fd, -1,
                                         errbuf[0] ? errbuf : "tune request failed", NULL);
            } else {
                lulod_send_tune_response(client_fd, 0, NULL, &tune_reply);
            }
            lulo_tune_snapshot_free(&tune_reply);
        } else if (type == LULOD_REQ_CGROUPS_FULL || type == LULOD_REQ_CGROUPS_ACTIVE) {
            if (lulod_recv_cgroups_state(client_fd, &cgroups_state) < 0) {
                lulod_send_cgroups_response(client_fd, -1, "bad request", NULL);
                close(client_fd);
                continue;
            }
            if (reply_for_cgroups_request(type, &cgroups_state, &cgroups_cache, &cgroups_cache_state,
                                          &have_cgroups_cache, &cgroups_due_ms, &cgroups_reply,
                                          errbuf, sizeof(errbuf)) < 0) {
                lulod_send_cgroups_response(client_fd, -1,
                                            errbuf[0] ? errbuf : "cgroups request failed", NULL);
            } else {
                lulod_send_cgroups_response(client_fd, 0, NULL, &cgroups_reply);
            }
            lulo_cgroups_snapshot_free(&cgroups_reply);
        } else if (type == LULOD_REQ_UDEV_FULL || type == LULOD_REQ_UDEV_ACTIVE) {
            if (lulod_recv_udev_state(client_fd, &udev_state) < 0) {
                lulod_send_udev_response(client_fd, -1, "bad request", NULL);
                close(client_fd);
                continue;
            }
            if (reply_for_udev_request(type, &udev_state, &udev_cache,
                                       &have_udev_cache, &udev_due_ms, &udev_reply,
                                       errbuf, sizeof(errbuf)) < 0) {
                lulod_send_udev_response(client_fd, -1,
                                         errbuf[0] ? errbuf : "udev request failed", NULL);
            } else {
                lulod_send_udev_response(client_fd, 0, NULL, &udev_reply);
            }
            lulo_udev_snapshot_free(&udev_reply);
        } else {
            lulod_send_systemd_response(client_fd, -1, "unknown request", NULL);
        }
        close(client_fd);
    }

    focus_sync_set(&focus_sync, 0, 0,
                   focus_monitor.provider[0] ? focus_monitor.provider : "",
                   mono_ms_now());
    (void)lulod_sched_focus_update(focus_sync.pid, focus_sync.start_time,
                                   focus_sync.provider, focus_err,
                                   sizeof(focus_err));
    lulod_focus_stop(&focus_monitor);
    close(listen_fd);
    unlink(socket_path);
    lulo_systemd_snapshot_free(&cache);
    lulo_sched_snapshot_free(&sched_cache);
    lulo_tune_snapshot_free(&tune_cache);
    lulo_cgroups_snapshot_free(&cgroups_cache);
    lulo_udev_snapshot_free(&udev_cache);
    return 0;
}
