#pragma once

#include "dynqueue_types.h"

#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;
class QToolButton;

class DynQueueWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit DynQueueWidget(QWidget *parent = nullptr);

    void setQueue(const DynQueueItems &items, bool queueActive);
    void clearQueue();

Q_SIGNALS:
    void addCommandRequested(const QString &command);
    void removeCommandRequested(const QString &itemId);
    void moveCommandRequested(const QString &itemId, int direction);
    void closeRequested();

private Q_SLOTS:
    void addCommand();
    void removeCommand();
    void moveUp();
    void moveDown();
    void updateControls();

private:
    static QString displayText(const DynQueueItem &item);
    static QString stateText(DynQueueState state);
    static QString statePrefix(DynQueueState state);

    void rebuildList(const QString &selectedId);

    QListWidget *_list = nullptr;
    QLabel *_selectionLabel = nullptr;
    QPushButton *_addButton = nullptr;
    QPushButton *_removeButton = nullptr;
    QToolButton *_upButton = nullptr;
    QToolButton *_downButton = nullptr;

    DynQueueItems _items;
    bool _queueActive = false;
};
