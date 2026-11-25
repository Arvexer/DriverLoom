#pragma once

#include "common/AppCommon.hpp"
#include "core/DeviceScanner.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hdm {

struct HttpRequestRegistry;

enum class ComponentCategory {
    GamingRuntime,
    PeripheralSoftware,
    MonitorAndGpu
};

struct SystemComponentItem {
    std::string id;
    ComponentCategory category{ComponentCategory::GamingRuntime};
    std::string name;
    std::string vendor;
    std::string purposeDescription;
    std::string detectedHardware;
    std::string installedVersion;
    std::string latestVersion;
    int installedDateYmd{0};
    int latestDateYmd{0};
    std::string officialPageUrl;
    std::string directInstallerUrl;
    std::string expectedSha256;
    std::string installerArgs;
    std::string nestedInstallerHint;
    std::string wingetRepoPath;
    std::string catalogSearchKey;
    std::string catalogFallbackKey;
    std::string preferredArch;
    std::string statusText;
    bool installed{false};
    bool updateAvailable{false};
    bool hardwareConnected{false};
    bool onlineChecked{false};
    bool compareVersions{true};
};

class SystemComponentsScanner {
public:
    using CatalogInstallCallback = std::function<void(bool ok, bool rebootRequired)>;

    SystemComponentsScanner();
    ~SystemComponentsScanner();

    SystemComponentsScanner(const SystemComponentsScanner&) = delete;
    SystemComponentsScanner& operator=(const SystemComponentsScanner&) = delete;

    // Fast local scan (<50ms): Registry + System32/SysWOW64 DLLs + matched PnP/EDID hardware.
    // When autoCheckOnline is true, automatically resolves latest official versions & download URLs.
    void StartLocalScanAsync(
        std::vector<DeviceInfo> devicesSnapshot,
        LogCallback onLog,
        bool autoCheckOnline = true);

    // Silent fast local re-check preserving previously fetched online versions & direct URLs.
    void RefreshLocalPreserveOnlineAsync(
        std::vector<DeviceInfo> devicesSnapshot,
        bool rescanDevices = false);

    // Background WinHTTP check for latest official online versions (.NET, official endpoints).
    void StartOnlineVersionCheckAsync(LogCallback onLog);

    // Downloads official installer via WinHTTP, verifies Authenticode signature via WinVerifyTrust,
    // runs installer, and refreshes local component status.
    void DownloadAndInstallAsync(
        const SystemComponentItem& item,
        std::vector<DeviceInfo> devicesSnapshot,
        LogCallback onLog);

    void DownloadAndInstallBatchAsync(
        std::vector<SystemComponentItem> itemsToInstall,
        std::vector<DeviceInfo> devicesSnapshot,
        LogCallback onLog,
        bool createRestorePointBefore = true);

    // Queries Microsoft Update Catalog by hardware search key (VEN_xxxx&DEV_xxxx / VID_xxxx&PID_xxxx),
    // downloads the latest WHQL .cab package, verifies Authenticode signature via WinVerifyTrust,
    // extracts .cab via SetupIterateCabinetW, and installs .inf via DiInstallDriverW.
    void SearchAndInstallFromCatalogAsync(
        const std::string& deviceName,
        const std::string& searchHardwareKey,
        const std::string& fullHardwareId,
        const std::string& installedVersion,
        std::vector<DeviceInfo> devicesSnapshot,
        LogCallback onLog,
        CatalogInstallCallback onComplete = nullptr);

    void RequestCancelInstall() noexcept;

    [[nodiscard]] bool IsScanning() const noexcept { return scanning_.load(); }
    [[nodiscard]] bool IsCheckingOnline() const noexcept { return checkingOnline_.load(); }
    [[nodiscard]] bool IsInstalling() const noexcept { return installing_.load(); }
    [[nodiscard]] float Progress() const noexcept { return progress_.load(); }
    [[nodiscard]] uint64_t Generation() const noexcept { return generation_.load(); }
    [[nodiscard]] bool ConsumeDeviceRescanNeeded() noexcept {
        return deviceRescanNeeded_.exchange(false);
    }
    [[nodiscard]] std::string PhaseMessage() const;

    std::vector<SystemComponentItem> GetItemsSnapshot() const;

    static std::vector<SystemComponentItem> ScanLocalSynchronously(
        const std::vector<DeviceInfo>& devicesSnapshot);

private:
    void SetPhaseMessage(const std::string& msg);
    bool InstallSingleComponentSync(
        const SystemComponentItem& item,
        float baseProgress,
        float progressSpan,
        std::vector<DeviceInfo>& devs,
        const LogCallback& log,
        bool* outRebootRequired = nullptr);

    std::atomic<bool> scanning_{false};
    std::atomic<bool> checkingOnline_{false};
    std::atomic<bool> installing_{false};
    std::atomic<bool> cancelInstallRequested_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> deviceRescanNeeded_{false};
    std::atomic<float> progress_{0.0f};
    std::atomic<uint64_t> generation_{0};

    mutable std::mutex dataMutex_;
    mutable std::mutex threadMutex_;
    std::vector<SystemComponentItem> items_;
    std::string phaseMessage_;

    std::thread scanThread_;
    std::thread onlineThread_;
    std::thread installThread_;
    std::shared_ptr<HttpRequestRegistry> onlineHttpRegistry_;
    std::shared_ptr<HttpRequestRegistry> installHttpRegistry_;
};

} // namespace hdm
