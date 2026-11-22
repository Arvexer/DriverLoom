#pragma once

#include "common/AppCommon.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hdm {

enum class DeviceHealth {
    Ok,
    Warning,
    Error
};

enum class DeviceCategory {
    Other,
    Monitor,
    Peripheral,
    DisplayAndChipset
};

enum class DeviceAction {
    Restart,         // DIF_PROPERTYCHANGE / DICS_PROPCHANGE
    Enable,          // DIF_PROPERTYCHANGE / DICS_ENABLE
    Disable,         // DIF_PROPERTYCHANGE / DICS_DISABLE
    RollbackDriver,  // DiRollbackDriver (newdev.dll)
    UninstallDevice, // DiUninstallDevice (newdev.dll)
    Reenumerate      // CM_Reenumerate_DevNode (cfgmgr32.dll)
};

struct DeviceInfo {
    std::string name;
    std::string manufacturer;
    std::string deviceClass;
    std::string driverVersion;
    std::string driverDate;
    std::string driverProvider;
    std::string infName;
    std::string hardwareId;
    std::string searchHardwareKey;
    std::string instanceId;
    std::string usbVendorId;
    std::string pciVendorId;
    std::string edidManufacturer;
    std::string edidModel;
    std::string peripheralBrand;
    std::wstring searchIndexLower;
    unsigned long devNodeStatus{0};
    unsigned long problemCode{0};
    DeviceHealth health{DeviceHealth::Ok};
    DeviceCategory category{DeviceCategory::Other};
    bool hasDriver{false};
    bool isGenericMonitorDriver{false};
    std::string statusText;
    std::string displayModeText;
    std::string connectedGpuName;
    uint64_t gpuVramBytes{0};
};

class DeviceScanner {
public:
    using ActionCallback = std::function<void(bool ok, bool rebootRequired)>;

    DeviceScanner() = default;
    ~DeviceScanner();

    DeviceScanner(const DeviceScanner&) = delete;
    DeviceScanner& operator=(const DeviceScanner&) = delete;

    void StartScanAsync(LogCallback onLog);

    void RunDeviceActionAsync(
        DeviceAction action,
        const std::string& instanceId,
        const std::string& deviceName,
        LogCallback onLog,
        ActionCallback onComplete = nullptr);

    void OpenDevicePropertiesAsync(const std::string& instanceId, LogCallback onLog = nullptr);

    [[nodiscard]] bool IsScanning() const noexcept { return scanning_.load(); }
    [[nodiscard]] bool IsBusy() const noexcept { return scanning_.load() || runningAction_.load(); }
    [[nodiscard]] uint64_t Generation() const noexcept { return generation_.load(); }
    std::vector<DeviceInfo> GetDevicesSnapshot() const;

    static std::vector<DeviceInfo> ScanSynchronously(const LogCallback& onLog = nullptr);

    static bool RunDeviceActionSync(
        DeviceAction action,
        const std::string& instanceId,
        const LogCallback& onLog,
        bool* outRebootRequired = nullptr);

    static bool RestoreInboxDriverByHardwareKeySync(
        const std::string& searchKey,
        const std::string& deviceName,
        const LogCallback& onLog,
        bool* outRebootRequired = nullptr);

    static std::string DeviceActionToCliVerb(DeviceAction action);
    static bool ParseDeviceActionCliVerb(const std::string& verb, DeviceAction& outAction);

private:
    struct PropDialogThread {
        std::thread thread;
        std::atomic<unsigned long> tid{0};
        std::atomic<bool> done{false};
    };

    std::atomic<bool> scanning_{false};
    std::atomic<bool> runningAction_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<uint64_t> generation_{0};
    mutable std::mutex dataMutex_;
    std::mutex threadMutex_;
    std::vector<DeviceInfo> devices_;
    std::thread worker_;
    std::thread actionWorker_;
    std::vector<std::unique_ptr<PropDialogThread>> propThreads_;
};

} // namespace hdm
