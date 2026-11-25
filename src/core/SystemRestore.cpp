#include "core/SystemRestore.hpp"

#include <srrestoreptapi.h>

#include <algorithm>
#include <filesystem>

namespace hdm {

SystemRestore::~SystemRestore() {
    stopRequested_.store(true);
    std::thread w;
    {
        std::lock_guard<std::mutex> lock(threadMutex_);
        if (worker_.joinable()) {
            w = std::move(worker_);
        }
    }
    if (w.joinable()) {
        w.join();
    }
}

void SystemRestore::CreateRestorePointAsync(
    const std::string& description,
    LogCallback onLog,
    CompletionCallback onComplete) {
    std::thread oldWorker;
    {
        std::lock_guard<std::mutex> lock(threadMutex_);
        if (stopRequested_.load()) {
            return;
        }
        if (busy_.exchange(true)) {
            if (onLog) {
                onLog(LogLevel::Warning, "Создание точки восстановления уже выполняется.");
            }
            return;
        }
        if (worker_.joinable()) {
            oldWorker = std::move(worker_);
        }
    }
    if (oldWorker.joinable()) {
        oldWorker.join();
    }

    std::lock_guard<std::mutex> lock(threadMutex_);
    if (stopRequested_.load()) {
        busy_.store(false);
        return;
    }

    worker_ = std::thread([this, description, log = std::move(onLog), complete = std::move(onComplete)]() {
        std::string outMsg;
        bool ok = false;

        if (IsProcessElevated()) {
            ok = CreateRestorePointSync(description, log, &outMsg);
        } else {
            if (log) {
                log(LogLevel::Info,
                    "Запрос прав администратора (UAC) для создания точки восстановления...");
            }
            const std::filesystem::path tempLog = CreateTempLogPath(L"hdm_sr");
            const std::wstring wideDesc = Utf8ToWide(description.empty() ? "DriverLoom" : description);
            std::wstring args = L"--cli-restore-point " + QuoteWin32Arg(wideDesc);
            if (!tempLog.empty()) {
                args += L" --log " + QuoteWin32Arg(tempLog.wstring());
            }

            const ElevatedRunResult run = ExecuteElevatedSelf(
                args, true, tempLog.empty() ? nullptr : &tempLog, log, nullptr, &stopRequested_);
            std::error_code ec;
            if (!tempLog.empty()) {
                std::filesystem::remove(tempLog, ec);
            }

            if (!run.launched) {
                if (run.launchError == ERROR_CANCELLED) {
                    outMsg = "Пользователь отменил запрос UAC для создания точки восстановления.";
                } else {
                    outMsg = "Не удалось запустить процесс с правами администратора: " +
                             FormatWin32Error(run.launchError);
                }
                if (log) {
                    log(LogLevel::Warning, outMsg);
                }
            } else {
                ok = (!run.cancelled && !run.timedOutWaiting && run.exitCode == 0);
                outMsg = ok ? "Точка восстановления успешно создана."
                            : (run.cancelled
                                   ? "Создание точки восстановления отменено."
                                   : "Не удалось создать точку восстановления (код " +
                                         std::to_string(run.exitCode) + ").");
            }
        }

        busy_.store(false);
        if (complete && !stopRequested_.load()) {
            complete(ok, outMsg);
        }
    });
}

bool SystemRestore::CreateRestorePointSync(
    const std::string& description,
    const LogCallback& onLog,
    std::string* outMessage) {
    if (onLog) {
        onLog(LogLevel::Info, "Создание точки восстановления системы: \"" + description + "\"...");
    }

    using SRSetRestorePointWFn = BOOL(WINAPI*)(PRESTOREPOINTINFOW, PSTATEMGRSTATUS);
    ModuleGuard srclient(
        LoadLibraryExW(L"srclient.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    SRSetRestorePointWFn srSetRestorePointW = nullptr;
    if (srclient) {
        srSetRestorePointW = reinterpret_cast<SRSetRestorePointWFn>(
            reinterpret_cast<void*>(GetProcAddress(srclient.Get(), "SRSetRestorePointW")));
    }
    if (srSetRestorePointW == nullptr) {
        const std::string errMsg =
            "Не удалось загрузить SRSetRestorePointW из srclient.dll: " +
            FormatWin32Error(GetLastError());
        if (onLog) {
            onLog(LogLevel::Error, errMsg);
        }
        if (outMessage != nullptr) {
            *outMessage = errMsg;
        }
        return false;
    }

    RESTOREPOINTINFOW rpInfo{};
    rpInfo.dwEventType = BEGIN_SYSTEM_CHANGE;
    rpInfo.dwRestorePtType = DEVICE_DRIVER_INSTALL;
    rpInfo.llSequenceNumber = 0;

    std::wstring wDesc = Utf8ToWide(description.empty() ? "DriverLoom" : description);
    if (wDesc.size() >= MAX_DESC_W) {
        wDesc.resize(MAX_DESC_W - 1);
    }
    std::copy(wDesc.begin(), wDesc.end(), rpInfo.szDescription);
    rpInfo.szDescription[wDesc.size()] = L'\0';

    STATEMGRSTATUS smgrStatus{};
    if (!srSetRestorePointW(&rpInfo, &smgrStatus)) {
        const DWORD err = smgrStatus.nStatus != 0 ? smgrStatus.nStatus : GetLastError();
        std::string errMsg;
        if (err == ERROR_SERVICE_DISABLED) {
            errMsg = "Защита системы (Восстановление системы) отключена в параметрах Windows.";
        } else {
            errMsg = "Ошибка SRSetRestorePointW: " + FormatWin32Error(err);
        }
        if (onLog) {
            onLog(LogLevel::Error, errMsg);
        }
        if (outMessage != nullptr) {
            *outMessage = errMsg;
        }
        return false;
    }

    // End the restore point change immediately so it is committed.
    const INT64 seqNum = smgrStatus.llSequenceNumber;
    rpInfo.dwEventType = END_SYSTEM_CHANGE;
    rpInfo.llSequenceNumber = seqNum;
    if (!srSetRestorePointW(&rpInfo, &smgrStatus)) {
        const DWORD err = smgrStatus.nStatus != 0 ? smgrStatus.nStatus : GetLastError();
        if (onLog) {
            onLog(LogLevel::Warning,
                  "Предупреждение при завершении точки восстановления: " + FormatWin32Error(err));
        }
    }

    const std::string okMsg = "Точка восстановления системы создана (последовательность #" +
                              std::to_string(seqNum) + ").";
    if (onLog) {
        onLog(LogLevel::Success, okMsg);
    }
    if (outMessage != nullptr) {
        *outMessage = okMsg;
    }
    return true;
}

} // namespace hdm
