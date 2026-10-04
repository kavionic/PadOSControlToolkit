// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2022 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <map>
#include <vector>
#include <QStringList>

#include <QWidget>
#include "ui_FirmwareUpdater.h"

class DeviceSession;
class QFileSystemWatcher;
class QTimer;
enum class MainState : int;

namespace SerialProtocol
{
struct BeginFirmwareUpdateReply;
struct EraseFlashSectionProgress;
struct WriteFlashSectionReply;
struct GetFlashChecksumReply;
}

enum class FirmwareUpdaterState : int
{
    Initializing,
    Idle,
    LoadingFirmware,
    WaitingForBootloader,
    InitiatingUpdate,
    ErasingFlash,
    SendingFirmware,
    CalculatingChecksum
};

struct FlashSection
{
    uint32_t             Address;
    std::vector<uint8_t> Buffer;
};

class FirmwareUpdater : public QWidget
{
    Q_OBJECT

public:
    FirmwareUpdater(QWidget *parent = Q_NULLPTR);
    ~FirmwareUpdater();

    void SetDeviceSession(DeviceSession* deviceSession);

    bool LoadHEXFile(const QString& path);

private slots:
    void SlotMainStateChanged(MainState state);
    void SlotKernelBrowseButtonClicked();
    void SlotFirmwareBrowseButtonClicked();
    void SlotKernelRefreshButtonClicked();
    void SlotFirmwareRefreshButtonClicked();
    void SlotKernelComboChanged();
    void SlotFirmwareComboChanged();
    void SlotFileChanged(const QString& path);
    void SlotRefreshDebounced();
    void SlotUpdateKernelButtonClicked();
    void SlotUpdateFirmwareButtonClicked();
    void SlotUpdateBothButtonClicked();
    void SlotAutoUpdateButtonClicked();
    void SlotCancelButtonClicked();

private:
    void SetState(FirmwareUpdaterState state);
    void StartFlash();
    bool SendNextFirmwareBlock();
    void UpdateKernelStats();
    void UpdateFirmwareStats();
    void UpdateWatchedFiles();
    void LoadRecentKernelPaths();
    void SaveRecentKernelPaths();
    void LoadRecentFirmwarePaths();
    void SaveRecentFirmwarePaths();
    void BeginFlash(const QStringList& imagePaths);
    QStringList GetPendingAutoUpdatePaths() const;
    void UpdateAutoUpdateButtonState();
    void TouchFlashedImageTimestampFiles();

    void HandleBeginFirmwareUpdateReply(const SerialProtocol::BeginFirmwareUpdateReply& packet);
    void HandleEraseFlashSectionProgress(const SerialProtocol::EraseFlashSectionProgress& packet);
    void HandleWriteFlashSectionReply(const SerialProtocol::WriteFlashSectionReply& packet);
    void HandleGetFlashChecksumReply(const SerialProtocol::GetFlashChecksumReply& packet);

    Ui::FirmwareUpdater ui;

    DeviceSession*      m_DeviceSession      = nullptr;
    QFileSystemWatcher*  m_FileWatcher         = nullptr;
    QTimer*              m_StatsDebounceTimer  = nullptr;
    bool                 m_KernelStatsDirty    = false;
    bool                 m_FirmwareStatsDirty  = false;
    bool                 m_FlashHadChecksumError = false;
    QStringList          m_FlashImagePaths;

    FirmwareUpdaterState m_State = FirmwareUpdaterState::Initializing;

    uint32_t    m_SectorBytesSent = 0;
    size_t      m_TotalBytesToSend = 0;
    size_t      m_TotalBytesSent = 0;
    size_t      m_AcknowledgedBytes = 0;
    std::map<uint32_t, std::vector<uint8_t>> m_PendingFlashSections;
    std::map<uint32_t, std::vector<uint8_t>>::const_iterator m_CurrentFlashSection;
};
