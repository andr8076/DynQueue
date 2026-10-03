/*
    SPDX-License-Identifier: GPL-2.0-or-later

    DynQueue uses Konsole's current plugin ABI.  Konsole currently does not
    install a separate external-plugin development package, so the minimum
    declarations needed by the plugin are kept here.

    The IKonsolePlugin declaration mirrors Konsole's
    src/pluginsystem/IKonsolePlugin.h.  The controller shim mirrors the
    beginning of SessionController's object layout only far enough to call
    its inline session() accessor.  The install/build checks require matching
    Konsole major and minor versions, and the README explains this constraint.
*/

#pragma once

#include <QAction>
#include <QColor>
#include <QIcon>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <KXMLGUIClient>

#include <memory>
#include <optional>

#if defined(_WIN32)
#define DYNQUEUE_KONSOLE_EXPORT __declspec(dllimport)
#else
#define DYNQUEUE_KONSOLE_EXPORT __attribute__((visibility("default")))
#endif

namespace Konsole
{
class MainWindow;
class SessionController;

class DYNQUEUE_KONSOLE_EXPORT IKonsolePlugin : public QObject
{
    Q_OBJECT

public:
    IKonsolePlugin(QObject *parent, const QVariantList &args);
    ~IKonsolePlugin() override;

    QString name() const;

    void addMainWindow(Konsole::MainWindow *mainWindow);
    void removeMainWindow(Konsole::MainWindow *mainWindow);

    virtual void createWidgetsForMainWindow(Konsole::MainWindow *mainWindow) = 0;
    virtual void activeViewChanged(Konsole::SessionController *controller, Konsole::MainWindow *mainWindow) = 0;

    virtual QList<QAction *> menuBarActions(Konsole::MainWindow *mainWindow) const
    {
        Q_UNUSED(mainWindow)
        return {};
    }

protected:
    void setName(const QString &pluginName);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

class DYNQUEUE_KONSOLE_EXPORT Session : public QObject
{
public:
    QString shellSessionId() const;
};

class DYNQUEUE_KONSOLE_EXPORT SessionDisplayConnection
{
public:
    QPointer<Session> session();
};

// Only sizeof(ViewProperties) is relevant to the controller shim.  These
// fields mirror current Konsole's ViewProperties.h.
class ViewPropertiesCompat : public QObject
{
public:
    virtual ~ViewPropertiesCompat() = default;
    virtual QUrl url() const;
    virtual QString currentDir() const;
    virtual bool confirmClose() const;

private:
    QIcon _icon;
    QString _title;
    QColor _color;
    QColor _activityColor;
    int _identifier = 0;
    std::optional<int> _progress;
};

// SessionController has two direct bases followed by these two pointers.
// This is deliberately isolated so a future Konsole plugin SDK can replace
// it without changing the rest of DynQueue.
class SessionControllerCompat : public ViewPropertiesCompat, public KXMLGUIClient
{
private:
    void *_copyToGroup = nullptr;
    SessionDisplayConnection *_sessionDisplayConnection = nullptr;

public:
    QPointer<Session> session() const
    {
        return _sessionDisplayConnection == nullptr ? QPointer<Session>() : _sessionDisplayConnection->session();
    }
};
}
