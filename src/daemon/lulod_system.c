#define _GNU_SOURCE

#include "lulod_system_edit.h"
#include "lulod_system_ipc.h"
#include "lulod_system_sched.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t lulod_system_running = 1;

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

static int client_peer_ids(int fd, uid_t *uid, gid_t *gid)
{
    struct ucred cred;
    socklen_t len = sizeof(cred);

    if (!uid || !gid) {
        errno = EINVAL;
        return -1;
    }
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0) return -1;
    *uid = cred.uid;
    *gid = cred.gid;
    return 0;
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
    char socket_path[108];
    char config_root[320];
    int listen_fd = -1;
    long long scan_due_ms = 0;
    LuloSchedSnapshot state = {0};
    char errbuf[160] = {0};

    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        fprintf(stderr, "Usage: lulod-system\n");
        return 0;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

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
            uid_t peer_uid = 0;
            gid_t peer_gid = 0;

            if (client_fd < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                lulod_system_running = 0;
                continue;
            }
            if (client_peer_ids(client_fd, &peer_uid, &peer_gid) < 0) {
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
                    rc = lulod_system_edit_commit(session_id, peer_uid, &reload_sched, errbuf, sizeof(errbuf));
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
                rc = lulod_system_write_file(path, content, &reload_sched, errbuf, sizeof(errbuf));
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
                rc = lulod_system_delete_file(path, &reload_sched, errbuf, sizeof(errbuf));
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
                lulod_system_send_sched_response(client_fd, 0, NULL, &state);
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
