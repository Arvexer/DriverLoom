#include "core/WindowsUpdateManager.hpp"
#include "core/SystemRestore.hpp"

#include <oleauto.h>
#include <initguid.h>
#include <wuapi.h>

#include <chrono>
#include <cstdio>
#include <set>

namespace hdm {

namespace {

// Standard Windows Update UpdateCollection CLSID ({13639463-00DB-4646-803D-528026140D88}).
const CLSID kClsidUpdateCollection = {
    0x13639463, 0x00db, 0x4646, {0x80, 0x3d, 0x52, 0x80, 0x26, 0x14, 0x0d, 0x88}};

// Callback interface IIDs required by IUpdateSearcher::BeginSearch,
// IUpdateDownloader::BeginDownload, and IUpdateInstaller::BeginInstall.
const IID kIidSearchCompletedCallback = {
    0x88aee058, 0xd4b0, 0x4725, {0xa2, 0xf1, 0x81, 0x4a, 0x67, 0xae, 0x96, 0x4c}};
const IID kIidDownloadProgressChangedCallback = {
    0x8c3f1cdd, 0x6173, 0x4591, {0xae, 0xbd, 0xa5, 0x6a, 0x53, 0xca, 0x77, 0xc1}};
const IID kIidDownloadCompletedCallback = {
    0x77254866, 0x9f5b, 0x4c8e, {0xb9, 0xe2, 0xc7, 0x7a, 0x85, 0x30, 0xd6, 0x4b}};
const IID kIidInstallationProgressChangedCallback = {
    0xe01402d5, 0xf8da, 0x43ba, {0xa0, 0x12, 0x38, 0x89, 0x4b, 0xd0, 0x48, 0xf1}};
const IID kIidInstallationCompletedCallback = {
    0x45f4f6f3, 0xd602, 0x4f98, {0x9a, 0x8a, 0x3e, 0xfa, 0x15, 0x2a, 0xd2, 0xd3}};

// Generic COM callback for WUAPI async completed/progress interfaces
// (all share IUnknown + Invoke(job, args)).
class WuCompletedCallback final : public IUnknown {
public:
    explicit WuCompletedCallback(REFIID callbackIid, bool createEvent = true) noexcept
        : callbackIid_(callbackIid),
          event_(createEvent ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr) {}

    [[nodiscard]] HANDLE EventHandle() const noexcept { return event_.Get(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (ppvObject == nullptr) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, callbackIid_)) {
            *ppvObject = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(refCount_.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG remaining = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
            return 0;
        }
        return static_cast<ULONG>(remaining);
    }

    virtual HRESULT STDMETHODCALLTYPE Invoke(IUnknown* /*job*/, IUnknown* /*callbackArgs*/) {
        if (event_.IsValid()) {
            SetEvent(event_.Get());
        }
        return S_OK;
    }

private:
    ~WuCompletedCallback() = default;

    std::atomic<LONG> refCount_{1};
    IID callbackIid_{};
    HandleGuard event_;
};

std::string TryGetDispatchStringProperty(IDispatch* disp, const wchar_t* propName) {
    if (disp == nullptr || propName == nullptr) {
        return {};
    }
    DISPID dispId = DISPID_UNKNOWN;
    LPOLESTR namePtr = const_cast<LPOLESTR>(propName);
    if (FAILED(disp->GetIDsOfNames(IID_NULL, &namePtr, 1, LOCALE_USER_DEFAULT, &dispId))) {
        return {};
    }
    DISPPARAMS params{};
    VARIANT varResult;
    VariantInit(&varResult);
    HRESULT hr = disp->Invoke(
        dispId, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &params, &varResult, nullptr, nullptr);
    std::string result;
    if (SUCCEEDED(hr)) {
        if (varResult.vt == VT_BSTR && varResult.bstrVal != nullptr) {
            result = WideToUtf8(std::wstring_view(varResult.bstrVal, SysStringLen(varResult.bstrVal)));
        } else if (varResult.vt == VT_DATE) {
            SYSTEMTIME st{};
            if (VariantTimeToSystemTime(varResult.date, &st)) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
                result = buf;
            }
        }
    }
    VariantClear(&varResult);
    return result;
}

uint64_t DecimalToUint64(const DECIMAL& dec) {
    ULONG64 val = 0;
    if (SUCCEEDED(VarUI8FromDec(const_cast<DECIMAL*>(&dec), &val))) {
        return static_cast<uint64_t>(val);
    }
    return dec.Lo64;
}

std::string ResultCodeToText(OperationResultCode code) {
    switch (code) {
    case orcNotStarted:
        return "Не начато";
    case orcInProgress:
        return "Выполняется";
    case orcSucceeded:
        return "Успешно";
    case orcSucceededWithErrors:
        return "Успешно (с предупреждениями)";
    case orcFailed:
        return "Ошибка";
    case orcAborted:
        return "Отменено";
    default:
        return "Неизвестно";
    }
}

std::string JoinUpdateIds(const std::vector<std::string>& ids) {
    std::string joined;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) {
            joined += ',';
        }
        joined += ids[i];
    }
    return joined;
}

} // namespace

WindowsUpdateManager::~WindowsUpdateManager() {
    cancelRequested_.store(true);
    if (searchThread_.joinable()) {
        searchThread_.join();
    }
    if (installThread_.joinable()) {
        installThread_.join();
    }
}

void WindowsUpdateManager::SetPhaseMessage(const std::string& msg) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    phaseMessage_ = msg;
}

std::string WindowsUpdateManager::PhaseMessage() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    return phaseMessage_;
}

std::vector<DriverUpdateItem> WindowsUpdateManager::GetUpdatesSnapshot() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    return updates_;
}

void WindowsUpdateManager::SetUpdateSelected(const std::string& updateId, bool selected) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    for (auto& item : updates_) {
        if (item.updateId == updateId) {
            item.selected = selected;
            break;
        }
    }
}

void WindowsUpdateManager::SelectAll(bool selected) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    for (auto& item : updates_) {
        item.selected = selected;
    }
}

void WindowsUpdateManager::StartSearchAsync(LogCallback onLog, bool driversOnly) {
    if (IsBusy()) {
        if (onLog) {
            onLog(LogLevel::Warning, "Windows Update уже выполняет операцию.");
        }
        return;
    }
    if (searchThread_.joinable()) {
        searchThread_.join();
    }

    cancelRequested_.store(false);
    searching_.store(true);
    progress_.store(0.0f);
    SetPhaseMessage("Подключение к службе Windows Update...");

    searchThread_ = std::thread([this, driversOnly, log = std::move(onLog)]() {
        CoInitGuard comInit(COINIT_MULTITHREADED);
        std::vector<DriverUpdateItem> found;

        if (!comInit.Succeeded()) {
            if (log) {
                log(LogLevel::Error,
                    "Ошибка CoInitializeEx (MTA): " + FormatHResult(comInit.Result()));
            }
            searching_.store(false);
            return;
        }

        const wchar_t* queryW = driversOnly ? L"IsInstalled=0 and Type='Driver'" : L"IsInstalled=0";
        if (log) {
            log(LogLevel::Info,
                driversOnly
                    ? "Поиск доступных драйверов через Windows Update API (Type='Driver')..."
                    : "Расширенный поиск драйверов и системных обновлений Windows Update...");
        }

        ComPtr<IUpdateSession> session;
        HRESULT hr = CoCreateInstance(
            CLSID_UpdateSession,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_IUpdateSession,
            reinterpret_cast<void**>(session.Put()));
        if (FAILED(hr) || !session) {
            if (log) {
                log(LogLevel::Error,
                    "Не удалось создать IUpdateSession: " + FormatHResult(hr));
            }
            SetPhaseMessage("Ошибка создания IUpdateSession");
            searching_.store(false);
            return;
        }

        BstrGuard appId(L"DriverLoom");
        session->put_ClientApplicationID(appId.Get());

        ComPtr<IUpdateSearcher> searcher;
        hr = session->CreateUpdateSearcher(searcher.Put());
        if (FAILED(hr) || !searcher) {
            if (log) {
                log(LogLevel::Error,
                    "Не удалось создать IUpdateSearcher: " + FormatHResult(hr));
            }
            SetPhaseMessage("Ошибка создания IUpdateSearcher");
            searching_.store(false);
            return;
        }

        SetPhaseMessage("Поиск обновлений на сервере Windows Update...");
        BstrGuard criteria(queryW);
        VARIANT emptyState;
        VariantInit(&emptyState);

        ComPtr<WuCompletedCallback> searchCb;
        *searchCb.Put() = new WuCompletedCallback(kIidSearchCompletedCallback);

        ComPtr<ISearchJob> searchJob;
        hr = searcher->BeginSearch(
            criteria.Get(), searchCb.Get(), emptyState, searchJob.Put());
        if (FAILED(hr) || !searchJob) {
            if (log) {
                log(LogLevel::Error,
                    "Ошибка BeginSearch Windows Update: " + FormatHResult(hr));
            }
            SetPhaseMessage("Ошибка поиска обновлений");
            searching_.store(false);
            return;
        }

        while (!cancelRequested_.load()) {
            if (searchCb->EventHandle() != nullptr &&
                WaitForSingleObject(searchCb->EventHandle(), 150) == WAIT_OBJECT_0) {
                break;
            }
            VARIANT_BOOL completed = VARIANT_FALSE;
            if (SUCCEEDED(searchJob->get_IsCompleted(&completed)) && completed == VARIANT_TRUE) {
                break;
            }
        }

        if (cancelRequested_.load()) {
            searchJob->RequestAbort();
            searchJob->CleanUp();
            if (log) {
                log(LogLevel::Warning, "Поиск обновлений Windows Update отменён.");
            }
            SetPhaseMessage("Поиск отменён");
            searching_.store(false);
            return;
        }

        ComPtr<ISearchResult> searchResult;
        hr = searcher->EndSearch(searchJob.Get(), searchResult.Put());
        searchJob->CleanUp();

        if (FAILED(hr) || !searchResult) {
            if (log) {
                log(LogLevel::Error,
                    "Ошибка EndSearch Windows Update: " + FormatHResult(hr));
            }
            SetPhaseMessage("Ошибка получения результатов поиска");
            searching_.store(false);
            return;
        }

        OperationResultCode resultCode = orcNotStarted;
        searchResult->get_ResultCode(&resultCode);
        if (resultCode != orcSucceeded && resultCode != orcSucceededWithErrors) {
            if (log) {
                log(LogLevel::Warning,
                    "Поиск Windows Update завершился со статусом: " + ResultCodeToText(resultCode));
            }
        }

        ComPtr<IUpdateCollection> updateColl;
        hr = searchResult->get_Updates(updateColl.Put());
        if (SUCCEEDED(hr) && updateColl) {
            LONG count = 0;
            updateColl->get_Count(&count);
            for (LONG i = 0; i < count; ++i) {
                ComPtr<IUpdate> upd;
                if (FAILED(updateColl->get_Item(i, upd.Put())) || !upd) {
                    continue;
                }

                DriverUpdateItem item;
                BstrGuard titleBstr;
                if (SUCCEEDED(upd->get_Title(titleBstr.Put()))) {
                    item.title = titleBstr.ToUtf8();
                }

                BstrGuard descBstr;
                if (SUCCEEDED(upd->get_Description(descBstr.Put()))) {
                    item.description = descBstr.ToUtf8();
                }

                ComPtr<IUpdateIdentity> identity;
                if (SUCCEEDED(upd->get_Identity(identity.Put())) && identity) {
                    BstrGuard idBstr;
                    if (SUCCEEDED(identity->get_UpdateID(idBstr.Put()))) {
                        item.updateId = idBstr.ToUtf8();
                    }
                    LONG rev = 0;
                    if (SUCCEEDED(identity->get_RevisionNumber(&rev))) {
                        item.revisionNumber = static_cast<int>(rev);
                    }
                }

                DECIMAL maxDec{};
                if (SUCCEEDED(upd->get_MaxDownloadSize(&maxDec))) {
                    item.maxSizeBytes = DecimalToUint64(maxDec);
                }
                item.sizeFormatted = FormatBytes(item.maxSizeBytes);

                VARIANT_BOOL downloaded = VARIANT_FALSE;
                if (SUCCEEDED(upd->get_IsDownloaded(&downloaded))) {
                    item.isDownloaded = (downloaded == VARIANT_TRUE);
                }

                item.driverModel = TryGetDispatchStringProperty(upd.Get(), L"DriverModel");
                item.driverManufacturer = TryGetDispatchStringProperty(upd.Get(), L"DriverManufacturer");
                item.driverProvider = TryGetDispatchStringProperty(upd.Get(), L"DriverProvider");
                item.driverVerDate = TryGetDispatchStringProperty(upd.Get(), L"DriverVerDate");
                item.driverHardwareId = TryGetDispatchStringProperty(upd.Get(), L"DriverHardwareID");

                if (item.driverVerDate.empty()) {
                    DATE deployDate = 0;
                    if (SUCCEEDED(upd->get_LastDeploymentChangeTime(&deployDate))) {
                        SYSTEMTIME st{};
                        if (VariantTimeToSystemTime(deployDate, &st)) {
                            char buf[32];
                            std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u",
                                          st.wYear, st.wMonth, st.wDay);
                            item.driverVerDate = buf;
                        }
                    }
                }

                item.statusText = item.isDownloaded ? "Загружен (готов к установке)" : "Доступен";
                item.selected = true;
                found.push_back(std::move(item));
            }
        }

        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            updates_ = std::move(found);
        }
        generation_.fetch_add(1);
        progress_.store(1.0f);

        const size_t total = GetUpdatesSnapshot().size();
        if (log) {
            if (total == 0) {
                log(LogLevel::Success, "Новых обновлений драйверов в Windows Update не найдено.");
            } else {
                log(LogLevel::Success,
                    "Найдено обновлений драйверов в Windows Update: " + std::to_string(total));
            }
        }
        SetPhaseMessage(total == 0 ? "Обновления не найдены"
                                   : "Найдено обновлений: " + std::to_string(total));
        searching_.store(false);
    });
}

void WindowsUpdateManager::StartDownloadAndInstallAsync(
    const std::vector<std::string>& selectedUpdateIds,
    bool createRestorePoint,
    LogCallback onLog,
    InstallDoneCallback onComplete) {
    if (IsBusy()) {
        if (onLog) {
            onLog(LogLevel::Warning, "Windows Update уже выполняет операцию.");
        }
        return;
    }
    if (selectedUpdateIds.empty()) {
        if (onLog) {
            onLog(LogLevel::Warning, "Не выбрано ни одного обновления для установки.");
        }
        return;
    }
    if (installThread_.joinable()) {
        installThread_.join();
    }

    cancelRequested_.store(false);
    installing_.store(true);
    progress_.store(0.0f);
    SetPhaseMessage("Подготовка к загрузке и установке...");

    installThread_ = std::thread([this,
                                  selectedUpdateIds,
                                  createRestorePoint,
                                  log = std::move(onLog),
                                  complete = std::move(onComplete)]() {
        WuInstallSummary summary;
        summary.requestedCount = selectedUpdateIds.size();

        if (IsProcessElevated()) {
            summary = DownloadAndInstallSync(
                selectedUpdateIds,
                createRestorePoint,
                log,
                &progress_,
                &cancelRequested_,
                [this](const std::string& phase) { SetPhaseMessage(phase); });

            // Update item statuses in UI list.
            std::set<std::string> reqSet(selectedUpdateIds.begin(), selectedUpdateIds.end());
            std::lock_guard<std::mutex> lock(dataMutex_);
            for (auto& item : updates_) {
                if (reqSet.count(item.updateId) > 0) {
                    if (summary.failedCount == 0) {
                        item.installedOk = true;
                        item.rebootRequired = summary.rebootRequired;
                        item.statusText = summary.rebootRequired
                                              ? "Установлено (нужна перезагрузка)"
                                              : "Установлено";
                    } else if (summary.installedCount > 0) {
                        item.statusText = "Обработано (проверьте лог)";
                    } else {
                        item.statusText = "Ошибка установки";
                    }
                }
            }
        } else {
            if (log) {
                log(LogLevel::Info,
                    "Запрос прав администратора (UAC) для установки драйверов Windows Update...");
            }
            SetPhaseMessage("Ожидание подтверждения UAC...");

            const std::filesystem::path tempLog = CreateTempLogPath(L"hdm_wu");
            std::wstring args = L"--cli-wu-install \"" + Utf8ToWide(JoinUpdateIds(selectedUpdateIds)) + L"\"";
            if (createRestorePoint) {
                args += L" --restore-before";
            }
            args += L" --log \"" + tempLog.wstring() + L"\"";

            SetPhaseMessage("Скачивание и установка с правами администратора...");
            progress_.store(0.25f);

            const ElevatedRunResult run = ExecuteElevatedSelf(args, true, &tempLog, log);
            std::error_code ec;
            std::filesystem::remove(tempLog, ec);

            if (!run.launched) {
                summary.failedCount = selectedUpdateIds.size();
                if (run.launchError == ERROR_CANCELLED) {
                    if (log) {
                        log(LogLevel::Warning, "Установка отменена пользователем в окне UAC.");
                    }
                } else if (log) {
                    log(LogLevel::Error,
                        "Не удалось запустить elevated-процесс: " + FormatWin32Error(run.launchError));
                }
            } else {
                if (run.exitCode == 2) {
                    summary.installedCount = selectedUpdateIds.size();
                    summary.rebootRequired = true;
                } else if (run.exitCode == 0) {
                    summary.installedCount = selectedUpdateIds.size();
                } else {
                    summary.failedCount = selectedUpdateIds.size();
                }

                std::set<std::string> reqSet(selectedUpdateIds.begin(), selectedUpdateIds.end());
                std::lock_guard<std::mutex> lock(dataMutex_);
                for (auto& item : updates_) {
                    if (reqSet.count(item.updateId) > 0) {
                        if (run.exitCode == 0 || run.exitCode == 2) {
                            item.installedOk = true;
                            item.rebootRequired = (run.exitCode == 2);
                            item.statusText = item.rebootRequired
                                                  ? "Установлено (нужна перезагрузка)"
                                                  : "Установлено";
                        } else {
                            item.statusText = "Ошибка установки";
                        }
                    }
                }
            }
            progress_.store(1.0f);
        }

        SetPhaseMessage(summary.rebootRequired
                            ? "Установка завершена (требуется перезагрузка)"
                            : "Операция Windows Update завершена");
        installing_.store(false);
        if (complete) {
            complete(summary);
        }
    });
}

WuInstallSummary WindowsUpdateManager::DownloadAndInstallSync(
    const std::vector<std::string>& updateIds,
    bool createRestorePoint,
    const LogCallback& onLog,
    std::atomic<float>* outProgress,
    std::atomic<bool>* cancelFlag,
    std::function<void(const std::string&)> onPhase) {
    WuInstallSummary summary;
    summary.requestedCount = updateIds.size();

    if (createRestorePoint) {
        if (onPhase) onPhase("Создание точки восстановления системы...");
        SystemRestore::CreateRestorePointSync(
            "DriverLoom: перед установкой драйверов Windows Update", onLog);
    }

    CoInitGuard comInit(COINIT_MULTITHREADED);
    if (!comInit.Succeeded()) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Ошибка CoInitializeEx (MTA): " + FormatHResult(comInit.Result()));
        }
        summary.failedCount = updateIds.size();
        return summary;
    }

    ComPtr<IUpdateSession> session;
    HRESULT hr = CoCreateInstance(
        CLSID_UpdateSession,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IUpdateSession,
        reinterpret_cast<void**>(session.Put()));
    if (FAILED(hr) || !session) {
        if (onLog) {
            onLog(LogLevel::Error, "Не удалось создать IUpdateSession: " + FormatHResult(hr));
        }
        summary.failedCount = updateIds.size();
        return summary;
    }

    BstrGuard appId(L"DriverLoom");
    session->put_ClientApplicationID(appId.Get());

    ComPtr<IUpdateSearcher> searcher;
    hr = session->CreateUpdateSearcher(searcher.Put());
    if (FAILED(hr) || !searcher) {
        if (onLog) {
            onLog(LogLevel::Error, "Не удалось создать IUpdateSearcher: " + FormatHResult(hr));
        }
        summary.failedCount = updateIds.size();
        return summary;
    }

    if (onPhase) onPhase("Подготовка списка выбранных обновлений...");
    if (onLog) {
        onLog(LogLevel::Info, "Получение метаданных выбранных обновлений Windows Update...");
    }

    BstrGuard criteria(L"IsInstalled=0");
    ComPtr<ISearchResult> searchResult;
    hr = searcher->Search(criteria.Get(), searchResult.Put());
    if (FAILED(hr) || !searchResult) {
        if (onLog) {
            onLog(LogLevel::Error, "Ошибка IUpdateSearcher::Search: " + FormatHResult(hr));
        }
        summary.failedCount = updateIds.size();
        return summary;
    }

    ComPtr<IUpdateCollection> allUpdates;
    hr = searchResult->get_Updates(allUpdates.Put());
    if (FAILED(hr) || !allUpdates) {
        summary.failedCount = updateIds.size();
        return summary;
    }

    ComPtr<IUpdateCollection> selectedColl;
    hr = CoCreateInstance(
        kClsidUpdateCollection,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IUpdateCollection,
        reinterpret_cast<void**>(selectedColl.Put()));
    if (FAILED(hr) || !selectedColl) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Не удалось создать IUpdateCollection: " + FormatHResult(hr));
        }
        summary.failedCount = updateIds.size();
        return summary;
    }

    std::set<std::string> targetIds(updateIds.begin(), updateIds.end());
    LONG allCount = 0;
    allUpdates->get_Count(&allCount);
    for (LONG i = 0; i < allCount; ++i) {
        ComPtr<IUpdate> upd;
        if (FAILED(allUpdates->get_Item(i, upd.Put())) || !upd) {
            continue;
        }
        ComPtr<IUpdateIdentity> idObj;
        if (FAILED(upd->get_Identity(idObj.Put())) || !idObj) {
            continue;
        }
        BstrGuard idBstr;
        if (FAILED(idObj->get_UpdateID(idBstr.Put()))) {
            continue;
        }
        if (targetIds.count(idBstr.ToUtf8()) > 0) {
            VARIANT_BOOL eulaAccepted = VARIANT_TRUE;
            if (SUCCEEDED(upd->get_EulaAccepted(&eulaAccepted)) && eulaAccepted == VARIANT_FALSE) {
                upd->AcceptEula();
            }
            LONG addedIdx = 0;
            selectedColl->Add(upd.Get(), &addedIdx);
        }
    }

    LONG selectedCount = 0;
    selectedColl->get_Count(&selectedCount);
    if (selectedCount == 0) {
        if (onLog) {
            onLog(LogLevel::Warning,
                  "Выбранные обновления больше не найдены в очереди Windows Update.");
        }
        return summary;
    }

    // --- Step 1: Download ---
    if (onPhase) onPhase("Скачивание драйверов из Windows Update...");
    if (onLog) {
        onLog(LogLevel::Info,
              "Скачивание пакетов драйверов (" + std::to_string(selectedCount) + " шт.)...");
    }

    ComPtr<IUpdateDownloader> downloader;
    hr = session->CreateUpdateDownloader(downloader.Put());
    if (FAILED(hr) || !downloader) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Не удалось создать IUpdateDownloader: " + FormatHResult(hr));
        }
        summary.failedCount = static_cast<size_t>(selectedCount);
        return summary;
    }

    downloader->put_Updates(selectedColl.Get());
    VARIANT emptyState;
    VariantInit(&emptyState);

    ComPtr<WuCompletedCallback> dlProgCb;
    *dlProgCb.Put() = new WuCompletedCallback(kIidDownloadProgressChangedCallback, false);
    ComPtr<WuCompletedCallback> dlCb;
    *dlCb.Put() = new WuCompletedCallback(kIidDownloadCompletedCallback);

    ComPtr<IDownloadJob> dlJob;
    hr = downloader->BeginDownload(dlProgCb.Get(), dlCb.Get(), emptyState, dlJob.Put());
    if (FAILED(hr) || !dlJob) {
        if (onLog) {
            onLog(LogLevel::Error, "Ошибка BeginDownload: " + FormatHResult(hr));
        }
        summary.failedCount = static_cast<size_t>(selectedCount);
        return summary;
    }

    while (true) {
        if (cancelFlag != nullptr && cancelFlag->load()) {
            dlJob->RequestAbort();
            break;
        }
        ComPtr<IDownloadProgress> dlProg;
        if (SUCCEEDED(dlJob->GetProgress(dlProg.Put())) && dlProg) {
            LONG pct = 0;
            dlProg->get_PercentComplete(&pct);
            if (outProgress != nullptr) {
                outProgress->store(0.5f * (static_cast<float>(pct) / 100.0f));
            }
            if (onPhase) {
                onPhase("Скачивание драйверов: " + std::to_string(pct) + "%");
            }
        }
        if (dlCb->EventHandle() != nullptr &&
            WaitForSingleObject(dlCb->EventHandle(), 200) == WAIT_OBJECT_0) {
            break;
        }
        VARIANT_BOOL done = VARIANT_FALSE;
        if (SUCCEEDED(dlJob->get_IsCompleted(&done)) && done == VARIANT_TRUE) {
            break;
        }
    }

    ComPtr<IDownloadResult> dlResult;
    hr = downloader->EndDownload(dlJob.Get(), dlResult.Put());
    dlJob->CleanUp();

    if (cancelFlag != nullptr && cancelFlag->load()) {
        if (onLog) {
            onLog(LogLevel::Warning, "Скачивание драйверов отменено.");
        }
        summary.failedCount = static_cast<size_t>(selectedCount);
        return summary;
    }

    if (FAILED(hr) || !dlResult) {
        if (onLog) {
            onLog(LogLevel::Error, "Ошибка EndDownload: " + FormatHResult(hr));
        }
        summary.failedCount = static_cast<size_t>(selectedCount);
        return summary;
    }

    OperationResultCode dlCode = orcNotStarted;
    dlResult->get_ResultCode(&dlCode);
    if (onLog) {
        onLog(dlCode == orcSucceeded ? LogLevel::Success : LogLevel::Warning,
              "Результат скачивания: " + ResultCodeToText(dlCode));
    }

    // Collect only updates that are actually downloaded.
    ComPtr<IUpdateCollection> toInstallColl;
    CoCreateInstance(
        kClsidUpdateCollection,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IUpdateCollection,
        reinterpret_cast<void**>(toInstallColl.Put()));

    for (LONG i = 0; i < selectedCount; ++i) {
        ComPtr<IUpdate> upd;
        if (SUCCEEDED(selectedColl->get_Item(i, upd.Put())) && upd) {
            VARIANT_BOOL isDl = VARIANT_FALSE;
            if (SUCCEEDED(upd->get_IsDownloaded(&isDl)) && isDl == VARIANT_TRUE && toInstallColl) {
                LONG idx = 0;
                toInstallColl->Add(upd.Get(), &idx);
            }
        }
    }

    LONG installCount = 0;
    if (toInstallColl) {
        toInstallColl->get_Count(&installCount);
    }
    if (installCount == 0) {
        if (onLog) {
            onLog(LogLevel::Error, "Ни один из выбранных драйверов не был загружен.");
        }
        summary.failedCount = static_cast<size_t>(selectedCount);
        return summary;
    }

    // --- Step 2: Install ---
    if (onPhase) onPhase("Установка загруженных драйверов...");
    if (onLog) {
        onLog(LogLevel::Info,
              "Установка загруженных драйверов (" + std::to_string(installCount) + " шт.)...");
    }

    ComPtr<IUpdateInstaller> installer;
    hr = session->CreateUpdateInstaller(installer.Put());
    if (FAILED(hr) || !installer) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Не удалось создать IUpdateInstaller: " + FormatHResult(hr));
        }
        summary.failedCount = static_cast<size_t>(installCount);
        return summary;
    }

    installer->put_Updates(toInstallColl.Get());

    ComPtr<WuCompletedCallback> instProgCb;
    *instProgCb.Put() = new WuCompletedCallback(kIidInstallationProgressChangedCallback, false);
    ComPtr<WuCompletedCallback> instCb;
    *instCb.Put() = new WuCompletedCallback(kIidInstallationCompletedCallback);

    ComPtr<IInstallationJob> instJob;
    hr = installer->BeginInstall(instProgCb.Get(), instCb.Get(), emptyState, instJob.Put());
    if (FAILED(hr) || !instJob) {
        if (onLog) {
            onLog(LogLevel::Error, "Ошибка BeginInstall: " + FormatHResult(hr));
        }
        summary.failedCount = static_cast<size_t>(installCount);
        return summary;
    }

    while (true) {
        if (cancelFlag != nullptr && cancelFlag->load()) {
            instJob->RequestAbort();
            break;
        }
        ComPtr<IInstallationProgress> instProg;
        if (SUCCEEDED(instJob->GetProgress(instProg.Put())) && instProg) {
            LONG pct = 0;
            instProg->get_PercentComplete(&pct);
            if (outProgress != nullptr) {
                outProgress->store(0.5f + 0.5f * (static_cast<float>(pct) / 100.0f));
            }
            if (onPhase) {
                onPhase("Установка драйверов: " + std::to_string(pct) + "%");
            }
        }
        if (instCb->EventHandle() != nullptr &&
            WaitForSingleObject(instCb->EventHandle(), 200) == WAIT_OBJECT_0) {
            break;
        }
        VARIANT_BOOL done = VARIANT_FALSE;
        if (SUCCEEDED(instJob->get_IsCompleted(&done)) && done == VARIANT_TRUE) {
            break;
        }
    }

    ComPtr<IInstallationResult> instResult;
    hr = installer->EndInstall(instJob.Get(), instResult.Put());
    instJob->CleanUp();

    if (FAILED(hr) || !instResult) {
        if (onLog) {
            onLog(LogLevel::Error, "Ошибка EndInstall: " + FormatHResult(hr));
        }
        summary.failedCount = static_cast<size_t>(installCount);
        return summary;
    }

    OperationResultCode instCode = orcNotStarted;
    instResult->get_ResultCode(&instCode);
    VARIANT_BOOL rebootReq = VARIANT_FALSE;
    instResult->get_RebootRequired(&rebootReq);
    summary.rebootRequired = (rebootReq == VARIANT_TRUE);

    for (LONG i = 0; i < installCount; ++i) {
        ComPtr<IUpdate> upd;
        BstrGuard titleBstr;
        if (SUCCEEDED(toInstallColl->get_Item(i, upd.Put())) && upd) {
            upd->get_Title(titleBstr.Put());
        }
        ComPtr<IUpdateInstallationResult> itemRes;
        OperationResultCode itemCode = orcNotStarted;
        if (SUCCEEDED(instResult->GetUpdateResult(i, itemRes.Put())) && itemRes) {
            itemRes->get_ResultCode(&itemCode);
        }

        if (itemCode == orcSucceeded || itemCode == orcSucceededWithErrors) {
            ++summary.installedCount;
            if (onLog) {
                onLog(LogLevel::Success,
                      "Установлен: " + titleBstr.ToUtf8() + " [" + ResultCodeToText(itemCode) + "]");
            }
        } else {
            ++summary.failedCount;
            if (onLog) {
                onLog(LogLevel::Error,
                      "Ошибка установки: " + titleBstr.ToUtf8() + " [" +
                          ResultCodeToText(itemCode) + "]");
            }
        }
    }

    if (summary.rebootRequired && onLog) {
        onLog(LogLevel::Warning,
              "Для завершения установки драйверов требуется перезагрузка компьютера!");
    }
    if (outProgress != nullptr) {
        outProgress->store(1.0f);
    }
    return summary;
}

} // namespace hdm
