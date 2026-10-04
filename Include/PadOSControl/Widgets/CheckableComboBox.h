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

class CheckableComboBox : public QComboBox
{
    Q_OBJECT

public:
    explicit CheckableComboBox(QWidget* parent = nullptr);

    void AddCheckableItem(const QString& text, const QVariant& userData = QVariant(), bool checked = true);
    void ClearCheckableItems();

    bool      IsItemChecked(int index) const;
    void      SetItemChecked(int index, bool checked);
    void      SetAllChecked(bool checked);
    bool      IsAllChecked() const;

    int       GetCheckableItemCount() const;
    QString   GetItemText(int index) const;
    QVariant  GetItemUserData(int index) const;

    QString   GetSearchText() const;
    void      SetSearchText(const QString& text);

Q_SIGNALS:
    void CheckStateChanged();

protected:
    void showPopup() override {}
    void hidePopup() override {}
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    static constexpr int ALL_ITEM_ROW   = 0;
    static constexpr int FIRST_DATA_ROW = 1;

    void OpenPopup();
    void ClosePopup();
    void FilterRows(const QString& text);
    void UpdateDisplayText();
    void UpdateAllItemState();
    void ToggleRow(int row);
    void OnItemChanged(QStandardItem* item);

    QStandardItemModel* m_ItemModel   = nullptr;
    QFrame*             m_PopupFrame  = nullptr;
    QLineEdit*          m_SearchEdit  = nullptr;
    QListView*          m_ListView    = nullptr;
    bool                m_BlockUpdate = false;
    bool                m_PopupOpen   = false;
    QString             m_SearchText;
};
