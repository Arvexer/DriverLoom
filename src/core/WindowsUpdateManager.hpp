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
};

class WindowsUpdateManager {
public:
    using InstallDoneCallback = std::function<void(const WuInstallSummary& summary)>;

    WindowsUpdateManager() = default;
    ~WindowsUpdateManager();

    WindowsUpdateManager(const WindowsUpdateManager&) = delete;
    WindowsUpdateManager& operator=(const WindowsUpdateManager&) = delete;

    void StartSearchAsync(LogCallback onLog, bool driversOnly = true);
    void StartDownloadAndInstallAsync(
        const std::vector<std::string>& selectedUpdateIds,
        bool createRestorePoint,
        LogCallback onLog,
        InstallDoneCallback onComplete = nullptr);

    void RequestCancel() noexcept { cancelRequested_.store(true); }

    [[nodiscard]] bool IsSearching() const noexcept { return searching_.load(); }
    [[nodiscard]] bool IsInstalling() const noexcept { return installing_.load(); }
    [[nodiscard]] bool IsBusy() const noexcept { return searching_.load() || installing_.load(); }
    [[nodiscard]] float Progress() const noexcept { return progress_.load(); }
    [[nodiscard]] uint64_t Generation() const noexcept { return generation_.load(); }
    [[nodiscard]] std::string PhaseMessage() const;

    std::vector<DriverUpdateItem> GetUpdatesSnapshot() const;
    void SetUpdateSelected(const std::string& updateId, bool selected);
    void SelectAll(bool selected);

    // Synchronous execution used directly (when elevated) or inside the elevated CLI worker.
    static WuInstallSummary DownloadAndInstallSync(
        const std::vector<std::string>& updateIds,
        bool createRestorePoint,
        const LogCallback& onLog,
        std::atomic<float>* outProgress = nullptr,
        std::atomic<bool>* cancelFlag = nullptr,
        std::function<void(const std::string&)> onPhase = nullptr);

private:
    void SetPhaseMessage(const std::string& msg);

    std::atomic<bool> searching_{false};
    std::atomic<bool> installing_{false};
    std::atomic<bool> cancelRequested_{false};
    std::atomic<float> progress_{0.0f};
    std::atomic<uint64_t> generation_{0};

    mutable std::mutex dataMutex_;
    std::vector<DriverUpdateItem> updates_;
    std::string phaseMessage_;

    std::thread searchThread_;
    std::thread installThread_;
};

} // namespace hdm
