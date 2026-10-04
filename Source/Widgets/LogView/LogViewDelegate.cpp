// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "PadOSControl/Widgets/LogView/LogViewDelegate.h"
#include "PadOSControl/Widgets/LogView/LogViewModel.h"

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

LogViewDelegate::LogViewDelegate(const LogViewModel* model, QObject* parent)
    : QStyledItemDelegate(parent)
    , m_Model(model)
{
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QWidget* LogViewDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem& /*option*/, const QModelIndex& index) const
{
    if (static_cast<LogViewModel::Column>(index.column()) == LogViewModel::Column::Message)
    {
        QPlainTextEdit* editor = new QPlainTextEdit(parent);
        editor->setReadOnly(true);
        editor->setFrameShape(QFrame::NoFrame);
        editor->setFont(QFont("Consolas", 9));
        return editor;
    }

    QLineEdit* editor = new QLineEdit(parent);
    editor->setReadOnly(true);
    return editor;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const
{
    const QString text = index.data(Qt::DisplayRole).toString();

    if (static_cast<LogViewModel::Column>(index.column()) == LogViewModel::Column::Message)
    {
        static_cast<QPlainTextEdit*>(editor)->setPlainText(text);
    }
    else
    {
        static_cast<QLineEdit*>(editor)->setText(text);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewDelegate::updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex& /*index*/) const
{
    editor->setGeometry(option.rect);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewDelegate::setModelData(QWidget* /*editor*/, QAbstractItemModel* /*model*/, const QModelIndex& /*index*/) const
{
    // Read-only editor — no data is written back to the model.
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QSize LogViewDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    if (static_cast<LogViewModel::Column>(index.column()) == LogViewModel::Column::Message)
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        const QString text = index.data(Qt::DisplayRole).toString();
        const QFontMetrics fm(opt.font);
        const int width = qMax(opt.rect.width(), 100);
        const QRect textRect = fm.boundingRect(0, 0, width - 4, 0,
                                               Qt::AlignLeft | Qt::TextWordWrap, text);
        return QSize(width, qMax(textRect.height() + 4, 20));
    }
    return QStyledItemDelegate::sizeHint(option, index);
}
