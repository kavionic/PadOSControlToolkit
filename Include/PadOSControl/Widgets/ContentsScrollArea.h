// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <QScrollArea>

class ContentsScrollArea : public QScrollArea
{
public:
    explicit ContentsScrollArea(QWidget* parent = nullptr);

    QSize sizeHint() const override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};
