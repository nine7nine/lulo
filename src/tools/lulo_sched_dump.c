#define _GNU_SOURCE

#include "lulod_ipc.h"
#include "lulod_system_ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void dump_snapshot(const char *label, const LuloSchedSnapshot *snap)
{
    printf("[%s]\n", label);
    printf("config_root=%s\n", snap->config_root);
    printf("watcher_interval_ms=%d scan_generation=%d\n",
           snap->watcher_interval_ms, snap->scan_generation);
    printf("focus_enabled=%d focus_provider=%s focused_pid=%d focus_profile=%s background_enabled=%d background_profile=%s\n",
           snap->focus_enabled, snap->focus_provider, snap->focused_pid,
           snap->focus_profile, snap->background_enabled, snap->background_profile);
    printf("background_match_app_slice=%d background_match_background_slice=%d background_match_app_unit_prefix=%d\n",
           snap->background_match_app_slice, snap->background_match_background_slice,
           snap->background_match_app_unit_prefix);
    printf("profiles=%d rules=%d live=%d\n",
           snap->profile_count, snap->rule_count, snap->live_count);
    for (int i = 0; i < snap->profile_count; i++) {
        const LuloSchedProfileRow *row = &snap->profiles[i];

        printf("PROFILE[%d] name=%s enabled=%d nice=%d has_nice=%d policy=%d has_policy=%d rt=%d has_rt=%d\n",
               i, row->name, row->enabled, row->nice, row->has_nice,
               row->policy, row->has_policy, row->rt_priority, row->has_rt_priority);
    }
    for (int i = 0; i < snap->live_count; i++) {
        const LuloSchedLiveRow *row = &snap->live[i];

        printf("%5d %-20s profile=%-14s rule=%-18s pol=%d ni=%d rt=%d focused=%d status=%s\n",
               row->pid, row->comm, row->profile, row->rule,
               row->policy, row->nice, row->rt_priority, row->focused, row->status);
    }
}

static int fetch_via_lulod(LuloSchedSnapshot *snap, char *err, size_t errlen)
{
    char socket_path[108];
    LuloSchedState state = {0};
    int fd = -1;

    if (err && errlen > 0) err[0] = '\0';
    if (lulod_socket_path(socket_path, sizeof(socket_path)) < 0) {
        snprintf(err, errlen, "bad lulod socket path");
        return -1;
    }
    fd = lulod_connect_socket(socket_path);
    if (fd < 0) {
        snprintf(err, errlen, "failed to connect to lulod");
        return -1;
    }
    if (lulod_send_sched_request(fd, LULOD_REQ_SCHED_FULL, &state) < 0) {
        snprintf(err, errlen, "failed to send request to lulod");
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

static int fetch_via_lulod_system(LuloSchedSnapshot *snap, char *err, size_t errlen)
{
    char socket_path[108];
    int fd = -1;

    if (err && errlen > 0) err[0] = '\0';
    if (lulod_system_socket_path(socket_path, sizeof(socket_path)) < 0) {
        snprintf(err, errlen, "bad lulod-system socket path");
        return -1;
    }
    fd = lulod_system_connect_socket(socket_path);
    if (fd < 0) {
        snprintf(err, errlen, "failed to connect to lulod-system");
        return -1;
    }
    if (lulod_system_send_sched_request(fd, LULOD_SYSTEM_REQ_SCHED_FULL) < 0) {
        snprintf(err, errlen, "failed to send request to lulod-system");
        close(fd);
        return -1;
    }
    if (lulod_system_recv_sched_response(fd, snap, err, errlen) < 0) {
        if (err && errlen > 0 && !err[0]) snprintf(err, errlen, "failed to read lulod-system response");
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    LuloSchedSnapshot snap = {0};
    char err[160] = {0};
    int rc;

    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        fprintf(stderr, "Usage: lulo_sched_dump [lulod|system]\n");
        return 0;
    }

    if (argc > 1 && !strcmp(argv[1], "system")) {
        rc = fetch_via_lulod_system(&snap, err, sizeof(err));
        if (rc == 0) dump_snapshot("lulod-system", &snap);
    } else {
        rc = fetch_via_lulod(&snap, err, sizeof(err));
        if (rc == 0) dump_snapshot("lulod", &snap);
    }

    if (rc < 0) {
        fprintf(stderr, "%s\n", err[0] ? err : "scheduler dump failed");
        return 1;
    }
    lulo_sched_snapshot_free(&snap);
    return 0;
}
