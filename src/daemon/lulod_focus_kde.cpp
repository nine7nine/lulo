#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QTextStream>

#include <cstdio>
#include <cstring>
#include <limits.h>
#include <unistd.h>

#ifndef LULO_DATADIR
#define LULO_DATADIR ""
#endif

namespace {

constexpr const char *kBridgeService = "org.ninez.LulodFocus";
constexpr const char *kBridgePath = "/LulodFocus";
constexpr const char *kBridgeInterface = "org.ninez.LulodFocus";
constexpr const char *kKWinService = "org.kde.KWin";
constexpr const char *kKWinScriptingPath = "/Scripting";
constexpr const char *kKWinScriptingInterface = "org.kde.kwin.Scripting";
constexpr const char *kPluginName = "lulo_focus_reporter";
constexpr const char *kScriptRelativePath = "share/lulo/kwin/lulod_focus_kde.js";

static QString helper_root_path()
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);

    if (n < 0) {
        return {};
    }
    exe[n] = '\0';
    return QFileInfo(QString::fromLocal8Bit(exe)).absolutePath();
}

static QString installed_share_script_path(const QString &root)
{
    QFileInfo root_info(root);
    QDir dir(root_info.absoluteFilePath());

    if (root.isEmpty()) {
        return {};
    }
    if (QFileInfo::exists(root + QLatin1String("/share/lulo/kwin/lulod_focus_kde.js"))) {
        return QFileInfo(root + QLatin1String("/share/lulo/kwin/lulod_focus_kde.js")).absoluteFilePath();
    }
    if (dir.dirName() == QLatin1String("lulo")) {
        dir.cdUp();
        if (dir.dirName() == QLatin1String("libexec")) {
            dir.cdUp();
            const QString candidate =
                QFileInfo(dir.absoluteFilePath(QLatin1String("share/lulo/kwin/lulod_focus_kde.js")))
                    .absoluteFilePath();
            if (QFileInfo::exists(candidate)) {
                return candidate;
            }
        }
    }
    return {};
}

static QString helper_script_path()
{
    if (LULO_DATADIR[0] != '\0') {
        const QString installed =
            QString::fromLatin1(LULO_DATADIR "/kwin/lulod_focus_kde.js");

        if (QFileInfo::exists(installed)) {
            return installed;
        }
    }

    const QString root = helper_root_path();

    if (root.isEmpty()) {
        return {};
    }
    {
        const QString installed = installed_share_script_path(root);

        if (!installed.isEmpty()) {
            return installed;
        }
    }
    return QFileInfo(root + QLatin1Char('/') + QLatin1String(kScriptRelativePath)).absoluteFilePath();
}

static void debug_log(const char *msg)
{
    if (qEnvironmentVariableIsEmpty("LULOD_FOCUS_DEBUG")) {
        return;
    }
    std::fprintf(stderr, "[lulod-focus-kde] %s\n", msg);
    std::fflush(stderr);
}

class FocusBridge : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.ninez.LulodFocus")

public:
    explicit FocusBridge(QObject *parent = nullptr)
        : QObject(parent)
        , m_out(stdout, QIODevice::WriteOnly)
        , m_current_pid(std::numeric_limits<int>::min())
    {
    }

public Q_SLOTS:
    void UpdateFocusedPid(int pid)
    {
        if (pid < 0) {
            pid = 0;
        }
        if (pid == m_current_pid) {
            return;
        }
        m_current_pid = pid;
        m_out << pid << '\n';
        m_out.flush();
    }

private:
    QTextStream m_out;
    int m_current_pid;
};

class ScriptLoader : public QObject
{
public:
    explicit ScriptLoader(QObject *parent = nullptr)
        : QObject(parent)
        , m_iface(kKWinService, kKWinScriptingPath, kKWinScriptingInterface,
                  QDBusConnection::sessionBus())
    {
    }

    bool load(QString *err)
    {
        const QString script_path = helper_script_path();

        if (!m_iface.isValid()) {
            if (err) {
                *err = QStringLiteral("kwin scripting dbus interface is unavailable");
            }
            return false;
        }
        if (script_path.isEmpty() || !QFileInfo::exists(script_path)) {
            if (err) {
                *err = QStringLiteral("focus script not found: %1").arg(script_path);
            }
            return false;
        }

        (void)m_iface.call(QStringLiteral("unloadScript"), QString::fromLatin1(kPluginName));
        QDBusReply<int> load_reply =
            m_iface.call(QStringLiteral("loadScript"), script_path, QString::fromLatin1(kPluginName));
        if (!load_reply.isValid()) {
            if (err) {
                *err = load_reply.error().message();
            }
            return false;
        }
        m_loaded = true;
        QDBusReply<void> start_reply = m_iface.call(QStringLiteral("start"));
        if (!start_reply.isValid()) {
            if (err) {
                *err = start_reply.error().message();
            }
            unload();
            return false;
        }
        debug_log("loaded kwin focus script");
        return true;
    }

    void unload()
    {
        if (!m_loaded || !m_iface.isValid()) {
            return;
        }
        (void)m_iface.call(QStringLiteral("unloadScript"), QString::fromLatin1(kPluginName));
        m_loaded = false;
        debug_log("unloaded kwin focus script");
    }

private:
    QDBusInterface m_iface;
    bool m_loaded = false;
};

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QDBusConnection bus = QDBusConnection::sessionBus();
    FocusBridge bridge;
    ScriptLoader loader;
    QString err;

    if (!bus.isConnected()) {
        std::fprintf(stderr, "failed to connect to session bus\n");
        return 1;
    }
    if (!bus.registerService(QString::fromLatin1(kBridgeService))) {
        std::fprintf(stderr, "failed to register dbus service %s\n", kBridgeService);
        return 1;
    }
    if (!bus.registerObject(QString::fromLatin1(kBridgePath), &bridge, QDBusConnection::ExportAllSlots)) {
        std::fprintf(stderr, "failed to register dbus object %s\n", kBridgePath);
        return 1;
    }
    if (!loader.load(&err)) {
        std::fprintf(stderr, "failed to load kwin focus script: %s\n",
                     err.toLocal8Bit().constData());
        return 1;
    }

    const int rc = app.exec();

    loader.unload();
    return rc;
}

#include "lulod_focus_kde.moc"
