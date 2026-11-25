#pragma once

#include "common/AppCommon.hpp"
#include "core/DeviceScanner.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hdm {

struct InfFileEntry {
    std::string fileName;
    std::string relativePath;
    std::wstring fullPath;
    uint64_t sizeBytes{0};
};

struct LocalInstallSummary {
    size_t totalFiles{0};
    size_t installedCount{0};
    size_t stagedOnlyCount{0};
    size_t failedCount{0};
    bool rebootRequired{false};
};

struct DriverStorePackageEntry {
    std::string oemInfName;        // e.g. "oem13.inf"
    std::string originalInfName;   // e.g. "rt25cx21x64.inf"
    std::string provider;          // e.g. "Realtek"
    std::string className;         // e.g. "Net"
    std::string driverVersion;     // e.g. "1125.027.50.0919"
    std::string driverDate;        // e.g. "09/19/2025"
    std::string activeDeviceName;  // e.g. "Realtek Gaming 2.5GbE Family Controller"
    std::wstring storeInfPath;     // Full path to .inf inside FileRepository
    std::wstring storeFolderPath;  // Full path to package folder inside FileRepository
    uint64_t sizeBytes{0};
    bool inUse{false};
    bool isOldDuplicate{false};
    bool selected{false};
};

struct DriverStoreSnapshot {
    std::vector<DriverStorePackageEntry> packages;
    uint64_t totalStoreBytes{0};
    uint64_t reclaimableDuplicateBytes{0};
    size_t activeCount{0};
    size_t duplicateCount{0};
    bool valid{false};
};

class LocalDriverInstaller {
public:
    using FolderPickedCallback = std::function<void(bool selected, const std::wstring& folderPath)>;
    using InstallCompleteCallback = std::function<void(const LocalInstallSummary& summary)>;
    using BackupCompleteCallback = std::function<void(bool ok, size_t exportedCount)>;
    using RemoveStoreCompleteCallback = std::function<void(size_t removedCount, size_t failedCount, bool rebootRequired)>;

    LocalDriverInstaller() = default;
    ~LocalDriverInstaller();

    LocalDriverInstaller(const LocalDriverInstaller&) = delete;
    LocalDriverInstaller& operator=(const LocalDriverInstaller&) = delete;

    void PickFolderAsync(HWND ownerWindow, LogCallback onLog, FolderPickedCallback onPicked);
    void ScanInfFilesAsync(const std::wstring& folderPath, LogCallback onLog);
    void InstallFromFolderAsync(
        const std::wstring& folderPath,
        bool forceInstall,
        bool createRestorePoint,
        LogCallback onLog,
        InstallCompleteCallback onComplete = nullptr);

    void ScanDriverStoreAsync(std::vector<DeviceInfo> devicesSnapshot, LogCallback onLog = nullptr);
    void ExportDriverStoreBackupAsync(
        const std::wstring& destFolderPath,
        bool skipOldDuplicates,
        LogCallback onLog,
        BackupCompleteCallback onComplete = nullptr);
    void RemoveDriverStorePackagesAsync(
        std::vector<std::string> oemInfNames,
        bool createRestorePoint,
        std::vector<DeviceInfo> devicesSnapshot,
        LogCallback onLog,
        RemoveStoreCompleteCallback onComplete = nullptr);

    [[nodiscard]] bool IsPickingFolder() const noexcept { return picking_.load(); }
    [[nodiscard]] bool IsScanningInf() const noexcept { return scanning_.load(); }
    [[nodiscard]] bool IsInstalling() const noexcept { return installing_.load(); }
    [[nodiscard]] bool IsScanningStore() const noexcept { return scanningStore_.load(); }
    [[nodiscard]] bool IsExportingBackup() const noexcept { return exportingBackup_.load(); }
    [[nodiscard]] bool IsRemovingStorePackages() const noexcept { return removingPackages_.load(); }
    [[nodiscard]] float Progress() const noexcept { return progress_.load(); }
    [[nodiscard]] uint64_t StoreGeneration() const noexcept { return storeGeneration_.load(); }
    [[nodiscard]] std::string StatusMessage() const;

    std::vector<InfFileEntry> GetInfFilesSnapshot() const;
    DriverStoreSnapshot GetDriverStoreSnapshot() const;

    static std::vector<InfFileEntry> EnumerateInfFiles(
        const std::wstring& folderPath, const LogCallback& onLog = nullptr);

    static DriverStoreSnapshot ScanDriverStoreSynchronously(
        const std::vector<DeviceInfo>& devicesSnapshot);

    // Synchronous native installation via DiInstallDriverW (requires administrator privileges).
    static LocalInstallSummary InstallFromFolderSync(
        const std::wstring& folderPath,
        bool forceInstall,
        bool createRestorePoint,
        const LogCallback& onLog,
        std::atomic<float>* outProgress = nullptr);

    // Synchronous removal of unused oem*.inf packages via DiUninstallDriverW (requires admin).
    static bool RemoveDriverStorePackagesSync(
        const std::vector<std::string>& oemInfNames,
        bool createRestorePoint,
        const LogCallback& onLog,
        size_t* outRemovedCount = nullptr,
        size_t* outFailedCount = nullptr,
        bool* outRebootRequired = nullptr);

private:
    std::atomic<bool> picking_{false};
    std::atomic<bool> scanning_{false};
    std::atomic<bool> installing_{false};
    std::atomic<bool> scanningStore_{false};
    std::atomic<bool> exportingBackup_{false};
    std::atomic<bool> removingPackages_{false};
    std::atomic<bool> shuttingDown_{false};
    std::atomic<unsigned long> pickThreadId_{0};
    std::atomic<float> progress_{0.0f};
    std::atomic<uint64_t> storeGeneration_{0};

    mutable std::mutex dataMutex_;
    std::mutex threadMutex_;
    std::vector<InfFileEntry> infFiles_;
    DriverStoreSnapshot storeSnapshot_;
    std::string statusMessage_;

    std::thread pickThread_;
    std::thread scanThread_;
    std::thread installThread_;
    std::thread storeThread_;
    std::thread backupThread_;
    std::thread removeThread_;
};

} // namespace hdm
