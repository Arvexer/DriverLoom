#include "app/CliWorker.hpp"

#include "common/AppCommon.hpp"
#include "core/DeviceScanner.hpp"
#include "core/LocalDriverInstaller.hpp"
#include "core/PackageSecurity.hpp"
#include "core/SystemDiagnostics.hpp"
#include "core/SystemRestore.hpp"
#include "core/WindowsUpdateManager.hpp"

#include <setupapi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

bool EqualsIgnoreCaseWide(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) {
            return false;
        }
    }
    return true;
}

std::wstring GetFinalPathString(HANDLE handle) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return {};
    }
    const DWORD needed = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED);
    if (needed == 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed) + 1, L'\0');
    const DWORD written =
        GetFinalPathNameByHandleW(handle, out.data(), needed + 1, FILE_NAME_NORMALIZED);
    if (written == 0 || written >= out.size()) {
        return {};
    }
    out.resize(written);
    return out;
}

hdm::HandleGuard OpenValidatedTempLogFile(const std::wstring& rawPath) {
    std::wstring canonicalTemp;
    std::wstring fileName;
    if (!hdm::ValidateWorkerTempPath(rawPath, L".log", canonicalTemp, fileName)) {
        return hdm::HandleGuard{};
    }

    const std::wstring safePath = canonicalTemp + L"\\" + fileName;
    const DWORD attrs = GetFileAttributesW(safePath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        return hdm::HandleGuard{};
    }

    hdm::HandleGuard handle(CreateFileW(
        safePath.c_str(),
        GENERIC_WRITE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!handle.IsValid()) {
        return hdm::HandleGuard{};
    }

    BY_HANDLE_FILE_INFORMATION fileInfo{};
    if (!GetFileInformationByHandle(handle.Get(), &fileInfo) ||
        (fileInfo.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
        fileInfo.nNumberOfLinks != 1) {
        return hdm::HandleGuard{};
    }

    const std::wstring actualFinal = GetFinalPathString(handle.Get());
    if (!EqualsIgnoreCaseWide(actualFinal, safePath)) {
        return hdm::HandleGuard{};
    }

    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(handle.Get(), zero, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(handle.Get())) {
        return hdm::HandleGuard{};
    }
    return handle;
}

} // namespace

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
        EnableSystemProtection,
        InstallInf,
        InstallCab,
        InstallPackage,
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
    bool duplicateMode = false;
    auto setMode = [&](Mode nextMode) {
        if (mode != Mode::None) {
            duplicateMode = true;
        }
        mode = nextMode;
    };
    std::wstring targetArg;
    std::wstring instanceArg;
    std::wstring publishersArg;
    std::wstring sha256Arg;
    std::wstring installerArgsArg;
    std::wstring nestedHintArg;
    std::wstring pkgKindArg;
    std::wstring restoreDescArg;
    std::wstring logPath;
    std::wstring cancelFilePath;
    bool forceInf = false;
    bool restoreBefore = false;
    bool driversOnly = false;
    bool autoStartService = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--cli-restore-point" && i + 1 < argc) {
            setMode(Mode::RestorePoint);
            targetArg = argv[++i];
        } else if (arg == L"--cli-enable-system-protection") {
            setMode(Mode::EnableSystemProtection);
        } else if (arg == L"--restore-desc" && i + 1 < argc) {
            restoreDescArg = argv[++i];
        } else if (arg == L"--cli-install-inf" && i + 1 < argc) {
            setMode(Mode::InstallInf);
            targetArg = argv[++i];
        } else if (arg == L"--cli-install-cab" && i + 1 < argc) {
            setMode(Mode::InstallCab);
            targetArg = argv[++i];
        } else if (arg == L"--cli-install-package" && i + 1 < argc) {
            setMode(Mode::InstallPackage);
            targetArg = argv[++i];
        } else if (arg == L"--cli-remove-oem-infs" && i + 1 < argc) {
            setMode(Mode::RemoveOemInfs);
            targetArg = argv[++i];
        } else if (arg == L"--cli-wu-install" && i + 1 < argc) {
            setMode(Mode::WuInstall);
            targetArg = argv[++i];
        } else if (arg == L"--cli-wu-hide" && i + 1 < argc) {
            setMode(Mode::WuHide);
            targetArg = argv[++i];
        } else if (arg == L"--cli-wu-unhide" && i + 1 < argc) {
            setMode(Mode::WuUnhide);
            targetArg = argv[++i];
        } else if (arg == L"--cli-enable-service" && i + 1 < argc) {
            setMode(Mode::EnableService);
            targetArg = argv[++i];
        } else if (arg == L"--cli-device-action" && i + 1 < argc) {
            setMode(Mode::DeviceAction);
            targetArg = argv[++i];
        } else if (arg == L"--cli-restore-inbox" && i + 1 < argc) {
            setMode(Mode::RestoreInboxDriver);
            targetArg = argv[++i];
        } else if (arg == L"--instance" && i + 1 < argc) {
            instanceArg = argv[++i];
        } else if (arg == L"--publishers" && i + 1 < argc) {
            publishersArg = argv[++i];
        } else if (arg == L"--sha256" && i + 1 < argc) {
            sha256Arg = argv[++i];
        } else if (arg == L"--installer-args" && i + 1 < argc) {
            installerArgsArg = argv[++i];
        } else if (arg == L"--nested-hint" && i + 1 < argc) {
            nestedHintArg = argv[++i];
        } else if (arg == L"--pkg-kind" && i + 1 < argc) {
            pkgKindArg = argv[++i];
        } else if (arg == L"--cli-reset-wucache") {
            setMode(Mode::ResetWuCache);
        } else if (arg == L"--cli-sfc") {
            setMode(Mode::SfcScan);
        } else if (arg == L"--cli-dism-check") {
            setMode(Mode::DismCheck);
        } else if (arg == L"--cli-dism-restore") {
            setMode(Mode::DismRestore);
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
    if (duplicateMode) {
        return 1;
    }

    std::mutex fileLogMutex;
    hdm::HandleGuard logFile = OpenValidatedTempLogFile(logPath);
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

    if (mode == Mode::EnableSystemProtection) {
        if (!hdm::SystemRestore::EnableSystemProtectionSync(fileLogger)) {
            return 1;
        }
        if (!restoreDescArg.empty()) {
            hdm::SystemRestore::CreateRestorePointSync(
                hdm::WideToUtf8(restoreDescArg), fileLogger);
        }
        return 0;
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

    struct CancelFileWatcherGuard {
        std::atomic<bool> cancel{false};
        std::atomic<bool> stop{false};
        std::thread watcher;

        explicit CancelFileWatcherGuard(const std::wstring& path) {
            std::wstring canonicalTemp;
            std::wstring fileName;
            if (hdm::ValidateWorkerTempPath(path, L".log.cancel", canonicalTemp, fileName)) {
                const std::wstring safePath = canonicalTemp + L"\\" + fileName;
                watcher = std::thread([safePath, this]() {
                    while (!stop.load()) {
                        const DWORD attrs = GetFileAttributesW(safePath.c_str());
                        if (attrs != INVALID_FILE_ATTRIBUTES &&
                            (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0) {
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

    if (mode == Mode::InstallCab) {
        CancelFileWatcherGuard cancelGuard(cancelFilePath);
        if (cancelGuard.cancel.load()) {
            return 3;
        }
        std::filesystem::path adminRoot;
        std::string dirErr;
        if (!hdm::CreateAdminOnlyTempWorkDir(adminRoot, &dirErr)) {
            fileLogger(hdm::LogLevel::Error,
                       dirErr.empty() ? "Не удалось создать защищённый каталог установки." : dirErr);
            return 1;
        }
        hdm::TempWorkDirGuard adminDir;
        adminDir.Set(adminRoot);

        const std::filesystem::path stagedCab = adminDir.Path() / L"driver.cab";
        std::string copyErr;
        if (!hdm::CopyFileBoundedNoReparse(targetArg, stagedCab, hdm::kMaxCabPackageBytes, &copyErr)) {
            fileLogger(hdm::LogLevel::Error,
                       copyErr.empty() ? "Ошибка копирования CAB-пакета в защищённый каталог." : copyErr);
            return 1;
        }

        hdm::HandleGuard cabLock;
        std::string sigErr;
        if (!hdm::VerifyAndLockAuthenticodeFile(
                stagedCab, &cabLock, &sigErr, hdm::DefaultCabPublishers())) {
            fileLogger(hdm::LogLevel::Error,
                       "Цифровая подпись CAB-пакета не прошла проверку (" + sigErr + ").");
            return 1;
        }
        if (cancelGuard.cancel.load()) {
            return 3;
        }

        const std::filesystem::path cabExtractDir = adminDir.Path() / L"cab_extract";
        if (!CreateDirectoryW(cabExtractDir.c_str(), nullptr)) {
            fileLogger(hdm::LogLevel::Error, "Не удалось создать каталог распаковки CAB.");
            return 1;
        }
        hdm::CabExtractContext cabCtx{};
        cabCtx.baseDir = cabExtractDir;
        if (!SetupIterateCabinetW(stagedCab.c_str(), 0, hdm::CabExtractCallback, &cabCtx)) {
            cabLock.Reset();
            fileLogger(hdm::LogLevel::Error,
                       "Ошибка распаковки CAB-архива: " + hdm::FormatWin32Error(GetLastError()));
            return 1;
        }
        cabLock.Reset();
        if (cancelGuard.cancel.load()) {
            return 3;
        }

        const hdm::LocalInstallSummary summary =
            hdm::LocalDriverInstaller::InstallFromFolderSync(
                cabExtractDir.wstring(), false, false, fileLogger);
        if (summary.failedCount > 0 || (summary.installedCount == 0 && summary.stagedOnlyCount == 0)) {
            return 1;
        }
        return summary.rebootRequired ? 2 : 0;
    }

    if (mode == Mode::InstallPackage) {
        std::vector<std::string> publishers;
        if (!hdm::ParsePipeSeparatedPublishers(hdm::WideToUtf8(publishersArg), publishers)) {
            fileLogger(hdm::LogLevel::Error, "Не задан или повреждён список доверенных издателей.");
            return 1;
        }
        if (!pkgKindArg.empty() && pkgKindArg != L"auto" && pkgKindArg != L"directx") {
            fileLogger(hdm::LogLevel::Error, "Недопустимый тип пакета установки.");
            return 1;
        }
        if (pkgKindArg == L"directx") {
            for (const auto& pub : publishers) {
                if (pub != "Microsoft Corporation" && pub != "Microsoft Windows") {
                    fileLogger(
                        hdm::LogLevel::Error,
                        "Для пакета DirectX допустимы только издатели Microsoft.");
                    return 1;
                }
            }
        }

        hdm::StagedPackageOptions opts;
        opts.sourcePackagePath = std::filesystem::path(targetArg);
        opts.expectedSha256 = hdm::WideToUtf8(sha256Arg);
        opts.allowedPublishers = std::move(publishers);
        opts.installerArgs = hdm::WideToUtf8(installerArgsArg);
        opts.nestedInstallerHint = hdm::WideToUtf8(nestedHintArg);
        opts.isDirectXLegacy = (pkgKindArg == L"directx");
        opts.maxPackageBytes = hdm::kMaxInstallerPackageBytes;

        CancelFileWatcherGuard cancelGuard(cancelFilePath);
        const hdm::StagedPackageResult res =
            hdm::RunStagedPackageInAdminDir(opts, fileLogger, &cancelGuard.cancel, nullptr);
        if (!res.fileVersion.empty()) {
            writeRawLine("[META] FILE_VERSION " + res.fileVersion);
        }
        if (res.launched) {
            writeRawLine("[META] PKG_EXIT " + std::to_string(res.exitCode));
        }
        if (res.cancelled || cancelGuard.cancel.load()) {
            return 3;
        }
        if (!res.launched) {
            return 1;
        }
        if (res.exitCode == 0) {
            return 0;
        }
        if (res.exitCode == 3010 || res.exitCode == 1641) {
            return 2;
        }
        return 1;
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
