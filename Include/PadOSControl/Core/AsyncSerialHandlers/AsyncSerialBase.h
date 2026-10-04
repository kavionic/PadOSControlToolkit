// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <QTimer>

class AsyncSerialBase
{
public:
    AsyncSerialBase();
    virtual ~AsyncSerialBase();

protected:
    void StartTimeout();
    void StopTimeout();

private:
    virtual void HandleTimeout() = 0;

    QTimer m_TimeoutTimer;
};
