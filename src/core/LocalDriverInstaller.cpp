#include "core/LocalDriverInstaller.hpp"
#include "core/SystemRestore.hpp"

#include <newdev.h>
#include <setupapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>

namespace hdm {

namespace {

bool HasInfExtension(const std::filesystem::path& p) {
    std::wstring ext = p.extension().wstring();
    for (wchar_t& ch : ext) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return ext == L".inf";
}

using DiInstallDriverWFn = BOOL(WINAPI*)(HWND, LPCWSTR, DWORD, PBOOL);
using DiUninstallDriverWFn = BOOL(WINAPI*)(HWND, LPCWSTR, DWORD, PBOOL);
using SetupGetInfDriverStoreLocationWFn =
    BOOL(WINAPI*)(PCWSTR, PSP_ALTPLATFORM_INFO, PCWSTR, PWSTR, DWORD, PDWORD);

int64_t ParseOemInfNumber(const std::string& oemName) {
    const std::string lower = ToLowerAscii(oemName);
    if (lower.rfind("oem", 0) != 0 || lower.size() <= 7 ||
        lower.compare(lower.size() - 4, 4, ".inf") != 0) {
        return -1;
    }
    const size_t digitsLen = lower.size() - 7;
    if (digitsLen > 12) {
        return -1;
    }
    int64_t num = 0;
    for (size_t i = 3; i < lower.size() - 4; ++i) {
        const char ch = lower[i];
        if (ch < '0' || ch > '9') {
            return -1;
        }
        num = num * 10 + (ch - '0');
    }
    return num;
}

int CompareVersionPartsNumeric(const std::string& a, const std::string& b) {
    auto parse = [](const std::string& s) {
        std::vector<unsigned long long> parts;
        unsigned long long cur = 0;
        bool inNum = false;
        for (char ch : s) {
            if (ch >= '0' && ch <= '9') {
                cur = cur * 10ULL + static_cast<unsigned long long>(ch - '0');
                inNum = true;
            } else if (inNum) {
                parts.push_back(cur);
                cur = 0;
                inNum = false;
            }
        }
        if (inNum) {
            parts.push_back(cur);
        }
        return parts;
    };
    const auto va = parse(a);
    const auto vb = parse(b);
    const size_t n = std::max(va.size(), vb.size());
    for (size_t i = 0; i < n; ++i) {
        const auto na = i < va.size() ? va[i] : 0ULL;
        const auto nb = i < vb.size() ? vb[i] : 0ULL;
        if (na != nb) {
            return (na < nb) ? -1 : 1;
        }
    }
    return 0;
}

std::wstring SanitizePathSegment(std::wstring_view raw) {
    std::wstring out;
    out.reserve(raw.size());
    for (wchar_t ch : raw) {
        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' ||
            ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') {
            out.push_back(L'_');
        } else {
            out.push_back(ch);
        }
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) {
        out.pop_back();
    }
    return out.empty() ? L"Driver" : out;
}

std::wstring GetWindowsInfDirectory() {
    wchar_t winDir[MAX_PATH]{};
    const UINT len = GetWindowsDirectoryW(winDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return L"C:\\Windows\\INF";
    }
    return (std::filesystem::path(winDir) / L"INF").wstring();
}

} // namespace

LocalDriverInstaller::~LocalDriverInstaller() {
    shuttingDown_.store(true);
    for (;;) {
        std::thread next;
        {
            std::lock_guard<std::mutex> lock(threadMutex_);
            if (pickThread_.joinable()) next = std::move(pickThread_);
            else if (backupThread_.joinable()) next = std::move(backupThread_);
            else if (removeThread_.joinable()) next = std::move(removeThread_);
            else if (installThread_.joinable()) next = std::move(installThread_);
            else if (storeThread_.joinable()) next = std::move(storeThread_);
            else if (scanThread_.joinable()) next = std::move(scanThread_);
        }
        if (!next.joinable()) {
            break;
        }
        next.join();
    }
}

std::string LocalDriverInstaller::StatusMessage() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    return statusMessage_;
}

std::vector<InfFileEntry> LocalDriverInstaller::GetInfFilesSnapshot() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    return infFiles_;
}

void LocalDriverInstaller::PickFolderAsync(
    HWND ownerWindow,
    LogCallback onLog,
    FolderPickedCallback onPicked) {
    (void)ownerWindow;
    std::thread oldThread;
    {
        std::lock_guard<std::mutex> lock(threadMutex_);
        if (shuttingDown_.load() || picking_.load()) {
            return;
        }
        picking_.store(true);
        if (pickThread_.joinable()) {
            oldThread = std::move(pickThread_);
        }
    }
    if (oldThread.joinable()) {
        oldThread.join();
    }

    std::lock_guard<std::mutex> lock(threadMutex_);
    if (shuttingDown_.load()) {
        picking_.store(false);
        return;
    }

    pickThread_ = std::thread([this, log = std::move(onLog), picked = std::move(onPicked)]() {
        CoInitGuard comInit(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        std::wstring chosenPath;
        bool ok = false;

        if (!comInit.Succeeded()) {
            if (log) {
                log(LogLevel::Error,
                    "Не удалось инициализировать COM STA для диалога выбора папки: " +
                        FormatHResult(comInit.Result()));
            }
        } else {
            ComPtr<IFileOpenDialog> dialog;
            HRESULT hr = CoCreateInstance(
                CLSID_FileOpenDialog,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_IFileOpenDialog,
                reinterpret_cast<void**>(dialog.Put()));
            if (FAILED(hr)) {
                if (log) {
                    log(LogLevel::Error,
                        "Не удалось создать IFileOpenDialog: " + FormatHResult(hr));
                }
            } else {
                DWORD options = 0;
                dialog->GetOptions(&options);
                dialog->SetOptions(
                    options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
                dialog->SetTitle(L"Выберите папку с распакованными драйверами (.inf)");

                // Pass nullptr owner across threads so UI thread teardown cannot deadlock in join()
                hr = dialog->Show(nullptr);
                if (SUCCEEDED(hr)) {
                    ComPtr<IShellItem> item;
                    if (SUCCEEDED(dialog->GetResult(item.Put())) && item) {
                        CoTaskMemGuard<wchar_t> rawPath;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, rawPath.Put())) &&
                            rawPath.Get() != nullptr) {
                            chosenPath = rawPath.Get();
                            ok = true;
                        }
                    }
                } else if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED) && log) {
                    log(LogLevel::Warning,
                        "Ошибка диалога выбора папки: " + FormatHResult(hr));
                }
            }
        }

        picking_.store(false);
        if (picked && !shuttingDown_.load()) {
            picked(ok, chosenPath);
        }
    });
}

std::vector<InfFileEntry> LocalDriverInstaller::EnumerateInfFiles(
    const std::wstring& folderPath,
    const LogCallback& onLog) {
    std::vector<InfFileEntry> results;
    if (folderPath.empty()) {
        return results;
    }

    std::error_code ec;
    const std::filesystem::path root(folderPath);
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        if (onLog) {
            onLog(LogLevel::Warning,
                  "Указанная папка не существует: " + WideToUtf8(folderPath));
        }
        return results;
    }

    auto opts = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(root, opts, ec), end;
         it != end && !ec;
         it.increment(ec)) {
        std::error_code entryEc;
        if (!it->is_regular_file(entryEc)) {
            continue;
        }
        const std::filesystem::path& p = it->path();
        if (!HasInfExtension(p)) {
            continue;
        }

        InfFileEntry entry;
        entry.fullPath = p.wstring();
        entry.fileName = WideToUtf8(p.filename().wstring());
        std::filesystem::path rel = std::filesystem::relative(p, root, entryEc);
        entry.relativePath = WideToUtf8((entryEc ? p : rel).wstring());
        std::error_code sizeEc;
        const uintmax_t sz = it->file_size(sizeEc);
        entry.sizeBytes = sizeEc ? 0 : static_cast<uint64_t>(sz);
        results.push_back(std::move(entry));
    }

    std::sort(results.begin(), results.end(), [](const InfFileEntry& a, const InfFileEntry& b) {
        return a.relativePath < b.relativePath;
    });
    return results;
}

void LocalDriverInstaller::ScanInfFilesAsync(const std::wstring& folderPath, LogCallback onLog) {
    std::thread oldThread;
    {
        std::lock_guard<std::mutex> lock(threadMutex_);
        if (shuttingDown_.load() || scanning_.load()) {
            return;
        }
        scanning_.store(true);
        if (scanThread_.joinable()) {
            oldThread = std::move(scanThread_);
        }
    }
    if (oldThread.joinable()) {
        oldThread.join();
    }

    std::lock_guard<std::mutex> lock(threadMutex_);
    if (shuttingDown_.load()) {
        scanning_.store(false);
        return;
    }

    scanThread_ = std::thread([this, folderPath, log = std::move(onLog)]() {
        auto files = EnumerateInfFiles(folderPath, log);
        const size_t foundCount = files.size();
        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            infFiles_ = std::move(files);
        }
        if (log) {
            log(LogLevel::Info,
                "Найдено .inf файлов в папке \"" + WideToUtf8(folderPath) + "\": " +
                    std::to_string(foundCount));
        }
        scanning_.store(false);
    });
}

void LocalDriverInstaller::InstallFromFolderAsync(
    const std::wstring& folderPath,
    bool forceInstall,
    bool createRestorePoint,
    LogCallback onLog,
    InstallCompleteCallback onComplete) {
    std::thread oldThread;
    {
        std::lock_guard<std::mutex> lock(threadMutex_);
        if (shuttingDown_.load()) {
            return;
        }
        if (installing_.load() || exportingBackup_.load() || removingPackages_.load()) {
            if (onLog) {
                onLog(LogLevel::Warning, "Установка локальных драйверов или операция DriverStore уже выполняется.");
            }
            return;
        }
        installing_.store(true);
        if (installThread_.joinable()) {
            oldThread = std::move(installThread_);
        }
    }
    if (oldThread.joinable()) {
        oldThread.join();
    }

    progress_.store(0.0f);
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        statusMessage_ = "Подготовка к установке драйверов...";
    }

    std::lock_guard<std::mutex> lock(threadMutex_);
    if (shuttingDown_.load()) {
        installing_.store(false);
        return;
    }

    installThread_ = std::thread([this,
                                  folderPath,
                                  forceInstall,
                                  createRestorePoint,
                                  log = std::move(onLog),
                                  complete = std::move(onComplete)]() {
        LocalInstallSummary summary;

        if (IsProcessElevated()) {
            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                statusMessage_ = "Установка .inf через DiInstallDriverW...";
            }
            summary = InstallFromFolderSync(
                folderPath, forceInstall, createRestorePoint, log, &progress_);
        } else {
            if (log) {
                log(LogLevel::Info,
                    "Запрос прав администратора (UAC) для установки локальных драйверов...");
            }
            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                statusMessage_ = "Ожидание подтверждения UAC...";
            }

            const std::filesystem::path tempLog = CreateTempLogPath(L"hdm_inf");
            std::wstring args = L"--cli-install-inf " + QuoteWin32Arg(folderPath);
            if (forceInstall) {
                args += L" --force";
            }
            if (createRestorePoint) {
                args += L" --restore-before";
            }
            args += L" --log " + QuoteWin32Arg(tempLog.wstring());

            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                statusMessage_ = "Выполнение установки с правами администратора...";
            }

            progress_.store(0.25f);
            const ElevatedRunResult run =
                ExecuteElevatedSelf(args, true, &tempLog, log, nullptr, &shuttingDown_);
            std::error_code ec;
            std::filesystem::remove(tempLog, ec);

            if (!run.launched) {
                if (run.launchError == ERROR_CANCELLED) {
                    if (log) {
                        log(LogLevel::Warning, "Установка отменена пользователем в окне UAC.");
                    }
                } else if (log) {
                    log(LogLevel::Error,
                        "Ошибка запуска ShellExecuteExW: " + FormatWin32Error(run.launchError));
                }
            } else if (run.cancelled || run.timedOutWaiting) {
                summary.failedCount = 1;
            } else {
                // Exit code from elevated worker:
                // 0 = success, 2 = reboot required (with success), 1 = error(s).
                if (run.exitCode == 2) {
                    summary.rebootRequired = true;
                    summary.installedCount = 1;
                } else if (run.exitCode == 0) {
                    summary.installedCount = 1;
                } else {
                    summary.failedCount = 1;
                }
            }
            progress_.store(1.0f);
        }

        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            statusMessage_ = summary.rebootRequired
                                 ? "Установка завершена (требуется перезагрузка)."
                                 : "Установка локальных драйверов завершена.";
        }
        installing_.store(false);
        if (complete && !shuttingDown_.load()) {
            complete(summary);
        }
    });
}

LocalInstallSummary LocalDriverInstaller::InstallFromFolderSync(
    const std::wstring& folderPath,
    bool forceInstall,
    bool createRestorePoint,
    const LogCallback& onLog,
    std::atomic<float>* outProgress) {
    LocalInstallSummary summary;

    if (createRestorePoint) {
        SystemRestore::CreateRestorePointSync(
            "DriverLoom: перед установкой локальных драйверов", onLog);
    }

    const std::vector<InfFileEntry> files = EnumerateInfFiles(folderPath, onLog);
    summary.totalFiles = files.size();

    if (files.empty()) {
        if (onLog) {
            onLog(LogLevel::Warning, "В выбранной папке и подпапках не найдено .inf файлов.");
        }
        if (outProgress != nullptr) {
            outProgress->store(1.0f);
        }
        return summary;
    }

    ModuleGuard newdevMod(LoadLibraryExW(L"newdev.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    DiInstallDriverWFn diInstallDriverW = nullptr;
    if (newdevMod) {
        diInstallDriverW = reinterpret_cast<DiInstallDriverWFn>(
            reinterpret_cast<void*>(GetProcAddress(newdevMod.Get(), "DiInstallDriverW")));
    }
    if (diInstallDriverW == nullptr) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Не удалось загрузить DiInstallDriverW из newdev.dll: " +
                      FormatWin32Error(GetLastError()));
        }
        summary.failedCount = files.size();
        if (outProgress != nullptr) {
            outProgress->store(1.0f);
        }
        return summary;
    }

    if (onLog) {
        onLog(LogLevel::Info,
              "Начинается нативная установка " + std::to_string(files.size()) +
                  " .inf пакет(ов) через DiInstallDriverW (newdev.dll)...");
    }

    const DWORD flags = forceInstall ? DIIRFLAG_FORCE_INF : 0;

    for (size_t i = 0; i < files.size(); ++i) {
        const InfFileEntry& entry = files[i];
        if (onLog) {
            onLog(LogLevel::Info,
                  "[" + std::to_string(i + 1) + "/" + std::to_string(files.size()) +
                      "] Установка: " + entry.relativePath);
        }

        BOOL needReboot = FALSE;
        if (diInstallDriverW(nullptr, entry.fullPath.c_str(), flags, &needReboot)) {
            ++summary.installedCount;
            if (needReboot) {
                summary.rebootRequired = true;
            }
            if (onLog) {
                onLog(LogLevel::Success,
                      "Успешно установлен: " + entry.fileName +
                          (needReboot ? " (требуется перезагрузка)" : ""));
            }
        } else {
            const DWORD err = GetLastError();
            if (err == ERROR_NO_MORE_ITEMS || err == ERROR_DEVICE_NOT_CONNECTED) {
                // Stage the INF into Driver Store even when hardware is not currently plugged in.
                if (SetupCopyOEMInfW(
                        entry.fullPath.c_str(), nullptr, SPOST_PATH, 0, nullptr, 0, nullptr, nullptr)) {
                    ++summary.stagedOnlyCount;
                    if (onLog) {
                        onLog(LogLevel::Warning,
                              "Добавлен в хранилище драйверов (подключенное устройство не найдено): " +
                                  entry.fileName);
                    }
                } else {
                    const DWORD stageErr = GetLastError();
                    ++summary.failedCount;
                    if (onLog) {
                        onLog(LogLevel::Error,
                              "Ошибка установки " + entry.fileName + ": " +
                                  FormatWin32Error(stageErr));
                    }
                }
            } else {
                ++summary.failedCount;
                if (onLog) {
                    onLog(LogLevel::Error,
                          "Ошибка DiInstallDriverW для " + entry.fileName + ": " +
                              FormatWin32Error(err));
                }
            }
        }

        if (outProgress != nullptr) {
            outProgress->store(static_cast<float>(i + 1) / static_cast<float>(files.size()));
        }
    }

    if (onLog) {
        onLog(summary.failedCount == 0 ? LogLevel::Success : LogLevel::Warning,
              "Итог локальной установки: установлено — " + std::to_string(summary.installedCount) +
                  ", добавлено в хранилище — " + std::to_string(summary.stagedOnlyCount) +
                  ", ошибок — " + std::to_string(summary.failedCount) + ".");
    }
    return summary;
}

DriverStoreSnapshot LocalDriverInstaller::GetDriverStoreSnapshot() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    return storeSnapshot_;
}

DriverStoreSnapshot LocalDriverInstaller::ScanDriverStoreSynchronously(
    const std::vector<DeviceInfo>& devicesSnapshot) {
    DriverStoreSnapshot snap;

    std::map<std::string, std::string> activeInfToDevice;
    for (const auto& d : devicesSnapshot) {
        if (d.infName.empty()) {
            continue;
        }
        const std::string key = ToLowerAscii(d.infName);
        if (key.rfind("oem", 0) != 0) {
            continue;
        }
        auto it = activeInfToDevice.find(key);
        if (it == activeInfToDevice.end()) {
            activeInfToDevice.emplace(key, d.name);
        } else if (!d.name.empty() && it->second.empty()) {
            it->second = d.name;
        }
    }

    hdm::ModuleGuard setupMod(
        LoadLibraryExW(L"setupapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    SetupGetInfDriverStoreLocationWFn getStoreLoc = nullptr;
    if (setupMod.Get() != nullptr) {
        getStoreLoc = reinterpret_cast<SetupGetInfDriverStoreLocationWFn>(
            reinterpret_cast<void*>(GetProcAddress(setupMod.Get(), "SetupGetInfDriverStoreLocationW")));
    }

    const std::filesystem::path infDir(GetWindowsInfDirectory());
    std::error_code ec;
    if (!std::filesystem::exists(infDir, ec)) {
        snap.valid = true;
        return snap;
    }

    auto resolveStorePath = [&](const wchar_t* query) -> std::wstring {
        if (getStoreLoc == nullptr || query == nullptr) {
            return {};
        }
        wchar_t storePathBuf[MAX_PATH]{};
        DWORD reqChars = 0;
        if (getStoreLoc(query, nullptr, nullptr, storePathBuf, MAX_PATH, &reqChars)) {
            return storePathBuf;
        }
        if (reqChars > MAX_PATH) {
            std::wstring dynBuf(reqChars, L'\0');
            if (getStoreLoc(query, nullptr, nullptr, dynBuf.data(), reqChars, &reqChars)) {
                return std::wstring(dynBuf.c_str());
            }
        }
        return {};
    };

    for (std::filesystem::directory_iterator it(
             infDir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
        const auto& entry = *it;
        std::error_code entryEc;
        if (!entry.is_regular_file(entryEc)) {
            continue;
        }
        const std::wstring fnW = entry.path().filename().wstring();
        const std::string oemName = WideToUtf8(fnW);
        if (ParseOemInfNumber(oemName) < 0) {
            continue;
        }

        DriverStorePackageEntry pkg;
        pkg.oemInfName = oemName;

        // Resolve original INF location inside DriverStore\FileRepository
        pkg.storeInfPath = resolveStorePath(fnW.c_str());
        if (pkg.storeInfPath.empty()) {
            pkg.storeInfPath = resolveStorePath(entry.path().c_str());
        }

        if (!pkg.storeInfPath.empty()) {
            const std::filesystem::path p(pkg.storeInfPath);
            pkg.originalInfName = WideToUtf8(p.filename().wstring());
            pkg.storeFolderPath = p.parent_path().wstring();

            std::error_code sizeEc;
            for (std::filesystem::recursive_directory_iterator fit(
                     p.parent_path(),
                     std::filesystem::directory_options::skip_permission_denied,
                     sizeEc);
                 !sizeEc && fit != std::filesystem::recursive_directory_iterator();
                 fit.increment(sizeEc)) {
                std::error_code fileEc;
                if (fit->is_regular_file(fileEc)) {
                    std::error_code szEc;
                    const uint64_t fsz = fit->file_size(szEc);
                    if (!szEc) {
                        pkg.sizeBytes += fsz;
                    }
                }
            }
        } else {
            pkg.storeInfPath = entry.path().wstring();
            pkg.originalInfName = pkg.oemInfName;
        }

        // Extract Provider, Class and DriverVer cleanly via native SetupOpenInfFileW
        UINT errLine = 0;
        hdm::InfFileGuard hInf(SetupOpenInfFileW(entry.path().c_str(), nullptr, INF_STYLE_WIN4, &errLine));
        if (hInf.IsValid()) {
            INFCONTEXT ctx{};
            wchar_t buf[256]{};
            if (SetupFindFirstLineW(hInf.Get(), L"Version", L"Provider", &ctx) &&
                SetupGetStringFieldW(&ctx, 1, buf, static_cast<DWORD>(std::size(buf)), nullptr)) {
                pkg.provider = WideToUtf8(buf);
            }
            if (SetupFindFirstLineW(hInf.Get(), L"Version", L"Class", &ctx) &&
                SetupGetStringFieldW(&ctx, 1, buf, static_cast<DWORD>(std::size(buf)), nullptr)) {
                pkg.className = WideToUtf8(buf);
            }
            if (SetupFindFirstLineW(hInf.Get(), L"Version", L"DriverVer", &ctx)) {
                if (SetupGetStringFieldW(&ctx, 1, buf, static_cast<DWORD>(std::size(buf)), nullptr)) {
                    pkg.driverDate = WideToUtf8(buf);
                }
                if (SetupGetStringFieldW(&ctx, 2, buf, static_cast<DWORD>(std::size(buf)), nullptr)) {
                    pkg.driverVersion = WideToUtf8(buf);
                }
            }
        }

        const auto devIt = activeInfToDevice.find(ToLowerAscii(pkg.oemInfName));
        if (devIt != activeInfToDevice.end()) {
            pkg.inUse = true;
            pkg.activeDeviceName = devIt->second;
            ++snap.activeCount;
        }

        snap.totalStoreBytes += pkg.sizeBytes;
        snap.packages.push_back(std::move(pkg));
    }

    // Group by lowercase (originalInfName + "|" + className) to spot superseded duplicates.
    std::map<std::string, std::vector<size_t>> groups;
    for (size_t i = 0; i < snap.packages.size(); ++i) {
        const auto& p = snap.packages[i];
        if (p.originalInfName.empty() || p.originalInfName == p.oemInfName) {
            continue;
        }
        const std::string gKey =
            ToLowerAscii(p.originalInfName) + "|" + ToLowerAscii(p.className);
        groups[gKey].push_back(i);
    }

    for (const auto& [gKey, indices] : groups) {
        (void)gKey;
        if (indices.size() <= 1) {
            continue;
        }

        bool groupHasActive = false;
        size_t bestIdx = indices.front();
        for (size_t idx : indices) {
            const auto& cur = snap.packages[idx];
            if (cur.inUse) {
                groupHasActive = true;
            }
            const auto& best = snap.packages[bestIdx];
            const int verCmp = CompareVersionPartsNumeric(cur.driverVersion, best.driverVersion);
            if (verCmp > 0 ||
                (verCmp == 0 && ParseOemInfNumber(cur.oemInfName) > ParseOemInfNumber(best.oemInfName))) {
                bestIdx = idx;
            }
        }

        for (size_t idx : indices) {
            auto& pkg = snap.packages[idx];
            if (!pkg.inUse && (groupHasActive || idx != bestIdx)) {
                pkg.isOldDuplicate = true;
                pkg.selected = true;
                ++snap.duplicateCount;
                snap.reclaimableDuplicateBytes += pkg.sizeBytes;
            }
        }
    }

    std::sort(
        snap.packages.begin(),
        snap.packages.end(),
        [](const DriverStorePackageEntry& a, const DriverStorePackageEntry& b) {
            auto rank = [](const DriverStorePackageEntry& x) {
                if (x.isOldDuplicate) return 0;
                if (x.inUse) return 1;
                return 2;
            };
            const int ra = rank(a);
            const int rb = rank(b);
            if (ra != rb) return ra < rb;
            if (a.sizeBytes != b.sizeBytes) return a.sizeBytes > b.sizeBytes;
            return a.oemInfName < b.oemInfName;
        });

    snap.valid = true;
    return snap;
}

void LocalDriverInstaller::ScanDriverStoreAsync(
    std::vector<DeviceInfo> devicesSnapshot,
    LogCallback onLog) {
    std::lock_guard<std::mutex> lock(threadMutex_);
    if (shuttingDown_.load() || removingPackages_.load() || exportingBackup_.load()) {
        return;
    }
    if (scanningStore_.exchange(true)) {
        return;
    }
    if (storeThread_.joinable()) {
        storeThread_.join();
    }

    storeThread_ = std::thread([this, devs = std::move(devicesSnapshot), log = std::move(onLog)]() {
        auto snap = ScanDriverStoreSynchronously(devs);
        const size_t totalCount = snap.packages.size();
        const size_t dupCount = snap.duplicateCount;
        const uint64_t dupBytes = snap.reclaimableDuplicateBytes;
        const uint64_t totBytes = snap.totalStoreBytes;

        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            storeSnapshot_ = std::move(snap);
        }
        storeGeneration_.fetch_add(1);
        scanningStore_.store(false);

        if (!shuttingDown_.load() && log) {
            log(LogLevel::Info,
                "DriverStore проанализирован: пакетов — " + std::to_string(totalCount) +
                    " (" + FormatBytes(totBytes) + "), старых дубликатов — " +
                    std::to_string(dupCount) + " (" + FormatBytes(dupBytes) + ").");
        }
    });
}

void LocalDriverInstaller::ExportDriverStoreBackupAsync(
    const std::wstring& destFolderPath,
    bool skipOldDuplicates,
    LogCallback onLog,
    BackupCompleteCallback onComplete) {
    std::lock_guard<std::mutex> lock(threadMutex_);
    if (shuttingDown_.load()) {
        return;
    }
    if (scanningStore_.load() || removingPackages_.load() || exportingBackup_.exchange(true)) {
        if (onLog) {
            onLog(LogLevel::Warning, "Операция с хранилищем драйверов уже выполняется.");
        }
        if (onComplete) {
            onComplete(false, 0);
        }
        return;
    }
    if (backupThread_.joinable()) {
        backupThread_.join();
    }

    progress_.store(0.0f);
    {
        std::lock_guard<std::mutex> dataLock(dataMutex_);
        statusMessage_ = "Подготовка экспорта драйверов...";
    }

    backupThread_ = std::thread([this,
                                 destFolderPath,
                                 skipOldDuplicates,
                                 log = std::move(onLog),
                                 complete = std::move(onComplete)]() {
        DriverStoreSnapshot snap;
        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            snap = storeSnapshot_;
        }
        if (!snap.valid && !shuttingDown_.load()) {
            snap = ScanDriverStoreSynchronously({});
        }

        if (shuttingDown_.load()) {
            exportingBackup_.store(false);
            return;
        }

        std::vector<DriverStorePackageEntry> toExport;
        toExport.reserve(snap.packages.size());
        for (const auto& pkg : snap.packages) {
            if (pkg.storeFolderPath.empty()) continue;
            if (skipOldDuplicates && pkg.isOldDuplicate) continue;
            toExport.push_back(pkg);
        }

        if (toExport.empty()) {
            if (!shuttingDown_.load() && log) {
                log(LogLevel::Warning, "Не найдено сторонних пакетов драйверов для экспорта.");
            }
            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                statusMessage_ = "Нет пакетов для экспорта.";
            }
            exportingBackup_.store(false);
            if (!shuttingDown_.load() && complete) complete(false, 0);
            return;
        }

        const std::filesystem::path destRoot(destFolderPath);
        std::error_code ec;
        std::filesystem::create_directories(destRoot, ec);
        if (ec) {
            if (!shuttingDown_.load() && log) {
                log(LogLevel::Error, "Не удалось создать папку бэкапа: " + ec.message());
            }
            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                statusMessage_ = "Ошибка создания папки бэкапа.";
            }
            exportingBackup_.store(false);
            if (!shuttingDown_.load() && complete) complete(false, 0);
            return;
        }

        if (!shuttingDown_.load() && log) {
            log(LogLevel::Info,
                "Начинается экспорт " + std::to_string(toExport.size()) +
                    " пакет(ов) драйверов в \"" + WideToUtf8(destFolderPath) + "\"...");
        }

        size_t exportedCount = 0;
        uint64_t exportedBytes = 0;

        std::ofstream manifest(destRoot / L"DriverLoom_Backup_Manifest.txt", std::ios::binary | std::ios::trunc);
        if (manifest.is_open()) {
            manifest << "DriverLoom DriverStore Backup Manifest\r\n";
            manifest << "Total packages targeted: " << toExport.size() << "\r\n\r\n";
        }

        for (size_t i = 0; i < toExport.size(); ++i) {
            if (shuttingDown_.load()) {
                break;
            }
            const auto& pkg = toExport[i];
            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                statusMessage_ =
                    "Экспорт [" + std::to_string(i + 1) + "/" +
                    std::to_string(toExport.size()) + "]: " + pkg.originalInfName;
            }

            const std::wstring clsSeg = SanitizePathSegment(
                Utf8ToWide(pkg.className.empty() ? "Other" : pkg.className));
            const std::wstring origStem = SanitizePathSegment(
                std::filesystem::path(Utf8ToWide(pkg.originalInfName)).stem().wstring());
            const std::wstring oemStem = SanitizePathSegment(
                std::filesystem::path(Utf8ToWide(pkg.oemInfName)).stem().wstring());
            const std::filesystem::path targetDir =
                destRoot / (clsSeg + L"_" + origStem + L"_" + oemStem);

            std::error_code copyEc;
            std::filesystem::create_directories(targetDir, copyEc);
            if (!copyEc) {
                std::filesystem::copy(
                    std::filesystem::path(pkg.storeFolderPath),
                    targetDir,
                    std::filesystem::copy_options::overwrite_existing |
                        std::filesystem::copy_options::recursive,
                    copyEc);
            }

            if (!copyEc) {
                ++exportedCount;
                exportedBytes += pkg.sizeBytes;
                if (manifest.is_open()) {
                    manifest << "[" << pkg.oemInfName << "] " << pkg.originalInfName
                             << " | Class: " << (pkg.className.empty() ? "-" : pkg.className)
                             << " | Provider: " << (pkg.provider.empty() ? "-" : pkg.provider)
                             << " | Version: " << (pkg.driverVersion.empty() ? "-" : pkg.driverVersion)
                             << " (" << (pkg.driverDate.empty() ? "-" : pkg.driverDate) << ")"
                             << (pkg.inUse ? (" | Active: " + pkg.activeDeviceName) : "")
                             << "\r\n";
                }
            } else if (!shuttingDown_.load() && log) {
                log(LogLevel::Warning,
                    "Пропуск " + pkg.oemInfName + " (" + pkg.originalInfName + "): " + copyEc.message());
            }

            progress_.store(static_cast<float>(i + 1) / static_cast<float>(toExport.size()));
        }

        if (manifest.is_open()) {
            manifest.close();
        }

        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            statusMessage_ =
                "Бэкап завершён (" + std::to_string(exportedCount) + " пакетов, " +
                FormatBytes(exportedBytes) + ").";
        }
        if (!shuttingDown_.load() && log) {
            log(exportedCount > 0 ? LogLevel::Success : LogLevel::Error,
                "Экспортировано драйверов: " + std::to_string(exportedCount) + " из " +
                    std::to_string(toExport.size()) + " (" + FormatBytes(exportedBytes) + ").");
        }
        exportingBackup_.store(false);
        if (!shuttingDown_.load() && complete) {
            complete(exportedCount > 0, exportedCount);
        }
    });
}

bool LocalDriverInstaller::RemoveDriverStorePackagesSync(
    const std::vector<std::string>& oemInfNames,
    bool createRestorePoint,
    const LogCallback& onLog,
    size_t* outRemovedCount,
    size_t* outFailedCount,
    bool* outRebootRequired) {
    if (outRemovedCount) *outRemovedCount = 0;
    if (outFailedCount) *outFailedCount = 0;
    if (outRebootRequired) *outRebootRequired = false;

    size_t removed = 0;
    size_t failed = 0;
    bool rebootReq = false;

    if (oemInfNames.empty()) {
        return false;
    }
    if (createRestorePoint) {
        SystemRestore::CreateRestorePointSync(
            "DriverLoom: перед очисткой старых пакетов DriverStore", onLog);
    }

    hdm::ModuleGuard newdevMod(
        LoadLibraryExW(L"newdev.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    DiUninstallDriverWFn diUninstallDriverW = nullptr;
    if (newdevMod.Get() != nullptr) {
        diUninstallDriverW = reinterpret_cast<DiUninstallDriverWFn>(
            reinterpret_cast<void*>(GetProcAddress(newdevMod.Get(), "DiUninstallDriverW")));
    }
    if (diUninstallDriverW == nullptr) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Не удалось загрузить DiUninstallDriverW из newdev.dll: " +
                      FormatWin32Error(GetLastError()));
        }
        if (outFailedCount) *outFailedCount = oemInfNames.size();
        return false;
    }

    const std::filesystem::path infDir(GetWindowsInfDirectory());
    for (const auto& name : oemInfNames) {
        if (ParseOemInfNumber(name) < 0) {
            ++failed;
            if (onLog) {
                onLog(LogLevel::Warning, "Некорректное имя OEM INF пропущено: " + name);
            }
            continue;
        }
        const std::filesystem::path fullInf = infDir / Utf8ToWide(name);
        BOOL needReboot = FALSE;
        if (diUninstallDriverW(nullptr, fullInf.c_str(), 0, &needReboot)) {
            ++removed;
            if (needReboot) {
                rebootReq = true;
            }
            if (onLog) {
                onLog(LogLevel::Success, "Удалён пакет из DriverStore: " + name);
            }
        } else {
            const DWORD err = GetLastError();
            ++failed;
            if (onLog) {
                onLog(LogLevel::Error,
                      "Не удалось удалить " + name + ": " + FormatWin32Error(err));
            }
        }
    }

    if (outRemovedCount) *outRemovedCount = removed;
    if (outFailedCount) *outFailedCount = failed;
    if (outRebootRequired) *outRebootRequired = rebootReq;

    if (onLog) {
        onLog(failed == 0 ? LogLevel::Success : LogLevel::Warning,
              "Очистка DriverStore завершена: удалено — " + std::to_string(removed) +
                  ", ошибок — " + std::to_string(failed) + ".");
    }
    return removed > 0 && failed == 0;
}

void LocalDriverInstaller::RemoveDriverStorePackagesAsync(
    std::vector<std::string> oemInfNames,
    bool createRestorePoint,
    std::vector<DeviceInfo> devicesSnapshot,
    LogCallback onLog,
    RemoveStoreCompleteCallback onComplete) {
    std::lock_guard<std::mutex> lock(threadMutex_);
    if (shuttingDown_.load()) {
        return;
    }
    if (scanningStore_.load() || exportingBackup_.load() || removingPackages_.exchange(true)) {
        if (onLog) {
            onLog(LogLevel::Warning, "Операция с хранилищем драйверов уже выполняется.");
        }
        if (onComplete) {
            onComplete(0, 0, false);
        }
        return;
    }
    if (removeThread_.joinable()) {
        removeThread_.join();
    }

    progress_.store(0.0f);
    {
        std::lock_guard<std::mutex> dataLock(dataMutex_);
        statusMessage_ = "Удаление неиспользуемых пакетов из DriverStore...";
    }

    removeThread_ = std::thread([this,
                                 names = std::move(oemInfNames),
                                 createRestorePoint,
                                 devs = std::move(devicesSnapshot),
                                 log = std::move(onLog),
                                 complete = std::move(onComplete)]() {
        size_t removedCount = 0;
        size_t failedCount = 0;
        bool rebootReq = false;

        if (names.empty() || shuttingDown_.load()) {
            removingPackages_.store(false);
            if (!shuttingDown_.load() && complete) complete(0, 0, false);
            return;
        }

        if (IsProcessElevated()) {
            RemoveDriverStorePackagesSync(
                names, createRestorePoint, log, &removedCount, &failedCount, &rebootReq);
            progress_.store(1.0f);
        } else {
            if (log) {
                log(LogLevel::Info,
                    "Запрос прав администратора (UAC) для очистки пакетов DriverStore...");
            }
            std::string joined;
            for (size_t i = 0; i < names.size(); ++i) {
                if (i > 0) joined += ",";
                joined += names[i];
            }
            const std::filesystem::path tempLog = CreateTempLogPath(L"hdm_rm_inf");
            std::wstring args = L"--cli-remove-oem-infs " + QuoteWin32Arg(Utf8ToWide(joined));
            if (createRestorePoint) {
                args += L" --restore-before";
            }
            args += L" --log " + QuoteWin32Arg(tempLog.wstring());

            progress_.store(0.35f);
            const ElevatedRunResult run =
                ExecuteElevatedSelf(args, true, &tempLog, log, nullptr, &shuttingDown_);
            std::error_code ec;
            std::filesystem::remove(tempLog, ec);

            if (!run.launched) {
                failedCount = names.size();
                if (!shuttingDown_.load() && log) {
                    if (run.launchError == ERROR_CANCELLED) {
                        log(LogLevel::Warning, "Очистка DriverStore отменена в окне UAC.");
                    } else {
                        log(LogLevel::Error,
                            "Ошибка запуска ShellExecuteExW: " + FormatWin32Error(run.launchError));
                    }
                }
            } else if (run.exitCode == 2) {
                removedCount = names.size();
                rebootReq = true;
            } else if (run.exitCode == 0) {
                removedCount = names.size();
            } else {
                failedCount = names.size();
            }
            progress_.store(1.0f);
        }

        if (!shuttingDown_.load()) {
            // Refresh DriverStore snapshot automatically after removal.
            auto refreshed = ScanDriverStoreSynchronously(devs);
            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                storeSnapshot_ = std::move(refreshed);
                statusMessage_ =
                    "Очистка DriverStore завершена (удалено пакетов: " +
                    std::to_string(removedCount) + ").";
            }
            storeGeneration_.fetch_add(1);
        }
        removingPackages_.store(false);

        if (!shuttingDown_.load() && complete) {
            complete(removedCount, failedCount, rebootReq);
        }
    });
}

} // namespace hdm
