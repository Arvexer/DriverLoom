#pragma once

#include "common/AppCommon.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace hdm {

class SystemRestore {
public:
    using CompletionCallback = std::function<void(bool success, const std::string& message)>;

    SystemRestore() = default;
    ~SystemRestore();

    SystemRestore(const SystemRestore&) = delete;
    SystemRestore& operator=(const SystemRestore&) = delete;

    void CreateRestorePointAsync(
        const std::string& description,
        LogCallback onLog,
        CompletionCallback onComplete = nullptr);

    [[nodiscard]] bool IsBusy() const noexcept { return busy_.load(); }

    // Synchronous execution (requires administrator privileges).
    static bool CreateRestorePointSync(
        const std::string& description,
        const LogCallback& onLog,
        std::string* outMessage = nullptr);

private:
    std::atomic<bool> busy_{false};
    std::thread worker_;
};

} // namespace hdm
