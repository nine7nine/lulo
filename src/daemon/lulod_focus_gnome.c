/*
 * lulod-focus-gnome: GNOME counterpart to lulod-focus-kde.
 *
 * GNOME Shell (Mutter) does not expose the focused window's PID to external
 * processes, so the "lulod-focus@ninez.org" GNOME Shell extension publishes it
 * on the session bus as org.ninez.LulodFocus. This helper subscribes to that
 * interface and prints the focused PID to stdout (one decimal PID per line),
 * exactly like lulod-focus-kde, so the lulod focus monitor can consume either
 * provider identically.
 *
 * The helper watches the bus name rather than requiring it up front: it works
 * whether the extension is loaded before or after the helper starts, and emits
 * 0 (no focus) when the extension goes away.
 */

#define _GNU_SOURCE

#include <gio/gio.h>
#include <glib.h>

#include <stdio.h>
#include <stdlib.h>

#define LULO_FOCUS_BUS_NAME "org.ninez.LulodFocus"
#define LULO_FOCUS_OBJECT_PATH "/LulodFocus"
#define LULO_FOCUS_INTERFACE "org.ninez.LulodFocus"

typedef struct {
    GMainLoop *loop;
    GDBusConnection *conn;
    guint watch_id;
    guint signal_sub_id;
    int last_pid;
} FocusHelper;

static void debug_log(const char *msg)
{
    const char *dbg = getenv("LULOD_FOCUS_DEBUG");

    if (!dbg || !*dbg) return;
    fprintf(stderr, "[lulod-focus-gnome] %s\n", msg);
    fflush(stderr);
}

static void report_pid(FocusHelper *helper, int pid)
{
    if (pid < 0) pid = 0;
    if (pid == helper->last_pid) return;
    helper->last_pid = pid;
    printf("%d\n", pid);
    fflush(stdout);
}

static void on_focus_signal(GDBusConnection *connection, const char *sender_name,
                            const char *object_path, const char *interface_name,
                            const char *signal_name, GVariant *parameters,
                            gpointer user_data)
{
    FocusHelper *helper = user_data;
    gint pid = 0;

    (void)connection;
    (void)sender_name;
    (void)object_path;
    (void)interface_name;
    (void)signal_name;
    if (g_variant_is_of_type(parameters, G_VARIANT_TYPE("(i)"))) {
        g_variant_get(parameters, "(i)", &pid);
        report_pid(helper, pid);
    }
}

static void query_initial_pid(FocusHelper *helper)
{
    GVariant *reply;
    GError *err = NULL;
    gint pid = 0;

    reply = g_dbus_connection_call_sync(
        helper->conn, LULO_FOCUS_BUS_NAME, LULO_FOCUS_OBJECT_PATH,
        LULO_FOCUS_INTERFACE, "GetFocusedPid", NULL, G_VARIANT_TYPE("(i)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err);
    if (!reply) {
        if (err) {
            debug_log(err->message);
            g_error_free(err);
        }
        return;
    }
    g_variant_get(reply, "(i)", &pid);
    report_pid(helper, pid);
    g_variant_unref(reply);
}

static void on_name_appeared(GDBusConnection *connection, const char *name,
                             const char *name_owner, gpointer user_data)
{
    FocusHelper *helper = user_data;

    (void)connection;
    (void)name;
    (void)name_owner;
    debug_log("focus reporter appeared");
    query_initial_pid(helper);
}

static void on_name_vanished(GDBusConnection *connection, const char *name,
                             gpointer user_data)
{
    FocusHelper *helper = user_data;

    (void)connection;
    (void)name;
    debug_log("focus reporter vanished");
    report_pid(helper, 0);
}

int main(void)
{
    FocusHelper helper;
    GError *err = NULL;

    helper.loop = NULL;
    helper.watch_id = 0;
    helper.signal_sub_id = 0;
    helper.last_pid = -1;

    helper.conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!helper.conn) {
        fprintf(stderr, "failed to connect to session bus: %s\n",
                err ? err->message : "unknown error");
        if (err) g_error_free(err);
        return 1;
    }

    helper.signal_sub_id = g_dbus_connection_signal_subscribe(
        helper.conn, LULO_FOCUS_BUS_NAME, LULO_FOCUS_INTERFACE,
        "FocusedPidChanged", LULO_FOCUS_OBJECT_PATH, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_focus_signal, &helper, NULL);

    helper.watch_id = g_bus_watch_name_on_connection(
        helper.conn, LULO_FOCUS_BUS_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
        on_name_appeared, on_name_vanished, &helper, NULL);

    helper.loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(helper.loop);

    g_bus_unwatch_name(helper.watch_id);
    g_dbus_connection_signal_unsubscribe(helper.conn, helper.signal_sub_id);
    g_main_loop_unref(helper.loop);
    g_object_unref(helper.conn);
    return 0;
}
