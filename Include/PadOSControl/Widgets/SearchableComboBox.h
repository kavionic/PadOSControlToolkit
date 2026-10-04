// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <QComboBox>
#include <QStandardItemModel>

class QFrame;
class QListView;

class SearchableComboBox : public QComboBox
{
    Q_OBJECT

public:
    explicit SearchableComboBox(QWidget* parent = nullptr);

    void AddItem(const QString& text, const QVariant& userData = QVariant());
    void Clear();

    int      GetItemCount() const;
    QString  GetItemText(int index) const;
    QVariant GetItemData(int index) const;

    void SetCurrentByData(const QVariant& userData);

Q_SIGNALS:
    void ItemSelected(int index);

protected:
    void showPopup() override {}
    void hidePopup() override {}
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void OpenPopup();
    void ClosePopup();
    void FilterRows(const QString& text);
    void CommitSelection(int row);
    int  NextVisibleRow(int from, int direction) const;

    QStandardItemModel* m_ItemModel  = nullptr;
    QFrame*             m_PopupFrame = nullptr;
    QLineEdit*          m_SearchEdit = nullptr;
    QListView*          m_ListView   = nullptr;
    bool                m_PopupOpen  = false;
    int                 m_SelectedRow = -1;
};
