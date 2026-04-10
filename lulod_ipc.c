#define _GNU_SOURCE

#include "lulod_ipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define LULOD_MAGIC 0x4c554c4fU
#define LULOD_VERSION 2U

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

static int write_i32(int fd, int32_t value)
{
    return write_all(fd, &value, sizeof(value));
}

static int read_u32(int fd, uint32_t *value)
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

static int serialize_state(int fd, const LuloSystemdState *state)
{
    int32_t value = 0;

    if (!state) {
        LuloSystemdState empty = {0};
        return serialize_state(fd, &empty);
    }
    value = (int32_t)state->view;
    if (write_i32(fd, value) < 0) return -1;
    if (write_i32(fd, state->cursor) < 0) return -1;
    if (write_i32(fd, state->selected) < 0) return -1;
    if (write_i32(fd, state->list_scroll) < 0) return -1;
    if (write_i32(fd, state->file_scroll) < 0) return -1;
    if (write_i32(fd, state->focus_preview) < 0) return -1;
    if (write_i32(fd, state->config_cursor) < 0) return -1;
    if (write_i32(fd, state->config_selected) < 0) return -1;
    if (write_i32(fd, state->config_list_scroll) < 0) return -1;
    if (write_i32(fd, state->config_file_scroll) < 0) return -1;
    if (write_i32(fd, state->selected_user_scope) < 0) return -1;
    if (write_string(fd, state->selected_unit) < 0) return -1;
    if (write_string(fd, state->selected_config) < 0) return -1;
    return 0;
}

static int deserialize_state(int fd, LuloSystemdState *state)
{
    int32_t value = 0;

    if (!state) return -1;
    memset(state, 0, sizeof(*state));
    if (read_i32(fd, &value) < 0) return -1;
    state->view = (LuloSystemdView)value;
    if (read_i32(fd, &value) < 0) return -1;
    state->cursor = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->selected = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->list_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->file_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->focus_preview = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->config_cursor = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->config_selected = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->config_list_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->config_file_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->selected_user_scope = value;
    if (read_string_fixed(fd, state->selected_unit, sizeof(state->selected_unit)) < 0) return -1;
    if (read_string_fixed(fd, state->selected_config, sizeof(state->selected_config)) < 0) return -1;
    return 0;
}

static int serialize_snapshot(int fd, const LuloSystemdSnapshot *snap)
{
    uint32_t count = 0;

    if (!snap) return -1;
    if (write_i32(fd, snap->count) < 0) return -1;
    for (int i = 0; i < snap->count; i++) {
        const LuloSystemdServiceRow *row = &snap->rows[i];

        if (write_i32(fd, row->user_scope) < 0) return -1;
        if (write_string(fd, row->raw_unit) < 0) return -1;
        if (write_string(fd, row->unit) < 0) return -1;
        if (write_string(fd, row->object_path) < 0) return -1;
        if (write_string(fd, row->load) < 0) return -1;
        if (write_string(fd, row->active) < 0) return -1;
        if (write_string(fd, row->sub) < 0) return -1;
        if (write_string(fd, row->file_state) < 0) return -1;
        if (write_string(fd, row->preset) < 0) return -1;
        if (write_string(fd, row->description) < 0) return -1;
    }

    if (write_i32(fd, snap->configs_loaded) < 0) return -1;
    if (write_string(fd, snap->file_title) < 0) return -1;
    if (write_string(fd, snap->file_status) < 0) return -1;
    if (write_string(fd, snap->dep_title) < 0) return -1;
    if (write_string(fd, snap->dep_status) < 0) return -1;
    if (write_string(fd, snap->config_title) < 0) return -1;
    if (write_string(fd, snap->config_status) < 0) return -1;

    count = (uint32_t)snap->file_line_count;
    if (write_u32(fd, count) < 0) return -1;
    for (uint32_t i = 0; i < count; i++) {
        if (write_string(fd, snap->file_lines[i]) < 0) return -1;
    }
    count = (uint32_t)snap->dep_line_count;
    if (write_u32(fd, count) < 0) return -1;
    for (uint32_t i = 0; i < count; i++) {
        if (write_string(fd, snap->dep_lines[i]) < 0) return -1;
    }

    if (write_i32(fd, snap->config_count) < 0) return -1;
    for (int i = 0; i < snap->config_count; i++) {
        if (write_string(fd, snap->configs[i].path) < 0) return -1;
        if (write_string(fd, snap->configs[i].name) < 0) return -1;
    }

    count = (uint32_t)snap->config_line_count;
    if (write_u32(fd, count) < 0) return -1;
    for (uint32_t i = 0; i < count; i++) {
        if (write_string(fd, snap->config_lines[i]) < 0) return -1;
    }
    return 0;
}

static int deserialize_snapshot(int fd, LuloSystemdSnapshot *snap)
{
    int32_t count = 0;
    uint32_t lines = 0;

    if (!snap) return -1;
    memset(snap, 0, sizeof(*snap));

    if (read_i32(fd, &count) < 0) goto fail;
    if (count < 0) goto fail;
    if (count > 0) {
        snap->rows = calloc((size_t)count, sizeof(*snap->rows));
        if (!snap->rows) goto fail;
    }
    snap->count = count;
    for (int i = 0; i < snap->count; i++) {
        LuloSystemdServiceRow *row = &snap->rows[i];

        if (read_i32(fd, &count) < 0) goto fail;
        row->user_scope = count;
        if (read_string_fixed(fd, row->raw_unit, sizeof(row->raw_unit)) < 0) goto fail;
        if (read_string_fixed(fd, row->unit, sizeof(row->unit)) < 0) goto fail;
        if (read_string_fixed(fd, row->object_path, sizeof(row->object_path)) < 0) goto fail;
        if (read_string_fixed(fd, row->load, sizeof(row->load)) < 0) goto fail;
        if (read_string_fixed(fd, row->active, sizeof(row->active)) < 0) goto fail;
        if (read_string_fixed(fd, row->sub, sizeof(row->sub)) < 0) goto fail;
        if (read_string_fixed(fd, row->file_state, sizeof(row->file_state)) < 0) goto fail;
        if (read_string_fixed(fd, row->preset, sizeof(row->preset)) < 0) goto fail;
        if (read_string_fixed(fd, row->description, sizeof(row->description)) < 0) goto fail;
    }

    if (read_i32(fd, &count) < 0) goto fail;
    snap->configs_loaded = count;
    if (read_string_fixed(fd, snap->file_title, sizeof(snap->file_title)) < 0) goto fail;
    if (read_string_fixed(fd, snap->file_status, sizeof(snap->file_status)) < 0) goto fail;
    if (read_string_fixed(fd, snap->dep_title, sizeof(snap->dep_title)) < 0) goto fail;
    if (read_string_fixed(fd, snap->dep_status, sizeof(snap->dep_status)) < 0) goto fail;
    if (read_string_fixed(fd, snap->config_title, sizeof(snap->config_title)) < 0) goto fail;
    if (read_string_fixed(fd, snap->config_status, sizeof(snap->config_status)) < 0) goto fail;

    if (read_u32(fd, &lines) < 0) goto fail;
    for (uint32_t i = 0; i < lines; i++) {
        char *line = NULL;
        if (read_string_dyn(fd, &line) < 0) goto fail;
        if (append_owned_line(&snap->file_lines, &snap->file_line_count, line) < 0) {
            free(line);
            goto fail;
        }
        free(line);
    }
    if (read_u32(fd, &lines) < 0) goto fail;
    for (uint32_t i = 0; i < lines; i++) {
        char *line = NULL;
        if (read_string_dyn(fd, &line) < 0) goto fail;
        if (append_owned_line(&snap->dep_lines, &snap->dep_line_count, line) < 0) {
            free(line);
            goto fail;
        }
        free(line);
    }

    if (read_i32(fd, &count) < 0) goto fail;
    if (count < 0) goto fail;
    if (count > 0) {
        snap->configs = calloc((size_t)count, sizeof(*snap->configs));
        if (!snap->configs) goto fail;
    }
    snap->config_count = count;
    for (int i = 0; i < snap->config_count; i++) {
        if (read_string_fixed(fd, snap->configs[i].path, sizeof(snap->configs[i].path)) < 0) goto fail;
        if (read_string_fixed(fd, snap->configs[i].name, sizeof(snap->configs[i].name)) < 0) goto fail;
    }

    if (read_u32(fd, &lines) < 0) goto fail;
    for (uint32_t i = 0; i < lines; i++) {
        char *line = NULL;
        if (read_string_dyn(fd, &line) < 0) goto fail;
        if (append_owned_line(&snap->config_lines, &snap->config_line_count, line) < 0) {
            free(line);
            goto fail;
        }
        free(line);
    }
    return 0;

fail:
    lulo_systemd_snapshot_free(snap);
    return -1;
}

int lulod_socket_path(char *buf, size_t len)
{
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");

    if (!buf || len == 0) return -1;
    if (runtime_dir && *runtime_dir) {
        snprintf(buf, len, "%s/lulod.sock", runtime_dir);
    } else {
        snprintf(buf, len, "/tmp/lulod-%u.sock", (unsigned)getuid());
    }
    return 0;
}

int lulod_create_server_socket(const char *path)
{
    struct sockaddr_un addr;
    int fd = -1;

    if (!path) return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (errno == EADDRINUSE) {
            int probe = socket(AF_UNIX, SOCK_STREAM, 0);
            if (probe >= 0) {
                if (connect(probe, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                    close(probe);
                    unlink(path);
                    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) goto fail;
                } else {
                    close(probe);
                    goto fail;
                }
            } else {
                goto fail;
            }
        } else {
            goto fail;
        }
    }
    if (listen(fd, 16) < 0) goto fail;
    return fd;

fail:
    close(fd);
    return -1;
}

int lulod_connect_socket(const char *path)
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

int lulod_send_systemd_request(int fd, uint32_t type, const LuloSystemdState *state)
{
    if (write_u32(fd, LULOD_MAGIC) < 0) return -1;
    if (write_u32(fd, LULOD_VERSION) < 0) return -1;
    if (write_u32(fd, type) < 0) return -1;
    return serialize_state(fd, state);
}

int lulod_recv_systemd_request(int fd, uint32_t *type, LuloSystemdState *state)
{
    uint32_t magic = 0;
    uint32_t version = 0;

    if (read_u32(fd, &magic) < 0) return -1;
    if (read_u32(fd, &version) < 0) return -1;
    if (magic != LULOD_MAGIC || version != LULOD_VERSION) return -1;
    if (read_u32(fd, type) < 0) return -1;
    return deserialize_state(fd, state);
}

int lulod_send_systemd_response(int fd, int status, const char *err, const LuloSystemdSnapshot *snap)
{
    if (write_u32(fd, LULOD_MAGIC) < 0) return -1;
    if (write_u32(fd, LULOD_VERSION) < 0) return -1;
    if (write_i32(fd, status) < 0) return -1;
    if (status < 0) return write_string(fd, err ? err : "request failed");
    return serialize_snapshot(fd, snap);
}

int lulod_recv_systemd_response(int fd, LuloSystemdSnapshot *snap, char *err, size_t errlen)
{
    uint32_t magic = 0;
    uint32_t version = 0;
    int32_t status = 0;
    char *msg = NULL;

    if (err && errlen > 0) err[0] = '\0';
    if (read_u32(fd, &magic) < 0) return -1;
    if (read_u32(fd, &version) < 0) return -1;
    if (read_i32(fd, &status) < 0) return -1;
    if (magic != LULOD_MAGIC || version != LULOD_VERSION) return -1;
    if (status < 0) {
        if (read_string_dyn(fd, &msg) < 0) return -1;
        if (err && errlen > 0) snprintf(err, errlen, "%s", msg ? msg : "request failed");
        free(msg);
        return -1;
    }
    return deserialize_snapshot(fd, snap);
}
