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
#define LULOD_VERSION 13U

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

static int serialize_tune_state(int fd, const LuloTuneState *state)
{
    int32_t value = 0;

    if (!state) {
        LuloTuneState empty = {0};
        return serialize_tune_state(fd, &empty);
    }
    value = (int32_t)state->view;
    if (write_i32(fd, value) < 0) return -1;
    if (write_i32(fd, state->cursor) < 0) return -1;
    if (write_i32(fd, state->selected) < 0) return -1;
    if (write_i32(fd, state->list_scroll) < 0) return -1;
    if (write_i32(fd, state->detail_scroll) < 0) return -1;
    if (write_i32(fd, state->focus_preview) < 0) return -1;
    if (write_i32(fd, state->snapshot_cursor) < 0) return -1;
    if (write_i32(fd, state->snapshot_selected) < 0) return -1;
    if (write_i32(fd, state->snapshot_list_scroll) < 0) return -1;
    if (write_i32(fd, state->snapshot_detail_scroll) < 0) return -1;
    if (write_i32(fd, state->preset_cursor) < 0) return -1;
    if (write_i32(fd, state->preset_selected) < 0) return -1;
    if (write_i32(fd, state->preset_list_scroll) < 0) return -1;
    if (write_i32(fd, state->preset_detail_scroll) < 0) return -1;
    if (write_string(fd, state->browse_path) < 0) return -1;
    if (write_string(fd, state->selected_path) < 0) return -1;
    if (write_string(fd, state->selected_snapshot_id) < 0) return -1;
    if (write_string(fd, state->selected_preset_id) < 0) return -1;
    if (write_string(fd, state->staged_path) < 0) return -1;
    if (write_string(fd, state->staged_value) < 0) return -1;
    return 0;
}

static int serialize_cgroups_state(int fd, const LuloCgroupsState *state)
{
    int32_t value = 0;

    if (!state) {
        LuloCgroupsState empty = {0};
        return serialize_cgroups_state(fd, &empty);
    }
    value = (int32_t)state->view;
    if (write_i32(fd, value) < 0) return -1;
    if (write_i32(fd, state->focus_preview) < 0) return -1;
    if (write_i32(fd, state->tree_cursor) < 0) return -1;
    if (write_i32(fd, state->tree_selected) < 0) return -1;
    if (write_i32(fd, state->tree_list_scroll) < 0) return -1;
    if (write_i32(fd, state->tree_detail_scroll) < 0) return -1;
    if (write_i32(fd, state->file_cursor) < 0) return -1;
    if (write_i32(fd, state->file_selected) < 0) return -1;
    if (write_i32(fd, state->file_list_scroll) < 0) return -1;
    if (write_i32(fd, state->file_detail_scroll) < 0) return -1;
    if (write_i32(fd, state->config_cursor) < 0) return -1;
    if (write_i32(fd, state->config_selected) < 0) return -1;
    if (write_i32(fd, state->config_list_scroll) < 0) return -1;
    if (write_i32(fd, state->config_detail_scroll) < 0) return -1;
    if (write_string(fd, state->browse_path) < 0) return -1;
    if (write_string(fd, state->selected_tree_path) < 0) return -1;
    if (write_string(fd, state->selected_file_path) < 0) return -1;
    if (write_string(fd, state->selected_config) < 0) return -1;
    return 0;
}

static int serialize_udev_state(int fd, const LuloUdevState *state)
{
    int32_t value = 0;

    if (!state) {
        LuloUdevState empty = {0};
        return serialize_udev_state(fd, &empty);
    }
    value = (int32_t)state->view;
    if (write_i32(fd, value) < 0) return -1;
    if (write_i32(fd, state->focus_preview) < 0) return -1;
    if (write_i32(fd, state->rule_cursor) < 0) return -1;
    if (write_i32(fd, state->rule_selected) < 0) return -1;
    if (write_i32(fd, state->rule_list_scroll) < 0) return -1;
    if (write_i32(fd, state->rule_detail_scroll) < 0) return -1;
    if (write_i32(fd, state->hwdb_cursor) < 0) return -1;
    if (write_i32(fd, state->hwdb_selected) < 0) return -1;
    if (write_i32(fd, state->hwdb_list_scroll) < 0) return -1;
    if (write_i32(fd, state->hwdb_detail_scroll) < 0) return -1;
    if (write_i32(fd, state->device_cursor) < 0) return -1;
    if (write_i32(fd, state->device_selected) < 0) return -1;
    if (write_i32(fd, state->device_list_scroll) < 0) return -1;
    if (write_i32(fd, state->device_detail_scroll) < 0) return -1;
    if (write_string(fd, state->selected_rule) < 0) return -1;
    if (write_string(fd, state->selected_hwdb) < 0) return -1;
    if (write_string(fd, state->selected_device) < 0) return -1;
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

static int deserialize_tune_state(int fd, LuloTuneState *state)
{
    int32_t value = 0;

    if (!state) return -1;
    memset(state, 0, sizeof(*state));
    if (read_i32(fd, &value) < 0) return -1;
    state->view = (LuloTuneView)value;
    if (read_i32(fd, &value) < 0) return -1;
    state->cursor = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->selected = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->list_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->detail_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->focus_preview = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->snapshot_cursor = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->snapshot_selected = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->snapshot_list_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->snapshot_detail_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->preset_cursor = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->preset_selected = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->preset_list_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->preset_detail_scroll = value;
    if (read_string_fixed(fd, state->browse_path, sizeof(state->browse_path)) < 0) return -1;
    if (read_string_fixed(fd, state->selected_path, sizeof(state->selected_path)) < 0) return -1;
    if (read_string_fixed(fd, state->selected_snapshot_id, sizeof(state->selected_snapshot_id)) < 0) return -1;
    if (read_string_fixed(fd, state->selected_preset_id, sizeof(state->selected_preset_id)) < 0) return -1;
    if (read_string_fixed(fd, state->staged_path, sizeof(state->staged_path)) < 0) return -1;
    if (read_string_fixed(fd, state->staged_value, sizeof(state->staged_value)) < 0) return -1;
    return 0;
}

static int deserialize_cgroups_state(int fd, LuloCgroupsState *state)
{
    int32_t value = 0;

    if (!state) return -1;
    memset(state, 0, sizeof(*state));
    if (read_i32(fd, &value) < 0) return -1;
    state->view = (LuloCgroupsView)value;
    if (read_i32(fd, &value) < 0) return -1;
    state->focus_preview = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->tree_cursor = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->tree_selected = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->tree_list_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->tree_detail_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->file_cursor = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->file_selected = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->file_list_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->file_detail_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->config_cursor = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->config_selected = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->config_list_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->config_detail_scroll = value;
    if (read_string_fixed(fd, state->browse_path, sizeof(state->browse_path)) < 0) return -1;
    if (read_string_fixed(fd, state->selected_tree_path, sizeof(state->selected_tree_path)) < 0) return -1;
    if (read_string_fixed(fd, state->selected_file_path, sizeof(state->selected_file_path)) < 0) return -1;
    if (read_string_fixed(fd, state->selected_config, sizeof(state->selected_config)) < 0) return -1;
    return 0;
}

static int deserialize_udev_state(int fd, LuloUdevState *state)
{
    int32_t value = 0;

    if (!state) return -1;
    memset(state, 0, sizeof(*state));
    if (read_i32(fd, &value) < 0) return -1;
    state->view = (LuloUdevView)value;
    if (read_i32(fd, &value) < 0) return -1;
    state->focus_preview = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->rule_cursor = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->rule_selected = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->rule_list_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->rule_detail_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->hwdb_cursor = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->hwdb_selected = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->hwdb_list_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->hwdb_detail_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->device_cursor = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->device_selected = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->device_list_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->device_detail_scroll = value;
    if (read_string_fixed(fd, state->selected_rule, sizeof(state->selected_rule)) < 0) return -1;
    if (read_string_fixed(fd, state->selected_hwdb, sizeof(state->selected_hwdb)) < 0) return -1;
    if (read_string_fixed(fd, state->selected_device, sizeof(state->selected_device)) < 0) return -1;
    return 0;
}

static int serialize_sched_state(int fd, const LuloSchedState *state)
{
    int32_t value = 0;

    if (!state) {
        LuloSchedState empty = {0};
        return serialize_sched_state(fd, &empty);
    }
    value = (int32_t)state->view;
    if (write_i32(fd, value) < 0) return -1;
    if (write_i32(fd, state->focus_preview) < 0) return -1;
    if (write_i32(fd, state->profile_cursor) < 0) return -1;
    if (write_i32(fd, state->profile_selected) < 0) return -1;
    if (write_i32(fd, state->profile_list_scroll) < 0) return -1;
    if (write_i32(fd, state->profile_detail_scroll) < 0) return -1;
    if (write_i32(fd, state->rule_cursor) < 0) return -1;
    if (write_i32(fd, state->rule_selected) < 0) return -1;
    if (write_i32(fd, state->rule_list_scroll) < 0) return -1;
    if (write_i32(fd, state->rule_detail_scroll) < 0) return -1;
    if (write_i32(fd, state->live_cursor) < 0) return -1;
    if (write_i32(fd, state->live_selected) < 0) return -1;
    if (write_i32(fd, state->live_list_scroll) < 0) return -1;
    if (write_i32(fd, state->live_detail_scroll) < 0) return -1;
    if (write_i32(fd, state->tunable_cursor) < 0) return -1;
    if (write_i32(fd, state->tunable_selected) < 0) return -1;
    if (write_i32(fd, state->tunable_list_scroll) < 0) return -1;
    if (write_i32(fd, state->tunable_detail_scroll) < 0) return -1;
    if (write_i32(fd, state->preset_cursor) < 0) return -1;
    if (write_i32(fd, state->preset_selected) < 0) return -1;
    if (write_i32(fd, state->preset_list_scroll) < 0) return -1;
    if (write_i32(fd, state->preset_detail_scroll) < 0) return -1;
    if (write_string(fd, state->selected_profile) < 0) return -1;
    if (write_string(fd, state->selected_rule) < 0) return -1;
    if (write_i32(fd, state->selected_live_pid) < 0) return -1;
    if (write_all(fd, &state->selected_live_start_time, sizeof(state->selected_live_start_time)) < 0) return -1;
    if (write_string(fd, state->selected_tunable_path) < 0) return -1;
    if (write_string(fd, state->selected_preset_id) < 0) return -1;
    return 0;
}

static int deserialize_sched_state(int fd, LuloSchedState *state)
{
    int32_t value = 0;

    if (!state) return -1;
    memset(state, 0, sizeof(*state));
    state->selected_live_pid = -1;
    if (read_i32(fd, &value) < 0) return -1;
    state->view = (LuloSchedView)value;
    if (read_i32(fd, &value) < 0) return -1;
    state->focus_preview = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->profile_cursor = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->profile_selected = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->profile_list_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->profile_detail_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->rule_cursor = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->rule_selected = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->rule_list_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->rule_detail_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->live_cursor = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->live_selected = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->live_list_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->live_detail_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->tunable_cursor = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->tunable_selected = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->tunable_list_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->tunable_detail_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->preset_cursor = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->preset_selected = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->preset_list_scroll = value;
    if (read_i32(fd, &value) < 0) return -1;
    state->preset_detail_scroll = value;
    if (read_string_fixed(fd, state->selected_profile, sizeof(state->selected_profile)) < 0) return -1;
    if (read_string_fixed(fd, state->selected_rule, sizeof(state->selected_rule)) < 0) return -1;
    if (read_i32(fd, &value) < 0) return -1;
    state->selected_live_pid = value;
    if (read_all(fd, &state->selected_live_start_time, sizeof(state->selected_live_start_time)) < 0) return -1;
    if (read_string_fixed(fd, state->selected_tunable_path, sizeof(state->selected_tunable_path)) < 0) return -1;
    if (read_string_fixed(fd, state->selected_preset_id, sizeof(state->selected_preset_id)) < 0) return -1;
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
        if (write_string(fd, row->fragment_path) < 0) return -1;
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

static int serialize_tune_snapshot(int fd, const LuloTuneSnapshot *snap)
{
    uint32_t count = 0;

    if (!snap) return -1;
    if (write_i32(fd, snap->count) < 0) return -1;
    for (int i = 0; i < snap->count; i++) {
        const LuloTuneRow *row = &snap->rows[i];

        if (write_string(fd, row->path) < 0) return -1;
        if (write_string(fd, row->name) < 0) return -1;
        if (write_string(fd, row->group) < 0) return -1;
        if (write_string(fd, row->value) < 0) return -1;
        if (write_i32(fd, row->writable) < 0) return -1;
        if (write_i32(fd, row->is_dir) < 0) return -1;
        if (write_i32(fd, (int32_t)row->source) < 0) return -1;
    }

    if (write_i32(fd, snap->snapshot_count) < 0) return -1;
    for (int i = 0; i < snap->snapshot_count; i++) {
        if (write_string(fd, snap->snapshots[i].id) < 0) return -1;
        if (write_string(fd, snap->snapshots[i].name) < 0) return -1;
        if (write_string(fd, snap->snapshots[i].created) < 0) return -1;
        if (write_i32(fd, snap->snapshots[i].item_count) < 0) return -1;
    }

    if (write_i32(fd, snap->preset_count) < 0) return -1;
    for (int i = 0; i < snap->preset_count; i++) {
        if (write_string(fd, snap->presets[i].id) < 0) return -1;
        if (write_string(fd, snap->presets[i].name) < 0) return -1;
        if (write_string(fd, snap->presets[i].created) < 0) return -1;
        if (write_i32(fd, snap->presets[i].item_count) < 0) return -1;
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

static int serialize_sched_snapshot(int fd, const LuloSchedSnapshot *snap)
{
    uint32_t count = 0;

    if (!snap) return -1;
    if (write_string(fd, snap->config_root) < 0) return -1;
    if (write_i32(fd, snap->watcher_interval_ms) < 0) return -1;
    if (write_i32(fd, snap->scan_generation) < 0) return -1;
    if (write_i32(fd, snap->focus_enabled) < 0) return -1;
    if (write_i32(fd, snap->focused_pid) < 0) return -1;
    if (write_all(fd, &snap->focused_start_time, sizeof(snap->focused_start_time)) < 0) return -1;
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
        if (write_all(fd, &row->start_time, sizeof(row->start_time)) < 0) return -1;
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

static int serialize_cgroups_snapshot(int fd, const LuloCgroupsSnapshot *snap)
{
    uint32_t count = 0;

    if (!snap) return -1;
    if (write_i32(fd, snap->tree_count) < 0) return -1;
    for (int i = 0; i < snap->tree_count; i++) {
        const LuloCgroupTreeRow *row = &snap->tree_rows[i];

        if (write_string(fd, row->path) < 0) return -1;
        if (write_string(fd, row->name) < 0) return -1;
        if (write_string(fd, row->type) < 0) return -1;
        if (write_string(fd, row->controllers) < 0) return -1;
        if (write_i32(fd, row->process_count) < 0) return -1;
        if (write_i32(fd, row->thread_count) < 0) return -1;
        if (write_i32(fd, row->subgroup_count) < 0) return -1;
        if (write_i32(fd, row->is_parent) < 0) return -1;
    }

    if (write_i32(fd, snap->file_count) < 0) return -1;
    for (int i = 0; i < snap->file_count; i++) {
        const LuloCgroupFileRow *row = &snap->file_rows[i];

        if (write_string(fd, row->path) < 0) return -1;
        if (write_string(fd, row->name) < 0) return -1;
        if (write_string(fd, row->value) < 0) return -1;
        if (write_i32(fd, row->writable) < 0) return -1;
    }

    if (write_i32(fd, snap->config_count) < 0) return -1;
    for (int i = 0; i < snap->config_count; i++) {
        const LuloCgroupConfigRow *row = &snap->configs[i];

        if (write_string(fd, row->path) < 0) return -1;
        if (write_string(fd, row->name) < 0) return -1;
        if (write_string(fd, row->source) < 0) return -1;
        if (write_string(fd, row->kind) < 0) return -1;
    }

    if (write_i32(fd, snap->configs_loaded) < 0) return -1;
    if (write_string(fd, snap->browse_path) < 0) return -1;
    if (write_string(fd, snap->detail_title) < 0) return -1;
    if (write_string(fd, snap->detail_status) < 0) return -1;
    count = (uint32_t)snap->detail_line_count;
    if (write_u32(fd, count) < 0) return -1;
    for (uint32_t i = 0; i < count; i++) {
        if (write_string(fd, snap->detail_lines[i]) < 0) return -1;
    }
    return 0;
}

static int serialize_udev_snapshot(int fd, const LuloUdevSnapshot *snap)
{
    uint32_t count = 0;

    if (!snap) return -1;
    if (write_i32(fd, snap->rule_count) < 0) return -1;
    for (int i = 0; i < snap->rule_count; i++) {
        const LuloUdevConfigRow *row = &snap->rules[i];

        if (write_string(fd, row->path) < 0) return -1;
        if (write_string(fd, row->name) < 0) return -1;
        if (write_string(fd, row->source) < 0) return -1;
    }
    if (write_i32(fd, snap->hwdb_count) < 0) return -1;
    for (int i = 0; i < snap->hwdb_count; i++) {
        const LuloUdevConfigRow *row = &snap->hwdb[i];

        if (write_string(fd, row->path) < 0) return -1;
        if (write_string(fd, row->name) < 0) return -1;
        if (write_string(fd, row->source) < 0) return -1;
    }
    if (write_i32(fd, snap->device_count) < 0) return -1;
    for (int i = 0; i < snap->device_count; i++) {
        const LuloUdevDeviceRow *row = &snap->devices[i];

        if (write_string(fd, row->path) < 0) return -1;
        if (write_string(fd, row->name) < 0) return -1;
        if (write_string(fd, row->subsystem) < 0) return -1;
        if (write_string(fd, row->devnode) < 0) return -1;
        if (write_string(fd, row->devpath) < 0) return -1;
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
        if (read_string_fixed(fd, row->fragment_path, sizeof(row->fragment_path)) < 0) goto fail;
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

static int deserialize_tune_snapshot(int fd, LuloTuneSnapshot *snap)
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
        if (read_string_fixed(fd, snap->rows[i].path, sizeof(snap->rows[i].path)) < 0) goto fail;
        if (read_string_fixed(fd, snap->rows[i].name, sizeof(snap->rows[i].name)) < 0) goto fail;
        if (read_string_fixed(fd, snap->rows[i].group, sizeof(snap->rows[i].group)) < 0) goto fail;
        if (read_string_fixed(fd, snap->rows[i].value, sizeof(snap->rows[i].value)) < 0) goto fail;
        if (read_i32(fd, &count) < 0) goto fail;
        snap->rows[i].writable = count;
        if (read_i32(fd, &count) < 0) goto fail;
        snap->rows[i].is_dir = count;
        if (read_i32(fd, &count) < 0) goto fail;
        snap->rows[i].source = (LuloTuneSource)count;
    }

    if (read_i32(fd, &count) < 0) goto fail;
    if (count < 0) goto fail;
    if (count > 0) {
        snap->snapshots = calloc((size_t)count, sizeof(*snap->snapshots));
        if (!snap->snapshots) goto fail;
    }
    snap->snapshot_count = count;
    for (int i = 0; i < snap->snapshot_count; i++) {
        if (read_string_fixed(fd, snap->snapshots[i].id, sizeof(snap->snapshots[i].id)) < 0) goto fail;
        if (read_string_fixed(fd, snap->snapshots[i].name, sizeof(snap->snapshots[i].name)) < 0) goto fail;
        if (read_string_fixed(fd, snap->snapshots[i].created, sizeof(snap->snapshots[i].created)) < 0) goto fail;
        if (read_i32(fd, &count) < 0) goto fail;
        snap->snapshots[i].item_count = count;
    }

    if (read_i32(fd, &count) < 0) goto fail;
    if (count < 0) goto fail;
    if (count > 0) {
        snap->presets = calloc((size_t)count, sizeof(*snap->presets));
        if (!snap->presets) goto fail;
    }
    snap->preset_count = count;
    for (int i = 0; i < snap->preset_count; i++) {
        if (read_string_fixed(fd, snap->presets[i].id, sizeof(snap->presets[i].id)) < 0) goto fail;
        if (read_string_fixed(fd, snap->presets[i].name, sizeof(snap->presets[i].name)) < 0) goto fail;
        if (read_string_fixed(fd, snap->presets[i].created, sizeof(snap->presets[i].created)) < 0) goto fail;
        if (read_i32(fd, &count) < 0) goto fail;
        snap->presets[i].item_count = count;
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
    lulo_tune_snapshot_free(snap);
    return -1;
}

static int deserialize_sched_snapshot(int fd, LuloSchedSnapshot *snap)
{
    int32_t count = 0;
    uint32_t lines = 0;
    unsigned long long start_time = 0;

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
    if (read_all(fd, &start_time, sizeof(start_time)) < 0) goto fail;
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
        if (read_all(fd, &row->start_time, sizeof(row->start_time)) < 0) goto fail;
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

static int deserialize_cgroups_snapshot(int fd, LuloCgroupsSnapshot *snap)
{
    int32_t count = 0;
    uint32_t lines = 0;

    if (!snap) return -1;
    memset(snap, 0, sizeof(*snap));

    if (read_i32(fd, &count) < 0) goto fail;
    if (count < 0) goto fail;
    if (count > 0) {
        snap->tree_rows = calloc((size_t)count, sizeof(*snap->tree_rows));
        if (!snap->tree_rows) goto fail;
    }
    snap->tree_count = count;
    for (int i = 0; i < snap->tree_count; i++) {
        LuloCgroupTreeRow *row = &snap->tree_rows[i];

        if (read_string_fixed(fd, row->path, sizeof(row->path)) < 0) goto fail;
        if (read_string_fixed(fd, row->name, sizeof(row->name)) < 0) goto fail;
        if (read_string_fixed(fd, row->type, sizeof(row->type)) < 0) goto fail;
        if (read_string_fixed(fd, row->controllers, sizeof(row->controllers)) < 0) goto fail;
        if (read_i32(fd, &count) < 0) goto fail;
        row->process_count = count;
        if (read_i32(fd, &count) < 0) goto fail;
        row->thread_count = count;
        if (read_i32(fd, &count) < 0) goto fail;
        row->subgroup_count = count;
        if (read_i32(fd, &count) < 0) goto fail;
        row->is_parent = count;
    }

    if (read_i32(fd, &count) < 0) goto fail;
    if (count < 0) goto fail;
    if (count > 0) {
        snap->file_rows = calloc((size_t)count, sizeof(*snap->file_rows));
        if (!snap->file_rows) goto fail;
    }
    snap->file_count = count;
    for (int i = 0; i < snap->file_count; i++) {
        LuloCgroupFileRow *row = &snap->file_rows[i];

        if (read_string_fixed(fd, row->path, sizeof(row->path)) < 0) goto fail;
        if (read_string_fixed(fd, row->name, sizeof(row->name)) < 0) goto fail;
        if (read_string_fixed(fd, row->value, sizeof(row->value)) < 0) goto fail;
        if (read_i32(fd, &count) < 0) goto fail;
        row->writable = count;
    }

    if (read_i32(fd, &count) < 0) goto fail;
    if (count < 0) goto fail;
    if (count > 0) {
        snap->configs = calloc((size_t)count, sizeof(*snap->configs));
        if (!snap->configs) goto fail;
    }
    snap->config_count = count;
    for (int i = 0; i < snap->config_count; i++) {
        LuloCgroupConfigRow *row = &snap->configs[i];

        if (read_string_fixed(fd, row->path, sizeof(row->path)) < 0) goto fail;
        if (read_string_fixed(fd, row->name, sizeof(row->name)) < 0) goto fail;
        if (read_string_fixed(fd, row->source, sizeof(row->source)) < 0) goto fail;
        if (read_string_fixed(fd, row->kind, sizeof(row->kind)) < 0) goto fail;
    }

    if (read_i32(fd, &count) < 0) goto fail;
    snap->configs_loaded = count;
    if (read_string_fixed(fd, snap->browse_path, sizeof(snap->browse_path)) < 0) goto fail;
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
    lulo_cgroups_snapshot_free(snap);
    return -1;
}

static int deserialize_udev_snapshot(int fd, LuloUdevSnapshot *snap)
{
    int32_t count = 0;
    uint32_t lines = 0;

    if (!snap) return -1;
    memset(snap, 0, sizeof(*snap));

    if (read_i32(fd, &count) < 0) goto fail;
    if (count < 0) goto fail;
    if (count > 0) {
        snap->rules = calloc((size_t)count, sizeof(*snap->rules));
        if (!snap->rules) goto fail;
    }
    snap->rule_count = count;
    for (int i = 0; i < snap->rule_count; i++) {
        LuloUdevConfigRow *row = &snap->rules[i];

        if (read_string_fixed(fd, row->path, sizeof(row->path)) < 0) goto fail;
        if (read_string_fixed(fd, row->name, sizeof(row->name)) < 0) goto fail;
        if (read_string_fixed(fd, row->source, sizeof(row->source)) < 0) goto fail;
    }

    if (read_i32(fd, &count) < 0) goto fail;
    if (count < 0) goto fail;
    if (count > 0) {
        snap->hwdb = calloc((size_t)count, sizeof(*snap->hwdb));
        if (!snap->hwdb) goto fail;
    }
    snap->hwdb_count = count;
    for (int i = 0; i < snap->hwdb_count; i++) {
        LuloUdevConfigRow *row = &snap->hwdb[i];

        if (read_string_fixed(fd, row->path, sizeof(row->path)) < 0) goto fail;
        if (read_string_fixed(fd, row->name, sizeof(row->name)) < 0) goto fail;
        if (read_string_fixed(fd, row->source, sizeof(row->source)) < 0) goto fail;
    }

    if (read_i32(fd, &count) < 0) goto fail;
    if (count < 0) goto fail;
    if (count > 0) {
        snap->devices = calloc((size_t)count, sizeof(*snap->devices));
        if (!snap->devices) goto fail;
    }
    snap->device_count = count;
    for (int i = 0; i < snap->device_count; i++) {
        LuloUdevDeviceRow *row = &snap->devices[i];

        if (read_string_fixed(fd, row->path, sizeof(row->path)) < 0) goto fail;
        if (read_string_fixed(fd, row->name, sizeof(row->name)) < 0) goto fail;
        if (read_string_fixed(fd, row->subsystem, sizeof(row->subsystem)) < 0) goto fail;
        if (read_string_fixed(fd, row->devnode, sizeof(row->devnode)) < 0) goto fail;
        if (read_string_fixed(fd, row->devpath, sizeof(row->devpath)) < 0) goto fail;
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
    lulo_udev_snapshot_free(snap);
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

int lulod_recv_request_header(int fd, uint32_t *type)
{
    uint32_t magic = 0;
    uint32_t version = 0;

    if (read_u32(fd, &magic) < 0) return -1;
    if (read_u32(fd, &version) < 0) return -1;
    if (magic != LULOD_MAGIC || version != LULOD_VERSION) return -1;
    if (read_u32(fd, type) < 0) return -1;
    return 0;
}

int lulod_send_systemd_request(int fd, uint32_t type, const LuloSystemdState *state)
{
    if (write_u32(fd, LULOD_MAGIC) < 0) return -1;
    if (write_u32(fd, LULOD_VERSION) < 0) return -1;
    if (write_u32(fd, type) < 0) return -1;
    return serialize_state(fd, state);
}

int lulod_recv_systemd_state(int fd, LuloSystemdState *state)
{
    return deserialize_state(fd, state);
}

int lulod_recv_systemd_request(int fd, uint32_t *type, LuloSystemdState *state)
{
    if (lulod_recv_request_header(fd, type) < 0) return -1;
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

int lulod_send_tune_request(int fd, uint32_t type, const LuloTuneState *state)
{
    if (write_u32(fd, LULOD_MAGIC) < 0) return -1;
    if (write_u32(fd, LULOD_VERSION) < 0) return -1;
    if (write_u32(fd, type) < 0) return -1;
    return serialize_tune_state(fd, state);
}

int lulod_recv_tune_state(int fd, LuloTuneState *state)
{
    return deserialize_tune_state(fd, state);
}

int lulod_recv_tune_request(int fd, uint32_t *type, LuloTuneState *state)
{
    if (lulod_recv_request_header(fd, type) < 0) return -1;
    return deserialize_tune_state(fd, state);
}

int lulod_send_tune_response(int fd, int status, const char *err, const LuloTuneSnapshot *snap)
{
    if (write_u32(fd, LULOD_MAGIC) < 0) return -1;
    if (write_u32(fd, LULOD_VERSION) < 0) return -1;
    if (write_i32(fd, status) < 0) return -1;
    if (status < 0) return write_string(fd, err ? err : "request failed");
    return serialize_tune_snapshot(fd, snap);
}

int lulod_recv_tune_response(int fd, LuloTuneSnapshot *snap, char *err, size_t errlen)
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
    return deserialize_tune_snapshot(fd, snap);
}

int lulod_send_sched_request(int fd, uint32_t type, const LuloSchedState *state)
{
    if (write_u32(fd, LULOD_MAGIC) < 0) return -1;
    if (write_u32(fd, LULOD_VERSION) < 0) return -1;
    if (write_u32(fd, type) < 0) return -1;
    return serialize_sched_state(fd, state);
}

int lulod_recv_sched_state(int fd, LuloSchedState *state)
{
    return deserialize_sched_state(fd, state);
}

int lulod_recv_sched_request(int fd, uint32_t *type, LuloSchedState *state)
{
    if (lulod_recv_request_header(fd, type) < 0) return -1;
    return deserialize_sched_state(fd, state);
}

int lulod_send_sched_response(int fd, int status, const char *err, const LuloSchedSnapshot *snap)
{
    if (write_u32(fd, LULOD_MAGIC) < 0) return -1;
    if (write_u32(fd, LULOD_VERSION) < 0) return -1;
    if (write_i32(fd, status) < 0) return -1;
    if (status < 0) return write_string(fd, err ? err : "request failed");
    return serialize_sched_snapshot(fd, snap);
}

int lulod_recv_sched_response(int fd, LuloSchedSnapshot *snap, char *err, size_t errlen)
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
    return deserialize_sched_snapshot(fd, snap);
}

int lulod_send_cgroups_request(int fd, uint32_t type, const LuloCgroupsState *state)
{
    if (write_u32(fd, LULOD_MAGIC) < 0) return -1;
    if (write_u32(fd, LULOD_VERSION) < 0) return -1;
    if (write_u32(fd, type) < 0) return -1;
    return serialize_cgroups_state(fd, state);
}

int lulod_recv_cgroups_state(int fd, LuloCgroupsState *state)
{
    return deserialize_cgroups_state(fd, state);
}

int lulod_recv_cgroups_request(int fd, uint32_t *type, LuloCgroupsState *state)
{
    if (lulod_recv_request_header(fd, type) < 0) return -1;
    return deserialize_cgroups_state(fd, state);
}

int lulod_send_cgroups_response(int fd, int status, const char *err, const LuloCgroupsSnapshot *snap)
{
    if (write_u32(fd, LULOD_MAGIC) < 0) return -1;
    if (write_u32(fd, LULOD_VERSION) < 0) return -1;
    if (write_i32(fd, status) < 0) return -1;
    if (status < 0) return write_string(fd, err ? err : "request failed");
    return serialize_cgroups_snapshot(fd, snap);
}

int lulod_recv_cgroups_response(int fd, LuloCgroupsSnapshot *snap, char *err, size_t errlen)
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
    return deserialize_cgroups_snapshot(fd, snap);
}

int lulod_send_udev_request(int fd, uint32_t type, const LuloUdevState *state)
{
    if (write_u32(fd, LULOD_MAGIC) < 0) return -1;
    if (write_u32(fd, LULOD_VERSION) < 0) return -1;
    if (write_u32(fd, type) < 0) return -1;
    return serialize_udev_state(fd, state);
}

int lulod_recv_udev_state(int fd, LuloUdevState *state)
{
    return deserialize_udev_state(fd, state);
}

int lulod_recv_udev_request(int fd, uint32_t *type, LuloUdevState *state)
{
    if (lulod_recv_request_header(fd, type) < 0) return -1;
    return deserialize_udev_state(fd, state);
}

int lulod_send_udev_response(int fd, int status, const char *err, const LuloUdevSnapshot *snap)
{
    if (write_u32(fd, LULOD_MAGIC) < 0) return -1;
    if (write_u32(fd, LULOD_VERSION) < 0) return -1;
    if (write_i32(fd, status) < 0) return -1;
    if (status < 0) return write_string(fd, err ? err : "request failed");
    return serialize_udev_snapshot(fd, snap);
}

int lulod_recv_udev_response(int fd, LuloUdevSnapshot *snap, char *err, size_t errlen)
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
    return deserialize_udev_snapshot(fd, snap);
}
