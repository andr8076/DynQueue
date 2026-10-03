#include "dynqueueplugin.h"

#include "dynqueuewidget.h"

#include <KLocalizedString>
#include <KPluginFactory>

#include <algorithm>
#include <cerrno>
#include <iterator>
#include <utility>

#include <QCoreApplication>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QMenu>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <signal.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(DynQueueLog, "org.kde.konsole.dynqueue")

K_PLUGIN_CLASS_WITH_JSON(DynQueuePlugin, "konsole_dynqueue.json")

namespace
{
constexpr auto SnapshotFileName = "snapshot";
constexpr auto PendingFileName = "pending";
constexpr auto CancelFileName = "cancel";

#ifndef DYNQUEUE_BUILT_KONSOLE_VERSION
#define DYNQUEUE_BUILT_KONSOLE_VERSION "unknown"
#endif

QString safeSessionName(const QString &value)
{
    QString result;
    result.reserve(value.size());
    for (const QChar character : value) {
        if (character.isLetterOrNumber() || character == u'.' || character == u'_' || character == u'-') {
            result.append(character);
        } else {
            result.append(u'_');
        }
    }
    return result;
}

DynQueueState stateFromString(const QByteArray &value)
{
    if (value == "running") {
        return DynQueueState::Running;
    }
    if (value == "completed") {
        return DynQueueState::Completed;
    }
    if (value == "failed") {
        return DynQueueState::Failed;
    }
    if (value == "stopped") {
        return DynQueueState::Stopped;
    }
    return DynQueueState::Waiting;
}

struct Snapshot {
    QString queueId;
    DynQueueItems items;
    QByteArray content;
};

bool readSnapshot(const QString &path, Snapshot &snapshot)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    const QByteArray content = file.readAll();
    const QList<QByteArray> lines = content.split('\n');
    if (lines.isEmpty()) {
        return false;
    }

    const QList<QByteArray> header = lines.constFirst().split('\t');
    if (header.size() < 3 || header.at(0) != "DYNQUEUE" || header.at(1) != "1") {
        return false;
    }

    DynQueueItems items;
    for (int lineNumber = 1; lineNumber < lines.size(); ++lineNumber) {
        const QByteArray line = lines.at(lineNumber);
        if (line.isEmpty()) {
            continue;
        }
        const QList<QByteArray> fields = line.split('\t');
        if (fields.size() < 3 || fields.at(0).isEmpty()) {
            continue;
        }

        const QByteArray decoded = QByteArray::fromBase64(fields.at(2));
        items.append(DynQueueItem{QString::fromUtf8(fields.at(0)), QString::fromUtf8(decoded), stateFromString(fields.at(1))});
    }

    snapshot.queueId = QString::fromUtf8(header.at(2));
    snapshot.items = items;
    snapshot.content = content;
    return !snapshot.queueId.isEmpty();
}

bool liveShell(const QString &sessionDirectory)
{
    QFile pidFile(sessionDirectory + QStringLiteral("/shell.pid"));
    if (!pidFile.open(QIODevice::ReadOnly)) {
        return false;
    }

    bool ok = false;
    const qint64 pid = pidFile.readAll().trimmed().toLongLong(&ok);
    if (!ok || pid <= 0) {
        return false;
    }

    errno = 0;
    const int result = ::kill(static_cast<pid_t>(pid), 0);
    return result == 0 || errno == EPERM;
}

bool konsoleVersionMatches()
{
    const QString builtVersion = QStringLiteral(DYNQUEUE_BUILT_KONSOLE_VERSION);
    const QString runtimeVersion = QCoreApplication::applicationVersion();
    if (runtimeVersion.isEmpty()) {
        qCWarning(DynQueueLog) << "Konsole did not expose an application version; ABI compatibility could not be checked";
        return true;
    }
    if (runtimeVersion != builtVersion) {
        qCWarning(DynQueueLog) << "DynQueue was built for Konsole" << builtVersion << "but is running in" << runtimeVersion << "; disabling plugin";
        return false;
    }
    return true;
}

bool hasRunningItem(const DynQueueItems &items)
{
    return std::any_of(items.cbegin(), items.cend(), [](const DynQueueItem &item) {
        return item.state == DynQueueState::Running;
    });
}

} // namespace

void writePending(const QString &sessionDirectory, const QString &queueId, const DynQueueItems &items);
void writeCancel(const QString &sessionDirectory, const QString &queueId);

struct DynQueuePlugin::Private {
    struct WindowState {
        Konsole::MainWindow *mainWindow = nullptr;
        QPointer<QDockWidget> dock;
        QPointer<DynQueueWidget> widget;
        QPointer<QObject> controller;
        QString sessionId;
        QString queueId;
        QString sessionDirectory;
        QByteArray snapshotContent;
        DynQueueItems items;
        bool manuallyHidden = false;
        bool changingVisibility = false;
        bool stopRequested = false;
    };

    QHash<Konsole::MainWindow *, WindowState *> windows;
    QTimer *pollTimer = nullptr;
    QString runtimeRoot;
    bool compatible = true;
};

DynQueuePlugin::DynQueuePlugin(QObject *parent, const QVariantList &args)
    : Konsole::IKonsolePlugin(parent, args)
    , d(std::make_unique<Private>())
{
    setName(QStringLiteral("DynQueue"));
    d->compatible = konsoleVersionMatches();

    QString runtimeDirectory = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtimeDirectory.isEmpty()) {
        runtimeDirectory = QStringLiteral("/tmp");
    }
    d->runtimeRoot = runtimeDirectory + QStringLiteral("/dynqueue-%1").arg(QString::number(getuid()));

    d->pollTimer = new QTimer(this);
    d->pollTimer->setInterval(150);
    connect(d->pollTimer, &QTimer::timeout, this, [this] {
        for (Private::WindowState *state : std::as_const(d->windows)) {
            if (state == nullptr) {
                continue;
            }

            if (!state->controller.isNull()) {
                auto *controller = reinterpret_cast<Konsole::SessionController *>(state->controller.data());
                auto *compatController = reinterpret_cast<Konsole::SessionControllerCompat *>(controller);
                const auto sessionPointer = compatController->session();
                if (!sessionPointer.isNull()) {
                    const QString sessionId = safeSessionName(sessionPointer->shellSessionId());
                    if (!sessionId.isEmpty() && sessionId != state->sessionId) {
                        state->sessionId = sessionId;
                        state->queueId.clear();
                        state->sessionDirectory = d->runtimeRoot + QLatin1Char('/') + sessionId;
                        state->snapshotContent.clear();
                        state->manuallyHidden = false;
                        state->stopRequested = false;
                    }
