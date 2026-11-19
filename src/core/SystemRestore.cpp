#include "core/SystemRestore.hpp"

#include <srrestoreptapi.h>

#include <algorithm>
#include <filesystem>

namespace hdm {

SystemRestore::~SystemRestore() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

void SystemRestore::CreateRestorePointAsync(
    const std::string& description,
    LogCallback onLog,
    CompletionCallback onComplete) {
    if (busy_.exchange(true)) {
        if (onLog) {
            onLog(LogLevel::Warning, "Создание точки восстановления уже выполняется.");
        }
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
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
            const std::wstring wideDesc = Utf8ToWide(description);
            const std::wstring args =
                L"--cli-restore-point \"" + wideDesc + L"\" --log \"" + tempLog.wstring() + L"\"";

            const ElevatedRunResult run = ExecuteElevatedSelf(args, true, &tempLog, log);
            std::error_code ec;
            std::filesystem::remove(tempLog, ec);

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
                ok = (run.exitCode == 0);
                outMsg = ok ? "Точка восстановления успешно создана."
                            : "Не удалось создать точку восстановления (код " +
                                  std::to_string(run.exitCode) + ").";
            }
        }

        busy_.store(false);
        if (complete) {
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
    if (!SRSetRestorePointW(&rpInfo, &smgrStatus)) {
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
    rpInfo.dwEventType = END_SYSTEM_CHANGE;
    rpInfo.llSequenceNumber = smgrStatus.llSequenceNumber;
    if (!SRSetRestorePointW(&rpInfo, &smgrStatus)) {
        const DWORD err = smgrStatus.nStatus != 0 ? smgrStatus.nStatus : GetLastError();
        if (onLog) {
            onLog(LogLevel::Warning,
                  "Предупреждение при завершении точки восстановления: " + FormatWin32Error(err));
        }
    }

    const std::string okMsg = "Точка восстановления системы создана (последовательность #" +
                              std::to_string(smgrStatus.llSequenceNumber) + ").";
    if (onLog) {
        onLog(LogLevel::Success, okMsg);
    }
    if (outMessage != nullptr) {
        *outMessage = okMsg;
    }
    return true;
}

} // namespace hdm
