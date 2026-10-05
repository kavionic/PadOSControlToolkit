// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "PadOSControl/Widgets/ContentsScrollArea.h"

#include <QEvent>
#include <QScrollBar>

ContentsScrollArea::ContentsScrollArea(QWidget* parent)
    : QScrollArea(parent)
{
    setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
}

QSize ContentsScrollArea::sizeHint() const
{
    // QScrollArea's override only accounts for scrollbars with ScrollBarAlwaysOn.
    return QAbstractScrollArea::sizeHint();
}

bool ContentsScrollArea::eventFilter(QObject* watched, QEvent* event)
{
    const bool handled = QScrollArea::eventFilter(watched, event);
    if ((watched == verticalScrollBar() || watched == horizontalScrollBar())
        && (event->type() == QEvent::Show || event->type() == QEvent::Hide)) {
        updateGeometry();
    }
    return handled;
}
