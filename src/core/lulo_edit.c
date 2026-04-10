#define _GNU_SOURCE

#include "lulo_edit.h"

#include "lulod_system_ipc.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void lulo_edit_session_init(LuloEditSession *session)
{
    if (!session) return;
    memset(session, 0, sizeof(*session));
}

void lulo_edit_session_clear(LuloEditSession *session)
{
    if (!session) return;
    memset(session, 0, sizeof(*session));
}

static int with_system_socket(int (*fn)(int fd, void *opaque), void *opaque,
                              char *err, size_t errlen)
{
    char socket_path[108];
    int fd = -1;
    int rc;

    if (err && errlen > 0) err[0] = '\0';
    if (lulod_system_socket_path(socket_path, sizeof(socket_path)) < 0) {
        if (err && errlen > 0) snprintf(err, errlen, "bad lulod-system socket path");
        return -1;
    }
    fd = lulod_system_connect_socket(socket_path);
    if (fd < 0) {
        if (err && errlen > 0) {
            snprintf(err, errlen, "failed to connect to lulod-system: %s", strerror(errno));
        }
        return -1;
    }
    rc = fn(fd, opaque);
    close(fd);
    return rc;
}

typedef struct {
    const char *path;
    LuloEditSession *session;
    char *err;
    size_t errlen;
} EditBeginCtx;

static int edit_begin_request(int fd, void *opaque)
{
    EditBeginCtx *ctx = opaque;

    if (lulod_system_send_edit_begin_request(fd, ctx->path) < 0) {
        if (ctx->err && ctx->errlen > 0) snprintf(ctx->err, ctx->errlen, "failed to send edit request");
        return -1;
    }
    if (lulod_system_recv_edit_begin_response(fd,
                                              ctx->session->session_id, sizeof(ctx->session->session_id),
                                              ctx->session->edit_path, sizeof(ctx->session->edit_path),
                                              ctx->err, ctx->errlen) < 0) {
        if (ctx->err && ctx->errlen > 0 && !ctx->err[0]) {
            snprintf(ctx->err, ctx->errlen, "failed to read edit response");
        }
        return -1;
    }
    ctx->session->privileged = 1;
    return 0;
}

typedef struct {
    const char *session_id;
    char *err;
    size_t errlen;
} EditStatusCtx;

static int edit_commit_request(int fd, void *opaque)
{
    EditStatusCtx *ctx = opaque;

    if (lulod_system_send_edit_session_request(fd, LULOD_SYSTEM_REQ_EDIT_COMMIT, ctx->session_id) < 0) {
        if (ctx->err && ctx->errlen > 0) snprintf(ctx->err, ctx->errlen, "failed to send edit commit");
        return -1;
    }
    if (lulod_system_recv_status_response(fd, ctx->err, ctx->errlen) < 0) {
        if (ctx->err && ctx->errlen > 0 && !ctx->err[0]) {
            snprintf(ctx->err, ctx->errlen, "failed to read edit commit response");
        }
        return -1;
    }
    return 0;
}

static int edit_cancel_request(int fd, void *opaque)
{
    EditStatusCtx *ctx = opaque;

    if (lulod_system_send_edit_session_request(fd, LULOD_SYSTEM_REQ_EDIT_CANCEL, ctx->session_id) < 0) {
        if (ctx->err && ctx->errlen > 0) snprintf(ctx->err, ctx->errlen, "failed to send edit cancel");
        return -1;
    }
    if (lulod_system_recv_status_response(fd, ctx->err, ctx->errlen) < 0) {
        if (ctx->err && ctx->errlen > 0 && !ctx->err[0]) {
            snprintf(ctx->err, ctx->errlen, "failed to read edit cancel response");
        }
        return -1;
    }
    return 0;
}

typedef struct {
    const char *path;
    const char *content;
    char *err;
    size_t errlen;
} FileWriteCtx;

typedef struct {
    const char *path;
    char *err;
    size_t errlen;
} FileDeleteCtx;

static int file_write_request(int fd, void *opaque)
{
    FileWriteCtx *ctx = opaque;

    if (lulod_system_send_file_write_request(fd, ctx->path, ctx->content) < 0) {
        if (ctx->err && ctx->errlen > 0) snprintf(ctx->err, ctx->errlen, "failed to send file write");
        return -1;
    }
    if (lulod_system_recv_status_response(fd, ctx->err, ctx->errlen) < 0) {
        if (ctx->err && ctx->errlen > 0 && !ctx->err[0]) {
            snprintf(ctx->err, ctx->errlen, "failed to read file write response");
        }
        return -1;
    }
    return 0;
}

static int file_delete_request(int fd, void *opaque)
{
    FileDeleteCtx *ctx = opaque;

    if (lulod_system_send_file_delete_request(fd, ctx->path) < 0) {
        if (ctx->err && ctx->errlen > 0) snprintf(ctx->err, ctx->errlen, "failed to send file delete");
        return -1;
    }
    if (lulod_system_recv_status_response(fd, ctx->err, ctx->errlen) < 0) {
        if (ctx->err && ctx->errlen > 0 && !ctx->err[0]) {
            snprintf(ctx->err, ctx->errlen, "failed to read file delete response");
        }
        return -1;
    }
    return 0;
}

int lulo_edit_session_begin(const char *path, LuloEditSession *session,
                            char *err, size_t errlen)
{
    EditBeginCtx ctx;

    if (err && errlen > 0) err[0] = '\0';
    if (!path || !*path || !session) {
        if (err && errlen > 0) snprintf(err, errlen, "invalid edit path");
        errno = EINVAL;
        return -1;
    }

    lulo_edit_session_init(session);
    if (snprintf(session->original_path, sizeof(session->original_path), "%s", path) >=
        (int)sizeof(session->original_path)) {
        if (err && errlen > 0) snprintf(err, errlen, "edit path too long");
        errno = ENAMETOOLONG;
        return -1;
    }
    if (access(path, W_OK) == 0) {
        if (snprintf(session->edit_path, sizeof(session->edit_path), "%s", path) >=
            (int)sizeof(session->edit_path)) {
            if (err && errlen > 0) snprintf(err, errlen, "edit path too long");
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.path = path;
    ctx.session = session;
    ctx.err = err;
    ctx.errlen = errlen;
    if (with_system_socket(edit_begin_request, &ctx, err, errlen) < 0) {
        lulo_edit_session_clear(session);
        return -1;
    }
    return 0;
}

int lulo_edit_session_commit(LuloEditSession *session, char *err, size_t errlen)
{
    EditStatusCtx ctx;

    if (err && errlen > 0) err[0] = '\0';
    if (!session) {
        if (err && errlen > 0) snprintf(err, errlen, "invalid edit session");
        errno = EINVAL;
        return -1;
    }
    if (!session->privileged) return 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.session_id = session->session_id;
    ctx.err = err;
    ctx.errlen = errlen;
    if (with_system_socket(edit_commit_request, &ctx, err, errlen) < 0) return -1;
    lulo_edit_session_clear(session);
    return 0;
}

int lulo_edit_session_cancel(LuloEditSession *session, char *err, size_t errlen)
{
    EditStatusCtx ctx;

    if (err && errlen > 0) err[0] = '\0';
    if (!session) {
        if (err && errlen > 0) snprintf(err, errlen, "invalid edit session");
        errno = EINVAL;
        return -1;
    }
    if (!session->privileged) {
        lulo_edit_session_clear(session);
        return 0;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.session_id = session->session_id;
    ctx.err = err;
    ctx.errlen = errlen;
    if (with_system_socket(edit_cancel_request, &ctx, err, errlen) < 0) return -1;
    lulo_edit_session_clear(session);
    return 0;
}

int lulo_edit_write_file(const char *path, const char *content,
                         char *err, size_t errlen)
{
    FILE *fp;
    FileWriteCtx ctx;

    if (err && errlen > 0) err[0] = '\0';
    if (!path || !*path) {
        if (err && errlen > 0) snprintf(err, errlen, "invalid write path");
        errno = EINVAL;
        return -1;
    }
    fp = fopen(path, "w");
    if (fp) {
        size_t len = content ? strlen(content) : 0;
        int close_rc = 0;

        if ((len > 0 && fwrite(content, 1, len, fp) != len) || fflush(fp) != 0) {
            if (err && errlen > 0) snprintf(err, errlen, "write %s: %s", path, strerror(errno));
            fclose(fp);
            return -1;
        }
        close_rc = fclose(fp);
        if (close_rc != 0) {
            if (err && errlen > 0) snprintf(err, errlen, "write %s: %s", path, strerror(errno));
            return -1;
        }
        return 0;
    }
    if (!(errno == EACCES || errno == EPERM || errno == ENOENT)) {
        if (err && errlen > 0) snprintf(err, errlen, "write %s: %s", path, strerror(errno));
        return -1;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.path = path;
    ctx.content = content ? content : "";
    ctx.err = err;
    ctx.errlen = errlen;
    return with_system_socket(file_write_request, &ctx, err, errlen);
}

int lulo_edit_delete_file(const char *path, char *err, size_t errlen)
{
    FileDeleteCtx ctx;

    if (err && errlen > 0) err[0] = '\0';
    if (!path || !*path) {
        if (err && errlen > 0) snprintf(err, errlen, "invalid delete path");
        errno = EINVAL;
        return -1;
    }
    if (unlink(path) == 0) return 0;
    if (!(errno == EACCES || errno == EPERM)) {
        if (err && errlen > 0) snprintf(err, errlen, "delete %s: %s", path, strerror(errno));
        return -1;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.path = path;
    ctx.err = err;
    ctx.errlen = errlen;
    return with_system_socket(file_delete_request, &ctx, err, errlen);
}
