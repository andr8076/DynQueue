#pragma once

#include "dynqueue_types.h"
#include "konsole_api_compat.h"

#include <QList>
#include <QObject>

#include <memory>

class DynQueuePlugin final : public Konsole::IKonsolePlugin
{
    Q_OBJECT

public:
    DynQueuePlugin(QObject *parent, const QVariantList &args);
    ~DynQueuePlugin() override;

    void createWidgetsForMainWindow(Konsole::MainWindow *mainWindow) override;
    void activeViewChanged(Konsole::SessionController *controller, Konsole::MainWindow *mainWindow) override;
    QList<QAction *> menuBarActions(Konsole::MainWindow *mainWindow) const override;

private:
    struct Private;
    std::unique_ptr<Private> d;
};
