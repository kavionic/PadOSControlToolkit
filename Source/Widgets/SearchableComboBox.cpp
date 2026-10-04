// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "PadOSControl/Widgets/SearchableComboBox.h"

#include <QFrame>
#include <QLineEdit>
#include <QListView>
#include <QVBoxLayout>
#include <QApplication>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QCursor>

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SearchableComboBox::SearchableComboBox(QWidget* parent)
    : QComboBox(parent)
{
    m_ItemModel = new QStandardItemModel(0, 1, this);
    QComboBox::addItem("");
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SearchableComboBox::AddItem(const QString& text, const QVariant& userData)
{
    QStandardItem* item = new QStandardItem(text);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    if (userData.isValid()) {
        item->setData(userData, Qt::UserRole);
    }
    m_ItemModel->appendRow(item);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SearchableComboBox::Clear()
{
    m_ItemModel->removeRows(0, m_ItemModel->rowCount());
    m_SelectedRow = -1;
    QComboBox::setItemText(0, "");
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

int SearchableComboBox::GetItemCount() const
{
    return m_ItemModel->rowCount();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SearchableComboBox::GetItemText(int index) const
{
    if (index < 0 || index >= m_ItemModel->rowCount()) { return {}; }
    return m_ItemModel->item(index)->text();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QVariant SearchableComboBox::GetItemData(int index) const
{
    if (index < 0 || index >= m_ItemModel->rowCount()) { return {}; }
    return m_ItemModel->item(index)->data(Qt::UserRole);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SearchableComboBox::SetCurrentByData(const QVariant& userData)
{
    for (int row = 0; row < m_ItemModel->rowCount(); ++row)
    {
        if (m_ItemModel->item(row)->data(Qt::UserRole) == userData)
        {
            CommitSelection(row);
            return;
        }
    }
    CommitSelection(-1);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SearchableComboBox::OpenPopup()
{
    if (m_PopupOpen) {
        return;
    }

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
        m_ListView->setSelectionMode(QAbstractItemView::SingleSelection);
        m_ListView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_ListView->setMouseTracking(true);
        m_ListView->viewport()->installEventFilter(this);
        layout->addWidget(m_ListView);

        connect(m_SearchEdit, &QLineEdit::textEdited, this, &SearchableComboBox::FilterRows);
        connect(m_SearchEdit, &QLineEdit::returnPressed, this, [this]()
            {
                const QModelIndex current = m_ListView->currentIndex();
                if (current.isValid() && !m_ListView->isRowHidden(current.row())) {
                    CommitSelection(current.row());
                }
                ClosePopup();
            }
        );
    }

    m_PopupOpen = true;

    m_SearchEdit->clear();
    FilterRows({});

    if (m_SelectedRow >= 0 && m_SelectedRow < m_ItemModel->rowCount()) {
        m_ListView->selectionModel()->setCurrentIndex(m_ItemModel->index(m_SelectedRow, 0), QItemSelectionModel::ClearAndSelect);
    }

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

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SearchableComboBox::ClosePopup()
{
    if (!m_PopupOpen) {
        return;
    }

    m_PopupOpen = false;

    qApp->removeEventFilter(this);
    if (m_PopupFrame != nullptr) {
        m_PopupFrame->hide();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SearchableComboBox::mousePressEvent(QMouseEvent* event)
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

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SearchableComboBox::keyPressEvent(QKeyEvent* event)
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

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SearchableComboBox::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_SearchEdit && event->type() == QEvent::KeyPress)
    {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Escape)
        {
            ClosePopup();
            return true;
        }
        if (keyEvent->key() == Qt::Key_Down || keyEvent->key() == Qt::Key_Up)
        {
            const int direction = (keyEvent->key() == Qt::Key_Down) ? 1 : -1;
            const QModelIndex current = m_ListView->currentIndex();
            const int from = current.isValid() ? current.row() : -1;
            const int nextRow = NextVisibleRow(from, direction);
            if (nextRow >= 0) {
                m_ListView->selectionModel()->setCurrentIndex(m_ItemModel->index(nextRow, 0), QItemSelectionModel::ClearAndSelect);
            }
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
            if (idx.isValid() && !m_ListView->isRowHidden(idx.row()))
            {
                CommitSelection(idx.row());
                ClosePopup();
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

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SearchableComboBox::FilterRows(const QString& text)
{
    m_ListView->setUpdatesEnabled(false);
    for (int row = 0; row < m_ItemModel->rowCount(); ++row)
    {
        const bool hidden = !text.isEmpty() &&
                            !m_ItemModel->item(row)->text().contains(text, Qt::CaseInsensitive);
        m_ListView->setRowHidden(row, hidden);
    }
    m_ListView->setUpdatesEnabled(true);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SearchableComboBox::CommitSelection(int row)
{
    m_SelectedRow = row;
    if (row >= 0 && row < m_ItemModel->rowCount()) {
        QComboBox::setItemText(0, m_ItemModel->item(row)->text());
    } else {
        QComboBox::setItemText(0, "");
    }
    emit ItemSelected(row);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

int SearchableComboBox::NextVisibleRow(int from, int direction) const
{
    const int rowCount = m_ItemModel->rowCount();
    if (rowCount == 0) { return -1; }

    int row = from + direction;
    while (row >= 0 && row < rowCount)
    {
        if (!m_ListView->isRowHidden(row)) { return row; }
        row += direction;
    }
    return -1;
}
