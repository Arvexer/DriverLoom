#include "app/CliWorker.hpp"

#include "common/AppCommon.hpp"
#include "core/DeviceScanner.hpp"
#include "core/LocalDriverInstaller.hpp"
#include "core/PackageSecurity.hpp"
#include "core/SystemDiagnostics.hpp"
#include "core/SystemRestore.hpp"
#include "core/WindowsUpdateManager.hpp"

#include <knownfolders.h>
#include <setupapi.h>
#include <shellapi.h>
#include <shlobj.h>

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

std::wstring StripTrailingSeparators(std::wstring s) {
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) {
        s.pop_back();
    }
    return s;
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

std::wstring ResolveCanonicalTempDir() {
    auto openVerifiedTempDir = [](const std::filesystem::path& dirPath) -> std::wstring {
        hdm::HandleGuard dirHandle(CreateFileW(
            dirPath.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        BY_HANDLE_FILE_INFORMATION fi{};
        if (!dirHandle.IsValid() || !GetFileInformationByHandle(dirHandle.Get(), &fi) ||
            (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return {};
        }
        return StripTrailingSeparators(GetFinalPathString(dirHandle.Get()));
    };

    hdm::CoTaskMemGuard<wchar_t> localAppRaw;
    if (SUCCEEDED(SHGetKnownFolderPath(
            FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, localAppRaw.Put())) &&
        localAppRaw.Get() != nullptr) {
        const std::wstring localAppCanon = openVerifiedTempDir(std::filesystem::path(localAppRaw.Get()));
        if (!localAppCanon.empty()) {
            const std::wstring knownTempCanon =
                openVerifiedTempDir(std::filesystem::path(localAppCanon) / L"Temp");
            if (!knownTempCanon.empty()) {
                return knownTempCanon;
            }
        }
    }

    const DWORD needed = GetTempPathW(0, nullptr);
    if (needed == 0) {
        return {};
    }
    std::wstring tempBuf(static_cast<size_t>(needed) + 1, L'\0');
    const DWORD len = GetTempPathW(needed + 1, tempBuf.data());
    if (len == 0 || len > needed) {
        return {};
    }
    tempBuf.resize(len);
    return openVerifiedTempDir(std::filesystem::path(tempBuf));
}

bool ValidateWorkerTempPath(
    const std::wstring& rawPath,
    const std::wstring& requiredSuffix,
    std::wstring& outCanonicalTempDir,
    std::wstring& outFileName) {
    outCanonicalTempDir.clear();
    outFileName.clear();
    if (rawPath.empty() || rawPath.rfind(L"\\\\", 0) == 0 || rawPath.rfind(L"//", 0) == 0) {
        return false;
    }

    const std::filesystem::path candidate(rawPath);
    if (!candidate.is_absolute()) {
        return false;
    }
    for (const auto& part : candidate) {
        if (part == L"..") {
            return false;
        }
    }

    const std::wstring fileName = candidate.filename().wstring();
    const std::wstring kRequiredPrefix = L"DriverLoom_";
    if (fileName.size() <= kRequiredPrefix.size() + requiredSuffix.size() ||
        fileName.rfind(kRequiredPrefix, 0) != 0 ||
        fileName.compare(
            fileName.size() - requiredSuffix.size(), requiredSuffix.size(), requiredSuffix) != 0) {
        return false;
    }
    for (wchar_t ch : fileName) {
        const bool ok = (ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'Z') ||
                        (ch >= L'a' && ch <= L'z') || ch == L'_' || ch == L'-' || ch == L'.';
        if (!ok) {
            return false;
        }
    }

    const std::wstring canonicalTemp = ResolveCanonicalTempDir();
    if (canonicalTemp.empty()) {
        return false;
    }
    const std::filesystem::path parentPath = candidate.parent_path();
    if (parentPath.empty()) {
        return false;
    }
    hdm::HandleGuard parentHandle(CreateFileW(
        parentPath.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!parentHandle.IsValid()) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION parentInfo{};
    if (!GetFileInformationByHandle(parentHandle.Get(), &parentInfo) ||
        (parentInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return false;
    }
    const std::wstring parentCanon = StripTrailingSeparators(GetFinalPathString(parentHandle.Get()));
    if (!EqualsIgnoreCaseWide(parentCanon, canonicalTemp)) {
        return false;
    }

    outCanonicalTempDir = canonicalTemp;
    outFileName = fileName;
    return true;
}

hdm::HandleGuard OpenValidatedTempLogFile(const std::wstring& rawPath) {
    std::wstring canonicalTemp;
    std::wstring fileName;
    if (!ValidateWorkerTempPath(rawPath, L".log", canonicalTemp, fileName)) {
        return hdm::HandleGuard{};
    }

    const DWORD attrs = GetFileAttributesW(rawPath.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        return hdm::HandleGuard{};
    }

    hdm::HandleGuard handle(CreateFileW(
        rawPath.c_str(),
        GENERIC_WRITE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ,
        nullptr,
        OPEN_ALWAYS,
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

    const std::wstring expectedFinal = canonicalTemp + L"\\" + fileName;
    const std::wstring actualFinal = GetFinalPathString(handle.Get());
    if (!EqualsIgnoreCaseWide(actualFinal, expectedFinal)) {
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
        InstallInf,
        InstallCab,
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
        } else if (arg == L"--cli-install-inf" && i + 1 < argc) {
            setMode(Mode::InstallInf);
            targetArg = argv[++i];
        } else if (arg == L"--cli-install-cab" && i + 1 < argc) {
            setMode(Mode::InstallCab);
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

    if (mode == Mode::InstallCab) {
        std::filesystem::path adminRoot;
        std::string dirErr;
        if (!hdm::CreateAdminOnlyTempWorkDir(adminRoot, &dirErr)) {
            fileLogger(hdm::LogLevel::Error,
                       dirErr.empty() ? "Не удалось создать защищённый каталог установки." : dirErr);
            return 1;
        }
        hdm::TempWorkDirGuard adminDir;
        adminDir.Set(adminRoot);

        hdm::HandleGuard srcHandle(CreateFileW(
            targetArg.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        BY_HANDLE_FILE_INFORMATION srcInfo{};
        if (!srcHandle.IsValid() || !GetFileInformationByHandle(srcHandle.Get(), &srcInfo) ||
            (srcInfo.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
            fileLogger(hdm::LogLevel::Error, "Недоступен или небезопасен исходный CAB-файл.");
            return 1;
        }

        const std::filesystem::path stagedCab = adminDir.Path() / L"driver.cab";
        hdm::HandleGuard dstHandle(CreateFileW(
            stagedCab.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        if (!dstHandle.IsValid()) {
            fileLogger(hdm::LogLevel::Error, "Не удалось скопировать CAB-пакет в защищённый каталог.");
            return 1;
        }
        std::vector<char> copyBuf(65536);
        bool copyOk = true;
        for (;;) {
            DWORD readBytes = 0;
            if (!ReadFile(srcHandle.Get(), copyBuf.data(), static_cast<DWORD>(copyBuf.size()), &readBytes, nullptr)) {
                copyOk = false;
                break;
            }
            if (readBytes == 0) {
                break;
            }
            DWORD offset = 0;
            while (offset < readBytes) {
                DWORD written = 0;
                if (!WriteFile(dstHandle.Get(), copyBuf.data() + offset, readBytes - offset, &written, nullptr) ||
                    written == 0) {
                    copyOk = false;
                    break;
                }
                offset += written;
            }
            if (!copyOk) {
                break;
            }
        }
        dstHandle.Reset();
        srcHandle.Reset();
        if (!copyOk) {
            fileLogger(hdm::LogLevel::Error, "Ошибка копирования CAB-пакета в защищённый каталог.");
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

        const hdm::LocalInstallSummary summary =
            hdm::LocalDriverInstaller::InstallFromFolderSync(
                cabExtractDir.wstring(), false, false, fileLogger);
        if (summary.failedCount > 0 || (summary.installedCount == 0 && summary.stagedOnlyCount == 0)) {
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
            std::wstring canonicalTemp;
            std::wstring fileName;
            if (ValidateWorkerTempPath(path, L".log.cancel", canonicalTemp, fileName)) {
                watcher = std::thread([path, this]() {
                    while (!stop.load()) {
                        const DWORD attrs = GetFileAttributesW(path.c_str());
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
