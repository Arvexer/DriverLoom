#include "core/LocalDriverInstaller.hpp"
#include "core/SystemRestore.hpp"

#include <newdev.h>
#include <setupapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <filesystem>

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

struct ModuleGuard {
    HMODULE mod{nullptr};
    ~ModuleGuard() {
        if (mod != nullptr) {
            FreeLibrary(mod);
        }
    }
};

using DiInstallDriverWFn = BOOL(WINAPI*)(HWND, LPCWSTR, DWORD, PBOOL);

} // namespace

LocalDriverInstaller::~LocalDriverInstaller() {
    if (pickThread_.joinable()) {
        pickThread_.join();
    }
    if (scanThread_.joinable()) {
        scanThread_.join();
    }
    if (installThread_.joinable()) {
        installThread_.join();
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
    if (picking_.exchange(true)) {
        return;
    }
    if (pickThread_.joinable()) {
        pickThread_.join();
    }

    pickThread_ = std::thread([this, ownerWindow, log = std::move(onLog), picked = std::move(onPicked)]() {
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

                hr = dialog->Show(ownerWindow);
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
        if (picked) {
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
        entry.sizeBytes = it->file_size(entryEc);
        results.push_back(std::move(entry));
    }

    std::sort(results.begin(), results.end(), [](const InfFileEntry& a, const InfFileEntry& b) {
        return a.relativePath < b.relativePath;
    });
    return results;
}

void LocalDriverInstaller::ScanInfFilesAsync(const std::wstring& folderPath, LogCallback onLog) {
    if (scanning_.exchange(true)) {
        return;
    }
    if (scanThread_.joinable()) {
        scanThread_.join();
    }

    scanThread_ = std::thread([this, folderPath, log = std::move(onLog)]() {
        auto files = EnumerateInfFiles(folderPath, log);
        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            infFiles_ = std::move(files);
        }
        if (log) {
            std::lock_guard<std::mutex> lock(dataMutex_);
            log(LogLevel::Info,
                "Найдено .inf файлов в папке \"" + WideToUtf8(folderPath) + "\": " +
                    std::to_string(infFiles_.size()));
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
    if (installing_.exchange(true)) {
        if (onLog) {
            onLog(LogLevel::Warning, "Установка локальных драйверов уже выполняется.");
        }
        return;
    }
    if (installThread_.joinable()) {
        installThread_.join();
    }

    progress_.store(0.0f);
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        statusMessage_ = "Подготовка к установке драйверов...";
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
            std::wstring args = L"--cli-install-inf \"" + folderPath + L"\"";
            if (forceInstall) {
                args += L" --force";
            }
            if (createRestorePoint) {
                args += L" --restore-before";
            }
            args += L" --log \"" + tempLog.wstring() + L"\"";

            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                statusMessage_ = "Выполнение установки с правами администратора...";
            }

            progress_.store(0.25f);
            const ElevatedRunResult run = ExecuteElevatedSelf(args, true, &tempLog, log);
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
            } else {
                // Exit code bitmask from elevated worker:
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
        if (complete) {
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

    ModuleGuard newdevMod{LoadLibraryW(L"newdev.dll")};
    DiInstallDriverWFn diInstallDriverW = nullptr;
    if (newdevMod.mod != nullptr) {
        diInstallDriverW = reinterpret_cast<DiInstallDriverWFn>(
            reinterpret_cast<void*>(GetProcAddress(newdevMod.mod, "DiInstallDriverW")));
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

} // namespace hdm
