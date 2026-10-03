#pragma once

#include <QString>
#include <QList>

enum class DynQueueState {
    Waiting,
    Running,
    Completed,
    Failed,
    Stopped,
};

struct DynQueueItem {
    QString id;
    QString command;
    DynQueueState state = DynQueueState::Waiting;
};

inline bool operator==(const DynQueueItem &left, const DynQueueItem &right)
{
    return left.id == right.id && left.command == right.command && left.state == right.state;
}

inline bool operator!=(const DynQueueItem &left, const DynQueueItem &right)
{
    return !(left == right);
}

using DynQueueItems = QList<DynQueueItem>;
