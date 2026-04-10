#define _GNU_SOURCE

#include "lulod_systemd.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#define SYSTEMD_DEST "org.freedesktop.systemd1"
#define SYSTEMD_PATH "/org/freedesktop/systemd1"
#define SYSTEMD_MANAGER_IFACE "org.freedesktop.systemd1.Manager"
#define SYSTEMD_UNIT_IFACE "org.freedesktop.systemd1.Unit"

static int append_snapshot_line(char ***lines, int *count, const char *text)
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

static void clear_lines(char ***lines, int *count)
{
    if (!lines || !count) return;
    for (int i = 0; i < *count; i++) free((*lines)[i]);
    free(*lines);
    *lines = NULL;
    *count = 0;
}

static void trim_right(char *buf)
{
    size_t len;

    if (!buf) return;
    len = strlen(buf);
    while (len > 0 && isspace((unsigned char)buf[len - 1])) {
        buf[--len] = '\0';
    }
}

static void decode_unit_name(const char *src, char *dst, size_t len)
{
    size_t out = 0;

    if (len == 0) return;
    while (src && *src && out + 1 < len) {
        if (src[0] == '\\' && src[1] == 'x' &&
            isxdigit((unsigned char)src[2]) && isxdigit((unsigned char)src[3])) {
            char hex[3] = { src[2], src[3], '\0' };
            int ch = (int)strtol(hex, NULL, 16);
            dst[out++] = isprint(ch) ? (char)ch : '?';
            src += 4;
            continue;
        }
        dst[out++] = *src++;
    }
    dst[out] = '\0';
}

static int append_service_row(LuloSystemdSnapshot *snap, int user_scope, const char *raw_unit)
{
    LuloSystemdServiceRow *rows;
    LuloSystemdServiceRow *row;

    rows = realloc(snap->rows, (size_t)(snap->count + 1) * sizeof(*rows));
    if (!rows) return -1;
    snap->rows = rows;
    row = &snap->rows[snap->count++];
    memset(row, 0, sizeof(*row));
    row->user_scope = user_scope;
    snprintf(row->raw_unit, sizeof(row->raw_unit), "%s", raw_unit ? raw_unit : "");
    decode_unit_name(row->raw_unit, row->unit, sizeof(row->unit));
    snprintf(row->load, sizeof(row->load), "-");
    snprintf(row->active, sizeof(row->active), "-");
    snprintf(row->sub, sizeof(row->sub), "-");
    snprintf(row->file_state, sizeof(row->file_state), "-");
    snprintf(row->preset, sizeof(row->preset), "-");
    return snap->count - 1;
}

static int append_config_row(LuloSystemdSnapshot *snap, const char *path, const char *name)
{
    LuloSystemdConfigRow *rows;
    LuloSystemdConfigRow *row;

    rows = realloc(snap->configs, (size_t)(snap->config_count + 1) * sizeof(*rows));
    if (!rows) return -1;
    snap->configs = rows;
    row = &snap->configs[snap->config_count++];
    memset(row, 0, sizeof(*row));
    snprintf(row->path, sizeof(row->path), "%s", path ? path : "");
    snprintf(row->name, sizeof(row->name), "%s", name ? name : row->path);
    return snap->config_count - 1;
}

static int find_service_row(const LuloSystemdSnapshot *snap, int user_scope, const char *raw_unit)
{
    for (int i = 0; i < snap->count; i++) {
        if (snap->rows[i].user_scope == user_scope &&
            strcmp(snap->rows[i].raw_unit, raw_unit) == 0) return i;
    }
    return -1;
}

static int service_rank(const LuloSystemdServiceRow *row)
{
    if (!strcmp(row->active, "failed")) return 0;
    if (!strcmp(row->active, "active") && !strcmp(row->sub, "running")) return 1;
    if (!strcmp(row->active, "activating")) return 2;
    if (!strcmp(row->active, "reloading")) return 3;
    if (!strcmp(row->active, "active")) return 4;
    if (!strcmp(row->active, "inactive")) return 5;
    return 6;
}

static int service_cmp(const void *a, const void *b)
{
    const LuloSystemdServiceRow *sa = a;
    const LuloSystemdServiceRow *sb = b;
    int ra = service_rank(sa);
    int rb = service_rank(sb);

    if (ra != rb) return ra - rb;
    if (sa->user_scope != sb->user_scope) return sa->user_scope - sb->user_scope;
    return strcmp(sa->unit, sb->unit);
}

static int config_cmp(const void *a, const void *b)
{
    const LuloSystemdConfigRow *ca = a;
    const LuloSystemdConfigRow *cb = b;

    return strcmp(ca->path, cb->path);
}

static int string_cmp(const void *a, const void *b)
{
    const char *const *sa = a;
    const char *const *sb = b;

    return strcmp(*sa, *sb);
}

static int is_config_name(const char *name)
{
    size_t len;

    if (!name) return 0;
    len = strlen(name);
    if (len >= 5 && strcmp(name + len - 5, ".conf") == 0) return 1;
    if (len >= 12 && strcmp(name + len - 12, ".conf.pacnew") == 0) return 1;
    return 0;
}

static int scan_config_dir(LuloSystemdSnapshot *snap, const char *root, const char *path, int depth)
{
    DIR *dir;
    struct dirent *ent;

    if (depth > 8) return 0;
    dir = opendir(path);
    if (!dir) return 0;

    while ((ent = readdir(dir)) != NULL) {
        char full[512];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        if (lstat(full, &st) < 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (scan_config_dir(snap, root, full, depth + 1) < 0) {
                closedir(dir);
                return -1;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode) || !is_config_name(ent->d_name)) continue;
        if (strncmp(full, root, strlen(root)) == 0 && full[strlen(root)] == '/') {
            if (append_config_row(snap, full, full + strlen(root) + 1) < 0) {
                closedir(dir);
                return -1;
            }
        } else if (append_config_row(snap, full, ent->d_name) < 0) {
            closedir(dir);
            return -1;
        }
    }

    closedir(dir);
    return 0;
}

static int ensure_configs_loaded(LuloSystemdSnapshot *snap)
{
    if (!snap) return -1;
    if (snap->configs_loaded) return 0;
    if (scan_config_dir(snap, "/etc/systemd", "/etc/systemd", 0) < 0) return -1;
    if (snap->config_count > 1) {
        qsort(snap->configs, (size_t)snap->config_count, sizeof(*snap->configs), config_cmp);
    }
    snap->configs_loaded = 1;
    return 0;
}

static int service_selection_active(const LuloSystemdState *state)
{
    return state && state->selected >= 0 && state->selected_unit[0];
}

static int config_selection_active(const LuloSystemdState *state)
{
    return state && state->config_selected >= 0 && state->selected_config[0];
}

static int selected_service_index(const LuloSystemdSnapshot *snap, const LuloSystemdState *state)
{
    if (!snap || !state || snap->count <= 0) return -1;
    if (state->selected_unit[0]) {
        for (int i = 0; i < snap->count; i++) {
            if (snap->rows[i].user_scope == state->selected_user_scope &&
                strcmp(snap->rows[i].raw_unit, state->selected_unit) == 0) {
                return i;
            }
        }
    }
    if (state->selected >= 0 && state->selected < snap->count) return state->selected;
    return -1;
}

static int selected_config_index(const LuloSystemdSnapshot *snap, const LuloSystemdState *state)
{
    if (!snap || !state || snap->config_count <= 0) return -1;
    if (state->selected_config[0]) {
        for (int i = 0; i < snap->config_count; i++) {
            if (strcmp(snap->configs[i].path, state->selected_config) == 0) return i;
        }
    }
    if (state->config_selected >= 0 && state->config_selected < snap->config_count) return state->config_selected;
    return -1;
}

static void free_strv_local(char **items)
{
    if (!items) return;
    for (size_t i = 0; items[i]; i++) free(items[i]);
    free(items);
}

static void set_service_preview_idle(LuloSystemdSnapshot *snap, const char *status)
{
    clear_lines(&snap->file_lines, &snap->file_line_count);
    snprintf(snap->file_title, sizeof(snap->file_title), "unit file");
    snprintf(snap->file_status, sizeof(snap->file_status), "%s", status ? status : "select a service");
}

static void set_dep_preview_idle(LuloSystemdSnapshot *snap, const char *status)
{
    clear_lines(&snap->dep_lines, &snap->dep_line_count);
    snprintf(snap->dep_title, sizeof(snap->dep_title), "reverse deps");
    snprintf(snap->dep_status, sizeof(snap->dep_status), "%s", status ? status : "select a service");
}

static void set_config_preview_idle(LuloSystemdSnapshot *snap, const char *status)
{
    clear_lines(&snap->config_lines, &snap->config_line_count);
    snprintf(snap->config_title, sizeof(snap->config_title), "config");
    snprintf(snap->config_status, sizeof(snap->config_status), "%s", status ? status : "select a config");
}

static int append_file_section(char ***lines, int *count, const char *path)
{
    FILE *fp;
    char *line = NULL;
    size_t linecap = 0;
    int start_count;

    if (!path || !*path) return 0;
    if (*count > 0 && append_snapshot_line(lines, count, "") < 0) return -1;
    {
        char header[512];
        snprintf(header, sizeof(header), "# %s", path);
        if (append_snapshot_line(lines, count, header) < 0) return -1;
    }

    start_count = *count;
    fp = fopen(path, "r");
    if (!fp) {
        char msg[512];
        snprintf(msg, sizeof(msg), "failed to open %s: %s", path, strerror(errno));
        return append_snapshot_line(lines, count, msg);
    }

    while (getline(&line, &linecap, fp) > 0) {
        trim_right(line);
        if (append_snapshot_line(lines, count, line) < 0) {
            free(line);
            fclose(fp);
            return -1;
        }
    }
    free(line);
    fclose(fp);
    if (*count == start_count && append_snapshot_line(lines, count, "(empty file)") < 0) return -1;
    return 0;
}

static int open_bus_for_scope(int user_scope, sd_bus **bus)
{
    int rc;

    *bus = NULL;
    rc = user_scope ? sd_bus_open_user(bus) : sd_bus_open_system(bus);
    if (rc < 0) return -1;
    return 0;
}

static int manager_call_with_service_pattern(sd_bus *bus, const char *member, sd_bus_message **reply)
{
    sd_bus_message *m = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int rc;

    *reply = NULL;
    rc = sd_bus_message_new_method_call(bus, &m, SYSTEMD_DEST, SYSTEMD_PATH, SYSTEMD_MANAGER_IFACE, member);
    if (rc < 0) goto fail;
    rc = sd_bus_message_open_container(m, 'a', "s");
    if (rc < 0) goto fail;
    rc = sd_bus_message_close_container(m);
    if (rc < 0) goto fail;
    rc = sd_bus_message_open_container(m, 'a', "s");
    if (rc < 0) goto fail;
    rc = sd_bus_message_append(m, "s", "*.service");
    if (rc < 0) goto fail;
    rc = sd_bus_message_close_container(m);
    if (rc < 0) goto fail;
    rc = sd_bus_call(bus, m, 0, &error, reply);
    if (rc < 0) goto fail;
    sd_bus_error_free(&error);
    sd_bus_message_unref(m);
    return 0;

fail:
    sd_bus_error_free(&error);
    sd_bus_message_unref(m);
    sd_bus_message_unref(*reply);
    *reply = NULL;
    return -1;
}

static int gather_unit_files(sd_bus *bus, int user_scope, LuloSystemdSnapshot *snap)
{
    sd_bus_message *reply = NULL;
    int rc;

    rc = manager_call_with_service_pattern(bus, "ListUnitFilesByPatterns", &reply);
    if (rc < 0) return -1;

    rc = sd_bus_message_enter_container(reply, 'a', "(ss)");
    if (rc < 0) goto fail;
    while ((rc = sd_bus_message_enter_container(reply, 'r', "ss")) > 0) {
        const char *raw = NULL;
        const char *state = NULL;
        int idx;

        if (sd_bus_message_read(reply, "ss", &raw, &state) < 0) goto fail;
        if (!raw || !strstr(raw, ".service")) {
            if (sd_bus_message_exit_container(reply) < 0) goto fail;
            continue;
        }
        idx = find_service_row(snap, user_scope, raw);
        if (idx < 0) {
            idx = append_service_row(snap, user_scope, raw);
            if (idx < 0) goto fail;
        }
        snprintf(snap->rows[idx].file_state, sizeof(snap->rows[idx].file_state), "%s", state ? state : "-");
        if (sd_bus_message_exit_container(reply) < 0) goto fail;
    }
    if (rc < 0) goto fail;
    if (sd_bus_message_exit_container(reply) < 0) goto fail;
    sd_bus_message_unref(reply);
    return 0;

fail:
    sd_bus_message_unref(reply);
    return -1;
}

static int gather_runtime_units(sd_bus *bus, int user_scope, LuloSystemdSnapshot *snap)
{
    sd_bus_message *reply = NULL;
    int rc;

    rc = manager_call_with_service_pattern(bus, "ListUnitsByPatterns", &reply);
    if (rc < 0) return -1;

    rc = sd_bus_message_enter_container(reply, 'a', "(ssssssouso)");
    if (rc < 0) goto fail;
    while ((rc = sd_bus_message_enter_container(reply, 'r', "ssssssouso")) > 0) {
        const char *raw = NULL;
        const char *desc = NULL;
        const char *load = NULL;
        const char *active = NULL;
        const char *sub = NULL;
        const char *following = NULL;
        const char *path = NULL;
        const char *job_type = NULL;
        const char *job_path = NULL;
        uint32_t job_id = 0;
        int idx;

        if (sd_bus_message_read(reply, "ssssssouso",
                                &raw, &desc, &load, &active, &sub,
                                &following, &path, &job_id, &job_type, &job_path) < 0) {
            goto fail;
        }
        (void)following;
        (void)job_id;
        (void)job_type;
        (void)job_path;
        if (!raw || !strstr(raw, ".service")) {
            if (sd_bus_message_exit_container(reply) < 0) goto fail;
            continue;
        }
        idx = find_service_row(snap, user_scope, raw);
        if (idx < 0) {
            idx = append_service_row(snap, user_scope, raw);
            if (idx < 0) goto fail;
        }
        snprintf(snap->rows[idx].load, sizeof(snap->rows[idx].load), "%s", load ? load : "-");
        snprintf(snap->rows[idx].active, sizeof(snap->rows[idx].active), "%s", active ? active : "-");
        snprintf(snap->rows[idx].sub, sizeof(snap->rows[idx].sub), "%s", sub ? sub : "-");
        snprintf(snap->rows[idx].description, sizeof(snap->rows[idx].description), "%s", desc ? desc : "");
        snprintf(snap->rows[idx].object_path, sizeof(snap->rows[idx].object_path), "%s", path ? path : "");
        if (sd_bus_message_exit_container(reply) < 0) goto fail;
    }
    if (rc < 0) goto fail;
    if (sd_bus_message_exit_container(reply) < 0) goto fail;
    sd_bus_message_unref(reply);
    return 0;

fail:
    sd_bus_message_unref(reply);
    return -1;
}

static int gather_scope(int user_scope, LuloSystemdSnapshot *snap)
{
    sd_bus *bus = NULL;
    int rc = 0;

    if (open_bus_for_scope(user_scope, &bus) < 0) return 0;
    if (gather_unit_files(bus, user_scope, snap) < 0) rc = -1;
    else if (gather_runtime_units(bus, user_scope, snap) < 0) rc = -1;
    sd_bus_flush_close_unref(bus);
    return rc;
}

static void init_preview_placeholders(LuloSystemdSnapshot *snap, const LuloSystemdState *state)
{
    if (!snap || !state) return;
    switch (state->view) {
    case LULO_SYSTEMD_VIEW_SERVICES:
        snprintf(snap->file_title, sizeof(snap->file_title), "unit file");
        snprintf(snap->file_status, sizeof(snap->file_status), "select a service");
        break;
    case LULO_SYSTEMD_VIEW_DEPS:
        snprintf(snap->dep_title, sizeof(snap->dep_title), "reverse deps");
        snprintf(snap->dep_status, sizeof(snap->dep_status), "select a service");
        break;
    case LULO_SYSTEMD_VIEW_CONFIG:
    default:
        snprintf(snap->config_title, sizeof(snap->config_title), "config");
        snprintf(snap->config_status, sizeof(snap->config_status), "select a config");
        break;
    }
}

static int get_unit_object_path(sd_bus *bus, const LuloSystemdServiceRow *row, char *out, size_t len)
{
    sd_bus_message *reply = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    const char *path = NULL;
    int rc;

    if (!bus || !row || !out || len == 0) return -1;
    if (row->object_path[0]) {
        snprintf(out, len, "%s", row->object_path);
        return 0;
    }

    rc = sd_bus_call_method(bus, SYSTEMD_DEST, SYSTEMD_PATH, SYSTEMD_MANAGER_IFACE,
                            "LoadUnit", &error, &reply, "s", row->raw_unit);
    if (rc < 0) goto fail;
    if (sd_bus_message_read(reply, "o", &path) < 0 || !path || !*path) goto fail;
    snprintf(out, len, "%s", path);
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    return 0;

fail:
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    return -1;
}

static int get_unit_property_string(sd_bus *bus, const char *object_path, const char *member, char **out)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int rc;

    *out = NULL;
    rc = sd_bus_get_property_string(bus, SYSTEMD_DEST, object_path, SYSTEMD_UNIT_IFACE, member, &error, out);
    sd_bus_error_free(&error);
    if (rc < 0) {
        *out = strdup("");
        if (!*out) return -1;
    }
    return 0;
}

static int get_unit_property_strv(sd_bus *bus, const char *object_path, const char *member, char ***out)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int rc;

    *out = NULL;
    rc = sd_bus_get_property_strv(bus, SYSTEMD_DEST, object_path, SYSTEMD_UNIT_IFACE, member, &error, out);
    sd_bus_error_free(&error);
    if (rc < 0) {
        *out = calloc(1, sizeof(char *));
        if (!*out) return -1;
    }
    return 0;
}

static int append_dependency_section(char ***lines, int *count, const char *title, char **items)
{
    size_t n = 0;

    if (!items || !items[0]) return 0;
    while (items[n]) n++;
    if (n > 1) qsort(items, n, sizeof(*items), string_cmp);
    if (*count > 0 && append_snapshot_line(lines, count, "") < 0) return -1;
    {
        char header[256];
        snprintf(header, sizeof(header), "# %s", title);
        if (append_snapshot_line(lines, count, header) < 0) return -1;
    }
    for (size_t i = 0; i < n; i++) {
        if (append_snapshot_line(lines, count, items[i]) < 0) return -1;
    }
    return 0;
}

static int load_selected_unit_file(LuloSystemdSnapshot *snap, const LuloSystemdState *state)
{
    const LuloSystemdServiceRow *row;
    sd_bus *bus = NULL;
    char object_path[320] = {0};
    char *fragment = NULL;
    char *source = NULL;
    char *following = NULL;
    char **dropins = NULL;
    int idx;
    int rc = 0;
    int rendered_files = 0;

    set_service_preview_idle(snap, "select a service");
    if (!snap || !state || !service_selection_active(state)) return 0;

    idx = selected_service_index(snap, state);
    if (idx < 0) {
        if (append_snapshot_line(&snap->file_lines, &snap->file_line_count, "selected service is no longer available") < 0) {
            return -1;
        }
        snprintf(snap->file_status, sizeof(snap->file_status), "selection unavailable");
        return 0;
    }

    row = &snap->rows[idx];
    snprintf(snap->file_title, sizeof(snap->file_title), "%s unit file", row->user_scope ? "user" : "system");
    snprintf(snap->file_status, sizeof(snap->file_status), "%.20s  %.20s/%.20s  %.80s",
             row->file_state, row->active, row->sub, row->unit);

    if (open_bus_for_scope(row->user_scope, &bus) < 0) {
        if (append_snapshot_line(&snap->file_lines, &snap->file_line_count, "failed to open D-Bus connection") < 0) {
            return -1;
        }
        snprintf(snap->file_status, sizeof(snap->file_status), "D-Bus unavailable");
        return 0;
    }
    if (get_unit_object_path(bus, row, object_path, sizeof(object_path)) < 0) {
        rc = append_snapshot_line(&snap->file_lines, &snap->file_line_count, "failed to resolve unit object path");
        if (rc == 0) snprintf(snap->file_status, sizeof(snap->file_status), "unit lookup failed");
        sd_bus_flush_close_unref(bus);
        return rc;
    }
    if (get_unit_property_string(bus, object_path, "FragmentPath", &fragment) < 0 ||
        get_unit_property_string(bus, object_path, "SourcePath", &source) < 0 ||
        get_unit_property_string(bus, object_path, "Following", &following) < 0 ||
        get_unit_property_strv(bus, object_path, "DropInPaths", &dropins) < 0) {
        sd_bus_flush_close_unref(bus);
        free(fragment);
        free(source);
        free(following);
        free_strv_local(dropins);
        return -1;
    }
    sd_bus_flush_close_unref(bus);

    {
        char meta[512];
        snprintf(meta, sizeof(meta), "# unit: %s (%s)", row->unit, row->user_scope ? "user" : "system");
        if (append_snapshot_line(&snap->file_lines, &snap->file_line_count, meta) < 0) goto fail;
        snprintf(meta, sizeof(meta), "# state: %s/%s  file: %s  preset: %s",
                 row->active, row->sub, row->file_state, row->preset);
        if (append_snapshot_line(&snap->file_lines, &snap->file_line_count, meta) < 0) goto fail;
        if (following && following[0]) {
            snprintf(meta, sizeof(meta), "# following: %s", following);
            if (append_snapshot_line(&snap->file_lines, &snap->file_line_count, meta) < 0) goto fail;
        }
        if (source && source[0]) {
            snprintf(meta, sizeof(meta), "# source: %s", source);
            if (append_snapshot_line(&snap->file_lines, &snap->file_line_count, meta) < 0) goto fail;
        }
    }

    if (fragment && fragment[0]) {
        if (append_file_section(&snap->file_lines, &snap->file_line_count, fragment) < 0) goto fail;
        rendered_files = 1;
    }
    if (dropins) {
        for (size_t i = 0; dropins[i]; i++) {
            if (append_file_section(&snap->file_lines, &snap->file_line_count, dropins[i]) < 0) goto fail;
            rendered_files = 1;
        }
    }
    if (!rendered_files) {
        if (append_snapshot_line(&snap->file_lines, &snap->file_line_count, "") < 0) goto fail;
        if (append_snapshot_line(&snap->file_lines, &snap->file_line_count,
                                 "no fragment or drop-in files are available for this unit") < 0) {
            goto fail;
        }
    }

    free(fragment);
    free(source);
    free(following);
    free_strv_local(dropins);
    return 0;

fail:
    free(fragment);
    free(source);
    free(following);
    free_strv_local(dropins);
    return -1;
}

static int load_selected_dependencies(LuloSystemdSnapshot *snap, const LuloSystemdState *state)
{
    static const struct {
        const char *property;
        const char *title;
    } sections[] = {
        { "RequiredBy", "RequiredBy" },
        { "RequisiteOf", "RequisiteOf" },
        { "WantedBy", "WantedBy" },
        { "BoundBy", "BoundBy" },
        { "UpheldBy", "UpheldBy" },
        { "ConsistsOf", "ConsistsOf" },
        { "TriggeredBy", "TriggeredBy" },
        { "OnSuccessOf", "OnSuccessOf" },
        { "OnFailureOf", "OnFailureOf" },
        { "ReloadPropagatedFrom", "ReloadPropagatedFrom" },
        { "StopPropagatedFrom", "StopPropagatedFrom" },
        { "ConflictedBy", "ConflictedBy" },
    };
    const LuloSystemdServiceRow *row;
    sd_bus *bus = NULL;
    char object_path[320] = {0};
    int idx;
    int rc = 0;

    set_dep_preview_idle(snap, "select a service");
    if (!snap || !state || !service_selection_active(state)) return 0;

    idx = selected_service_index(snap, state);
    if (idx < 0) {
        if (append_snapshot_line(&snap->dep_lines, &snap->dep_line_count, "selected service is no longer available") < 0) {
            return -1;
        }
        snprintf(snap->dep_status, sizeof(snap->dep_status), "selection unavailable");
        return 0;
    }

    row = &snap->rows[idx];
    snprintf(snap->dep_title, sizeof(snap->dep_title), "reverse deps");
    snprintf(snap->dep_status, sizeof(snap->dep_status), "%.8s  %.20s/%.20s  %.80s",
             row->user_scope ? "user" : "system", row->active, row->sub, row->unit);

    if (open_bus_for_scope(row->user_scope, &bus) < 0) {
        if (append_snapshot_line(&snap->dep_lines, &snap->dep_line_count, "failed to open D-Bus connection") < 0) {
            return -1;
        }
        snprintf(snap->dep_status, sizeof(snap->dep_status), "D-Bus unavailable");
        return 0;
    }
    if (get_unit_object_path(bus, row, object_path, sizeof(object_path)) < 0) {
        rc = append_snapshot_line(&snap->dep_lines, &snap->dep_line_count, "failed to resolve unit object path");
        if (rc == 0) snprintf(snap->dep_status, sizeof(snap->dep_status), "unit lookup failed");
        sd_bus_flush_close_unref(bus);
        return rc;
    }

    for (size_t i = 0; i < sizeof(sections) / sizeof(sections[0]); i++) {
        char **items = NULL;

        if (get_unit_property_strv(bus, object_path, sections[i].property, &items) < 0) {
            sd_bus_flush_close_unref(bus);
            return -1;
        }
        if (append_dependency_section(&snap->dep_lines, &snap->dep_line_count, sections[i].title, items) < 0) {
            free_strv_local(items);
            sd_bus_flush_close_unref(bus);
            return -1;
        }
        free_strv_local(items);
    }
    sd_bus_flush_close_unref(bus);

    if (snap->dep_line_count == 0 &&
        append_snapshot_line(&snap->dep_lines, &snap->dep_line_count, "no reverse dependencies") < 0) {
        return -1;
    }
    return 0;
}

static int load_selected_config(LuloSystemdSnapshot *snap, const LuloSystemdState *state)
{
    const LuloSystemdConfigRow *row;
    FILE *fp;
    char *line = NULL;
    size_t linecap = 0;
    int idx;

    set_config_preview_idle(snap, "select a config");
    if (!snap || !state) return 0;
    if (ensure_configs_loaded(snap) < 0) return -1;
    if (!config_selection_active(state)) return 0;

    idx = selected_config_index(snap, state);
    if (idx < 0) {
        if (append_snapshot_line(&snap->config_lines, &snap->config_line_count, "selected config is no longer available") < 0) {
            return -1;
        }
        snprintf(snap->config_status, sizeof(snap->config_status), "selection unavailable");
        return 0;
    }

    row = &snap->configs[idx];
    snprintf(snap->config_title, sizeof(snap->config_title), "%s", row->name);
    snprintf(snap->config_status, sizeof(snap->config_status), "%.156s", row->path);

    fp = fopen(row->path, "r");
    if (!fp) {
        char msg[512];
        snprintf(msg, sizeof(msg), "failed to open config: %s", strerror(errno));
        return append_snapshot_line(&snap->config_lines, &snap->config_line_count, msg);
    }

    while (getline(&line, &linecap, fp) > 0) {
        trim_right(line);
        if (append_snapshot_line(&snap->config_lines, &snap->config_line_count, line) < 0) {
            free(line);
            fclose(fp);
            return -1;
        }
    }
    free(line);
    fclose(fp);
    if (snap->config_line_count == 0 &&
        append_snapshot_line(&snap->config_lines, &snap->config_line_count, "(empty file)") < 0) {
        return -1;
    }
    return 0;
}

int lulod_systemd_snapshot_gather(LuloSystemdSnapshot *snap, const LuloSystemdState *state)
{
    memset(snap, 0, sizeof(*snap));
    if (gather_scope(0, snap) < 0) goto fail;
    if (gather_scope(1, snap) < 0) goto fail;
    if (snap->count > 1) {
        qsort(snap->rows, (size_t)snap->count, sizeof(*snap->rows), service_cmp);
    }
    if (state && state->view == LULO_SYSTEMD_VIEW_CONFIG && ensure_configs_loaded(snap) < 0) goto fail;
    if (state) init_preview_placeholders(snap, state);
    return 0;

fail:
    lulo_systemd_snapshot_free(snap);
    return -1;
}

int lulod_systemd_snapshot_refresh_active(LuloSystemdSnapshot *snap, const LuloSystemdState *state)
{
    if (!snap || !state) return -1;

    switch (state->view) {
    case LULO_SYSTEMD_VIEW_SERVICES:
        return load_selected_unit_file(snap, state);
    case LULO_SYSTEMD_VIEW_DEPS:
        return load_selected_dependencies(snap, state);
    case LULO_SYSTEMD_VIEW_CONFIG:
    default:
        return load_selected_config(snap, state);
    }
}
