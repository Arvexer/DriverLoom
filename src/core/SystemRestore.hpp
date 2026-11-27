#pragma once

#include "common/AppCommon.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace hdm {

enum class SystemProtectionState {
    Enabled,
    Disabled,
    DisabledByPolicy
};

class SystemRestore {
public:
    using CompletionCallback = std::function<void(bool success, const std::string& message)>;

    SystemRestore() = default;
    ~SystemRestore();

    SystemRestore(const SystemRestore&) = delete;
    SystemRestore& operator=(const SystemRestore&) = delete;

    [[nodiscard]] static SystemProtectionState ClassifySystemProtectionState(
        bool hasPolicyDisable,
        DWORD policyDisableVal,
        bool hasRpInterval,
        DWORD rpIntervalVal) noexcept;

    [[nodiscard]] static SystemProtectionState QuerySystemProtectionState() noexcept;
    [[nodiscard]] static std::wstring GetSystemDriveRootW();

    void CreateRestorePointAsync(
        const std::string& description,
        LogCallback onLog,
        CompletionCallback onComplete = nullptr);

    void EnableSystemProtectionAsync(
        bool createRestorePointAfter,
        const std::string& restorePointDescription,
        LogCallback onLog,
        CompletionCallback onComplete = nullptr);

    [[nodiscard]] bool IsBusy() const noexcept { return busy_.load(); }

    // Synchronous execution (requires administrator privileges).
    static bool CreateRestorePointSync(
        const std::string& description,
        const LogCallback& onLog,
        std::string* outMessage = nullptr);

    static bool EnableSystemProtectionSync(
        const LogCallback& onLog,
        std::string* outMessage = nullptr);

private:
    std::atomic<bool> busy_{false};
    std::atomic<bool> stopRequested_{false};
    std::mutex threadMutex_;
    std::thread worker_;
};

} // namespace hdm
