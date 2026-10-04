// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include <QString>

class SerialHandler;

class SDCardSyncState
{
public:
    enum class CompletionStatus : int
    {
        Succeeded,
        Failed,
        Canceled
    };

    using StateCompletedDelegate = std::function<void(
        SDCardSyncState& state,
        CompletionStatus status,
        const QString& statusText
    )>;

    using ProgressChangedDelegate = std::function<void(
        SDCardSyncState& state,
        std::optional<float> totalProgress,
        std::optional<float> subProgress,
        const QString& progressText
    )>;

    using StatusTextChangedDelegate = std::function<void(
        SDCardSyncState& state,
        const QString& statusText
    )>;

    struct Context
    {
        SerialHandler* SerialHandler = nullptr;
        int32_t SessionID = -1;
    };

    explicit SDCardSyncState(Context context);
    virtual ~SDCardSyncState();

    virtual void Start() = 0;
    virtual void Cancel();
    virtual QString GetStatusText() const = 0;

protected:
    SerialHandler& GetSerialHandler() const;
    int32_t GetSessionID() const;
    void FinishSucceeded();
    void FinishFailed(const QString& statusText);
    void FinishCanceled();
    void SetProgress(std::optional<float> totalProgress, std::optional<float> subProgress, const QString& progressText);
    void SetStatusText(const QString& statusText);

private:
    friend class SDCardSync;

    void Complete(CompletionStatus status, const QString& statusText);
    static std::optional<float> ClampProgress(std::optional<float> progress);

    StateCompletedDelegate StateCompleted;
    ProgressChangedDelegate ProgressChanged;
    StatusTextChangedDelegate StatusTextChanged;

    Context m_Context;
    bool    m_Completed = false;
};
