// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "PadOSControl/Widgets/CheckableComboBox.h"

#include <QFrame>
#include <QLineEdit>
#include <QListView>
#include <QVBoxLayout>
#include <QApplication>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QCursor>

CheckableComboBox::CheckableComboBox(QWidget* parent)
    : QComboBox(parent)
{
    m_ItemModel = new QStandardItemModel(0, 1, this);

    QStandardItem* allItem = new QStandardItem("All");
    allItem->setCheckable(true);
    allItem->setCheckState(Qt::Checked);
    allItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
    m_ItemModel->appendRow(allItem);

    QComboBox::addItem("All");

    connect(m_ItemModel, &QStandardItemModel::itemChanged, this, &CheckableComboBox::OnItemChanged);
}

void CheckableComboBox::AddCheckableItem(const QString& text, const QVariant& userData, bool checked)
{
    m_BlockUpdate = true;

    QStandardItem* item = new QStandardItem(text);
    item->setCheckable(true);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
    if (userData.isValid()) {
        item->setData(userData, Qt::UserRole);
    }
    m_ItemModel->appendRow(item);

    m_BlockUpdate = false;
    UpdateAllItemState();
    UpdateDisplayText();
}

void CheckableComboBox::ClearCheckableItems()
{
    m_BlockUpdate = true;
    while (m_ItemModel->rowCount() > FIRST_DATA_ROW) {
        m_ItemModel->removeRow(m_ItemModel->rowCount() - 1);
    }
    m_BlockUpdate = false;
    UpdateAllItemState();
    UpdateDisplayText();
}

bool CheckableComboBox::IsItemChecked(int index) const
{
    const int row = index + FIRST_DATA_ROW;
    if (row >= m_ItemModel->rowCount()) { return false; }
    return m_ItemModel->item(row)->checkState() == Qt::Checked;
}

void CheckableComboBox::SetItemChecked(int index, bool checked)
{
    const int row = index + FIRST_DATA_ROW;
    if (row >= m_ItemModel->rowCount()) { return; }
    m_ItemModel->item(row)->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
}

void CheckableComboBox::SetAllChecked(bool checked)
{
    m_BlockUpdate = true;
    for (int row = FIRST_DATA_ROW; row < m_ItemModel->rowCount(); ++row) {
        m_ItemModel->item(row)->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
    m_ItemModel->item(ALL_ITEM_ROW)->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    m_BlockUpdate = false;
    UpdateDisplayText();
    emit CheckStateChanged();
}

bool CheckableComboBox::IsAllChecked() const
{
    for (int row = FIRST_DATA_ROW; row < m_ItemModel->rowCount(); ++row) {
        if (m_ItemModel->item(row)->checkState() != Qt::Checked) { return false; }
    }
    return true;
}

int CheckableComboBox::GetCheckableItemCount() const
{
    return m_ItemModel->rowCount() - FIRST_DATA_ROW;
}

QString CheckableComboBox::GetItemText(int index) const
{
    const int row = index + FIRST_DATA_ROW;
    if (row >= m_ItemModel->rowCount()) { return {}; }
    return m_ItemModel->item(row)->text();
}

QVariant CheckableComboBox::GetItemUserData(int index) const
{
    const int row = index + FIRST_DATA_ROW;
    if (row >= m_ItemModel->rowCount()) { return {}; }
    return m_ItemModel->item(row)->data(Qt::UserRole);
}

QString CheckableComboBox::GetSearchText() const
{
    return m_SearchText;
}

void CheckableComboBox::SetSearchText(const QString& text)
{
    m_SearchText = text;
    if (m_SearchEdit != nullptr) {
        m_SearchEdit->setText(text);
        FilterRows(text);
    }
}

void CheckableComboBox::OpenPopup()
{
    if (m_PopupOpen) { return; }

    if (m_PopupFrame == nullptr)
    {
        m_PopupFrame = new QFrame(window());
        m_PopupFrame->setFrameShape(QFrame::StyledPanel);
        m_PopupFrame->setFrameShadow(QFrame::Plain);
        m_PopupFrame->setAutoFillBackground(true);

        QVBoxLayout* layout = new QVBoxLayout(m_PopupFrame);
        layout->setContentsMargins(2, 2, 2, 2);
        layout->setSpacing(2);

        m_SearchEdit = new QLineEdit(m_PopupFrame);
        m_SearchEdit->setPlaceholderText("Search...");
        m_SearchEdit->setClearButtonEnabled(true);
        m_SearchEdit->installEventFilter(this);
        layout->addWidget(m_SearchEdit);

        m_ListView = new QListView(m_PopupFrame);
        m_ListView->setModel(m_ItemModel);
        m_ListView->setSelectionMode(QAbstractItemView::NoSelection);
        m_ListView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_ListView->setMouseTracking(true);
        m_ListView->viewport()->installEventFilter(this);
        layout->addWidget(m_ListView);

        connect(m_SearchEdit, &QLineEdit::textEdited, this, [this](const QString& text) {
            m_SearchText = text;
            FilterRows(text);
        });
        connect(m_SearchEdit, &QLineEdit::returnPressed, this, [this]() { ClosePopup(); });
    }

    m_PopupOpen = true;

    m_SearchEdit->setText(m_SearchText);
    FilterRows(m_SearchText);

    const int searchH = m_SearchEdit->sizeHint().height();
    const int itemH   = qMax(m_ListView->sizeHintForRow(0), 18);
    const int visRows = qMin(m_ItemModel->rowCount(), 12);
    const int listH   = visRows * itemH + 4;
    const int popupW  = qMax(width(), 180);
    const int popupH  = searchH + listH + 10;

    QPoint pos = window()->mapFromGlobal(mapToGlobal(QPoint(0, height())));
    if ((pos.y() + popupH) > window()->height()) {
        pos.setY(window()->mapFromGlobal(mapToGlobal(QPoint(0, 0))).y() - popupH);
    }
    if (pos.x() < 0) {
        pos.setX(0);
    }
    if ((pos.x() + popupW) > window()->width()) {
        pos.setX(window()->width() - popupW);
    }

    m_PopupFrame->setGeometry(pos.x(), pos.y(), popupW, popupH);
    m_PopupFrame->show();
    m_PopupFrame->raise();
    m_SearchEdit->setFocus(Qt::OtherFocusReason);

    qApp->removeEventFilter(this);
    qApp->installEventFilter(this);
}

void CheckableComboBox::ClosePopup()
{
    if (!m_PopupOpen) { return; }

    m_PopupOpen = false;
    UpdateDisplayText();

    qApp->removeEventFilter(this);
    if (m_PopupFrame != nullptr) {
        m_PopupFrame->hide();
    }
}

void CheckableComboBox::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        if (m_PopupOpen) {
            ClosePopup();
        } else {
            OpenPopup();
        }
        event->accept();
        return;
    }
    QComboBox::mousePressEvent(event);
}

void CheckableComboBox::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_F4 ||
        (event->key() == Qt::Key_Down && (event->modifiers() & Qt::AltModifier)))
    {
        if (m_PopupOpen) {
            ClosePopup();
        } else {
            OpenPopup();
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && m_PopupOpen)
    {
        ClosePopup();
        event->accept();
        return;
    }
    QComboBox::keyPressEvent(event);
}

bool CheckableComboBox::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_SearchEdit && event->type() == QEvent::KeyPress)
    {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Escape)
        {
            ClosePopup();
            return true;
        }
    }

    if (m_PopupOpen && event->type() == QEvent::MouseButtonPress)
    {
        QWidget* target = qobject_cast<QWidget*>(obj);
        if (target != nullptr &&
            target != m_PopupFrame &&
            !m_PopupFrame->isAncestorOf(target) &&
            target != this &&
            !isAncestorOf(target))
        {
            ClosePopup();
        }
    }

    if (m_ListView != nullptr && obj == m_ListView->viewport())
    {
        if (event->type() == QEvent::MouseButtonPress) {
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease)
        {
            const QPoint clickPos = static_cast<QMouseEvent*>(event)->pos();
            const QModelIndex idx = m_ListView->indexAt(clickPos);
            if (idx.isValid()) {
                ToggleRow(idx.row());
            }
            return true;
        }
        if (event->type() == QEvent::MouseMove)
        {
            const QPoint movePos = static_cast<QMouseEvent*>(event)->pos();
            const QModelIndex idx = m_ListView->indexAt(movePos);
            if (idx.isValid() && !m_ListView->isRowHidden(idx.row())) {
                m_ListView->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect);
            }
            return false;
        }
        if (event->type() == QEvent::Leave)
        {
            const QPoint cursorPos = m_ListView->viewport()->mapFromGlobal(QCursor::pos());
            if (m_ListView->viewport()->rect().contains(cursorPos)) {
                return true;
            }
        }
    }

    return QObject::eventFilter(obj, event);
}

void CheckableComboBox::FilterRows(const QString& text)
{
    m_ListView->setUpdatesEnabled(false);
    m_ListView->setRowHidden(ALL_ITEM_ROW, !text.isEmpty());
    for (int row = FIRST_DATA_ROW; row < m_ItemModel->rowCount(); ++row)
    {
        const bool hidden = !text.isEmpty() &&
                            !m_ItemModel->item(row)->text().contains(text, Qt::CaseInsensitive);
        m_ListView->setRowHidden(row, hidden);
    }
    m_ListView->setUpdatesEnabled(true);
}

void CheckableComboBox::UpdateDisplayText()
{
    const int dataRows = m_ItemModel->rowCount() - FIRST_DATA_ROW;

    QString text;
    if (dataRows == 0)
    {
        text = "All";
    }
    else
    {
        int checkedCount = 0;
        for (int row = FIRST_DATA_ROW; row < m_ItemModel->rowCount(); ++row) {
            if (m_ItemModel->item(row)->checkState() == Qt::Checked) { ++checkedCount; }
        }

        if (checkedCount == dataRows) {
            text = "All";
        } else if (checkedCount == 0) {
            text = "None";
        } else {
            text = QString("%1 selected").arg(checkedCount);
        }
    }

    QComboBox::setItemText(0, text);
}

void CheckableComboBox::UpdateAllItemState()
{
    if (m_BlockUpdate) { return; }

    const int dataRows = m_ItemModel->rowCount() - FIRST_DATA_ROW;
    if (dataRows == 0)
    {
        m_ItemModel->item(ALL_ITEM_ROW)->setCheckState(Qt::Checked);
        return;
    }

    int checkedCount = 0;
    for (int row = FIRST_DATA_ROW; row < m_ItemModel->rowCount(); ++row) {
        if (m_ItemModel->item(row)->checkState() == Qt::Checked) { ++checkedCount; }
    }

    m_BlockUpdate = true;
    if (checkedCount == dataRows) {
        m_ItemModel->item(ALL_ITEM_ROW)->setCheckState(Qt::Checked);
    } else if (checkedCount == 0) {
        m_ItemModel->item(ALL_ITEM_ROW)->setCheckState(Qt::Unchecked);
    } else {
        m_ItemModel->item(ALL_ITEM_ROW)->setCheckState(Qt::PartiallyChecked);
    }
    m_BlockUpdate = false;
}

void CheckableComboBox::ToggleRow(int row)
{
    if (row < 0 || row >= m_ItemModel->rowCount()) { return; }

    if (row == ALL_ITEM_ROW)
    {
        const bool newChecked = m_ItemModel->item(ALL_ITEM_ROW)->checkState() != Qt::Checked;
        SetAllChecked(newChecked);
    }
    else
    {
        QStandardItem* item = m_ItemModel->item(row);
        item->setCheckState((item->checkState() == Qt::Checked) ? Qt::Unchecked : Qt::Checked);
    }
}

void CheckableComboBox::OnItemChanged(QStandardItem* item)
{
    if (m_BlockUpdate) { return; }

    if (m_ItemModel->indexFromItem(item).row() == ALL_ITEM_ROW)
    {
        const bool checked = item->checkState() == Qt::Checked;
        m_BlockUpdate = true;
        for (int row = FIRST_DATA_ROW; row < m_ItemModel->rowCount(); ++row) {
            m_ItemModel->item(row)->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        }
        m_BlockUpdate = false;
    }
    else
    {
        UpdateAllItemState();
    }

    UpdateDisplayText();
    emit CheckStateChanged();
}
