#include "dynqueuewidget.h"

#include <QAbstractItemView>
#include <QDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

namespace
{
constexpr int ItemIdRole = Qt::UserRole + 1;
}

DynQueueWidget::DynQueueWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumWidth(300);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(6);

    _list = new QListWidget(this);
    _list->setSelectionMode(QAbstractItemView::SingleSelection);
    _list->setAlternatingRowColors(true);
    _list->setWordWrap(true);
    _list->setTextElideMode(Qt::ElideMiddle);
    _list->setUniformItemSizes(false);
    connect(_list, &QListWidget::currentRowChanged, this, &DynQueueWidget::updateControls);
    mainLayout->addWidget(_list, 1);

    auto *actionsLayout = new QHBoxLayout;
    _addButton = new QPushButton(tr("+ Add"), this);
    _addButton->setToolTip(tr("Add a command to the end of the pending queue"));
    connect(_addButton, &QPushButton::clicked, this, &DynQueueWidget::addCommand);
    actionsLayout->addWidget(_addButton);

    _stopButton = new QPushButton(tr("Stop"), this);
    _stopButton->setToolTip(tr("Finish the current command, then stop the queue"));
    connect(_stopButton, &QPushButton::clicked, this, &DynQueueWidget::stopQueue);
    actionsLayout->addWidget(_stopButton);
    actionsLayout->addStretch();

    _upButton = new QToolButton(this);
    _upButton->setAutoRaise(true);
    _upButton->setIcon(QIcon::fromTheme(QStringLiteral("go-up")));
    _upButton->setToolTip(tr("Move selected command up"));
    connect(_upButton, &QToolButton::clicked, this, &DynQueueWidget::moveUp);
    actionsLayout->addWidget(_upButton);

    _downButton = new QToolButton(this);
    _downButton->setAutoRaise(true);
    _downButton->setIcon(QIcon::fromTheme(QStringLiteral("go-down")));
    _downButton->setToolTip(tr("Move selected command down"));
    connect(_downButton, &QToolButton::clicked, this, &DynQueueWidget::moveDown);
    actionsLayout->addWidget(_downButton);

    _removeButton = new QPushButton(this);
    _removeButton->setAutoDefault(false);
    _removeButton->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete")));
    _removeButton->setToolTip(tr("Remove selected waiting command"));
    connect(_removeButton, &QPushButton::clicked, this, &DynQueueWidget::removeCommand);
    actionsLayout->addWidget(_removeButton);
    mainLayout->addLayout(actionsLayout);

    _selectionLabel = new QLabel(tr("Select a waiting command to edit"), this);
    _selectionLabel->setWordWrap(true);
    _selectionLabel->setStyleSheet(QStringLiteral("color: palette(mid);"));
    mainLayout->addWidget(_selectionLabel);

    updateControls();
}

void DynQueueWidget::setQueue(const DynQueueItems &items, bool queueActive, bool stopRequested)
{
    QString selectedId;
    if (_list->currentItem() != nullptr) {
        selectedId = _list->currentItem()->data(ItemIdRole).toString();
    }

    _items = items;
    _queueActive = queueActive;
    _stopRequested = stopRequested;
    rebuildList(selectedId);
}

void DynQueueWidget::clearQueue()
{
    _items.clear();
    _queueActive = false;
    _stopRequested = false;
    _list->clear();
    updateControls();
}

QString DynQueueWidget::statePrefix(DynQueueState state)
{
    switch (state) {
    case DynQueueState::Running:
        return QStringLiteral("▶");
    case DynQueueState::Completed:
        return QStringLiteral("✓");
    case DynQueueState::Failed:
        return QStringLiteral("✕");
    case DynQueueState::Stopped:
        return QStringLiteral("—");
    case DynQueueState::Waiting:
        return QStringLiteral("○");
    }
    return QStringLiteral("○");
}

QString DynQueueWidget::stateText(DynQueueState state)
{
    switch (state) {
    case DynQueueState::Running:
        return tr("Running");
    case DynQueueState::Completed:
        return tr("Completed");
    case DynQueueState::Failed:
        return tr("Failed");
    case DynQueueState::Stopped:
        return tr("Stopped");
    case DynQueueState::Waiting:
        return tr("Waiting");
    }
    return tr("Waiting");
}

QString DynQueueWidget::displayText(const DynQueueItem &item)
{
    return QStringLiteral("%1  %2\n%3").arg(statePrefix(item.state), stateText(item.state), item.command);
}

void DynQueueWidget::rebuildList(const QString &selectedId)
{
    _list->blockSignals(true);
    _list->clear();

    int selectedRow = -1;
    for (int i = 0; i < _items.size(); ++i) {
        const auto &item = _items.at(i);
        auto *listItem = new QListWidgetItem(displayText(item), _list);
        listItem->setData(ItemIdRole, item.id);
        listItem->setToolTip(item.command);
        listItem->setSizeHint(QSize(0, 48));
        if (item.id == selectedId) {
            selectedRow = i;
        }
    }

    if (selectedRow < 0 && !_items.isEmpty()) {
        selectedRow = 0;
    }
    if (selectedRow >= 0) {
        _list->setCurrentRow(selectedRow);
    }
    _list->blockSignals(false);
    updateControls();
}

void DynQueueWidget::addCommand()
{
    if (!_queueActive) {
        return;
    }

    bool accepted = false;
    const QString command = QInputDialog::getText(this,
                                                  tr("Add command"),
                                                  tr("Command:"),
                                                  QLineEdit::Normal,
                                                  QString(),
                                                  &accepted);
    if (accepted && !command.trimmed().isEmpty()) {
        Q_EMIT addCommandRequested(command);
    }
}

void DynQueueWidget::stopQueue()
{
    if (_queueActive && !_stopRequested) {
        Q_EMIT stopQueueRequested();
    }
}

void DynQueueWidget::removeCommand()
{
    const int row = _list->currentRow();
    if (row >= 0 && row < _items.size() && _items.at(row).state == DynQueueState::Waiting) {
        Q_EMIT removeCommandRequested(_items.at(row).id);
    }
}

void DynQueueWidget::moveUp()
{
    const int row = _list->currentRow();
    if (row > 1 && row < _items.size() && _items.at(row).state == DynQueueState::Waiting) {
        Q_EMIT moveCommandRequested(_items.at(row).id, -1);
    }
}

void DynQueueWidget::moveDown()
{
    const int row = _list->currentRow();
    if (row >= 1 && row + 1 < _items.size() && _items.at(row).state == DynQueueState::Waiting) {
        Q_EMIT moveCommandRequested(_items.at(row).id, 1);
    }
}

void DynQueueWidget::updateControls()
{
    const int row = _list->currentRow();
    const bool waiting = row >= 0 && row < _items.size() && _items.at(row).state == DynQueueState::Waiting;
    _addButton->setEnabled(_queueActive && !_stopRequested);
    _stopButton->setText(_stopRequested ? tr("Stopping…") : tr("Stop"));
    _stopButton->setEnabled(_queueActive && !_stopRequested);
    _removeButton->setEnabled(waiting && !_stopRequested);
    _upButton->setEnabled(waiting && !_stopRequested && row > 1);
    _downButton->setEnabled(waiting && !_stopRequested && row + 1 < _items.size());

    if (waiting) {
        if (_stopRequested) {
            _selectionLabel->setText(tr("Stopping after the current command"));
        } else {
            _selectionLabel->setText(tr("Selected command: %1").arg(_items.at(row).command));
        }
    } else if (row >= 0 && row < _items.size()) {
        if (_stopRequested) {
            _selectionLabel->setText(tr("Stopping after the current command"));
        } else {
            _selectionLabel->setText(tr("Only waiting commands can be edited"));
        }
    } else if (_stopRequested) {
        _selectionLabel->setText(tr("Stopping after the current command"));
    } else {
        _selectionLabel->setText(tr("Select a waiting command to edit"));
    }
}
