#include "dynqueueplugin.h"

#include "dynqueuewidget.h"

#include <KLocalizedString>
#include <KAboutData>
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
#include <QRegularExpression>
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

QPointer<Konsole::Session> sessionForController(Konsole::SessionController *controller)
{
    if (controller == nullptr) {
        return {};
    }

    // SessionController::session() is an inline method in Konsole's private
    // headers.  Its SessionDisplayConnection is an exported QObject child,
    // so finding that child avoids depending on the private controller's
    // member offsets and multiple-inheritance layout.
    auto *controllerObject = reinterpret_cast<QObject *>(controller);
    const auto connections = controllerObject->findChildren<QObject *>(QString(), Qt::FindDirectChildrenOnly);
    for (QObject *child : connections) {
        if (child != nullptr && child->inherits("Konsole::SessionDisplayConnection")) {
            auto *connection = reinterpret_cast<Konsole::SessionDisplayConnection *>(child);
            return connection->session();
        }
    }
    return {};
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

QString normalizedKonsoleVersion(const QString &value)
{
    static const QRegularExpression versionPattern(QStringLiteral(R"(^(\d+\.\d+\.\d+))"));
    const auto match = versionPattern.match(value.trimmed());
    return match.hasMatch() ? match.captured(1) : QString();
}

bool konsoleVersionMatches()
{
    const QString builtVersion = normalizedKonsoleVersion(QStringLiteral(DYNQUEUE_BUILT_KONSOLE_VERSION));
    QString runtimeVersion = normalizedKonsoleVersion(KAboutData::applicationData().version());
    if (runtimeVersion.isEmpty()) {
        runtimeVersion = normalizedKonsoleVersion(QCoreApplication::applicationVersion());
    }
    if (builtVersion.isEmpty() || runtimeVersion.isEmpty()) {
        qCWarning(DynQueueLog) << "Could not verify the exact Konsole version; disabling DynQueue for safety"
                               << "(built:" << DYNQUEUE_BUILT_KONSOLE_VERSION
                               << ", runtime:" << KAboutData::applicationData().version()
                               << QCoreApplication::applicationVersion() << ')';
        return false;
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
                const auto sessionPointer = sessionForController(controller);
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
                } else if (!state->sessionId.isEmpty()) {
                    state->sessionId.clear();
                    state->queueId.clear();
                    state->sessionDirectory.clear();
                    state->snapshotContent.clear();
                    state->stopRequested = false;
                }
            }

            const QString snapshotPath = state->sessionDirectory + QLatin1Char('/') + QLatin1String(SnapshotFileName);
            Snapshot snapshot;
            if (state->sessionId.isEmpty() || !liveShell(state->sessionDirectory) || !readSnapshot(snapshotPath, snapshot)) {
                state->items.clear();
                state->queueId.clear();
                state->snapshotContent.clear();
                state->stopRequested = false;
                state->widget->clearQueue();
                state->changingVisibility = true;
                state->dock->hide();
                state->changingVisibility = false;
                continue;
            }

            const bool newQueue = state->queueId != snapshot.queueId;
            if (!newQueue && state->snapshotContent == snapshot.content) {
                continue;
            }
            if (newQueue) {
                state->manuallyHidden = false;
                state->stopRequested = false;
                qCDebug(DynQueueLog) << "Detected queue" << snapshot.queueId << "for session" << state->sessionId;
            }

            state->queueId = snapshot.queueId;
            state->items = snapshot.items;
            state->snapshotContent = snapshot.content;
            state->sessionDirectory = QFileInfo(snapshotPath).absolutePath();
            const bool queueActive = hasRunningItem(state->items);
            if (!queueActive) {
                state->stopRequested = false;
            }
            state->widget->setQueue(state->items, queueActive, state->stopRequested);

            if (!state->manuallyHidden && !state->dock->isVisible()) {
                state->changingVisibility = true;
                state->dock->show();
                state->changingVisibility = false;
            }
        }
    });
    d->pollTimer->start();
}

DynQueuePlugin::~DynQueuePlugin()
{
    qDeleteAll(d->windows);
}

void DynQueuePlugin::createWidgetsForMainWindow(Konsole::MainWindow *mainWindow)
{
    if (!d->compatible || mainWindow == nullptr || d->windows.contains(mainWindow)) {
        return;
    }

    // Konsole::MainWindow directly inherits KXmlGuiWindow/QMainWindow.  The
    // compatibility header intentionally keeps MainWindow opaque, so use the
    // known base relationship instead of asking Qt's meta-object system to
    // rediscover it through an incomplete type.
    auto *qtWindow = reinterpret_cast<QMainWindow *>(mainWindow);

    auto *state = new Private::WindowState;
    state->mainWindow = mainWindow;
    state->dock = new QDockWidget(i18n("Command Queue"), qtWindow);
    state->dock->setObjectName(QStringLiteral("DynQueueDock"));
    state->dock->setAllowedAreas(Qt::RightDockWidgetArea);
    state->dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
    state->widget = new DynQueueWidget(state->dock);
    state->dock->setWidget(state->widget);
    state->dock->hide();
    qtWindow->addDockWidget(Qt::RightDockWidgetArea, state->dock);

    d->windows.insert(mainWindow, state);

    connect(state->dock, &QDockWidget::visibilityChanged, this, [state](bool visible) {
        if (!state->changingVisibility) {
            state->manuallyHidden = !visible;
        }
    });
    connect(state->widget, &DynQueueWidget::closeRequested, this, [state] {
        state->manuallyHidden = true;
        state->changingVisibility = true;
        state->dock->hide();
        state->changingVisibility = false;
    });
    connect(state->widget, &DynQueueWidget::stopQueueRequested, this, [this, mainWindow] {
        auto *state = d->windows.value(mainWindow, nullptr);
        if (state == nullptr || !hasRunningItem(state->items) || state->stopRequested) {
            return;
        }
        writeCancel(state->sessionDirectory, state->queueId);
        state->stopRequested = true;
        state->widget->setQueue(state->items, true, true);
        qCDebug(DynQueueLog) << "Requested queue stop after current item for" << state->queueId;
    });
    connect(state->widget, &DynQueueWidget::addCommandRequested, this, [this, mainWindow](const QString &command) {
        auto *state = d->windows.value(mainWindow, nullptr);
        if (state == nullptr || !hasRunningItem(state->items)) {
            return;
        }
        state->items.append(DynQueueItem{QUuid::createUuid().toString(QUuid::WithoutBraces), command, DynQueueState::Waiting});
        writePending(state->sessionDirectory, state->queueId, state->items);
        state->widget->setQueue(state->items, true, state->stopRequested);
        qCDebug(DynQueueLog) << "Added pending command to" << state->queueId;
    });
    connect(state->widget, &DynQueueWidget::removeCommandRequested, this, [this, mainWindow](const QString &itemId) {
        auto *state = d->windows.value(mainWindow, nullptr);
        if (state == nullptr) {
            return;
        }
        const auto it = std::find_if(state->items.begin(), state->items.end(), [&itemId](const DynQueueItem &item) {
            return item.id == itemId && item.state == DynQueueState::Waiting;
        });
        if (it != state->items.end()) {
            state->items.erase(it);
            writePending(state->sessionDirectory, state->queueId, state->items);
            state->widget->setQueue(state->items, hasRunningItem(state->items), state->stopRequested);
            qCDebug(DynQueueLog) << "Removed pending command" << itemId;
        }
    });
    connect(state->widget, &DynQueueWidget::moveCommandRequested, this, [this, mainWindow](const QString &itemId, int direction) {
        auto *state = d->windows.value(mainWindow, nullptr);
        if (state == nullptr) {
            return;
        }
        const int index = std::distance(state->items.cbegin(), std::find_if(state->items.cbegin(), state->items.cend(), [&itemId](const DynQueueItem &item) {
            return item.id == itemId && item.state == DynQueueState::Waiting;
        }));
        const int target = index + direction;
        if (index < 0 || index >= state->items.size() || target < 1 || target >= state->items.size()
            || state->items.at(target).state != DynQueueState::Waiting) {
            return;
        }
        state->items.swapItemsAt(index, target);
        writePending(state->sessionDirectory, state->queueId, state->items);
        state->widget->setQueue(state->items, hasRunningItem(state->items), state->stopRequested);
        qCDebug(DynQueueLog) << "Moved pending command" << index << "to" << target;
    });

    connect(reinterpret_cast<QObject *>(mainWindow), &QObject::destroyed, this, [this, mainWindow] {
        auto *state = d->windows.take(mainWindow);
        delete state;
    });
}

void DynQueuePlugin::activeViewChanged(Konsole::SessionController *controller, Konsole::MainWindow *mainWindow)
{
    if (!d->compatible) {
        return;
    }
    auto *state = d->windows.value(mainWindow, nullptr);
    if (state == nullptr) {
        return;
    }

    state->controller = reinterpret_cast<QObject *>(controller);
    state->sessionId.clear();
    state->queueId.clear();
    state->sessionDirectory.clear();
    state->snapshotContent.clear();
    state->stopRequested = false;
    state->items.clear();
    state->widget->clearQueue();
    state->changingVisibility = true;
    state->dock->hide();
    state->changingVisibility = false;

    if (controller != nullptr) {
        const auto sessionPointer = sessionForController(controller);
        if (!sessionPointer.isNull()) {
            state->sessionId = safeSessionName(sessionPointer->shellSessionId());
            if (!state->sessionId.isEmpty()) {
                state->sessionDirectory = d->runtimeRoot + QLatin1Char('/') + state->sessionId;
            }
        }
    }
}

QList<QAction *> DynQueuePlugin::menuBarActions(Konsole::MainWindow *mainWindow) const
{
    if (!d->compatible) {
        return {};
    }
    auto *state = d->windows.value(mainWindow, nullptr);
    if (state == nullptr) {
        return {};
    }

    auto *qtWindow = reinterpret_cast<QMainWindow *>(mainWindow);
    auto *toggle = new QAction(i18n("Show Command Queue"), qtWindow);
    toggle->setCheckable(true);
    toggle->setIcon(QIcon::fromTheme(QStringLiteral("view-list-details")));
    toggle->setChecked(state->dock->isVisible());
    connect(toggle, &QAction::triggered, this, [state](bool visible) {
        state->manuallyHidden = !visible;
        state->changingVisibility = true;
        state->dock->setVisible(visible);
        state->changingVisibility = false;
    });
    connect(state->dock, &QDockWidget::visibilityChanged, toggle, &QAction::setChecked);
    return {toggle};
}

void writePending(const QString &sessionDirectory, const QString &queueId, const DynQueueItems &items)
{
    if (sessionDirectory.isEmpty() || queueId.isEmpty()) {
        return;
    }

    QSaveFile file(sessionDirectory + QLatin1Char('/') + QLatin1String(PendingFileName));
    if (!file.open(QIODevice::WriteOnly)) {
        qCWarning(DynQueueLog) << "Could not open pending queue file" << file.fileName();
        return;
    }

    file.write("DYNQUEUE_PENDING\t1\t");
    file.write(queueId.toUtf8());
    file.putChar('\n');
    for (const auto &item : std::as_const(items)) {
        if (item.state != DynQueueState::Waiting) {
            continue;
        }
        file.write(item.id.toUtf8());
        file.putChar('\t');
        file.write(item.command.toUtf8().toBase64());
        file.putChar('\n');
    }

    if (!file.commit()) {
        qCWarning(DynQueueLog) << "Could not commit pending queue file" << file.fileName();
    }
}

void writeCancel(const QString &sessionDirectory, const QString &queueId)
{
    if (sessionDirectory.isEmpty() || queueId.isEmpty()) {
        return;
    }

    QSaveFile file(sessionDirectory + QLatin1Char('/') + QLatin1String(CancelFileName));
    if (!file.open(QIODevice::WriteOnly)) {
        qCWarning(DynQueueLog) << "Could not open queue cancel file" << file.fileName();
        return;
    }
    file.write(queueId.toUtf8());
    file.putChar('\n');
    if (!file.commit()) {
        qCWarning(DynQueueLog) << "Could not commit queue cancel file" << file.fileName();
    }
}

#include "moc_dynqueueplugin.cpp"
#include "dynqueueplugin.moc"
