#define _GNU_SOURCE

#include "lulod_system_edit.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef enum {
    EDIT_SCOPE_NONE = 0,
    EDIT_SCOPE_SCHED,
    EDIT_SCOPE_SYSTEMD,
    EDIT_SCOPE_CGROUPS,
} EditScope;

typedef struct {
    uid_t uid;
    gid_t gid;
    EditScope scope;
    char original_path[PATH_MAX];
} EditMeta;

static const char *k_edit_meta_root = "/run/lulo-edit-meta";

static int ensure_dir_mode(const char *path, mode_t mode)
{
    struct stat st;

    if (mkdir(path, mode) == 0) return 0;
    if (errno != EEXIST) return -1;
    if (stat(path, &st) < 0) return -1;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

static int edit_runtime_dir(uid_t uid, char *buf, size_t len)
{
    if (snprintf(buf, len, "/run/user/%lu/lulo-edit", (unsigned long)uid) >= (int)len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int ensure_user_edit_dir(uid_t uid, gid_t gid, char *path, size_t path_len)
{
    struct stat st;

    if (edit_runtime_dir(uid, path, path_len) < 0) return -1;
    if (mkdir(path, 0700) < 0 && errno != EEXIST) return -1;
    if (stat(path, &st) < 0) return -1;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    if (chown(path, uid, gid) < 0) return -1;
    if (chmod(path, 0700) < 0) return -1;
    return 0;
}

static int scope_for_path(const char *path, EditScope *scope_out)
{
    static const struct {
        const char *prefix;
        EditScope scope;
    } allowed[] = {
        { "/etc/lulo/scheduler/", EDIT_SCOPE_SCHED },
        { "/etc/systemd/", EDIT_SCOPE_SYSTEMD },
        { "/usr/lib/systemd/", EDIT_SCOPE_SYSTEMD },
        { "/lib/systemd/", EDIT_SCOPE_SYSTEMD },
        { "/sys/fs/cgroup/", EDIT_SCOPE_CGROUPS },
    };

    if (!path || !scope_out) return -1;
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        size_t len = strlen(allowed[i].prefix);

        if (strncmp(path, allowed[i].prefix, len) == 0) {
            *scope_out = allowed[i].scope;
            return 0;
        }
    }
    errno = EPERM;
    return -1;
}

static int validate_session_id(const char *session_id)
{
    const unsigned char *p = (const unsigned char *)session_id;

    if (!session_id || !*session_id) return -1;
    while (*p) {
        if (!(isalnum(*p) || *p == '-' || *p == '_' || *p == '.')) return -1;
        p++;
    }
    return 0;
}

static int session_paths(const char *session_id,
                         uid_t uid,
                         char *edit_path, size_t edit_path_len,
                         char *meta_path, size_t meta_path_len)
{
    char edit_root[PATH_MAX];

    if (validate_session_id(session_id) < 0) {
        errno = EINVAL;
        return -1;
    }
    if (edit_runtime_dir(uid, edit_root, sizeof(edit_root)) < 0) return -1;
    if (snprintf(edit_path, edit_path_len, "%s/%s", edit_root, session_id) >= (int)edit_path_len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (snprintf(meta_path, meta_path_len, "%s/%s.meta", k_edit_meta_root, session_id) >= (int)meta_path_len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int copy_fd_all(int out_fd, int in_fd)
{
    char buf[8192];

    for (;;) {
        ssize_t nr = read(in_fd, buf, sizeof(buf));

        if (nr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (nr == 0) return 0;
        for (ssize_t off = 0; off < nr;) {
            ssize_t nw = write(out_fd, buf + off, (size_t)(nr - off));

            if (nw < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            off += nw;
        }
    }
}

static int derive_real_path(const char *path, char *buf, size_t len, EditScope *scope_out)
{
    char resolved[PATH_MAX];

    if (!realpath(path, resolved)) return -1;
    if (scope_for_path(resolved, scope_out) < 0) return -1;
    if (snprintf(buf, len, "%s", resolved) >= (int)len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int derive_parent_checked_path(const char *path, char *buf, size_t len, EditScope *scope_out)
{
    char original[PATH_MAX];
    char resolved_parent[PATH_MAX];
    char final_path[PATH_MAX];
    char *slash;
    const char *base;
    size_t parent_len;

    if (!path || !*path || path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    if (snprintf(original, sizeof(original), "%s", path) >= (int)sizeof(original)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    slash = strrchr(original, '/');
    if (!slash || slash == original) {
        errno = EINVAL;
        return -1;
    }
    base = slash + 1;
    if (!*base || strcmp(base, ".") == 0 || strcmp(base, "..") == 0 || strchr(base, '/')) {
        errno = EINVAL;
        return -1;
    }
    *slash = '\0';
    if (!realpath(original, resolved_parent)) return -1;
    parent_len = strlen(resolved_parent);
    if (parent_len + 1 + strlen(base) + 1 > sizeof(final_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(final_path, resolved_parent, parent_len);
    final_path[parent_len] = '/';
    snprintf(final_path + parent_len + 1, sizeof(final_path) - parent_len - 1, "%s", base);
    if (scope_for_path(final_path, scope_out) < 0) return -1;
    if (snprintf(buf, len, "%s", final_path) >= (int)len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int write_meta_file(const char *meta_path, const EditMeta *meta)
{
    FILE *fp;

    fp = fopen(meta_path, "w");
    if (!fp) return -1;
    if (fprintf(fp, "uid=%lu\ngid=%lu\nscope=%d\npath=%s\n",
                (unsigned long)meta->uid, (unsigned long)meta->gid,
                (int)meta->scope, meta->original_path) < 0) {
        fclose(fp);
        return -1;
    }
    if (fchmod(fileno(fp), 0600) < 0) {
        fclose(fp);
        return -1;
    }
    if (fclose(fp) < 0) return -1;
    return 0;
}

static int read_meta_file(const char *meta_path, EditMeta *meta)
{
    FILE *fp;
    char line[PATH_MAX + 64];

    memset(meta, 0, sizeof(*meta));
    fp = fopen(meta_path, "r");
    if (!fp) return -1;
    while (fgets(line, sizeof(line), fp)) {
        char *eq = strchr(line, '=');
        char *key;
        char *value;

        if (!eq) continue;
        *eq = '\0';
        key = line;
        value = eq + 1;
        value[strcspn(value, "\r\n")] = '\0';
        if (strcmp(key, "uid") == 0) meta->uid = (uid_t)strtoul(value, NULL, 10);
        else if (strcmp(key, "gid") == 0) meta->gid = (gid_t)strtoul(value, NULL, 10);
        else if (strcmp(key, "scope") == 0) meta->scope = (EditScope)strtol(value, NULL, 10);
        else if (strcmp(key, "path") == 0) snprintf(meta->original_path, sizeof(meta->original_path), "%s", value);
    }
    if (fclose(fp) < 0) return -1;
    if (!meta->original_path[0]) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int replace_file_from_edit(const char *edit_path, const char *original_path)
{
    struct stat st;
    char dir[PATH_MAX];
    char temp_path[PATH_MAX] = {0};
    char *slash;
    int src_fd = -1;
    int tmp_fd = -1;
    int rc = -1;

    if (stat(original_path, &st) < 0) return -1;
    if (!S_ISREG(st.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    src_fd = open(edit_path, O_RDONLY | O_NOFOLLOW);
    if (src_fd < 0) return -1;
    if (snprintf(dir, sizeof(dir), "%s", original_path) >= (int)sizeof(dir)) {
        errno = ENAMETOOLONG;
        goto out;
    }
    slash = strrchr(dir, '/');
    if (!slash || slash == dir) {
        errno = EINVAL;
        goto out;
    }
    *slash = '\0';
    if (snprintf(temp_path, sizeof(temp_path), "%s/.lulo-edit.XXXXXX", dir) >= (int)sizeof(temp_path)) {
        errno = ENAMETOOLONG;
        goto out;
    }
    tmp_fd = mkstemp(temp_path);
    if (tmp_fd < 0) goto out;
    if (fchmod(tmp_fd, st.st_mode & 07777) < 0) goto out;
    if (fchown(tmp_fd, st.st_uid, st.st_gid) < 0) goto out;
    if (copy_fd_all(tmp_fd, src_fd) < 0) goto out;
    if (fsync(tmp_fd) < 0) goto out;
    if (close(tmp_fd) < 0) {
        tmp_fd = -1;
        goto out;
    }
    tmp_fd = -1;
    if (rename(temp_path, original_path) < 0) goto out;
    rc = 0;

out:
    if (src_fd >= 0) close(src_fd);
    if (tmp_fd >= 0) close(tmp_fd);
    if (rc < 0 && temp_path[0]) unlink(temp_path);
    return rc;
}

static int write_fd_from_edit_copy(int out_fd, const char *edit_path)
{
    int src_fd = -1;
    int rc = -1;

    src_fd = open(edit_path, O_RDONLY | O_NOFOLLOW);
    if (src_fd < 0) return -1;
    if (lseek(out_fd, 0, SEEK_SET) < 0) goto out;
    if (copy_fd_all(out_fd, src_fd) < 0) goto out;
    if (fsync(out_fd) < 0) goto out;
    rc = 0;

out:
    if (src_fd >= 0) close(src_fd);
    return rc;
}

static int write_direct_file(const char *path, const char *content)
{
    int fd = -1;
    int rc = -1;
    size_t remaining = content ? strlen(content) : 0;
    const char *ptr = content ? content : "";

    fd = open(path, O_WRONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (lseek(fd, 0, SEEK_SET) < 0) goto out;
    while (remaining > 0) {
        ssize_t nw = write(fd, ptr, remaining);

        if (nw < 0) {
            if (errno == EINTR) continue;
            goto out;
        }
        ptr += (size_t)nw;
        remaining -= (size_t)nw;
    }
    if (fsync(fd) < 0) goto out;
    rc = 0;

out:
    if (fd >= 0) close(fd);
    return rc;
}

static int commit_direct_file_from_edit(const char *edit_path, const char *original_path)
{
    int out_fd = -1;
    int rc = -1;

    out_fd = open(original_path, O_WRONLY | O_NOFOLLOW);
    if (out_fd < 0) return -1;
    rc = write_fd_from_edit_copy(out_fd, edit_path);
    close(out_fd);
    return rc;
}

static int write_string_to_file(const char *path, const char *content)
{
    struct stat st;
    char dir[PATH_MAX];
    char temp_path[PATH_MAX] = {0};
    char *slash;
    int exists = 0;
    int tmp_fd = -1;
    int rc = -1;
    mode_t mode = 0644;
    uid_t uid = 0;
    gid_t gid = 0;
    size_t remaining = content ? strlen(content) : 0;
    const char *ptr = content ? content : "";

    if (lstat(path, &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            errno = EINVAL;
            return -1;
        }
        exists = 1;
        mode = st.st_mode & 07777;
        uid = st.st_uid;
        gid = st.st_gid;
    }
    if (snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    slash = strrchr(dir, '/');
    if (!slash || slash == dir) {
        errno = EINVAL;
        return -1;
    }
    *slash = '\0';
    if (snprintf(temp_path, sizeof(temp_path), "%s/.lulo-write.XXXXXX", dir) >= (int)sizeof(temp_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    tmp_fd = mkstemp(temp_path);
    if (tmp_fd < 0) return -1;
    if (fchmod(tmp_fd, mode) < 0) goto out;
    if (fchown(tmp_fd, uid, gid) < 0) goto out;
    while (remaining > 0) {
        ssize_t nw = write(tmp_fd, ptr, remaining);

        if (nw < 0) {
            if (errno == EINTR) continue;
            goto out;
        }
        ptr += (size_t)nw;
        remaining -= (size_t)nw;
    }
    if (fsync(tmp_fd) < 0) goto out;
    if (close(tmp_fd) < 0) {
        tmp_fd = -1;
        goto out;
    }
    tmp_fd = -1;
    if (!exists) {
        chmod(temp_path, mode);
        chown(temp_path, uid, gid);
    }
    if (rename(temp_path, path) < 0) goto out;
    rc = 0;

out:
    if (tmp_fd >= 0) close(tmp_fd);
    if (rc < 0 && temp_path[0]) unlink(temp_path);
    return rc;
}

int lulod_system_edit_begin(const char *path, uid_t uid, gid_t gid,
                            char *session_id, size_t session_id_len,
                            char *edit_path, size_t edit_path_len,
                            int *reload_sched,
                            char *err, size_t errlen)
{
    char resolved[PATH_MAX];
    char meta_path[PATH_MAX];
    char template_path[PATH_MAX];
    char edit_root[PATH_MAX];
    EditMeta meta;
    EditScope scope = EDIT_SCOPE_NONE;
    int src_fd = -1;
    int dst_fd = -1;
    const char *base;

    if (err && errlen > 0) err[0] = '\0';
    if (reload_sched) *reload_sched = 0;
    if (!path || !*path || !session_id || session_id_len == 0 || !edit_path || edit_path_len == 0) {
        if (err && errlen > 0) snprintf(err, errlen, "invalid edit request");
        errno = EINVAL;
        return -1;
    }
    if (ensure_dir_mode(k_edit_meta_root, 0700) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "%s: %s", k_edit_meta_root, strerror(errno));
        return -1;
    }
    if (ensure_user_edit_dir(uid, gid, edit_root, sizeof(edit_root)) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "%s: %s", edit_root, strerror(errno));
        return -1;
    }
    if (derive_real_path(path, resolved, sizeof(resolved), &scope) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "edit not allowed: %s", path);
        return -1;
    }
    src_fd = open(resolved, O_RDONLY | O_NOFOLLOW);
    if (src_fd < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "open %s: %s", resolved, strerror(errno));
        return -1;
    }
    if (snprintf(template_path, sizeof(template_path), "%s/u%lu-editXXXXXX",
                 edit_root, (unsigned long)uid) >= (int)sizeof(template_path)) {
        if (err && errlen > 0) snprintf(err, errlen, "edit path too long");
        close(src_fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    dst_fd = mkstemp(template_path);
    if (dst_fd < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "mkstemp: %s", strerror(errno));
        close(src_fd);
        return -1;
    }
    if (copy_fd_all(dst_fd, src_fd) < 0 || fsync(dst_fd) < 0 ||
        fchmod(dst_fd, 0600) < 0 || fchown(dst_fd, uid, gid) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "prepare edit copy: %s", strerror(errno));
        close(src_fd);
        close(dst_fd);
        unlink(template_path);
        return -1;
    }
    close(src_fd);
    close(dst_fd);
    base = strrchr(template_path, '/');
    base = base ? base + 1 : template_path;
    if (snprintf(session_id, session_id_len, "%s", base) >= (int)session_id_len ||
        snprintf(edit_path, edit_path_len, "%s", template_path) >= (int)edit_path_len) {
        if (err && errlen > 0) snprintf(err, errlen, "edit session id too long");
        unlink(template_path);
        errno = ENAMETOOLONG;
        return -1;
    }
    if (session_paths(session_id, uid, template_path, sizeof(template_path),
                      meta_path, sizeof(meta_path)) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "bad session paths");
        unlink(edit_path);
        return -1;
    }
    memset(&meta, 0, sizeof(meta));
    meta.uid = uid;
    meta.gid = gid;
    meta.scope = scope;
    snprintf(meta.original_path, sizeof(meta.original_path), "%s", resolved);
    if (write_meta_file(meta_path, &meta) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "write meta: %s", strerror(errno));
        unlink(edit_path);
        return -1;
    }
    if (reload_sched) *reload_sched = (scope == EDIT_SCOPE_SCHED);
    return 0;
}

int lulod_system_edit_commit(const char *session_id, uid_t uid, int *reload_sched,
                             char *err, size_t errlen)
{
    char edit_path[PATH_MAX];
    char meta_path[PATH_MAX];
    EditMeta meta;
    EditScope scope = EDIT_SCOPE_NONE;

    if (err && errlen > 0) err[0] = '\0';
    if (reload_sched) *reload_sched = 0;
    if (session_paths(session_id, uid, edit_path, sizeof(edit_path), meta_path, sizeof(meta_path)) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "invalid edit session");
        return -1;
    }
    if (read_meta_file(meta_path, &meta) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "load edit session: %s", strerror(errno));
        return -1;
    }
    if (meta.uid != uid) {
        if (err && errlen > 0) snprintf(err, errlen, "edit session ownership mismatch");
        errno = EPERM;
        return -1;
    }
    if (scope_for_path(meta.original_path, &scope) < 0 || scope != meta.scope) {
        if (err && errlen > 0) snprintf(err, errlen, "edit session path validation failed");
        errno = EPERM;
        return -1;
    }
    if ((meta.scope == EDIT_SCOPE_CGROUPS
         ? commit_direct_file_from_edit(edit_path, meta.original_path)
         : replace_file_from_edit(edit_path, meta.original_path)) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "commit %s: %s", meta.original_path, strerror(errno));
        return -1;
    }
    unlink(edit_path);
    unlink(meta_path);
    if (reload_sched) *reload_sched = (meta.scope == EDIT_SCOPE_SCHED);
    return 0;
}

int lulod_system_edit_cancel(const char *session_id, uid_t uid,
                             char *err, size_t errlen)
{
    char edit_path[PATH_MAX];
    char meta_path[PATH_MAX];
    EditMeta meta;

    if (err && errlen > 0) err[0] = '\0';
    if (session_paths(session_id, uid, edit_path, sizeof(edit_path), meta_path, sizeof(meta_path)) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "invalid edit session");
        return -1;
    }
    if (read_meta_file(meta_path, &meta) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "load edit session: %s", strerror(errno));
        return -1;
    }
    if (meta.uid != uid) {
        if (err && errlen > 0) snprintf(err, errlen, "edit session ownership mismatch");
        errno = EPERM;
        return -1;
    }
    unlink(edit_path);
    unlink(meta_path);
    return 0;
}

int lulod_system_write_file(const char *path, const char *content, int *reload_sched,
                            char *err, size_t errlen)
{
    char resolved[PATH_MAX];
    EditScope scope = EDIT_SCOPE_NONE;

    if (err && errlen > 0) err[0] = '\0';
    if (reload_sched) *reload_sched = 0;
    if (derive_parent_checked_path(path, resolved, sizeof(resolved), &scope) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "write not allowed: %s", path ? path : "");
        return -1;
    }
    if ((scope == EDIT_SCOPE_CGROUPS
         ? write_direct_file(resolved, content ? content : "")
         : write_string_to_file(resolved, content ? content : "")) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "write %s: %s", resolved, strerror(errno));
        return -1;
    }
    if (reload_sched) *reload_sched = (scope == EDIT_SCOPE_SCHED);
    return 0;
}

int lulod_system_delete_file(const char *path, int *reload_sched,
                             char *err, size_t errlen)
{
    char resolved[PATH_MAX];
    EditScope scope = EDIT_SCOPE_NONE;
    struct stat st;

    if (err && errlen > 0) err[0] = '\0';
    if (reload_sched) *reload_sched = 0;
    if (derive_real_path(path, resolved, sizeof(resolved), &scope) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "delete not allowed: %s", path ? path : "");
        return -1;
    }
    if (lstat(resolved, &st) < 0 || !S_ISREG(st.st_mode)) {
        if (err && errlen > 0) snprintf(err, errlen, "delete %s: invalid target", resolved);
        errno = EINVAL;
        return -1;
    }
    if (unlink(resolved) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "delete %s: %s", resolved, strerror(errno));
        return -1;
    }
    if (reload_sched) *reload_sched = (scope == EDIT_SCOPE_SCHED);
    return 0;
}
