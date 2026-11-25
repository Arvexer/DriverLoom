#pragma once

#include "common/AppCommon.hpp"
#include "core/DeviceScanner.hpp"
#include "core/SystemComponentsScanner.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hdm {

struct MotherboardInfo {
    std::string manufacturer;
    std::string product;
    std::string systemManufacturer;
    std::string systemProduct;
    std::string biosVersion;
    std::string biosDate;
    std::string supportUrl;
};

struct WindowsReadinessInfo {
    bool cbsRebootPending{false};
    bool wuRebootRequired{false};
    bool pendingFileRenames{false};
    bool systemRestoreEnabled{false};
    bool systemRestorePolicyDisabled{false};
    bool trimEnabled{true};
};

enum class ControllerHealth {
    Ok,
    Attention, // Generic Windows driver or aged vendor driver
    Problem    // Missing driver or SetupAPI problem code
};

struct PowerSchemeItem {
    std::string guidString;
    std::string friendlyName;
    bool isActive{false};
};

struct StorageDiagnosticItem {
    std::string role; // "NVMe контроллер", "SATA / AHCI контроллер", "Накопитель (NVMe)", "Накопитель (SATA / Диск)"
    std::string deviceName;
    std::string manufacturer;
    std::string driverVersion;
    std::string driverDate;
    std::string infName;
    std::string hardwareId;
    std::string searchKey;
    std::string statusText;
    ControllerHealth health{ControllerHealth::Ok};
    bool isController{false};
};

struct RestorePointItem {
    std::string timestamp;
    std::string description;
    std::string sourceProcess; // Sanitized process basename (no user paths)
    std::string categoryLabel; // "DriverLoom", "Windows Update", "Плановая точка", "Установщик"
    bool isDriverLoom{false};
};

struct ControllerDiagnosticItem {
    std::string role; // Чипсет, Видеокарта, Сеть (LAN), Wi-Fi / BT, Аудио
    std::string deviceName;
    std::string manufacturer;
    std::string driverVersion;
    std::string driverDate;
    std::string infName;
    std::string hardwareId;
    std::string searchKey;
    std::string statusText;
    ControllerHealth health{ControllerHealth::Ok};
};

enum class ServiceHealthState {
    Healthy,  // Running or Demand-start ready
    Disabled, // Disabled in SCM -> blocks updates/installs
    MissingOrError
};

struct ServiceHealthItem {
    std::string serviceName;
    std::string displayName;
    std::string purpose;
    std::string stateText;
    std::string startTypeText;
    ServiceHealthState health{ServiceHealthState::Healthy};
    bool criticalForUpdates{true};
    bool preferAutoStart{false};
};

struct SystemDiagnosticsSnapshot {
    MotherboardInfo motherboard;
    WindowsReadinessInfo readiness;
    std::vector<ControllerDiagnosticItem> controllers;
    std::vector<ServiceHealthItem> services;
    std::vector<RestorePointItem> restorePoints;
    std::vector<StorageDiagnosticItem> storageItems;
    std::vector<PowerSchemeItem> powerSchemes;
    std::string activePowerSchemeName;
    size_t disabledCriticalServices{0};
    size_t attentionControllers{0};
    bool valid{false};
};

enum class MaintenanceAction {
    EnableService,
    SfcScan,
    DismCheckHealth,
    DismRestoreHealth,
    ResetWuDownloadCache
};

enum class ReportFormat {
    Markdown,
    Text,
    Json
};

class SystemDiagnostics {
public:
    using ActionDoneCallback = std::function<void(bool ok)>;

    SystemDiagnostics() = default;
    ~SystemDiagnostics();

    SystemDiagnostics(const SystemDiagnostics&) = delete;
    SystemDiagnostics& operator=(const SystemDiagnostics&) = delete;

    void StartScanAsync(std::vector<DeviceInfo> devicesSnapshot, LogCallback onLog = nullptr);

    void RunMaintenanceAsync(
        MaintenanceAction action,
        const std::string& targetArg,
        std::vector<DeviceInfo> devicesSnapshot,
        LogCallback onLog,
        ActionDoneCallback onComplete = nullptr);

    void ExportReportInteractiveAsync(
        HWND ownerWindow,
        SystemDiagnosticsSnapshot diagSnapshot,
        std::vector<DeviceInfo> devicesSnapshot,
        std::vector<SystemComponentItem> componentsSnapshot,
        LogCallback onLog);

    void SetActivePowerSchemeAsync(
        const std::string& schemeGuidStr,
        LogCallback onLog);

    static bool SetActivePowerSchemeSync(
        const std::string& schemeGuidStr,
        const LogCallback& onLog = nullptr);

    void RequestCancel() noexcept { cancelRequested_.store(true); }

    [[nodiscard]] bool IsScanning() const noexcept { return scanning_.load(); }
    [[nodiscard]] bool IsRunningAction() const noexcept { return runningAction_.load(); }
    [[nodiscard]] bool IsExportingReport() const noexcept { return exportingReport_.load(); }
    [[nodiscard]] bool IsBusy() const noexcept { return scanning_.load() || runningAction_.load(); }
    [[nodiscard]] uint64_t Generation() const noexcept { return generation_.load(); }
    [[nodiscard]] std::string ActionPhase() const;

    SystemDiagnosticsSnapshot GetSnapshot() const;

    static SystemDiagnosticsSnapshot CollectSynchronously(
        const std::vector<DeviceInfo>& devicesSnapshot);

    static std::string BuildCleanSystemReport(
        ReportFormat format,
        const SystemDiagnosticsSnapshot& diag,
        const std::vector<DeviceInfo>& devices,
        const std::vector<SystemComponentItem>& components);

    static bool WriteSystemReportToFile(
        const std::filesystem::path& filePath,
        ReportFormat format,
        const SystemDiagnosticsSnapshot& diag,
        const std::vector<DeviceInfo>& devices,
        const std::vector<SystemComponentItem>& components,
        const LogCallback& onLog = nullptr);

    // Synchronous elevated CLI operations.
    static bool EnableServiceSync(
        const std::string& serviceName,
        bool autoStart,
        const LogCallback& onLog);
    static bool ResetWuDownloadCacheSync(const LogCallback& onLog);
    static bool RunSystemToolSync(
        const std::wstring& exeName,
        const std::wstring& arguments,
        const std::string& title,
        const LogCallback& onLog,
        const std::atomic<bool>* cancelFlag = nullptr);

private:
    void SetActionPhase(const std::string& msg);

    std::atomic<bool> scanning_{false};
    std::atomic<bool> runningAction_{false};
    std::atomic<bool> exportingReport_{false};
    std::atomic<bool> cancelRequested_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<unsigned long> exportThreadId_{0};
    std::atomic<uint64_t> generation_{0};

    mutable std::mutex dataMutex_;
    mutable std::mutex threadMutex_;
    SystemDiagnosticsSnapshot snapshot_;
    std::string actionPhase_;

    std::thread scanThread_;
    std::thread actionThread_;
    std::thread exportThread_;
};

} // namespace hdm
