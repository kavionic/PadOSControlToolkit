// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <QStyledItemDelegate>

class LogViewModel;

class LogViewDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit LogViewDelegate(const LogViewModel* model, QObject* parent = nullptr);

    virtual QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    virtual void     setEditorData(QWidget* editor, const QModelIndex& index) const override;
    virtual void     updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    virtual void     setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override;
    virtual QSize    sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
    const LogViewModel* m_Model = nullptr;
};
