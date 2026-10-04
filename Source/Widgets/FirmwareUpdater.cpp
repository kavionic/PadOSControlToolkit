// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2022 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include <algorithm>
#include <qprogressbar.h>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QSettings>
#include <QTimer>

#include <KernelSupport/MemoryLayout.h>

#include "PadOSControl/Widgets/FirmwareUpdater.h"
#include "PadOSControl/Core/DeviceSession.h"

#include <SerialConsole/BootloaderMessages.h>
#undef ERROR
#include <Utils/LogSeverity.h>

#include "PadOSControl/Core/HashCalculator.h"

static constexpr uint32_t INTERNAL_FLASH_WORD_SIZE = 32;

struct HexFileStats
{
    size_t iFlashBytes = 0;
    size_t eFlashBytes = 0;
    bool   valid       = false;
};

static HexFileStats ComputeHexStats(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    HexFileStats stats;
    stats.valid = true;
    uint32_t extendedAddress = 0;
    for (;;)
    {
        QByteArray line = file.readLine();
        if (line.isEmpty()) { break; }
        if (line[0] != ':') { continue; }

        uint32_t dataLength = strtol(line.mid(1, 2).data(), nullptr, 16);
        uint32_t recordAddr = strtol(line.mid(3, 4).data(), nullptr, 16) | extendedAddress;
        uint32_t recordType = strtol(line.mid(7, 2).data(), nullptr, 16);

        if (recordType == 0)
        {
            if (recordAddr >= IFLASH_ADDR && recordAddr < IFLASH_ADDR + IFLASH_SIZE) {
                stats.iFlashBytes += dataLength;
            } else if (recordAddr >= QSPI_START_ADDRESS && recordAddr < QSPI_START_ADDRESS + QSPI_SIZE) {
                stats.eFlashBytes += dataLength;
            }
        }
        else if (recordType == 1) { break; }
        else if (recordType == 4) { extendedAddress = strtol(line.mid(9, 4).data(), nullptr, 16) << 16; }
    }
    return stats;
}

static QString FormatBytes(size_t bytes)
{
    if (bytes < 1024) {
        return QString("%1 B").arg(bytes);
    } else if (bytes < 1024 * 1024) {
        return QString("%1 KB").arg(double(bytes) / 1024.0, 0, 'f', 1);
    } else {
        return QString("%1 MB").arg(double(bytes) / (1024.0 * 1024.0), 0, 'f', 2);
    }
}

static QString FormatFlashStats(size_t used, size_t max)
{
    const double pct = (max > 0) ? (100.0 * double(used) / double(max)) : 0.0;
    return QString("%1 / %2 (%3%)").arg(FormatBytes(used), FormatBytes(max)).arg(pct, 0, 'f', 1);
}

static QString FormatFlashStatsPlaceholder(size_t max)
{
    return QString("___ / %1 (??%)").arg(FormatBytes(max));
}

static QString GetFlashTimestampPath(const QString& imagePath)
{
    return imagePath + ".flashts";
}

static bool IsFlashImageNewerThanTimestamp(const QString& imagePath)
{
    if (imagePath.isEmpty())
    {
        return false;
    }

    const QFileInfo imageInfo(imagePath);
    if (!imageInfo.exists() || !imageInfo.isFile())
    {
        return false;
    }

    const QFileInfo timestampInfo(GetFlashTimestampPath(imagePath));
    if (!timestampInfo.exists())
    {
        return true;
    }

    return imageInfo.lastModified() > timestampInfo.lastModified();
}

static bool TouchFlashTimestampFile(const QString& imagePath)
{
    const QFileInfo imageInfo(imagePath);
    if (!imageInfo.exists() || !imageInfo.isFile())
    {
        return false;
    }

    QFile timestampFile(GetFlashTimestampPath(imagePath));
    if (!timestampFile.open(QIODevice::WriteOnly))
    {
        return false;
    }

    const bool timestampUpdated = timestampFile.setFileTime(imageInfo.lastModified(), QFileDevice::FileModificationTime);
    timestampFile.close();

    return timestampUpdated;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

FirmwareUpdater::FirmwareUpdater(QWidget *parent)
    : QWidget(parent)
{
    ui.setupUi(this);

    const QStyle* style = QApplication::style();
    ui.m_KernelRefreshButton->setIcon(style->standardIcon(QStyle::SP_BrowserReload));
    ui.m_FirmwareRefreshButton->setIcon(style->standardIcon(QStyle::SP_BrowserReload));

    connect(ui.m_KernelBrowseButton,   &QAbstractButton::clicked, this, &FirmwareUpdater::SlotKernelBrowseButtonClicked);
    connect(ui.m_FirmwareBrowseButton, &QAbstractButton::clicked, this, &FirmwareUpdater::SlotFirmwareBrowseButtonClicked);
    connect(ui.m_KernelRefreshButton,  &QAbstractButton::clicked, this, &FirmwareUpdater::SlotKernelRefreshButtonClicked);
    connect(ui.m_FirmwareRefreshButton,&QAbstractButton::clicked, this, &FirmwareUpdater::SlotFirmwareRefreshButtonClicked);
    connect(ui.m_UpdateKernelButton,   &QAbstractButton::clicked, this, &FirmwareUpdater::SlotUpdateKernelButtonClicked);
    connect(ui.m_UpdateFirmwareButton, &QAbstractButton::clicked, this, &FirmwareUpdater::SlotUpdateFirmwareButtonClicked);
    connect(ui.m_UpdateBothButton,     &QAbstractButton::clicked, this, &FirmwareUpdater::SlotUpdateBothButtonClicked);
    connect(ui.m_AutoUpdateButton,     &QAbstractButton::clicked, this, &FirmwareUpdater::SlotAutoUpdateButtonClicked);
    connect(ui.m_CancelButton,         &QAbstractButton::clicked, this, &FirmwareUpdater::SlotCancelButtonClicked);

    m_FileWatcher = new QFileSystemWatcher(this);
    connect(m_FileWatcher, &QFileSystemWatcher::fileChanged, this, &FirmwareUpdater::SlotFileChanged);

    m_StatsDebounceTimer = new QTimer(this);
    m_StatsDebounceTimer->setSingleShot(true);
    m_StatsDebounceTimer->setInterval(500);
    connect(m_StatsDebounceTimer, &QTimer::timeout, this, &FirmwareUpdater::SlotRefreshDebounced);

    LoadRecentKernelPaths();
    LoadRecentFirmwarePaths();

    connect(ui.m_KernelPathCombo,   &QComboBox::currentIndexChanged, this, &FirmwareUpdater::SlotKernelComboChanged);
    connect(ui.m_FirmwarePathCombo, &QComboBox::currentIndexChanged, this, &FirmwareUpdater::SlotFirmwareComboChanged);

    UpdateKernelStats();
    UpdateFirmwareStats();
    UpdateWatchedFiles();

    SetState(FirmwareUpdaterState::Idle);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

FirmwareUpdater::~FirmwareUpdater()
{
    if (m_DeviceSession != nullptr) {
        m_DeviceSession->GetSerialHandler().UnregisterAllPacketHandlers(this);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::SetDeviceSession(DeviceSession* deviceSession)
{
    Q_ASSERT(deviceSession != nullptr);
    Q_ASSERT(m_DeviceSession == nullptr);
    m_DeviceSession = deviceSession;
    connect(m_DeviceSession, &DeviceSession::SignalMainStateChanged, this, &FirmwareUpdater::SlotMainStateChanged);

    m_DeviceSession->GetSerialHandler().RegisterPacketHandler<SerialProtocol::BeginFirmwareUpdateReply>(this, &FirmwareUpdater::HandleBeginFirmwareUpdateReply);
    m_DeviceSession->GetSerialHandler().RegisterPacketHandler<SerialProtocol::EraseFlashSectionProgress>(this, &FirmwareUpdater::HandleEraseFlashSectionProgress);
    m_DeviceSession->GetSerialHandler().RegisterPacketHandler<SerialProtocol::WriteFlashSectionReply>(this, &FirmwareUpdater::HandleWriteFlashSectionReply);
    m_DeviceSession->GetSerialHandler().RegisterPacketHandler<SerialProtocol::GetFlashChecksumReply>(this, &FirmwareUpdater::HandleGetFlashChecksumReply);
    SlotMainStateChanged(m_DeviceSession->GetMainState());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool FirmwareUpdater::LoadHEXFile(const QString& path)
{
    QFile file(path);

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return false;
    }

    std::vector<uint8_t> buffer;
    uint32_t currentAddress = ~0;
    uint32_t startAddress = ~0;
    uint32_t extendedAddress = 0;
    for (;;)
    {
        QByteArray line = file.readLine();
        if (line.isEmpty())
        {
            break;
        }
        if (line[0] == ':')
        {
            uint32_t dataLength     = strtol(line.mid(1, 2).data(), nullptr, 16);
            uint32_t recordAddress  = strtol(line.mid(3, 4).data(), nullptr, 16) | extendedAddress;
            uint32_t recordType     = strtol(line.mid(7, 2).data(), nullptr, 16);

            if (recordType == 0)
            {
                if (recordAddress != currentAddress)
                {
                    if ((recordAddress - currentAddress) < 4096)
                    {
                        while (currentAddress < recordAddress)
                        {
                            buffer.push_back(0xff);
                            currentAddress++;
                        }
                    }
                    else
                    {
                        if (!buffer.empty())
                        {
                            while ((buffer.size() % INTERNAL_FLASH_WORD_SIZE) != 0) buffer.push_back(0xff);
                            m_TotalBytesToSend += buffer.size();
                            m_PendingFlashSections[startAddress].swap(buffer);
                        }
                        currentAddress = recordAddress;
                        startAddress = recordAddress;
                    }
                }
                for (uint32_t i = 0; i < dataLength; ++i)
                {
                    uint8_t data = strtol(line.mid(9+i*2, 2).data(), nullptr, 16);
                    buffer.push_back(data);
                }
                currentAddress += dataLength;
            }
            else if (recordType == 1) // End of file.
            {
                break;
            }
            else if (recordType == 4) // Extended linear address record.
            {
                extendedAddress = strtol(line.mid(9, 4).data(), nullptr, 16) << 16;
            }
            else
            {
                printf("Unknown HEX record: %d\n", recordType);
            }
        }
    }
    if (!buffer.empty())
    {
        while ((buffer.size() % INTERNAL_FLASH_WORD_SIZE) != 0) buffer.push_back(0xff);
        m_TotalBytesToSend += buffer.size();
        m_PendingFlashSections[startAddress].swap(buffer);
    }
    m_DeviceSession->AddLogMessage(PLogSeverity::INFO_LOW_VOL, "HEX file loaded: {}", path.toStdString());
    for (const auto& i : m_PendingFlashSections)
    {
        m_DeviceSession->AddLogMessage(PLogSeverity::INFO_HIGH_VOL, "    {:#08x} : {:#08x} ({:.2f}KB)", i.first, i.first + i.second.size(), double(i.second.size()) / 1024.0);
    }
    return true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::UpdateWatchedFiles()
{
    const QStringList watched = m_FileWatcher->files();
    if (!watched.isEmpty()) {
        m_FileWatcher->removePaths(watched);
    }
    const QString kernelPath   = ui.m_KernelPathCombo->currentText();
    const QString firmwarePath = ui.m_FirmwarePathCombo->currentText();
    if (!kernelPath.isEmpty())   m_FileWatcher->addPath(kernelPath);
    if (!firmwarePath.isEmpty()) m_FileWatcher->addPath(firmwarePath);

    const QString kernelTimestampPath = GetFlashTimestampPath(kernelPath);
    const QString firmwareTimestampPath = GetFlashTimestampPath(firmwarePath);
    if (QFileInfo::exists(kernelTimestampPath))
    {
        m_FileWatcher->addPath(kernelTimestampPath);
    }
    if (QFileInfo::exists(firmwareTimestampPath))
    {
        m_FileWatcher->addPath(firmwareTimestampPath);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::SlotKernelRefreshButtonClicked()
{
    UpdateKernelStats();
    UpdateAutoUpdateButtonState();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::SlotFirmwareRefreshButtonClicked()
{
    UpdateFirmwareStats();
    UpdateAutoUpdateButtonState();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::SlotFileChanged(const QString& path)
{
    // Re-add in case the build tool deleted and recreated the file.
    m_FileWatcher->addPath(path);

    if (path == ui.m_KernelPathCombo->currentText())   m_KernelStatsDirty  = true;
    if (path == ui.m_FirmwarePathCombo->currentText()) m_FirmwareStatsDirty = true;

    m_StatsDebounceTimer->start();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::SlotRefreshDebounced()
{
    // Re-watch both paths - the file may have been recreated since SlotFileChanged fired.
    UpdateWatchedFiles();

    if (m_KernelStatsDirty)
    {
        m_KernelStatsDirty = false;
        UpdateKernelStats();
    }
    if (m_FirmwareStatsDirty)
    {
        m_FirmwareStatsDirty = false;
        UpdateFirmwareStats();
    }

    UpdateAutoUpdateButtonState();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::UpdateKernelStats()
{
    const QString path = ui.m_KernelPathCombo->currentText();
    if (path.isEmpty())
    {
        ui.m_KernelStatsLabel->setText("");
        return;
    }
    ui.m_KernelStatsLabel->setText(
        QString("IFlash: %1   EFlash: %2")
            .arg(FormatFlashStatsPlaceholder(IFLASH_KRN_SIZE))
            .arg(FormatFlashStatsPlaceholder(SDRAM_KRN_TEXT_SIZE + SDRAM_KRN_DATA_SIZE)));
    ui.m_KernelStatsLabel->repaint();

    const HexFileStats stats = ComputeHexStats(path);
    if (!stats.valid)
    {
        ui.m_KernelStatsLabel->setText("(file not found)");
        return;
    }
    ui.m_KernelStatsLabel->setText(
        QString("IFlash: %1   EFlash: %2")
            .arg(FormatFlashStats(stats.iFlashBytes, IFLASH_KRN_SIZE))
            .arg(FormatFlashStats(stats.eFlashBytes, SDRAM_KRN_TEXT_SIZE + SDRAM_KRN_DATA_SIZE)));
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::UpdateFirmwareStats()
{
    const QString path = ui.m_FirmwarePathCombo->currentText();
    if (path.isEmpty())
    {
        ui.m_FirmwareStatsLabel->setText("");
        return;
    }
    ui.m_FirmwareStatsLabel->setText(
        QString("IFlash: %1   EFlash: %2")
            .arg(FormatFlashStatsPlaceholder(IFLASH_APP_SIZE))
            .arg(FormatFlashStatsPlaceholder(SDRAM_APP_TEXT_SIZE + SDRAM_APP_DATA_SIZE)));
    ui.m_FirmwareStatsLabel->repaint();

    const HexFileStats stats = ComputeHexStats(path);
    if (!stats.valid)
    {
        ui.m_FirmwareStatsLabel->setText("(file not found)");
        return;
    }
    ui.m_FirmwareStatsLabel->setText(
        QString("IFlash: %1   EFlash: %2")
            .arg(FormatFlashStats(stats.iFlashBytes, IFLASH_APP_SIZE))
            .arg(FormatFlashStats(stats.eFlashBytes, SDRAM_APP_TEXT_SIZE + SDRAM_APP_DATA_SIZE)));
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::SlotKernelComboChanged()
{
    SaveRecentKernelPaths();
    UpdateKernelStats();
    UpdateWatchedFiles();
    UpdateAutoUpdateButtonState();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::SlotFirmwareComboChanged()
{
    SaveRecentFirmwarePaths();
    UpdateFirmwareStats();
    UpdateWatchedFiles();
    UpdateAutoUpdateButtonState();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::SlotMainStateChanged(MainState state)
{
    if (state == MainState::ConnectedBootloader)
    {
        if (m_State == FirmwareUpdaterState::WaitingForBootloader)
        {
            SetState(FirmwareUpdaterState::InitiatingUpdate);
            m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::BeginFirmwareUpdate>();
        }
    }
    else if (state == MainState::ConnectedApplication)
    {
        SetState(FirmwareUpdaterState::Idle);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::SlotKernelBrowseButtonClicked()
{
    QString path = QFileDialog::getOpenFileName(this, "Select Kernel HEX File", ui.m_KernelPathCombo->currentText(), "HEX Files (*.hex);;All Files (*)");
    if (path.isEmpty()) {
        return;
    }
    int existingIndex = ui.m_KernelPathCombo->findText(path);
    if (existingIndex != -1) {
        ui.m_KernelPathCombo->removeItem(existingIndex);
    }
    ui.m_KernelPathCombo->insertItem(0, path);
    while (ui.m_KernelPathCombo->count() > 10) {
        ui.m_KernelPathCombo->removeItem(ui.m_KernelPathCombo->count() - 1);
    }
    ui.m_KernelPathCombo->setCurrentIndex(0);
    SaveRecentKernelPaths();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::SlotFirmwareBrowseButtonClicked()
{
    QString path = QFileDialog::getOpenFileName(this, "Select Firmware HEX File", ui.m_FirmwarePathCombo->currentText(), "HEX Files (*.hex);;All Files (*)");
    if (path.isEmpty()) {
        return;
    }
    int existingIndex = ui.m_FirmwarePathCombo->findText(path);
    if (existingIndex != -1) {
        ui.m_FirmwarePathCombo->removeItem(existingIndex);
    }
    ui.m_FirmwarePathCombo->insertItem(0, path);
    while (ui.m_FirmwarePathCombo->count() > 10) {
        ui.m_FirmwarePathCombo->removeItem(ui.m_FirmwarePathCombo->count() - 1);
    }
    ui.m_FirmwarePathCombo->setCurrentIndex(0);
    SaveRecentFirmwarePaths();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::SlotUpdateKernelButtonClicked()
{
    BeginFlash({ ui.m_KernelPathCombo->currentText() });
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::SlotUpdateFirmwareButtonClicked()
{
    BeginFlash({ ui.m_FirmwarePathCombo->currentText() });
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::SlotUpdateBothButtonClicked()
{
    BeginFlash({ ui.m_KernelPathCombo->currentText(), ui.m_FirmwarePathCombo->currentText() });
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::SlotAutoUpdateButtonClicked()
{
    const QStringList imagePaths = GetPendingAutoUpdatePaths();
    if (!imagePaths.isEmpty())
    {
        BeginFlash(imagePaths);
    }
    else
    {
        UpdateAutoUpdateButtonState();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::BeginFlash(const QStringList& imagePaths)
{
    if (imagePaths.isEmpty()) {
        return;
    }

    m_DeviceSession->AddLogMessage(PLogSeverity::INFO_LOW_VOL, "Start firmware update:");

    for (const QString& imagePath : imagePaths) {
        m_DeviceSession->AddLogMessage(PLogSeverity::INFO_LOW_VOL, "    HEX file: {}", imagePath.toStdString());
    }

    SetState(FirmwareUpdaterState::LoadingFirmware);
    m_FlashHadChecksumError = false;

    assert(m_TotalBytesToSend == 0);
    assert(m_TotalBytesSent == 0);
    assert(m_PendingFlashSections.empty());

    for (const QString& imagePath : imagePaths)
    {
        if (!LoadHEXFile(imagePath))
        {
            SetState(FirmwareUpdaterState::Idle);
            return;
        }
    }
    m_FlashImagePaths = imagePaths;
    StartFlash();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QStringList FirmwareUpdater::GetPendingAutoUpdatePaths() const
{
    QStringList imagePaths;
    const QString kernelPath = ui.m_KernelPathCombo->currentText();
    const QString firmwarePath = ui.m_FirmwarePathCombo->currentText();

    if (IsFlashImageNewerThanTimestamp(kernelPath))
    {
        imagePaths.append(kernelPath);
    }
    if (IsFlashImageNewerThanTimestamp(firmwarePath))
    {
        imagePaths.append(firmwarePath);
    }
    return imagePaths;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::UpdateAutoUpdateButtonState()
{
    QString updateFlags;
    if (IsFlashImageNewerThanTimestamp(ui.m_FirmwarePathCombo->currentText()))
    {
        updateFlags.append("F");
    }
    if (IsFlashImageNewerThanTimestamp(ui.m_KernelPathCombo->currentText()))
    {
        updateFlags.append("K");
    }

    ui.m_AutoUpdateButton->setText(updateFlags.isEmpty() ? "Auto Update" : QString("Auto Update (%1)").arg(updateFlags));
    ui.m_AutoUpdateButton->setEnabled(m_State == FirmwareUpdaterState::Idle && !updateFlags.isEmpty());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::TouchFlashedImageTimestampFiles()
{
    for (const QString& imagePath : m_FlashImagePaths)
    {
        if (!TouchFlashTimestampFile(imagePath))
        {
            m_DeviceSession->AddLogMessage(PLogSeverity::ERROR, "Failed to update flash timestamp: {}", GetFlashTimestampPath(imagePath).toStdString());
        }
    }
    UpdateWatchedFiles();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::SlotCancelButtonClicked()
{
    if (m_State == FirmwareUpdaterState::WaitingForBootloader || QMessageBox::warning(this, "WARNING!", "Are you sure you want to cancel the firmware upgrade? Doing so will leave the device in a non-usable state until a new firmware image have been uploaded.", QMessageBox::StandardButtons(QMessageBox::Yes | QMessageBox::No)) == QMessageBox::Yes)
    {
        SetState(FirmwareUpdaterState::Idle);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::StartFlash()
{
    if (m_DeviceSession->GetMainState() == MainState::ConnectedBootloader)
    {
        SetState(FirmwareUpdaterState::InitiatingUpdate);
        m_DeviceSession->AddLogMessage(PLogSeverity::INFO_LOW_VOL, "Begin firmware update");
        m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::BeginFirmwareUpdate>();
    }
    else
    {
        SetState(FirmwareUpdaterState::WaitingForBootloader);
        m_DeviceSession->AddLogMessage(PLogSeverity::INFO_LOW_VOL, "Initiate firmware update");
        m_DeviceSession->SetExpectedDeviceMode(SerialProtocol::ProbeDeviceType::Bootloader);
        m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::InitiateFirmwareUpdate>();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::SetState(FirmwareUpdaterState state)
{
    if (state != m_State)
    {
        m_State = state;

        switch (m_State)
        {
            case FirmwareUpdaterState::Idle:
                ui.m_StatusView->setText("Idle.");
                ui.m_ProgressBar->setValue(0);

                m_TotalBytesToSend = 0;
                m_TotalBytesSent = 0;
                m_PendingFlashSections.clear();
                m_CurrentFlashSection = m_PendingFlashSections.end();
                break;
            case FirmwareUpdaterState::LoadingFirmware:
                ui.m_StatusView->setText("Loading firmware...");
                break;
            case FirmwareUpdaterState::WaitingForBootloader:
                ui.m_ProgressBar->setValue(0);
                ui.m_StatusView->setText("Waiting for device...");
                break;
            case FirmwareUpdaterState::InitiatingUpdate:
                ui.m_ProgressBar->setValue(0);
                ui.m_StatusView->setText("Preparing for firmware update...");
                break;
            case FirmwareUpdaterState::ErasingFlash:
                ui.m_StatusView->setText("Erasing old firmware...");
                break;
            case FirmwareUpdaterState::SendingFirmware:
                ui.m_StatusView->setText("Writing firmware...");
                break;
            case  FirmwareUpdaterState::CalculatingChecksum:
                ui.m_StatusView->setText("Calculating checksum...");
                break;
            default:
                break;
        }
        const bool idle = m_State == FirmwareUpdaterState::Idle;
        ui.m_UpdateKernelButton->setEnabled(idle);
        ui.m_UpdateFirmwareButton->setEnabled(idle);
        ui.m_UpdateBothButton->setEnabled(idle);
        UpdateAutoUpdateButtonState();
        ui.m_CancelButton->setEnabled(!idle);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::LoadRecentKernelPaths()
{
    QSettings settings;
    QStringList paths = settings.value("firmware/recentKernelPaths").toStringList();
    ui.m_KernelPathCombo->clear();
    for (const QString& path : paths) {
        ui.m_KernelPathCombo->addItem(path);
    }
    ui.m_KernelPathCombo->setCurrentIndex(settings.value("firmware/recentKernelPaths/selection", 0).toInt());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::SaveRecentKernelPaths()
{
    QStringList paths;
    for (int i = 0; i < ui.m_KernelPathCombo->count(); ++i) {
        paths.append(ui.m_KernelPathCombo->itemText(i));
    }
    QSettings settings;
    settings.setValue("firmware/recentKernelPaths", paths);
    settings.setValue("firmware/recentKernelPaths/selection", ui.m_KernelPathCombo->currentIndex());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::LoadRecentFirmwarePaths()
{
    QSettings settings;
    QStringList paths = settings.value("firmware/recentFirmwarePaths").toStringList();
    ui.m_FirmwarePathCombo->clear();
    for (const QString& path : paths) {
        ui.m_FirmwarePathCombo->addItem(path);
    }
    ui.m_FirmwarePathCombo->setCurrentIndex(settings.value("firmware/recentFirmwarePaths/selection", 0).toInt());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::SaveRecentFirmwarePaths()
{
    QStringList paths;
    for (int i = 0; i < ui.m_FirmwarePathCombo->count(); ++i) {
        paths.append(ui.m_FirmwarePathCombo->itemText(i));
    }
    QSettings settings;
    settings.setValue("firmware/recentFirmwarePaths", paths);
    settings.setValue("firmware/recentFirmwarePaths/selection", ui.m_FirmwarePathCombo->currentIndex());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool FirmwareUpdater::SendNextFirmwareBlock()
{
    if (m_CurrentFlashSection == m_PendingFlashSections.end()) {
        return false;
    }

    const std::vector<uint8_t>& buffer = m_CurrentFlashSection->second;
    const uint32_t length = std::min(SerialProtocol::WriteFlashSection::SegmentSize, uint32_t(buffer.size()) - m_SectorBytesSent);

    if (length == 0) {
        return false;
    }
    const uint32_t address = m_CurrentFlashSection->first;

    m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::WriteFlashSection>(buffer.data() + m_SectorBytesSent, address + m_SectorBytesSent, length);
    m_SectorBytesSent += length;

    return true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::HandleBeginFirmwareUpdateReply(const SerialProtocol::BeginFirmwareUpdateReply& packet)
{
    if (m_State == FirmwareUpdaterState::InitiatingUpdate && !m_PendingFlashSections.empty())
    {
        m_CurrentFlashSection = m_PendingFlashSections.begin();

        SetState(FirmwareUpdaterState::ErasingFlash);
        ui.m_ProgressBar->setMaximum(m_TotalBytesToSend);
        m_DeviceSession->AddLogMessage(PLogSeverity::INFO_HIGH_VOL, "Erase flash section {:#08x} : {:#08x} ({:.2f}KB)", m_CurrentFlashSection->first, m_CurrentFlashSection->first + m_CurrentFlashSection->second.size(), double(m_CurrentFlashSection->second.size()) / 1024.0);
        m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::EraseFlashSection>(m_CurrentFlashSection->first, m_CurrentFlashSection->second.size());
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::HandleEraseFlashSectionProgress(const SerialProtocol::EraseFlashSectionProgress& packet)
{
    if (m_State != FirmwareUpdaterState::ErasingFlash || m_CurrentFlashSection == m_PendingFlashSections.end()) {
        return;
    }

    if (packet.BytesErased >= m_CurrentFlashSection->second.size())
    {
        m_TotalBytesSent += m_CurrentFlashSection->second.size();
        ui.m_ProgressBar->setValue(m_TotalBytesSent);

        if (++m_CurrentFlashSection != m_PendingFlashSections.end())
        {
            m_DeviceSession->AddLogMessage(PLogSeverity::INFO_HIGH_VOL, "Erase flash section {:#08x} : {:#08x} ({:.2f}KB)", m_CurrentFlashSection->first, m_CurrentFlashSection->first + m_CurrentFlashSection->second.size(), double(m_CurrentFlashSection->second.size()) / 1024.0);
            m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::EraseFlashSection>(m_CurrentFlashSection->first, m_CurrentFlashSection->second.size());
        }
        else
        {
            m_SectorBytesSent = 0;
            m_TotalBytesSent = 0;
            m_CurrentFlashSection = m_PendingFlashSections.begin();

            SetState(FirmwareUpdaterState::SendingFirmware);
            ui.m_ProgressBar->setValue(0);
            SendNextFirmwareBlock();
        }
    }
    else
    {
        ui.m_ProgressBar->setValue(m_TotalBytesSent + packet.BytesErased);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::HandleWriteFlashSectionReply(const SerialProtocol::WriteFlashSectionReply& packet)
{
    if (m_State != FirmwareUpdaterState::SendingFirmware || m_CurrentFlashSection == m_PendingFlashSections.end()) {
        return;
    }
    const std::vector<uint8_t>* buffer = &m_CurrentFlashSection->second;

    uint32_t acknowledgedBytes = packet.StartAddress + packet.Length - m_CurrentFlashSection->first;

    if (acknowledgedBytes == m_AcknowledgedBytes) {
        return;
    }

    m_AcknowledgedBytes = acknowledgedBytes;
    ui.m_ProgressBar->setValue(m_TotalBytesSent + m_AcknowledgedBytes);

    if (m_AcknowledgedBytes != buffer->size())
    {
        SendNextFirmwareBlock();
    }
    else
    {
        SetState(FirmwareUpdaterState::CalculatingChecksum);
        m_DeviceSession->AddLogMessage(PLogSeverity::INFO_HIGH_VOL, "Get flash checksum {:#08x} : {:#08x} ({:.2f}KB)", m_CurrentFlashSection->first, m_CurrentFlashSection->first + m_CurrentFlashSection->second.size(), double(m_CurrentFlashSection->second.size()) / 1024.0);
        m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::GetFlashChecksum>(m_CurrentFlashSection->first, m_CurrentFlashSection->second.size());
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FirmwareUpdater::HandleGetFlashChecksumReply(const SerialProtocol::GetFlashChecksumReply& packet)
{
    if (m_State != FirmwareUpdaterState::CalculatingChecksum || m_CurrentFlashSection == m_PendingFlashSections.end()) {
        return;
    }

    HashCalculator crcCalc(HashAlgorithm::CRC32);
    crcCalc.AddData(m_CurrentFlashSection->second.data(), m_CurrentFlashSection->second.size());

    if (packet.CRC32 != crcCalc.Finalize())
    {
        m_FlashHadChecksumError = true;
        if (QMessageBox::critical(this, "ERROR!", QString("Failed to write firmware image to %1 flash. Do you want to continue?").arg("internal"), QMessageBox::StandardButtons(QMessageBox::Yes | QMessageBox::No)) == QMessageBox::No)
        {
            SetState(FirmwareUpdaterState::Idle);
            m_DeviceSession->SetExpectedDeviceMode(SerialProtocol::ProbeDeviceType::Application);
            return;
        }
    }

    ++m_CurrentFlashSection;
    m_TotalBytesSent += m_SectorBytesSent;
    m_SectorBytesSent = 0;
    if (SendNextFirmwareBlock())
    {
        SetState(FirmwareUpdaterState::SendingFirmware);
    }
    else
    {
        m_DeviceSession->AddLogMessage(PLogSeverity::INFO_LOW_VOL, "End firmware update");
        if (!m_FlashHadChecksumError)
        {
            TouchFlashedImageTimestampFiles();
        }
        m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::EndFirmwareUpdate>();
        SetState(FirmwareUpdaterState::Idle);
        m_DeviceSession->SetExpectedDeviceMode(SerialProtocol::ProbeDeviceType::Application);
        return;
    }
}
