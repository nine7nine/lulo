#define _GNU_SOURCE

#include "lulo_auth.h"

#include "lulo_edit.h"
#include "lulo_proc_meta.h"

#include <gio/gio.h>
#include <polkit/polkit.h>
#include <polkitagent/polkitagent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *k_auth_agent_path = "/io/lulo/AuthAgent";

static long long auth_mono_ms_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

typedef struct LuloAuthWorker LuloAuthWorker;
typedef struct LuloAuthConversation LuloAuthConversation;

struct LuloAuthWorker {
    pthread_t thread;
    pthread_t request_thread;
    pthread_mutex_t mu;
    int started;
    int request_started;
    int finished;
    int success;
    int cancelled;
    int cancel_requested;
    int busy;
    int echo;
    unsigned long long start_time;
    char message[256];
    char identity[128];
    char prompt[160];
    char output[1024];
    GMainContext *context;
    GMainLoop *loop;
    GCancellable *cancellable;
    gpointer registration_handle;
    LuloAuthConversation *conversation;
};

struct LuloAuthConversation {
    LuloAuthWorker *worker;
    GTask *task;
    PolkitAgentSession *session;
    PolkitIdentity *identity;
};

typedef struct {
    PolkitAgentListener parent_instance;
    LuloAuthWorker *worker;
} LuloPolkitAgent;

typedef struct {
    PolkitAgentListenerClass parent_class;
} LuloPolkitAgentClass;

GType lulo_polkit_agent_get_type(void);
G_DEFINE_TYPE(LuloPolkitAgent, lulo_polkit_agent, POLKIT_AGENT_TYPE_LISTENER)

static void auth_worker_set_prompt(LuloAuthWorker *worker, const char *prompt, int echo)
{
    if (!worker) return;
    pthread_mutex_lock(&worker->mu);
    snprintf(worker->prompt, sizeof(worker->prompt), "%s", prompt ? prompt : "Password:");
    worker->echo = echo ? 1 : 0;
    worker->busy = 0;
    pthread_mutex_unlock(&worker->mu);
}

static void auth_worker_set_output(LuloAuthWorker *worker, const char *text)
{
    if (!worker) return;
    pthread_mutex_lock(&worker->mu);
    snprintf(worker->output, sizeof(worker->output), "%s", text ? text : "");
    pthread_mutex_unlock(&worker->mu);
}

static void auth_worker_finish(LuloAuthWorker *worker, int success, int cancelled, const char *message)
{
    if (!worker) return;
    pthread_mutex_lock(&worker->mu);
    worker->finished = 1;
    worker->success = success ? 1 : 0;
    worker->cancelled = cancelled ? 1 : 0;
    worker->busy = 0;
    if (message && *message) snprintf(worker->output, sizeof(worker->output), "%s", message);
    pthread_mutex_unlock(&worker->mu);
    if (worker->loop) g_main_loop_quit(worker->loop);
}

static void auth_conversation_clear(LuloAuthConversation *conv)
{
    if (!conv) return;
    if (conv->session) g_object_unref(conv->session);
    if (conv->identity) g_object_unref(conv->identity);
    if (conv->task) g_object_unref(conv->task);
    free(conv);
}

static void auth_session_request(PolkitAgentSession *session, const gchar *request,
                                 gboolean echo_on, gpointer user_data)
{
    LuloAuthConversation *conv = user_data;
    (void)session;

    auth_worker_set_prompt(conv->worker, request, echo_on ? 1 : 0);
}

static void auth_session_show_error(PolkitAgentSession *session, const gchar *text, gpointer user_data)
{
    LuloAuthConversation *conv = user_data;
    (void)session;

    auth_worker_set_output(conv->worker, text);
}

static void auth_session_show_info(PolkitAgentSession *session, const gchar *text, gpointer user_data)
{
    LuloAuthConversation *conv = user_data;
    (void)session;

    auth_worker_set_output(conv->worker, text);
}

static void auth_session_completed(PolkitAgentSession *session, gboolean gained_authorization,
                                   gpointer user_data)
{
    LuloAuthConversation *conv = user_data;
    LuloAuthWorker *worker = conv->worker;

    (void)session;
    (void)gained_authorization;
    pthread_mutex_lock(&worker->mu);
    worker->conversation = NULL;
    pthread_mutex_unlock(&worker->mu);
    g_task_return_boolean(conv->task, TRUE);
    auth_conversation_clear(conv);
}

static void lulo_polkit_agent_initiate_authentication(PolkitAgentListener *listener,
                                                      const gchar *action_id,
                                                      const gchar *message,
                                                      const gchar *icon_name,
                                                      PolkitDetails *details,
                                                      const gchar *cookie,
                                                      GList *identities,
                                                      GCancellable *cancellable,
                                                      GAsyncReadyCallback callback,
                                                      gpointer user_data)
{
    LuloPolkitAgent *agent = (LuloPolkitAgent *)listener;
    LuloAuthWorker *worker = agent->worker;
    LuloAuthConversation *conv;
    GTask *task;
    gchar *identity_text = NULL;

    (void)action_id;
    (void)icon_name;
    (void)details;

    if (!identities || !identities->data) {
        task = g_task_new(listener, cancellable, callback, user_data);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "no authentication identity");
        g_object_unref(task);
        return;
    }

    conv = calloc(1, sizeof(*conv));
    if (!conv) {
        task = g_task_new(listener, cancellable, callback, user_data);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "out of memory");
        g_object_unref(task);
        return;
    }

    conv->worker = worker;
    conv->task = g_task_new(listener, cancellable, callback, user_data);
    conv->identity = g_object_ref(identities->data);
    conv->session = polkit_agent_session_new(conv->identity, cookie);
    identity_text = polkit_identity_to_string(conv->identity);

    pthread_mutex_lock(&worker->mu);
    (void)message;
    snprintf(worker->message, sizeof(worker->message), "%s", "Unlock RW mode");
    if (identity_text && *identity_text) {
        const char *text = identity_text;
        if (strncmp(text, "unix-user:", 10) == 0) text += 10;
        snprintf(worker->identity, sizeof(worker->identity), "%s", text);
    } else {
        worker->identity[0] = '\0';
    }
    snprintf(worker->prompt, sizeof(worker->prompt), "%s", "Password:");
    worker->echo = 0;
    worker->busy = 1;
    worker->output[0] = '\0';
    worker->conversation = conv;
    pthread_mutex_unlock(&worker->mu);

    g_signal_connect(conv->session, "request", G_CALLBACK(auth_session_request), conv);
    g_signal_connect(conv->session, "show-error", G_CALLBACK(auth_session_show_error), conv);
    g_signal_connect(conv->session, "show-info", G_CALLBACK(auth_session_show_info), conv);
    g_signal_connect(conv->session, "completed", G_CALLBACK(auth_session_completed), conv);
    if (cancellable) {
        g_signal_connect_swapped(cancellable, "cancelled",
                                 G_CALLBACK(polkit_agent_session_cancel), conv->session);
    }
    polkit_agent_session_initiate(conv->session);
    g_free(identity_text);
}

static gboolean lulo_polkit_agent_initiate_authentication_finish(PolkitAgentListener *listener,
                                                                 GAsyncResult *res,
                                                                 GError **error)
{
    (void)listener;
    return g_task_propagate_boolean(G_TASK(res), error);
}

static void lulo_polkit_agent_class_init(LuloPolkitAgentClass *klass)
{
    PolkitAgentListenerClass *listener_class = POLKIT_AGENT_LISTENER_CLASS(klass);

    listener_class->initiate_authentication = lulo_polkit_agent_initiate_authentication;
    listener_class->initiate_authentication_finish = lulo_polkit_agent_initiate_authentication_finish;
}

static void lulo_polkit_agent_init(LuloPolkitAgent *agent)
{
    agent->worker = NULL;
}

static gboolean auth_submit_response_cb(gpointer user_data)
{
    struct SubmitCtx {
        LuloAuthWorker *worker;
        char *text;
    };
    struct SubmitCtx *ctx = user_data;
    PolkitAgentSession *session = NULL;

    pthread_mutex_lock(&ctx->worker->mu);
    if (ctx->worker->conversation && ctx->worker->conversation->session) {
        session = g_object_ref(ctx->worker->conversation->session);
    }
    ctx->worker->busy = 1;
    ctx->worker->output[0] = '\0';
    pthread_mutex_unlock(&ctx->worker->mu);

    if (session) {
        polkit_agent_session_response(session, ctx->text ? ctx->text : "");
        g_object_unref(session);
    }
    free(ctx->text);
    free(ctx);
    return G_SOURCE_REMOVE;
}

static gboolean auth_cancel_cb(gpointer user_data)
{
    LuloAuthWorker *worker = user_data;
    PolkitAgentSession *session = NULL;
    int have_session = 0;

    pthread_mutex_lock(&worker->mu);
    if (worker->cancel_requested) {
        pthread_mutex_unlock(&worker->mu);
        return G_SOURCE_REMOVE;
    }
    worker->cancel_requested = 1;
    if (worker->conversation && worker->conversation->session) {
        session = g_object_ref(worker->conversation->session);
        have_session = 1;
    }
    pthread_mutex_unlock(&worker->mu);
    if (session) {
        polkit_agent_session_cancel(session);
        g_object_unref(session);
    } else if (!have_session && worker->cancellable) {
        g_cancellable_cancel(worker->cancellable);
    }
    return G_SOURCE_REMOVE;
}

static void *auth_unlock_request_main(void *opaque)
{
    LuloAuthWorker *worker = opaque;
    char errbuf[160] = "";
    int cancelled = 0;

    if (lulo_system_auth_unlock(errbuf, sizeof(errbuf)) == 0) {
        auth_worker_finish(worker, 1, 0, "");
        return NULL;
    }

    pthread_mutex_lock(&worker->mu);
    cancelled = worker->cancel_requested;
    pthread_mutex_unlock(&worker->mu);
    auth_worker_finish(worker, 0, cancelled, cancelled ? "" :
                       (errbuf[0] ? errbuf : "rw authorization required"));
    return NULL;
}

static void *auth_worker_main(void *opaque)
{
    LuloAuthWorker *worker = opaque;
    PolkitSubject *subject = NULL;
    LuloPolkitAgent *listener = NULL;
    GError *error = NULL;

    worker->context = g_main_context_new();
    worker->loop = g_main_loop_new(worker->context, FALSE);
    worker->cancellable = g_cancellable_new();
    g_main_context_push_thread_default(worker->context);

    subject = polkit_unix_process_new_for_owner(getpid(), worker->start_time, getuid());
    listener = g_object_new(lulo_polkit_agent_get_type(), NULL);
    listener->worker = worker;
    worker->registration_handle = polkit_agent_listener_register(POLKIT_AGENT_LISTENER(listener),
                                                                 POLKIT_AGENT_REGISTER_FLAGS_NONE,
                                                                 subject,
                                                                 k_auth_agent_path,
                                                                 worker->cancellable,
                                                                 &error);
    if (!worker->registration_handle) {
        auth_worker_finish(worker, 0, 0, error && error->message ? error->message : "failed to register auth agent");
        goto out;
    }

    if (pthread_create(&worker->request_thread, NULL, auth_unlock_request_main, worker) != 0) {
        auth_worker_finish(worker, 0, 0, "failed to start rw unlock");
        goto out;
    }
    worker->request_started = 1;
    g_main_loop_run(worker->loop);

out:
    if (worker->conversation && worker->conversation->session) {
        polkit_agent_session_cancel(worker->conversation->session);
    }
    if (worker->context) {
        for (int i = 0; worker->conversation && i < 32; i++) {
            while (g_main_context_pending(worker->context)) {
                g_main_context_iteration(worker->context, FALSE);
            }
            if (!worker->conversation) break;
            g_usleep(1000);
        }
    }
    if (worker->conversation) {
        auth_conversation_clear(worker->conversation);
        worker->conversation = NULL;
    }
    if (worker->registration_handle) {
        polkit_agent_listener_unregister(worker->registration_handle);
        worker->registration_handle = NULL;
    }
    if (worker->request_started) pthread_join(worker->request_thread, NULL);
    if (listener) g_object_unref(listener);
    if (subject) g_object_unref(subject);
    if (error) g_error_free(error);
    if (worker->cancellable) g_object_unref(worker->cancellable);
    if (worker->loop) g_main_loop_unref(worker->loop);
    if (worker->context) {
        g_main_context_pop_thread_default(worker->context);
        g_main_context_unref(worker->context);
    }
    return NULL;
}

static void auth_worker_free(LuloAuthWorker *worker)
{
    if (!worker) return;
    pthread_mutex_destroy(&worker->mu);
    free(worker);
}

static LuloAuthWorker *auth_worker_from_app(AppState *app)
{
    return app ? (LuloAuthWorker *)app->auth_ctx : NULL;
}

int lulo_auth_start(AppState *app)
{
    LuloAuthWorker *worker;
    unsigned long long start_time = 0;
    char comm[128];

    if (!app || app->auth_ctx) return -1;
    if (lulo_proc_meta_read_basic(getpid(), comm, sizeof(comm), &start_time) < 0) return -1;

    worker = calloc(1, sizeof(*worker));
    if (!worker) return -1;
    pthread_mutex_init(&worker->mu, NULL);
    worker->busy = 1;
    worker->start_time = start_time;
    if (pthread_create(&worker->thread, NULL, auth_worker_main, worker) != 0) {
        auth_worker_free(worker);
        return -1;
    }
    worker->started = 1;

    app->auth_ctx = worker;
    app->auth_active = 1;
    app->auth_echo = 0;
    app->auth_busy = 1;
    app->auth_input_len = 0;
    app->auth_message[0] = '\0';
    app->auth_identity[0] = '\0';
    snprintf(app->auth_prompt, sizeof(app->auth_prompt), "%s", "Password:");
    app->auth_input[0] = '\0';
    snprintf(app->auth_output, sizeof(app->auth_output), "%s", "Waiting...");
    return 0;
}

void lulo_auth_cancel(AppState *app)
{
    LuloAuthWorker *worker = auth_worker_from_app(app);

    if (!app || !worker) return;
    app->auth_busy = 1;
    app->auth_echo = 0;
    app->auth_input_len = 0;
    app->auth_input[0] = '\0';
    snprintf(app->auth_output, sizeof(app->auth_output), "%s", "Cancelling...");
    if (worker->context) g_main_context_invoke(worker->context, auth_cancel_cb, worker);
}

void lulo_auth_append_text(AppState *app, const char *text, size_t len)
{
    size_t room;

    if (!app || !text || len == 0) return;
    room = sizeof(app->auth_input) - 1 - (size_t)app->auth_input_len;
    if (len > room) len = room;
    if (len == 0) return;
    memcpy(app->auth_input + app->auth_input_len, text, len);
    app->auth_input_len += (int)len;
    app->auth_input[app->auth_input_len] = '\0';
}

void lulo_auth_backspace(AppState *app)
{
    if (!app || app->auth_input_len <= 0) return;
    app->auth_input_len--;
    app->auth_input[app->auth_input_len] = '\0';
}

void lulo_auth_submit(AppState *app)
{
    struct SubmitCtx {
        LuloAuthWorker *worker;
        char *text;
    };
    struct SubmitCtx *ctx;
    LuloAuthWorker *worker = auth_worker_from_app(app);

    if (!app || !worker) return;
    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return;
    ctx->worker = worker;
    ctx->text = strdup(app->auth_input);
    app->auth_input_len = 0;
    app->auth_input[0] = '\0';
    app->auth_busy = 1;
    app->auth_output[0] = '\0';
    if (worker->context) {
        g_main_context_invoke(worker->context, auth_submit_response_cb, ctx);
    } else {
        free(ctx->text);
        free(ctx);
    }
}

void lulo_auth_poll(AppState *app, RenderFlags *render)
{
    LuloAuthWorker *worker = auth_worker_from_app(app);
    int finished = 0;
    int success = 0;
    int cancelled = 0;
    int changed = 0;
    int old_echo;
    int old_busy;
    char old_message[sizeof(app->auth_message)];
    char old_identity[sizeof(app->auth_identity)];
    char old_prompt[sizeof(app->auth_prompt)];
    char old_output[sizeof(app->auth_output)];

    if (!app || !worker) return;

    old_echo = app->auth_echo;
    old_busy = app->auth_busy;
    snprintf(old_message, sizeof(old_message), "%s", app->auth_message);
    snprintf(old_identity, sizeof(old_identity), "%s", app->auth_identity);
    snprintf(old_prompt, sizeof(old_prompt), "%s", app->auth_prompt);
    snprintf(old_output, sizeof(old_output), "%s", app->auth_output);

    pthread_mutex_lock(&worker->mu);
    finished = worker->finished;
    success = worker->success;
    cancelled = worker->cancelled;
    app->auth_echo = worker->echo;
    app->auth_busy = worker->busy;
    snprintf(app->auth_message, sizeof(app->auth_message), "%s", worker->message);
    snprintf(app->auth_identity, sizeof(app->auth_identity), "%s", worker->identity);
    snprintf(app->auth_prompt, sizeof(app->auth_prompt), "%s", worker->prompt);
    snprintf(app->auth_output, sizeof(app->auth_output), "%s", worker->output);
    pthread_mutex_unlock(&worker->mu);

    changed = old_echo != app->auth_echo ||
              old_busy != app->auth_busy ||
              strcmp(old_message, app->auth_message) != 0 ||
              strcmp(old_identity, app->auth_identity) != 0 ||
              strcmp(old_prompt, app->auth_prompt) != 0 ||
              strcmp(old_output, app->auth_output) != 0;

    if (render && changed) render->need_render = 1;
    if (!finished) return;

    if (worker->started) pthread_join(worker->thread, NULL);
    app->auth_ctx = NULL;
    app->auth_active = 0;
    app->auth_busy = 0;
    app->auth_input_len = 0;
    app->auth_input[0] = '\0';
    auth_worker_free(worker);

    if (success) {
        app->rw_mode = 1;
        snprintf(app->status, sizeof(app->status), "%s", "mode root/RW");
        app->status_until_ms = auth_mono_ms_now() + 2500;
    } else if (!cancelled) {
        app->rw_mode = 0;
        if (app->auth_output[0]) {
            snprintf(app->status, sizeof(app->status), "%.*s",
                     (int)sizeof(app->status) - 1, app->auth_output);
            app->status_until_ms = auth_mono_ms_now() + 3500;
        }
    }
    if (render) {
        render->need_header = 1;
        render->need_footer = 1;
        render->need_render = 1;
    }
}

void lulo_auth_shutdown(AppState *app)
{
    if (!app || !app->auth_ctx) return;
    lulo_auth_cancel(app);
    while (app->auth_ctx) lulo_auth_poll(app, NULL);
}
