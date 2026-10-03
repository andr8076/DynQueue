/*
    SPDX-License-Identifier: GPL-2.0-or-later

    DynQueue uses Konsole's current plugin ABI.  Konsole currently does not
    install a separate external-plugin development package, so the minimum
    declarations needed by the plugin are kept here.

    The IKonsolePlugin declaration mirrors Konsole's
    src/pluginsystem/IKonsolePlugin.h.  SessionController's public session()
    accessor is inline in Konsole's private headers, so DynQueue locates the
    controller's exported SessionDisplayConnection child through QObject
    introspection instead of assuming the private controller object layout.
    The install/build checks require an exact matching Konsole version.
*/

#pragma once

#include <QAction>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>

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

class DYNQUEUE_KONSOLE_EXPORT SessionDisplayConnection : public QObject
{
public:
    QPointer<Session> session();
};
}
