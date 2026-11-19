#pragma once

#include "common/AppCommon.hpp"

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

class LocalDriverInstaller {
public:
    using FolderPickedCallback = std::function<void(bool selected, const std::wstring& folderPath)>;
    using InstallCompleteCallback = std::function<void(const LocalInstallSummary& summary)>;

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

    [[nodiscard]] bool IsPickingFolder() const noexcept { return picking_.load(); }
    [[nodiscard]] bool IsScanningInf() const noexcept { return scanning_.load(); }
    [[nodiscard]] bool IsInstalling() const noexcept { return installing_.load(); }
    [[nodiscard]] float Progress() const noexcept { return progress_.load(); }
    [[nodiscard]] std::string StatusMessage() const;

    std::vector<InfFileEntry> GetInfFilesSnapshot() const;

    static std::vector<InfFileEntry> EnumerateInfFiles(
        const std::wstring& folderPath, const LogCallback& onLog = nullptr);

    // Synchronous native installation via DiInstallDriverW (requires administrator privileges).
    static LocalInstallSummary InstallFromFolderSync(
        const std::wstring& folderPath,
        bool forceInstall,
        bool createRestorePoint,
        const LogCallback& onLog,
        std::atomic<float>* outProgress = nullptr);

private:
    std::atomic<bool> picking_{false};
    std::atomic<bool> scanning_{false};
    std::atomic<bool> installing_{false};
    std::atomic<float> progress_{0.0f};

    mutable std::mutex dataMutex_;
    std::vector<InfFileEntry> infFiles_;
    std::string statusMessage_;

    std::thread pickThread_;
    std::thread scanThread_;
    std::thread installThread_;
};

} // namespace hdm
