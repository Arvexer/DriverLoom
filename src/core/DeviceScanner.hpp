#pragma once

#include "common/AppCommon.hpp"

#include <atomic>
#include <functional>
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
};

class DeviceScanner {
public:
    DeviceScanner() = default;
    ~DeviceScanner();

    DeviceScanner(const DeviceScanner&) = delete;
    DeviceScanner& operator=(const DeviceScanner&) = delete;

    void StartScanAsync(LogCallback onLog);

    [[nodiscard]] bool IsScanning() const noexcept { return scanning_.load(); }
    [[nodiscard]] uint64_t Generation() const noexcept { return generation_.load(); }
    std::vector<DeviceInfo> GetDevicesSnapshot() const;

    static std::vector<DeviceInfo> ScanSynchronously(const LogCallback& onLog = nullptr);

private:
    std::atomic<bool> scanning_{false};
    std::atomic<uint64_t> generation_{0};
    mutable std::mutex dataMutex_;
    std::vector<DeviceInfo> devices_;
    std::thread worker_;
};

} // namespace hdm
