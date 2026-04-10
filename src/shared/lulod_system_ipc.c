#define _GNU_SOURCE

#include "lulod_system_ipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define LULOD_SYSTEM_MAGIC 0x4c555359U
#define LULOD_SYSTEM_VERSION 8U

static int append_owned_line(char ***lines, int *count, const char *text)
{
    char **next;
    char *copy;

    next = realloc(*lines, (size_t)(*count + 1) * sizeof(*next));
    if (!next) return -1;
    *lines = next;
    copy = strdup(text ? text : "");
    if (!copy) return -1;
    (*lines)[(*count)++] = copy;
    return 0;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;

    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t len)
{
    unsigned char *p = buf;

    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int write_u32(int fd, uint32_t value)
{
    return write_all(fd, &value, sizeof(value));
}

static int write_u64(int fd, uint64_t value)
{
    return write_all(fd, &value, sizeof(value));
}

static int write_i32(int fd, int32_t value)
{
    return write_all(fd, &value, sizeof(value));
}

static int read_u32(int fd, uint32_t *value)
{
    return read_all(fd, value, sizeof(*value));
}

static int read_u64(int fd, uint64_t *value)
{
    return read_all(fd, value, sizeof(*value));
}

static int read_i32(int fd, int32_t *value)
{
    return read_all(fd, value, sizeof(*value));
}

static int write_string(int fd, const char *s)
{
    uint32_t len = s ? (uint32_t)strlen(s) : 0;

    if (write_u32(fd, len) < 0) return -1;
    if (len == 0) return 0;
    return write_all(fd, s, len);
}

static int read_string_dyn(int fd, char **out)
{
    uint32_t len = 0;
    char *buf;

    *out = NULL;
    if (read_u32(fd, &len) < 0) return -1;
    buf = calloc((size_t)len + 1, 1);
    if (!buf) return -1;
    if (len > 0 && read_all(fd, buf, len) < 0) {
        free(buf);
        return -1;
    }
    *out = buf;
    return 0;
}

static int read_string_fixed(int fd, char *dst, size_t len)
{
    char *tmp = NULL;

    if (read_string_dyn(fd, &tmp) < 0) return -1;
    if (len > 0) snprintf(dst, len, "%s", tmp ? tmp : "");
    free(tmp);
    return 0;
}

int lulod_system_send_file_write_request(int fd, const char *path, const char *content)
{
    if (write_u32(fd, LULOD_SYSTEM_MAGIC) < 0) return -1;
    if (write_u32(fd, LULOD_SYSTEM_VERSION) < 0) return -1;
    if (write_u32(fd, LULOD_SYSTEM_REQ_FILE_WRITE) < 0) return -1;
    if (write_string(fd, path) < 0) return -1;
    return write_string(fd, content ? content : "");
}

int lulod_system_recv_file_write_request(int fd, char *path, size_t path_len,
                                         char **content)
{
    if (read_string_fixed(fd, path, path_len) < 0) return -1;
    return read_string_dyn(fd, content);
}

int lulod_system_send_file_delete_request(int fd, const char *path)
{
    if (write_u32(fd, LULOD_SYSTEM_MAGIC) < 0) return -1;
    if (write_u32(fd, LULOD_SYSTEM_VERSION) < 0) return -1;
    if (write_u32(fd, LULOD_SYSTEM_REQ_FILE_DELETE) < 0) return -1;
    return write_string(fd, path);
}

int lulod_system_recv_file_delete_request(int fd, char *path, size_t path_len)
{
    return read_string_fixed(fd, path, path_len);
}

static int serialize_sched_snapshot(int fd, const LuloSchedSnapshot *snap)
{
    uint32_t count = 0;

    if (!snap) return -1;
    if (write_string(fd, snap->config_root) < 0) return -1;
    if (write_i32(fd, snap->watcher_interval_ms) < 0) return -1;
    if (write_i32(fd, snap->scan_generation) < 0) return -1;
    if (write_i32(fd, snap->focus_enabled) < 0) return -1;
    if (write_i32(fd, snap->focused_pid) < 0) return -1;
    if (write_u64(fd, snap->focused_start_time) < 0) return -1;
    if (write_string(fd, snap->focus_provider) < 0) return -1;
    if (write_string(fd, snap->focus_profile) < 0) return -1;
    if (write_i32(fd, snap->background_enabled) < 0) return -1;
    if (write_string(fd, snap->background_profile) < 0) return -1;
    if (write_i32(fd, snap->background_match_app_slice) < 0) return -1;
    if (write_i32(fd, snap->background_match_background_slice) < 0) return -1;
    if (write_i32(fd, snap->background_match_app_unit_prefix) < 0) return -1;
    if (write_string(fd, snap->tunables_startup_preset) < 0) return -1;
    if (write_string(fd, snap->focused_comm) < 0) return -1;
    if (write_string(fd, snap->focused_exe) < 0) return -1;
    if (write_string(fd, snap->focused_unit) < 0) return -1;
    if (write_string(fd, snap->focused_slice) < 0) return -1;
    if (write_string(fd, snap->focused_cgroup) < 0) return -1;

    if (write_i32(fd, snap->profile_count) < 0) return -1;
    for (int i = 0; i < snap->profile_count; i++) {
        const LuloSchedProfileRow *row = &snap->profiles[i];

        if (write_string(fd, row->name) < 0) return -1;
        if (write_string(fd, row->path) < 0) return -1;
        if (write_i32(fd, row->enabled) < 0) return -1;
        if (write_i32(fd, row->has_nice) < 0) return -1;
        if (write_i32(fd, row->nice) < 0) return -1;
        if (write_i32(fd, row->has_policy) < 0) return -1;
        if (write_i32(fd, row->policy) < 0) return -1;
        if (write_i32(fd, row->has_rt_priority) < 0) return -1;
        if (write_i32(fd, row->rt_priority) < 0) return -1;
        if (write_i32(fd, row->has_io_class) < 0) return -1;
        if (write_i32(fd, row->io_class) < 0) return -1;
        if (write_i32(fd, row->has_io_priority) < 0) return -1;
        if (write_i32(fd, row->io_priority) < 0) return -1;
    }

    if (write_i32(fd, snap->rule_count) < 0) return -1;
    for (int i = 0; i < snap->rule_count; i++) {
        const LuloSchedRuleRow *row = &snap->rules[i];

        if (write_string(fd, row->name) < 0) return -1;
        if (write_string(fd, row->path) < 0) return -1;
        if (write_i32(fd, row->enabled) < 0) return -1;
        if (write_i32(fd, row->exclude) < 0) return -1;
        if (write_i32(fd, (int32_t)row->match_kind) < 0) return -1;
        if (write_string(fd, row->pattern) < 0) return -1;
        if (write_string(fd, row->profile) < 0) return -1;
    }

    if (write_i32(fd, snap->live_count) < 0) return -1;
    for (int i = 0; i < snap->live_count; i++) {
        const LuloSchedLiveRow *row = &snap->live[i];

        if (write_i32(fd, row->pid) < 0) return -1;
        if (write_u64(fd, row->start_time) < 0) return -1;
        if (write_string(fd, row->comm) < 0) return -1;
        if (write_string(fd, row->exe) < 0) return -1;
        if (write_string(fd, row->unit) < 0) return -1;
        if (write_string(fd, row->slice) < 0) return -1;
        if (write_string(fd, row->cgroup) < 0) return -1;
        if (write_string(fd, row->profile) < 0) return -1;
        if (write_string(fd, row->rule) < 0) return -1;
        if (write_i32(fd, row->policy) < 0) return -1;
        if (write_i32(fd, row->rt_priority) < 0) return -1;
        if (write_i32(fd, row->nice) < 0) return -1;
        if (write_i32(fd, row->io_class) < 0) return -1;
        if (write_i32(fd, row->io_priority) < 0) return -1;
        if (write_i32(fd, row->focused) < 0) return -1;
        if (write_string(fd, row->status) < 0) return -1;
    }

    if (write_i32(fd, snap->tunable_count) < 0) return -1;
    for (int i = 0; i < snap->tunable_count; i++) {
        const LuloSchedTunableRow *row = &snap->tunables[i];

        if (write_string(fd, row->path) < 0) return -1;
        if (write_string(fd, row->source) < 0) return -1;
        if (write_string(fd, row->group) < 0) return -1;
        if (write_string(fd, row->name) < 0) return -1;
        if (write_string(fd, row->value) < 0) return -1;
        if (write_i32(fd, row->writable) < 0) return -1;
    }

    if (write_i32(fd, snap->preset_count) < 0) return -1;
    for (int i = 0; i < snap->preset_count; i++) {
        const LuloSchedPresetRow *row = &snap->presets[i];

        if (write_string(fd, row->id) < 0) return -1;
        if (write_string(fd, row->name) < 0) return -1;
        if (write_string(fd, row->created) < 0) return -1;
        if (write_string(fd, row->path) < 0) return -1;
        if (write_i32(fd, row->item_count) < 0) return -1;
        if (write_i32(fd, row->startup) < 0) return -1;
    }

    if (write_string(fd, snap->detail_title) < 0) return -1;
    if (write_string(fd, snap->detail_status) < 0) return -1;
    count = (uint32_t)snap->detail_line_count;
    if (write_u32(fd, count) < 0) return -1;
    for (uint32_t i = 0; i < count; i++) {
        if (write_string(fd, snap->detail_lines[i]) < 0) return -1;
    }
    return 0;
}

static int deserialize_sched_snapshot(int fd, LuloSchedSnapshot *snap)
{
    int32_t count = 0;
    uint64_t start_time = 0;
    uint32_t lines = 0;

    if (!snap) return -1;
    memset(snap, 0, sizeof(*snap));

    if (read_string_fixed(fd, snap->config_root, sizeof(snap->config_root)) < 0) goto fail;
    if (read_i32(fd, &count) < 0) goto fail;
    snap->watcher_interval_ms = count;
    if (read_i32(fd, &count) < 0) goto fail;
    snap->scan_generation = count;
    if (read_i32(fd, &count) < 0) goto fail;
    snap->focus_enabled = count;
    if (read_i32(fd, &count) < 0) goto fail;
    snap->focused_pid = count;
    if (read_u64(fd, &start_time) < 0) goto fail;
    snap->focused_start_time = start_time;
    if (read_string_fixed(fd, snap->focus_provider, sizeof(snap->focus_provider)) < 0) goto fail;
    if (read_string_fixed(fd, snap->focus_profile, sizeof(snap->focus_profile)) < 0) goto fail;
    if (read_i32(fd, &count) < 0) goto fail;
    snap->background_enabled = count;
    if (read_string_fixed(fd, snap->background_profile, sizeof(snap->background_profile)) < 0) goto fail;
    if (read_i32(fd, &count) < 0) goto fail;
    snap->background_match_app_slice = count;
    if (read_i32(fd, &count) < 0) goto fail;
    snap->background_match_background_slice = count;
    if (read_i32(fd, &count) < 0) goto fail;
    snap->background_match_app_unit_prefix = count;
    if (read_string_fixed(fd, snap->tunables_startup_preset, sizeof(snap->tunables_startup_preset)) < 0) goto fail;
    if (read_string_fixed(fd, snap->focused_comm, sizeof(snap->focused_comm)) < 0) goto fail;
    if (read_string_fixed(fd, snap->focused_exe, sizeof(snap->focused_exe)) < 0) goto fail;
    if (read_string_fixed(fd, snap->focused_unit, sizeof(snap->focused_unit)) < 0) goto fail;
    if (read_string_fixed(fd, snap->focused_slice, sizeof(snap->focused_slice)) < 0) goto fail;
    if (read_string_fixed(fd, snap->focused_cgroup, sizeof(snap->focused_cgroup)) < 0) goto fail;

    if (read_i32(fd, &count) < 0) goto fail;
    if (count < 0) goto fail;
    if (count > 0) {
        snap->profiles = calloc((size_t)count, sizeof(*snap->profiles));
        if (!snap->profiles) goto fail;
    }
    snap->profile_count = count;
    for (int i = 0; i < snap->profile_count; i++) {
        LuloSchedProfileRow *row = &snap->profiles[i];

        if (read_string_fixed(fd, row->name, sizeof(row->name)) < 0) goto fail;
        if (read_string_fixed(fd, row->path, sizeof(row->path)) < 0) goto fail;
        if (read_i32(fd, &count) < 0) goto fail;
        row->enabled = count;
        if (read_i32(fd, &count) < 0) goto fail;
        row->has_nice = count;
        if (read_i32(fd, &count) < 0) goto fail;
        row->nice = count;
        if (read_i32(fd, &count) < 0) goto fail;
        row->has_policy = count;
        if (read_i32(fd, &count) < 0) goto fail;
        row->policy = count;
        if (read_i32(fd, &count) < 0) goto fail;
        row->has_rt_priority = count;
        if (read_i32(fd, &count) < 0) goto fail;
        row->rt_priority = count;
        if (read_i32(fd, &count) < 0) goto fail;
        row->has_io_class = count;
        if (read_i32(fd, &count) < 0) goto fail;
        row->io_class = count;
        if (read_i32(fd, &count) < 0) goto fail;
        row->has_io_priority = count;
        if (read_i32(fd, &count) < 0) goto fail;
        row->io_priority = count;
    }

    if (read_i32(fd, &count) < 0) goto fail;
    if (count < 0) goto fail;
    if (count > 0) {
        snap->rules = calloc((size_t)count, sizeof(*snap->rules));
        if (!snap->rules) goto fail;
    }
    snap->rule_count = count;
    for (int i = 0; i < snap->rule_count; i++) {
        LuloSchedRuleRow *row = &snap->rules[i];

        if (read_string_fixed(fd, row->name, sizeof(row->name)) < 0) goto fail;
        if (read_string_fixed(fd, row->path, sizeof(row->path)) < 0) goto fail;
        if (read_i32(fd, &count) < 0) goto fail;
        row->enabled = count;
        if (read_i32(fd, &count) < 0) goto fail;
        row->exclude = count;
        if (read_i32(fd, &count) < 0) goto fail;
        row->match_kind = (LuloSchedMatchKind)count;
        if (read_string_fixed(fd, row->pattern, sizeof(row->pattern)) < 0) goto fail;
        if (read_string_fixed(fd, row->profile, sizeof(row->profile)) < 0) goto fail;
    }

    if (read_i32(fd, &count) < 0) goto fail;
    if (count < 0) goto fail;
    if (count > 0) {
        snap->live = calloc((size_t)count, sizeof(*snap->live));
        if (!snap->live) goto fail;
    }
    snap->live_count = count;
    for (int i = 0; i < snap->live_count; i++) {
        LuloSchedLiveRow *row = &snap->live[i];

        if (read_i32(fd, &count) < 0) goto fail;
        row->pid = count;
        if (read_u64(fd, &start_time) < 0) goto fail;
        row->start_time = start_time;
        if (read_string_fixed(fd, row->comm, sizeof(row->comm)) < 0) goto fail;
        if (read_string_fixed(fd, row->exe, sizeof(row->exe)) < 0) goto fail;
        if (read_string_fixed(fd, row->unit, sizeof(row->unit)) < 0) goto fail;
        if (read_string_fixed(fd, row->slice, sizeof(row->slice)) < 0) goto fail;
        if (read_string_fixed(fd, row->cgroup, sizeof(row->cgroup)) < 0) goto fail;
        if (read_string_fixed(fd, row->profile, sizeof(row->profile)) < 0) goto fail;
        if (read_string_fixed(fd, row->rule, sizeof(row->rule)) < 0) goto fail;
        if (read_i32(fd, &count) < 0) goto fail;
        row->policy = count;
        if (read_i32(fd, &count) < 0) goto fail;
        row->rt_priority = count;
        if (read_i32(fd, &count) < 0) goto fail;
        row->nice = count;
        if (read_i32(fd, &count) < 0) goto fail;
        row->io_class = count;
        if (read_i32(fd, &count) < 0) goto fail;
        row->io_priority = count;
        if (read_i32(fd, &count) < 0) goto fail;
        row->focused = count;
        if (read_string_fixed(fd, row->status, sizeof(row->status)) < 0) goto fail;
    }

    if (read_i32(fd, &count) < 0) goto fail;
    if (count < 0) goto fail;
    if (count > 0) {
        snap->tunables = calloc((size_t)count, sizeof(*snap->tunables));
        if (!snap->tunables) goto fail;
    }
    snap->tunable_count = count;
    for (int i = 0; i < snap->tunable_count; i++) {
        LuloSchedTunableRow *row = &snap->tunables[i];

        if (read_string_fixed(fd, row->path, sizeof(row->path)) < 0) goto fail;
        if (read_string_fixed(fd, row->source, sizeof(row->source)) < 0) goto fail;
        if (read_string_fixed(fd, row->group, sizeof(row->group)) < 0) goto fail;
        if (read_string_fixed(fd, row->name, sizeof(row->name)) < 0) goto fail;
        if (read_string_fixed(fd, row->value, sizeof(row->value)) < 0) goto fail;
        if (read_i32(fd, &count) < 0) goto fail;
        row->writable = count;
    }

    if (read_i32(fd, &count) < 0) goto fail;
    if (count < 0) goto fail;
    if (count > 0) {
        snap->presets = calloc((size_t)count, sizeof(*snap->presets));
        if (!snap->presets) goto fail;
    }
    snap->preset_count = count;
    for (int i = 0; i < snap->preset_count; i++) {
        LuloSchedPresetRow *row = &snap->presets[i];

        if (read_string_fixed(fd, row->id, sizeof(row->id)) < 0) goto fail;
        if (read_string_fixed(fd, row->name, sizeof(row->name)) < 0) goto fail;
        if (read_string_fixed(fd, row->created, sizeof(row->created)) < 0) goto fail;
        if (read_string_fixed(fd, row->path, sizeof(row->path)) < 0) goto fail;
        if (read_i32(fd, &count) < 0) goto fail;
        row->item_count = count;
        if (read_i32(fd, &count) < 0) goto fail;
        row->startup = count;
    }

    if (read_string_fixed(fd, snap->detail_title, sizeof(snap->detail_title)) < 0) goto fail;
    if (read_string_fixed(fd, snap->detail_status, sizeof(snap->detail_status)) < 0) goto fail;
    if (read_u32(fd, &lines) < 0) goto fail;
    for (uint32_t i = 0; i < lines; i++) {
        char *line = NULL;

        if (read_string_dyn(fd, &line) < 0) goto fail;
        if (append_owned_line(&snap->detail_lines, &snap->detail_line_count, line) < 0) {
            free(line);
            goto fail;
        }
        free(line);
    }
    return 0;

fail:
    lulo_sched_snapshot_free(snap);
    return -1;
}

int lulod_system_socket_path(char *buf, size_t len)
{
    const char *override = getenv("LULOD_SYSTEM_SOCKET");

    if (!buf || len == 0) return -1;
    if (override && *override) {
        snprintf(buf, len, "%s", override);
        return 0;
    }
    snprintf(buf, len, "/run/lulod-system.sock");
    return 0;
}

int lulod_system_create_server_socket(const char *path)
{
    struct sockaddr_un addr;
    int fd = -1;

    if (!path) return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    unlink(path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) goto fail;
    if (listen(fd, 16) < 0) goto fail;
    if (chmod(path, 0666) < 0) goto fail;
    return fd;

fail:
    close(fd);
    return -1;
}

int lulod_system_connect_socket(const char *path)
{
    struct sockaddr_un addr;
    int fd = -1;

    if (!path) return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int lulod_system_recv_request_header(int fd, uint32_t *type)
{
    uint32_t magic = 0;
    uint32_t version = 0;

    if (read_u32(fd, &magic) < 0) return -1;
    if (read_u32(fd, &version) < 0) return -1;
    if (magic != LULOD_SYSTEM_MAGIC || version != LULOD_SYSTEM_VERSION) return -1;
    return read_u32(fd, type);
}

int lulod_system_send_sched_request(int fd, uint32_t type)
{
    if (write_u32(fd, LULOD_SYSTEM_MAGIC) < 0) return -1;
    if (write_u32(fd, LULOD_SYSTEM_VERSION) < 0) return -1;
    return write_u32(fd, type);
}

int lulod_system_send_focus_update_request(int fd, pid_t pid, unsigned long long start_time,
                                           const char *provider)
{
    if (lulod_system_send_sched_request(fd, LULOD_SYSTEM_REQ_SCHED_FOCUS_UPDATE) < 0) return -1;
    if (write_i32(fd, (int32_t)pid) < 0) return -1;
    if (write_u64(fd, (uint64_t)start_time) < 0) return -1;
    return write_string(fd, provider ? provider : "");
}

int lulod_system_recv_focus_update_request(int fd, pid_t *pid, unsigned long long *start_time,
                                           char *provider, size_t provider_len)
{
    int32_t pid_value = 0;
    uint64_t start_value = 0;

    if (!pid || !start_time) return -1;
    if (read_i32(fd, &pid_value) < 0) return -1;
    if (read_u64(fd, &start_value) < 0) return -1;
    if (read_string_fixed(fd, provider, provider_len) < 0) return -1;
    *pid = (pid_t)pid_value;
    *start_time = (unsigned long long)start_value;
    return 0;
}

int lulod_system_send_sched_apply_preset_request(int fd, const char *preset_id)
{
    if (lulod_system_send_sched_request(fd, LULOD_SYSTEM_REQ_SCHED_APPLY_PRESET) < 0) return -1;
    return write_string(fd, preset_id ? preset_id : "");
}

int lulod_system_recv_sched_apply_preset_request(int fd, char *preset_id, size_t preset_id_len)
{
    return read_string_fixed(fd, preset_id, preset_id_len);
}

int lulod_system_send_trace_begin_request(int fd, pid_t target_pid)
{
    if (write_u32(fd, LULOD_SYSTEM_MAGIC) < 0) return -1;
    if (write_u32(fd, LULOD_SYSTEM_VERSION) < 0) return -1;
    if (write_u32(fd, LULOD_SYSTEM_REQ_TRACE_BEGIN) < 0) return -1;
    return write_i32(fd, (int32_t)target_pid);
}

int lulod_system_recv_trace_begin_request(int fd, pid_t *target_pid)
{
    int32_t raw = 0;

    if (!target_pid) {
        errno = EINVAL;
        return -1;
    }
    if (read_i32(fd, &raw) < 0) return -1;
    *target_pid = (pid_t)raw;
    return 0;
}

int lulod_system_send_trace_end_request(int fd, const char *session_id)
{
    if (write_u32(fd, LULOD_SYSTEM_MAGIC) < 0) return -1;
    if (write_u32(fd, LULOD_SYSTEM_VERSION) < 0) return -1;
    if (write_u32(fd, LULOD_SYSTEM_REQ_TRACE_END) < 0) return -1;
    return write_string(fd, session_id);
}

int lulod_system_recv_trace_end_request(int fd, char *session_id, size_t session_id_len)
{
    return read_string_fixed(fd, session_id, session_id_len);
}

int lulod_system_send_trace_begin_response(int fd, int status, const char *err,
                                           const char *session_id, const char *output_path)
{
    if (write_i32(fd, (int32_t)status) < 0) return -1;
    if (write_string(fd, err ? err : "") < 0) return -1;
    if (status < 0) return 0;
    if (write_string(fd, session_id ? session_id : "") < 0) return -1;
    return write_string(fd, output_path ? output_path : "");
}

int lulod_system_recv_trace_begin_response(int fd, char *session_id, size_t session_id_len,
                                           char *output_path, size_t output_path_len,
                                           char *err, size_t errlen)
{
    int32_t status = 0;

    if (read_i32(fd, &status) < 0) return -1;
    if (read_string_fixed(fd, err, errlen) < 0) return -1;
    if (status < 0) return -1;
    if (read_string_fixed(fd, session_id, session_id_len) < 0) return -1;
    return read_string_fixed(fd, output_path, output_path_len);
}

int lulod_system_send_edit_begin_request(int fd, const char *path)
{
    if (lulod_system_send_sched_request(fd, LULOD_SYSTEM_REQ_EDIT_BEGIN) < 0) return -1;
    return write_string(fd, path ? path : "");
}

int lulod_system_recv_edit_begin_request(int fd, char *path, size_t path_len)
{
    return read_string_fixed(fd, path, path_len);
}

int lulod_system_send_edit_session_request(int fd, uint32_t type, const char *session_id)
{
    if (lulod_system_send_sched_request(fd, type) < 0) return -1;
    return write_string(fd, session_id ? session_id : "");
}

int lulod_system_recv_edit_session_request(int fd, char *session_id, size_t session_id_len)
{
    return read_string_fixed(fd, session_id, session_id_len);
}

int lulod_system_send_edit_begin_response(int fd, int status, const char *err,
                                          const char *session_id, const char *edit_path)
{
    if (write_u32(fd, LULOD_SYSTEM_MAGIC) < 0) return -1;
    if (write_u32(fd, LULOD_SYSTEM_VERSION) < 0) return -1;
    if (write_i32(fd, status) < 0) return -1;
    if (status < 0) return write_string(fd, err ? err : "edit begin failed");
    if (write_string(fd, session_id ? session_id : "") < 0) return -1;
    return write_string(fd, edit_path ? edit_path : "");
}

int lulod_system_recv_edit_begin_response(int fd, char *session_id, size_t session_id_len,
                                          char *edit_path, size_t edit_path_len,
                                          char *err, size_t errlen)
{
    uint32_t magic = 0;
    uint32_t version = 0;
    int32_t status = 0;
    char *msg = NULL;

    if (err && errlen > 0) err[0] = '\0';
    if (read_u32(fd, &magic) < 0) return -1;
    if (read_u32(fd, &version) < 0) return -1;
    if (read_i32(fd, &status) < 0) return -1;
    if (magic != LULOD_SYSTEM_MAGIC || version != LULOD_SYSTEM_VERSION) return -1;
    if (status < 0) {
        if (read_string_dyn(fd, &msg) < 0) return -1;
        if (err && errlen > 0) snprintf(err, errlen, "%s", msg ? msg : "edit begin failed");
        free(msg);
        return -1;
    }
    if (read_string_fixed(fd, session_id, session_id_len) < 0) return -1;
    if (read_string_fixed(fd, edit_path, edit_path_len) < 0) return -1;
    return 0;
}

int lulod_system_send_status_response(int fd, int status, const char *err)
{
    if (write_u32(fd, LULOD_SYSTEM_MAGIC) < 0) return -1;
    if (write_u32(fd, LULOD_SYSTEM_VERSION) < 0) return -1;
    if (write_i32(fd, status) < 0) return -1;
    if (status < 0) return write_string(fd, err ? err : "request failed");
    return 0;
}

int lulod_system_recv_status_response(int fd, char *err, size_t errlen)
{
    uint32_t magic = 0;
    uint32_t version = 0;
    int32_t status = 0;
    char *msg = NULL;

    if (err && errlen > 0) err[0] = '\0';
    if (read_u32(fd, &magic) < 0) return -1;
    if (read_u32(fd, &version) < 0) return -1;
    if (read_i32(fd, &status) < 0) return -1;
    if (magic != LULOD_SYSTEM_MAGIC || version != LULOD_SYSTEM_VERSION) return -1;
    if (status < 0) {
        if (read_string_dyn(fd, &msg) < 0) return -1;
        if (err && errlen > 0) snprintf(err, errlen, "%s", msg ? msg : "request failed");
        free(msg);
        return -1;
    }
    return 0;
}

int lulod_system_send_sched_response(int fd, int status, const char *err, const LuloSchedSnapshot *snap)
{
    if (write_u32(fd, LULOD_SYSTEM_MAGIC) < 0) return -1;
    if (write_u32(fd, LULOD_SYSTEM_VERSION) < 0) return -1;
    if (write_i32(fd, status) < 0) return -1;
    if (status < 0) return write_string(fd, err ? err : "request failed");
    return serialize_sched_snapshot(fd, snap);
}

int lulod_system_recv_sched_response(int fd, LuloSchedSnapshot *snap, char *err, size_t errlen)
{
    uint32_t magic = 0;
    uint32_t version = 0;
    int32_t status = 0;
    char *msg = NULL;

    if (err && errlen > 0) err[0] = '\0';
    if (read_u32(fd, &magic) < 0) return -1;
    if (read_u32(fd, &version) < 0) return -1;
    if (read_i32(fd, &status) < 0) return -1;
    if (magic != LULOD_SYSTEM_MAGIC || version != LULOD_SYSTEM_VERSION) return -1;
    if (status < 0) {
        if (read_string_dyn(fd, &msg) < 0) return -1;
        if (err && errlen > 0) snprintf(err, errlen, "%s", msg ? msg : "request failed");
        free(msg);
        return -1;
    }
    return deserialize_sched_snapshot(fd, snap);
}
