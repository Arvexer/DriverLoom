#pragma once

#include "common/AppCommon.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hdm {

struct DriverUpdateItem {
    std::string updateId;
    int revisionNumber{0};
    std::string title;
    std::string description;
    std::string driverModel;
    std::string driverManufacturer;
    std::string driverProvider;
    std::string driverVerDate;
    std::string driverHardwareId;
    uint64_t maxSizeBytes{0};
    std::string sizeFormatted;
    bool isDownloaded{false};
    bool isHidden{false};
    bool selected{true};
    bool installedOk{false};
    bool rebootRequired{false};
    std::string statusText;
};

struct WuInstallSummary {
    size_t requestedCount{0};
    size_t installedCount{0};
    size_t failedCount{0};
    bool rebootRequired{false};
    bool cancelled{false};
};

class WindowsUpdateManager {
public:
    using InstallDoneCallback = std::function<void(const WuInstallSummary& summary)>;
    using ItemUpdateCallback = std::function<void(
        const std::string& updateId,
        bool installedOk,
        bool rebootRequired,
        const std::string& statusText)>;

    WindowsUpdateManager();
    ~WindowsUpdateManager();

    WindowsUpdateManager(const WindowsUpdateManager&) = delete;
    WindowsUpdateManager& operator=(const WindowsUpdateManager&) = delete;

    void StartSearchAsync(LogCallback onLog, bool driversOnly = true);
    void StartDownloadAndInstallAsync(
        const std::vector<std::string>& selectedUpdateIds,
        bool createRestorePoint,
        LogCallback onLog,
        InstallDoneCallback onComplete = nullptr,
        bool driversOnly = true);

    void RequestCancel() noexcept;

    [[nodiscard]] bool IsSearching() const noexcept;
    [[nodiscard]] bool IsInstalling() const noexcept;
    [[nodiscard]] bool IsBusy() const noexcept;
    [[nodiscard]] float Progress() const noexcept;
    [[nodiscard]] uint64_t Generation() const noexcept;
    [[nodiscard]] std::string PhaseMessage() const;

    std::vector<DriverUpdateItem> GetUpdatesSnapshot() const;
    void SetUpdateSelected(const std::string& updateId, bool selected);
    void SelectAll(bool selected, bool includeHidden = false);

    void SetUpdatesHiddenAsync(
        const std::vector<std::string>& updateIds,
        bool hide,
        LogCallback onLog,
        bool driversOnly = true);

    static bool SetUpdatesHiddenSync(
        const std::vector<std::string>& updateIds,
        bool hide,
        const LogCallback& onLog,
        bool driversOnly = false);

    // Synchronous execution used directly (when elevated) or inside the elevated CLI worker.
    static WuInstallSummary DownloadAndInstallSync(
        const std::vector<std::string>& updateIds,
        bool createRestorePoint,
        const LogCallback& onLog,
        std::atomic<float>* outProgress = nullptr,
        std::atomic<bool>* cancelFlag = nullptr,
        std::function<void(const std::string&)> onPhase = nullptr,
        ItemUpdateCallback onItemUpdate = nullptr,
        bool driversOnly = false);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace hdm
