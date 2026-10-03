#include "dynqueueplugin.h"

#include "dynqueuewidget.h"

#include <KLocalizedString>
#include <KPluginFactory>

#include <algorithm>
#include <iterator>
#include <utility>

#include <QDateTime>
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

#include <unistd.h>

Q_LOGGING_CATEGORY(DynQueueLog, "org.kde.konsole.dynqueue")

K_PLUGIN_CLASS_WITH_JSON(DynQueuePlugin, "konsole_dynqueue.json")

namespace
{
constexpr auto SnapshotFileName = "snapshot";
constexpr auto PendingFileName = "pending";

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
    QDateTime modified;
};

bool readSnapshot(const QString &path, Snapshot &snapshot)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    const QList<QByteArray> lines = file.readAll().split('\n');
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
    snapshot.modified = QFileInfo(path).lastModified();
    return !snapshot.queueId.isEmpty();
}

bool hasRunningItem(const DynQueueItems &items)
{
    return std::any_of(items.cbegin(), items.cend(), [](const DynQueueItem &item) {
        return item.state == DynQueueState::Running;
    });
}

} // namespace

void writePending(const QString &sessionDirectory, const QString &queueId, const DynQueueItems &items);

struct DynQueuePlugin::Private {
    struct WindowState {
        Konsole::MainWindow *mainWindow = nullptr;
        QPointer<QDockWidget> dock;
        QPointer<DynQueueWidget> widget;
        QPointer<QObject> controller;
        QString sessionId;
        QString queueId;
        QString sessionDirectory;
        QDateTime snapshotModified;
        DynQueueItems items;
        bool manuallyHidden = false;
        bool changingVisibility = false;
    };

    QHash<Konsole::MainWindow *, WindowState *> windows;
    QTimer *pollTimer = nullptr;
    QString runtimeRoot;
};

DynQueuePlugin::DynQueuePlugin(QObject *parent, const QVariantList &args)
    : Konsole::IKonsolePlugin(parent, args)
    , d(std::make_unique<Private>())
{
    setName(QStringLiteral("DynQueue"));

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
                        state->snapshotModified = {};
                        state->manuallyHidden = false;
                    }
                }
            }

            const QString snapshotPath = state->sessionDirectory + QLatin1Char('/') + QLatin1String(SnapshotFileName);
            Snapshot snapshot;
            if (state->sessionId.isEmpty() || !readSnapshot(snapshotPath, snapshot)) {
                state->items.clear();
                state->queueId.clear();
                state->snapshotModified = {};
                state->widget->clearQueue();
                state->changingVisibility = true;
                state->dock->hide();
                state->changingVisibility = false;
                continue;
            }

            const bool newQueue = state->queueId != snapshot.queueId;
            if (!newQueue && state->snapshotModified.isValid() && state->snapshotModified == snapshot.modified) {
                continue;
            }
            if (newQueue) {
                state->manuallyHidden = false;
                qCDebug(DynQueueLog) << "Detected queue" << snapshot.queueId << "for session" << state->sessionId;
            }

            state->queueId = snapshot.queueId;
            state->items = snapshot.items;
            state->snapshotModified = snapshot.modified;
            state->sessionDirectory = QFileInfo(snapshotPath).absolutePath();
            state->widget->setQueue(state->items, hasRunningItem(state->items));

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
    if (mainWindow == nullptr || d->windows.contains(mainWindow)) {
        return;
    }

    auto *qtWindow = qobject_cast<QMainWindow *>(reinterpret_cast<QObject *>(mainWindow));
    if (qtWindow == nullptr) {
        qCWarning(DynQueueLog) << "Could not resolve Konsole main window";
        return;
    }

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
    connect(state->widget, &DynQueueWidget::addCommandRequested, this, [this, mainWindow](const QString &command) {
        auto *state = d->windows.value(mainWindow, nullptr);
        if (state == nullptr || !hasRunningItem(state->items)) {
            return;
        }
        state->items.append(DynQueueItem{QUuid::createUuid().toString(QUuid::WithoutBraces), command, DynQueueState::Waiting});
        writePending(state->sessionDirectory, state->queueId, state->items);
        state->widget->setQueue(state->items, true);
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
            state->widget->setQueue(state->items, hasRunningItem(state->items));
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
        state->widget->setQueue(state->items, hasRunningItem(state->items));
        qCDebug(DynQueueLog) << "Moved pending command" << index << "to" << target;
    });

    connect(reinterpret_cast<QObject *>(mainWindow), &QObject::destroyed, this, [this, mainWindow] {
        auto *state = d->windows.take(mainWindow);
        delete state;
    });
}

void DynQueuePlugin::activeViewChanged(Konsole::SessionController *controller, Konsole::MainWindow *mainWindow)
{
    auto *state = d->windows.value(mainWindow, nullptr);
    if (state == nullptr) {
        return;
    }

    state->controller = reinterpret_cast<QObject *>(controller);
    state->sessionId.clear();
    state->queueId.clear();
    state->sessionDirectory.clear();
    state->snapshotModified = {};

    if (controller != nullptr) {
        auto *compatController = reinterpret_cast<Konsole::SessionControllerCompat *>(controller);
        const auto sessionPointer = compatController->session();
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
    auto *state = d->windows.value(mainWindow, nullptr);
    if (state == nullptr) {
        return {};
    }

    auto *qtWindow = qobject_cast<QMainWindow *>(reinterpret_cast<QObject *>(mainWindow));
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

#include "moc_dynqueueplugin.cpp"
#include "dynqueueplugin.moc"
