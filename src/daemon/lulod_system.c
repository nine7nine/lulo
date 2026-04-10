#define _GNU_SOURCE

#include "lulod_system_edit.h"
#include "lulod_system_ipc.h"
#include "lulod_system_sched.h"
#include "lulod_system_trace.h"
#include "lulo_proc_meta.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <polkit/polkit.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t lulod_system_running = 1;
static const char *k_rw_unlock_action = "io.lulo.system.unlock-rw";

typedef struct {
    int active;
    uid_t uid;
    pid_t pid;
    unsigned long long start_time;
} AuthLease;

static void handle_signal(int signo)
{
    (void)signo;
    lulod_system_running = 0;
}

static long long mono_ms_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int ms_until_deadline(long long deadline_ms)
{
    long long diff = deadline_ms - mono_ms_now();

    if (diff <= 0) return 0;
    if (diff > 2147483647LL) return 2147483647;
    return (int)diff;
}

static int next_scan_deadline_ms(const LuloSchedSnapshot *snap)
{
    int interval = snap && snap->watcher_interval_ms > 0 ? snap->watcher_interval_ms : 1000;
    if (interval < 100) interval = 100;
    return interval;
}

static int client_peer_ids(int fd, pid_t *pid, uid_t *uid, gid_t *gid)
{
    struct ucred cred;
    socklen_t len = sizeof(cred);

    if (!pid || !uid || !gid) {
        errno = EINVAL;
        return -1;
    }
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0) return -1;
    *pid = cred.pid;
    *uid = cred.uid;
    *gid = cred.gid;
    return 0;
}

static int read_peer_start_time(pid_t pid, unsigned long long *start_time)
{
    char comm[128];

    if (!start_time || pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    return lulo_proc_meta_read_basic(pid, comm, sizeof(comm), start_time);
}

static void auth_lease_revoke(AuthLease *leases, int count,
                              uid_t uid, pid_t pid, unsigned long long start_time)
{
    for (int i = 0; i < count; i++) {
        if (!leases[i].active) continue;
        if (leases[i].uid != uid) continue;
        if (leases[i].pid != pid) continue;
        if (start_time != 0 && leases[i].start_time != start_time) continue;
        memset(&leases[i], 0, sizeof(leases[i]));
    }
}

static int auth_lease_has(AuthLease *leases, int count,
                          uid_t uid, pid_t pid, unsigned long long start_time)
{
    for (int i = 0; i < count; i++) {
        if (!leases[i].active) continue;
        if (leases[i].uid == uid &&
            leases[i].pid == pid &&
            leases[i].start_time == start_time) {
            return 1;
        }
    }
    return 0;
}

static void auth_lease_grant(AuthLease *leases, int count,
                             uid_t uid, pid_t pid, unsigned long long start_time)
{
    int slot = -1;

    for (int i = 0; i < count; i++) {
        if (leases[i].active &&
            leases[i].uid == uid &&
            leases[i].pid == pid &&
            leases[i].start_time == start_time) {
            slot = i;
            break;
        }
        if (!leases[i].active && slot < 0) slot = i;
    }
    if (slot < 0) slot = 0;
    leases[slot].active = 1;
    leases[slot].uid = uid;
    leases[slot].pid = pid;
    leases[slot].start_time = start_time;
}

static int check_polkit_unlock(uid_t uid, pid_t pid, unsigned long long start_time,
                               char *err, size_t errlen)
{
    PolkitAuthority *authority = NULL;
    PolkitSubject *subject = NULL;
    PolkitAuthorizationResult *result = NULL;
    GError *error = NULL;
    int ok = -1;

    if (uid == 0) return 0;
    if (err && errlen > 0) err[0] = '\0';

    authority = polkit_authority_get_sync(NULL, &error);
    if (!authority) {
        if (err && errlen > 0) {
            snprintf(err, errlen, "%s", error && error->message ? error->message : "failed to contact polkit");
        }
        goto out;
    }
    subject = polkit_unix_process_new_for_owner(pid, start_time, uid);
    result = polkit_authority_check_authorization_sync(authority, subject, k_rw_unlock_action,
                                                       NULL, POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION,
                                                       NULL, &error);
    if (!result) {
        if (err && errlen > 0) {
            snprintf(err, errlen, "%s", error && error->message ? error->message : "rw authorization required");
        }
        goto out;
    }
    if (!polkit_authorization_result_get_is_authorized(result)) {
        if (err && errlen > 0) snprintf(err, errlen, "rw authorization required");
        errno = EPERM;
        goto out;
    }
    ok = 0;

out:
    if (result) g_object_unref(result);
    if (subject) g_object_unref(subject);
    if (authority) g_object_unref(authority);
    if (error) g_error_free(error);
    return ok;
}

static int require_rw_lease(AuthLease *leases, int count,
                            uid_t uid, pid_t pid, char *err, size_t errlen)
{
    unsigned long long start_time = 0;

    if (uid == 0) return 0;
    if (read_peer_start_time(pid, &start_time) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "rw authorization required");
        errno = EPERM;
        return -1;
    }
    if (auth_lease_has(leases, count, uid, pid, start_time)) return 0;
    if (err && errlen > 0) snprintf(err, errlen, "rw authorization required");
    errno = EPERM;
    return -1;
}

static int do_reload_and_scan(LuloSchedSnapshot *snap, const char *config_root,
                              char *err, size_t errlen)
{
    if (lulod_system_sched_reload(snap, config_root, err, errlen) < 0) return -1;
    if (lulod_system_sched_scan(snap, err, errlen) < 0) return -1;
    return 0;
}

int main(int argc, char *argv[])
{
    enum { AUTH_LEASE_MAX = 16 };
    char socket_path[108];
    char config_root[320];
    int listen_fd = -1;
    long long scan_due_ms = 0;
    LuloSchedSnapshot state = {0};
    char errbuf[160] = {0};
    AuthLease auth_leases[AUTH_LEASE_MAX];

    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        fprintf(stderr, "Usage: lulod-system\n");
        return 0;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);
    memset(auth_leases, 0, sizeof(auth_leases));

    if (lulod_system_socket_path(socket_path, sizeof(socket_path)) < 0) {
        fprintf(stderr, "failed to determine lulod-system socket path\n");
        return 1;
    }
    if (lulod_system_sched_default_config_root(config_root, sizeof(config_root)) < 0) {
        fprintf(stderr, "failed to determine scheduler config root\n");
        return 1;
    }
    if (do_reload_and_scan(&state, config_root, errbuf, sizeof(errbuf)) < 0) {
        fprintf(stderr, "failed to load scheduler config from %s: %s\n",
                config_root, errbuf[0] ? errbuf : "unknown error");
        return 1;
    }

    listen_fd = lulod_system_create_server_socket(socket_path);
    if (listen_fd < 0) {
        fprintf(stderr, "failed to create lulod-system socket at %s: %s\n",
                socket_path, strerror(errno));
        lulo_sched_snapshot_free(&state);
        return 1;
    }
    scan_due_ms = mono_ms_now() + next_scan_deadline_ms(&state);

    while (lulod_system_running) {
        struct pollfd pfd = {
            .fd = listen_fd,
            .events = POLLIN,
        };
        int timeout_ms = ms_until_deadline(scan_due_ms);
        int pr = poll(&pfd, 1, timeout_ms);

        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0 || mono_ms_now() >= scan_due_ms) {
            if (lulod_system_sched_scan(&state, errbuf, sizeof(errbuf)) < 0) {
                fprintf(stderr, "scheduler scan failed: %s\n", errbuf[0] ? errbuf : "unknown error");
            }
            scan_due_ms = mono_ms_now() + next_scan_deadline_ms(&state);
            if (pr == 0) continue;
        }
        if (!(pfd.revents & POLLIN)) {
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
            continue;
        }

        {
            int client_fd = accept(listen_fd, NULL, NULL);
            uint32_t type = 0;
            pid_t peer_pid = 0;
            uid_t peer_uid = 0;
            gid_t peer_gid = 0;

            if (client_fd < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                lulod_system_running = 0;
                continue;
            }
            if (client_peer_ids(client_fd, &peer_pid, &peer_uid, &peer_gid) < 0) {
                lulod_system_send_status_response(client_fd, -1, "failed to read client credentials");
                close(client_fd);
                continue;
            }
            if (lulod_system_recv_request_header(client_fd, &type) < 0) {
                lulod_system_send_sched_response(client_fd, -1, "bad request", NULL);
                close(client_fd);
                continue;
            }
            if (type == LULOD_SYSTEM_REQ_SCHED_RELOAD) {
                if (do_reload_and_scan(&state, config_root, errbuf, sizeof(errbuf)) < 0) {
                    lulod_system_send_sched_response(client_fd, -1,
                                                     errbuf[0] ? errbuf : "reload failed", NULL);
                } else {
                    scan_due_ms = mono_ms_now() + next_scan_deadline_ms(&state);
                    lulod_system_send_sched_response(client_fd, 0, NULL, &state);
                }
            } else if (type == LULOD_SYSTEM_REQ_AUTH_UNLOCK) {
                unsigned long long start_time = 0;

                if (read_peer_start_time(peer_pid, &start_time) < 0) {
                    lulod_system_send_status_response(client_fd, -1, "failed to resolve auth subject");
                } else if (check_polkit_unlock(peer_uid, peer_pid, start_time, errbuf, sizeof(errbuf)) < 0) {
                    lulod_system_send_status_response(client_fd, -1,
                                                      errbuf[0] ? errbuf : "rw authorization required");
                } else {
                    auth_lease_grant(auth_leases, AUTH_LEASE_MAX, peer_uid, peer_pid, start_time);
                    lulod_system_send_status_response(client_fd, 0, NULL);
                }
            } else if (type == LULOD_SYSTEM_REQ_AUTH_LOCK) {
                unsigned long long start_time = 0;

                if (read_peer_start_time(peer_pid, &start_time) == 0) {
                    auth_lease_revoke(auth_leases, AUTH_LEASE_MAX, peer_uid, peer_pid, start_time);
                } else {
                    auth_lease_revoke(auth_leases, AUTH_LEASE_MAX, peer_uid, peer_pid, 0);
                }
                lulod_system_send_status_response(client_fd, 0, NULL);
            } else if (type == LULOD_SYSTEM_REQ_SCHED_APPLY_PRESET) {
                char preset_id[96];

                if (lulod_system_recv_sched_apply_preset_request(client_fd, preset_id, sizeof(preset_id)) < 0) {
                    lulod_system_send_status_response(client_fd, -1, "bad scheduler preset request");
                } else if (require_rw_lease(auth_leases, AUTH_LEASE_MAX, peer_uid, peer_pid,
                                            errbuf, sizeof(errbuf)) < 0) {
                    lulod_system_send_status_response(client_fd, -1,
                                                      errbuf[0] ? errbuf : "rw authorization required");
                } else if (lulod_system_sched_apply_tunable_preset(&state, preset_id,
                                                                   errbuf, sizeof(errbuf)) < 0) {
                    lulod_system_send_status_response(client_fd, -1,
                                                      errbuf[0] ? errbuf : "failed to apply scheduler preset");
                } else {
                    lulod_system_send_status_response(client_fd, 0, NULL);
                }
            } else if (type == LULOD_SYSTEM_REQ_SCHED_FOCUS_UPDATE) {
                pid_t focus_pid = 0;
                unsigned long long focus_start_time = 0;
                char provider[32] = "";

                if (lulod_system_recv_focus_update_request(client_fd, &focus_pid, &focus_start_time,
                                                           provider, sizeof(provider)) < 0) {
                    lulod_system_send_status_response(client_fd, -1, "bad focus update");
                } else if (lulod_system_sched_set_focus(&state, focus_pid, focus_start_time,
                                                        provider) < 0 && focus_pid > 0) {
                    lulod_system_send_status_response(client_fd, -1, "failed to set focus");
                } else if (lulod_system_sched_scan(&state, errbuf, sizeof(errbuf)) < 0) {
                    lulod_system_send_status_response(client_fd, -1,
                                                      errbuf[0] ? errbuf : "scan failed");
                } else {
                    scan_due_ms = mono_ms_now() + next_scan_deadline_ms(&state);
                    lulod_system_send_status_response(client_fd, 0, NULL);
                }
            } else if (type == LULOD_SYSTEM_REQ_EDIT_BEGIN) {
                char path[PATH_MAX];
                char session_id[PATH_MAX];
                char edit_path[PATH_MAX];

                if (lulod_system_recv_edit_begin_request(client_fd, path, sizeof(path)) < 0) {
                    lulod_system_send_edit_begin_response(client_fd, -1, "bad edit request", NULL, NULL);
                } else if (require_rw_lease(auth_leases, AUTH_LEASE_MAX, peer_uid, peer_pid,
                                            errbuf, sizeof(errbuf)) < 0) {
                    lulod_system_send_edit_begin_response(client_fd, -1,
                                                          errbuf[0] ? errbuf : "rw authorization required",
                                                          NULL, NULL);
                } else if (lulod_system_edit_begin(path, peer_uid, peer_gid,
                                                   session_id, sizeof(session_id),
                                                   edit_path, sizeof(edit_path),
                                                   NULL, errbuf, sizeof(errbuf)) < 0) {
                    lulod_system_send_edit_begin_response(client_fd, -1,
                                                          errbuf[0] ? errbuf : "failed to start edit session",
                                                          NULL, NULL);
                } else {
                    lulod_system_send_edit_begin_response(client_fd, 0, NULL, session_id, edit_path);
                }
            } else if (type == LULOD_SYSTEM_REQ_TRACE_BEGIN) {
                pid_t target_pid = 0;
                char session_id[128];
                char output_path[PATH_MAX];

                if (lulod_system_recv_trace_begin_request(client_fd, &target_pid) < 0) {
                    lulod_system_send_trace_begin_response(client_fd, -1, "bad trace request", NULL, NULL);
                } else if (require_rw_lease(auth_leases, AUTH_LEASE_MAX, peer_uid, peer_pid,
                                            errbuf, sizeof(errbuf)) < 0) {
                    lulod_system_send_trace_begin_response(client_fd, -1,
                                                           errbuf[0] ? errbuf : "rw authorization required",
                                                           NULL, NULL);
                } else if (lulod_system_trace_begin(target_pid, peer_uid, peer_gid,
                                                    session_id, sizeof(session_id),
                                                    output_path, sizeof(output_path),
                                                    errbuf, sizeof(errbuf)) < 0) {
                    lulod_system_send_trace_begin_response(client_fd, -1,
                                                           errbuf[0] ? errbuf : "failed to start trace",
                                                           NULL, NULL);
                } else {
                    lulod_system_send_trace_begin_response(client_fd, 0, NULL, session_id, output_path);
                }
            } else if (type == LULOD_SYSTEM_REQ_TRACE_END) {
                char session_id[128];

                if (lulod_system_recv_trace_end_request(client_fd, session_id, sizeof(session_id)) < 0) {
                    lulod_system_send_status_response(client_fd, -1, "bad trace stop request");
                } else if (lulod_system_trace_end(session_id, peer_uid, errbuf, sizeof(errbuf)) < 0) {
                    lulod_system_send_status_response(client_fd, -1,
                                                      errbuf[0] ? errbuf : "failed to stop trace");
                } else {
                    lulod_system_send_status_response(client_fd, 0, NULL);
                }
            } else if (type == LULOD_SYSTEM_REQ_EDIT_COMMIT || type == LULOD_SYSTEM_REQ_EDIT_CANCEL) {
                char session_id[PATH_MAX];
                int reload_sched = 0;
                int rc;

                if (lulod_system_recv_edit_session_request(client_fd, session_id, sizeof(session_id)) < 0) {
                    lulod_system_send_status_response(client_fd, -1, "bad edit session request");
                    close(client_fd);
                    continue;
                }
                if (type == LULOD_SYSTEM_REQ_EDIT_COMMIT) {
                    if (require_rw_lease(auth_leases, AUTH_LEASE_MAX, peer_uid, peer_pid,
                                         errbuf, sizeof(errbuf)) < 0) {
                        rc = -1;
                    } else {
                        rc = lulod_system_edit_commit(session_id, peer_uid, &reload_sched, errbuf, sizeof(errbuf));
                    }
                    if (rc == 0 && reload_sched) {
                        rc = do_reload_and_scan(&state, config_root, errbuf, sizeof(errbuf));
                        if (rc == 0) scan_due_ms = mono_ms_now() + next_scan_deadline_ms(&state);
                    }
                } else {
                    rc = lulod_system_edit_cancel(session_id, peer_uid, errbuf, sizeof(errbuf));
                }
                if (rc < 0) {
                    lulod_system_send_status_response(client_fd, -1,
                                                      errbuf[0] ? errbuf : "edit request failed");
                } else {
                    lulod_system_send_status_response(client_fd, 0, NULL);
                }
            } else if (type == LULOD_SYSTEM_REQ_FILE_WRITE) {
                char path[PATH_MAX];
                char *content = NULL;
                int reload_sched = 0;
                int rc;

                if (lulod_system_recv_file_write_request(client_fd, path, sizeof(path), &content) < 0) {
                    lulod_system_send_status_response(client_fd, -1, "bad file write request");
                    close(client_fd);
                    continue;
                }
                if (require_rw_lease(auth_leases, AUTH_LEASE_MAX, peer_uid, peer_pid,
                                     errbuf, sizeof(errbuf)) < 0) {
                    rc = -1;
                } else {
                    rc = lulod_system_write_file(path, content, &reload_sched, errbuf, sizeof(errbuf));
                }
                free(content);
                if (rc == 0 && reload_sched) {
                    rc = do_reload_and_scan(&state, config_root, errbuf, sizeof(errbuf));
                    if (rc == 0) scan_due_ms = mono_ms_now() + next_scan_deadline_ms(&state);
                }
                if (rc < 0) {
                    lulod_system_send_status_response(client_fd, -1,
                                                      errbuf[0] ? errbuf : "file write failed");
                } else {
                    lulod_system_send_status_response(client_fd, 0, NULL);
                }
            } else if (type == LULOD_SYSTEM_REQ_FILE_DELETE) {
                char path[PATH_MAX];
                int reload_sched = 0;
                int rc;

                if (lulod_system_recv_file_delete_request(client_fd, path, sizeof(path)) < 0) {
                    lulod_system_send_status_response(client_fd, -1, "bad file delete request");
                    close(client_fd);
                    continue;
                }
                if (require_rw_lease(auth_leases, AUTH_LEASE_MAX, peer_uid, peer_pid,
                                     errbuf, sizeof(errbuf)) < 0) {
                    rc = -1;
                } else {
                    rc = lulod_system_delete_file(path, &reload_sched, errbuf, sizeof(errbuf));
                }
                if (rc == 0 && reload_sched) {
                    rc = do_reload_and_scan(&state, config_root, errbuf, sizeof(errbuf));
                    if (rc == 0) scan_due_ms = mono_ms_now() + next_scan_deadline_ms(&state);
                }
                if (rc < 0) {
                    lulod_system_send_status_response(client_fd, -1,
                                                      errbuf[0] ? errbuf : "file delete failed");
                } else {
                    lulod_system_send_status_response(client_fd, 0, NULL);
                }
            } else if (type == LULOD_SYSTEM_REQ_SCHED_FULL) {
                if (lulod_system_sched_refresh_aux(&state, errbuf, sizeof(errbuf)) < 0) {
                    lulod_system_send_sched_response(client_fd, -1,
                                                     errbuf[0] ? errbuf : "failed to refresh scheduler tunables",
                                                     NULL);
                } else {
                    lulod_system_send_sched_response(client_fd, 0, NULL, &state);
                }
            } else {
                lulod_system_send_sched_response(client_fd, -1, "unknown request", NULL);
            }
            close(client_fd);
        }
    }

    if (listen_fd >= 0) close(listen_fd);
    unlink(socket_path);
    lulo_sched_snapshot_free(&state);
    return 0;
}
