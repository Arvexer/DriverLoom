#include "app/CliWorker.hpp"

#include "common/AppCommon.hpp"
#include "core/DeviceScanner.hpp"
#include "core/LocalDriverInstaller.hpp"
#include "core/SystemDiagnostics.hpp"
#include "core/SystemRestore.hpp"
#include "core/WindowsUpdateManager.hpp"

#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// --- Elevated CLI Worker Mode ---
int RunCliModeIfRequested() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return -1;
    }

    struct ArgvGuard {
        LPWSTR* ptr;
        ~ArgvGuard() { LocalFree(ptr); }
    } guard{argv};

    enum class Mode {
        None,
        RestorePoint,
        InstallInf,
        RemoveOemInfs,
        WuInstall,
        WuHide,
        WuUnhide,
        EnableService,
        ResetWuCache,
        SfcScan,
        DismCheck,
        DismRestore,
        DeviceAction,
        RestoreInboxDriver
    };
    Mode mode = Mode::None;
    std::wstring targetArg;
    std::wstring instanceArg;
    std::wstring logPath;
    std::wstring cancelFilePath;
    bool forceInf = false;
    bool restoreBefore = false;
    bool driversOnly = false;
    bool autoStartService = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--cli-restore-point" && i + 1 < argc) {
            mode = Mode::RestorePoint;
            targetArg = argv[++i];
        } else if (arg == L"--cli-install-inf" && i + 1 < argc) {
            mode = Mode::InstallInf;
            targetArg = argv[++i];
        } else if (arg == L"--cli-remove-oem-infs" && i + 1 < argc) {
            mode = Mode::RemoveOemInfs;
            targetArg = argv[++i];
        } else if (arg == L"--cli-wu-install" && i + 1 < argc) {
            mode = Mode::WuInstall;
            targetArg = argv[++i];
        } else if (arg == L"--cli-wu-hide" && i + 1 < argc) {
            mode = Mode::WuHide;
            targetArg = argv[++i];
        } else if (arg == L"--cli-wu-unhide" && i + 1 < argc) {
            mode = Mode::WuUnhide;
            targetArg = argv[++i];
        } else if (arg == L"--cli-enable-service" && i + 1 < argc) {
            mode = Mode::EnableService;
            targetArg = argv[++i];
        } else if (arg == L"--cli-device-action" && i + 1 < argc) {
            mode = Mode::DeviceAction;
            targetArg = argv[++i];
        } else if (arg == L"--cli-restore-inbox" && i + 1 < argc) {
            mode = Mode::RestoreInboxDriver;
            targetArg = argv[++i];
        } else if (arg == L"--instance" && i + 1 < argc) {
            instanceArg = argv[++i];
        } else if (arg == L"--cli-reset-wucache") {
            mode = Mode::ResetWuCache;
        } else if (arg == L"--cli-sfc") {
            mode = Mode::SfcScan;
        } else if (arg == L"--cli-dism-check") {
            mode = Mode::DismCheck;
        } else if (arg == L"--cli-dism-restore") {
            mode = Mode::DismRestore;
        } else if (arg == L"--log" && i + 1 < argc) {
            logPath = argv[++i];
        } else if (arg == L"--cancel-file" && i + 1 < argc) {
            cancelFilePath = argv[++i];
        } else if (arg == L"--force") {
            forceInf = true;
        } else if (arg == L"--restore-before") {
            restoreBefore = true;
        } else if (arg == L"--drivers-only") {
            driversOnly = true;
        } else if (arg == L"--auto-start") {
            autoStartService = true;
        }
    }

    if (mode == Mode::None) {
        return -1;
    }

    std::mutex fileLogMutex;
    hdm::HandleGuard logFile;
    if (!logPath.empty()) {
        const std::filesystem::path logFsPath(logPath);
        const DWORD attrs = GetFileAttributesW(logFsPath.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            logFile.Reset(CreateFileW(
                logFsPath.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ,
                nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL,
                nullptr));
        } else if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
            logFile.Reset(CreateFileW(
                logFsPath.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ,
                nullptr,
                TRUNCATE_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr));
        }
    }
    auto writeRawLine = [&](const std::string& rawLine) {
        std::lock_guard<std::mutex> lock(fileLogMutex);
        if (!logFile.IsValid()) {
            return;
        }
        const std::string line = rawLine + "\n";
        DWORD offset = 0;
        const DWORD total = static_cast<DWORD>(line.size());
        while (offset < total) {
            DWORD written = 0;
            if (!WriteFile(logFile.Get(), line.data() + offset, total - offset, &written, nullptr) ||
                written == 0) {
                break;
            }
            offset += written;
        }
        FlushFileBuffers(logFile.Get());
    };
    hdm::LogCallback fileLogger = [&](hdm::LogLevel lvl, const std::string& msg) {
        const char* tag = "[INFO] ";
        if (lvl == hdm::LogLevel::Success) tag = "[OK] ";
        else if (lvl == hdm::LogLevel::Warning) tag = "[WARN] ";
        else if (lvl == hdm::LogLevel::Error) tag = "[ERR] ";
        writeRawLine(std::string(tag) + msg);
    };

    if (mode == Mode::RestorePoint) {
        const bool ok = hdm::SystemRestore::CreateRestorePointSync(
            hdm::WideToUtf8(targetArg), fileLogger);
        return ok ? 0 : 1;
    }

    if (mode == Mode::InstallInf) {
        const hdm::LocalInstallSummary summary =
            hdm::LocalDriverInstaller::InstallFromFolderSync(
                targetArg, forceInf, restoreBefore, fileLogger);
        writeRawLine("[META] INF_SUMMARY " + std::to_string(summary.totalFiles) + " " +
                     std::to_string(summary.installedCount) + " " +
                     std::to_string(summary.stagedOnlyCount) + " " +
                     std::to_string(summary.failedCount) + " " +
                     (summary.rebootRequired ? "1" : "0"));
        if (summary.totalFiles == 0 || summary.failedCount > 0) {
            return 1;
        }
        return summary.rebootRequired ? 2 : 0;
    }

    if (mode == Mode::RemoveOemInfs) {
        std::vector<std::string> names;
        std::stringstream ss(hdm::WideToUtf8(targetArg));
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (!token.empty()) {
                names.push_back(token);
            }
        }
        size_t removed = 0;
        size_t failed = 0;
        bool rebootReq = false;
        hdm::LocalDriverInstaller::RemoveDriverStorePackagesSync(
            names, restoreBefore, fileLogger, &removed, &failed, &rebootReq);
        writeRawLine("[META] RM_SUMMARY " + std::to_string(removed) + " " +
                     std::to_string(failed) + " " +
                     (rebootReq ? "1" : "0"));
        if (names.empty() || failed > 0) {
            return 1;
        }
        return rebootReq ? 2 : 0;
    }

    if (mode == Mode::EnableService) {
        return hdm::SystemDiagnostics::EnableServiceSync(
                   hdm::WideToUtf8(targetArg), autoStartService, fileLogger)
                   ? 0
                   : 1;
    }

    if (mode == Mode::ResetWuCache) {
        return hdm::SystemDiagnostics::ResetWuDownloadCacheSync(fileLogger) ? 0 : 1;
    }

    if (mode == Mode::DeviceAction) {
        hdm::DeviceAction devAct = hdm::DeviceAction::Restart;
        if (!hdm::DeviceScanner::ParseDeviceActionCliVerb(
                hdm::WideToUtf8(targetArg), devAct)) {
            fileLogger(hdm::LogLevel::Error, "Неизвестная команда управления устройством.");
            return 1;
        }
        bool rebootReq = false;
        const bool ok = hdm::DeviceScanner::RunDeviceActionSync(
            devAct, hdm::WideToUtf8(instanceArg), fileLogger, &rebootReq);
        if (!ok) {
            return 1;
        }
        return rebootReq ? 2 : 0;
    }

    if (mode == Mode::RestoreInboxDriver) {
        bool rebootReq = false;
        const std::string key = hdm::WideToUtf8(targetArg);
        const bool ok = hdm::DeviceScanner::RestoreInboxDriverByHardwareKeySync(
            key, key, fileLogger, &rebootReq);
        if (!ok) {
            return 1;
        }
        return rebootReq ? 2 : 0;
    }

    struct CancelFileWatcherGuard {
        std::atomic<bool> cancel{false};
        std::atomic<bool> stop{false};
        std::thread watcher;

        explicit CancelFileWatcherGuard(const std::wstring& path) {
            if (!path.empty()) {
                const std::filesystem::path marker(path);
                watcher = std::thread([marker, this]() {
                    while (!stop.load()) {
                        std::error_code ec;
                        if (std::filesystem::exists(marker, ec)) {
                            cancel.store(true);
                            break;
                        }
                        Sleep(200);
                    }
                });
            }
        }
        ~CancelFileWatcherGuard() {
            stop.store(true);
            if (watcher.joinable()) {
                watcher.join();
            }
        }
    };

    if (mode == Mode::SfcScan || mode == Mode::DismCheck || mode == Mode::DismRestore) {
        CancelFileWatcherGuard cancelGuard(cancelFilePath);
        if (mode == Mode::SfcScan) {
            return hdm::SystemDiagnostics::RunSystemToolSync(
                       L"sfc.exe", L"/scannow", "SFC /scannow", fileLogger, &cancelGuard.cancel)
                       ? 0
                       : 1;
        }
        if (mode == Mode::DismCheck) {
            return hdm::SystemDiagnostics::RunSystemToolSync(
                       L"dism.exe",
                       L"/Online /Cleanup-Image /CheckHealth",
                       "DISM /CheckHealth",
                       fileLogger,
                       &cancelGuard.cancel)
                       ? 0
                       : 1;
        }
        return hdm::SystemDiagnostics::RunSystemToolSync(
                   L"dism.exe",
                   L"/Online /Cleanup-Image /RestoreHealth /NoRestart",
                   "DISM /RestoreHealth",
                   fileLogger,
                   &cancelGuard.cancel)
                   ? 0
                   : 1;
    }

    if (mode == Mode::WuHide || mode == Mode::WuUnhide) {
        std::vector<std::string> ids;
        std::stringstream ss(hdm::WideToUtf8(targetArg));
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (!token.empty()) {
                ids.push_back(token);
            }
        }
        const bool ok = hdm::WindowsUpdateManager::SetUpdatesHiddenSync(
            ids, mode == Mode::WuHide, fileLogger);
        return ok ? 0 : 1;
    }

    if (mode == Mode::WuInstall) {
        std::vector<std::string> ids;
        std::stringstream ss(hdm::WideToUtf8(targetArg));
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (!token.empty()) {
                ids.push_back(token);
            }
        }

        auto sanitizeMeta = [](std::string text) {
            for (char& ch : text) {
                if (ch == '\r' || ch == '\n' || ch == '|') {
                    ch = ' ';
                }
            }
            return text;
        };

        std::atomic<float> workerProgress{0.0f};
        CancelFileWatcherGuard cancelGuard(cancelFilePath);

        const hdm::WuInstallSummary summary =
            hdm::WindowsUpdateManager::DownloadAndInstallSync(
                ids,
                restoreBefore,
                fileLogger,
                &workerProgress,
                &cancelGuard.cancel,
                [&](const std::string& phase) {
                    const int milli = static_cast<int>(
                        std::clamp(workerProgress.load(), 0.0f, 1.0f) * 1000.0f);
                    writeRawLine(
                        "[META] PROG|" + std::to_string(milli) + "|" + sanitizeMeta(phase));
                },
                [&](const std::string& updateId,
                    bool installedOk,
                    bool rebootReq,
                    const std::string& statusText) {
                    writeRawLine(
                        std::string("[META] ITEM|") + sanitizeMeta(updateId) + "|" +
                        (installedOk ? "1" : "0") + "|" +
                        (rebootReq ? "1" : "0") + "|" +
                        sanitizeMeta(statusText));
                },
                driversOnly);

        if (summary.cancelled) {
            return 3;
        }
        if (summary.failedCount > 0 && summary.installedCount == 0) {
            return 1;
        }
        return summary.rebootRequired ? 2 : 0;
    }

    return 1;
}
