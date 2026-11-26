#include "core/WindowsUpdateManager.hpp"
#include "core/SystemRestore.hpp"

#include <oleauto.h>
#include <initguid.h>
#include <wuapi.h>
#include <wuerror.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_set>

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

constexpr std::chrono::seconds kSearchOfflineTimeout{30};
constexpr std::chrono::seconds kSearchOnlineTimeout{90};
constexpr std::chrono::seconds kDownloadTotalTimeout{600};
constexpr std::chrono::seconds kDownloadStallTimeout{180};
constexpr std::chrono::seconds kInstallDriverTimeout{300};
constexpr std::chrono::seconds kInstallSoftwareTimeout{720};
constexpr std::chrono::seconds kAbortGraceTimeout{5};
constexpr std::chrono::seconds kBusyWaitTimeout{25};

// Generic COM callback for WUAPI async completion interfaces
// (share IUnknown + Invoke(job, args)).
class WuCompletedCallback final : public IUnknown {
public:
    explicit WuCompletedCallback(REFIID callbackIid) noexcept
        : callbackIid_(callbackIid),
          event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

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

class WuProgressCallback final : public IUnknown {
public:
    explicit WuProgressCallback(REFIID callbackIid) noexcept
        : callbackIid_(callbackIid) {}

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
        return S_OK;
    }

private:
    ~WuProgressCallback() = default;

    std::atomic<LONG> refCount_{1};
    IID callbackIid_{};
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

bool TryPutDispatchBoolProperty(IDispatch* disp, const wchar_t* propName, bool value) {
    if (disp == nullptr || propName == nullptr) {
        return false;
    }
    DISPID dispId = DISPID_UNKNOWN;
    LPOLESTR namePtr = const_cast<LPOLESTR>(propName);
    if (FAILED(disp->GetIDsOfNames(IID_NULL, &namePtr, 1, LOCALE_USER_DEFAULT, &dispId))) {
        return false;
    }
    VARIANT varVal;
    VariantInit(&varVal);
    varVal.vt = VT_BOOL;
    varVal.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    DISPID namedArg = DISPID_PROPERTYPUT;
    DISPPARAMS params{};
    params.rgvarg = &varVal;
    params.rgdispidNamedArgs = &namedArg;
    params.cArgs = 1;
    params.cNamedArgs = 1;
    const HRESULT hr = disp->Invoke(
        dispId, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYPUT, &params, nullptr, nullptr, nullptr);
    VariantClear(&varVal);
    return SUCCEEDED(hr);
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

std::string FormatWuErrorDiagnostic(HRESULT hr) {
    if (hr == S_OK) {
        return "Успешно";
    }
    if (hr == WU_E_EXCLUSIVE_INSTALL_CONFLICT) {
        return "Служба Windows Update занята другой установкой (" + FormatHResult(hr) + ")";
    }
    if (hr == WU_E_NO_INTERACTIVE_USER || hr == WU_E_UH_CANREQUIREINPUT ||
        hr == WU_E_UH_DOESNOTSUPPORTACTION) {
        return "Пакет требует интерактивного ввода (" + FormatHResult(hr) +
               ") — установите его через «Параметры Windows -> Центр обновления Windows»";
    }
    if (hr == WU_E_UH_INSTALLERHUNG) {
        return "Внутренний установщик Windows Update перестал отвечать (WU_E_UH_INSTALLERHUNG, " +
               FormatHResult(hr) + ")";
    }
    if (hr == WU_E_SETUP_REBOOTREQUIRED || hr == WU_E_UH_POSTREBOOTSTILLPENDING) {
        return "Для продолжения требуется перезагрузка Windows (" + FormatHResult(hr) + ")";
    }
    return FormatHResult(hr);
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

std::string GetUpdateIdentityId(IUpdate* upd) {
    if (upd == nullptr) {
        return {};
    }
    ComPtr<IUpdateIdentity> idObj;
    if (FAILED(upd->get_Identity(idObj.Put())) || !idObj) {
        return {};
    }
    BstrGuard idBstr;
    if (FAILED(idObj->get_UpdateID(idBstr.Put()))) {
        return {};
    }
    return idBstr.ToUtf8();
}

std::string GetUpdateTitle(IUpdate* upd) {
    if (upd == nullptr) {
        return {};
    }
    BstrGuard titleBstr;
    if (FAILED(upd->get_Title(titleBstr.Put()))) {
        return {};
    }
    return titleBstr.ToUtf8();
}

bool CheckUpdateCanRequestUserInput(IUpdate* upd) {
    if (upd == nullptr) {
        return false;
    }
    ComPtr<IInstallationBehavior> behavior;
    if (FAILED(upd->get_InstallationBehavior(behavior.Put())) || !behavior) {
        return false;
    }
    VARIANT_BOOL canInput = VARIANT_FALSE;
    if (FAILED(behavior->get_CanRequestUserInput(&canInput))) {
        return false;
    }
    return canInput == VARIANT_TRUE;
}

bool IsDriverUpdate(IUpdate* upd) {
    if (upd == nullptr) {
        return false;
    }
    UpdateType type = utSoftware;
    if (SUCCEEDED(upd->get_Type(&type))) {
        return type == utDriver;
    }
    return false;
}

enum class JobWaitOutcome {
    Completed,
    Cancelled,
    TimedOut
};

// Transfers hung COM job ownership to a detached MTA cleanup thread so blocked RPC
// teardown calls (EndInstall / EndDownload / EndSearch) never freeze worker or UI teardown.
template <typename WorkerT, typename JobT, typename EndFn>
void AbandonWuJobAsync(
    ComPtr<IUpdateSession>& session,
    ComPtr<WorkerT>& worker,
    ComPtr<JobT>& job,
    ComPtr<WuCompletedCallback>& cb,
    EndFn&& endFn) {
    IUpdateSession* rawSession = session.Detach();
    WorkerT* rawWorker = worker.Detach();
    JobT* rawJob = job.Detach();
    WuCompletedCallback* rawCb = cb.Detach();
    std::thread([rawSession, rawWorker, rawJob, rawCb, fn = std::forward<EndFn>(endFn)]() {
        CoInitGuard comInit(COINIT_MULTITHREADED);
        if (rawWorker != nullptr && rawJob != nullptr) {
            fn(rawWorker, rawJob);
            rawJob->CleanUp();
        }
        if (rawJob != nullptr) rawJob->Release();
        if (rawCb != nullptr) rawCb->Release();
        if (rawWorker != nullptr) rawWorker->Release();
        if (rawSession != nullptr) rawSession->Release();
    }).detach();
}

template <typename JobT, typename TickFn>
JobWaitOutcome WaitForWuJob(
    JobT* job,
    WuCompletedCallback* cb,
    std::chrono::seconds maxTotalTimeout,
    std::chrono::seconds stallTimeout,
    const std::atomic<bool>* cancelFlag,
    TickFn&& onTick,
    bool* outAbortedCleanly = nullptr) {
    using clock = std::chrono::steady_clock;
    const auto startTime = clock::now();
    auto lastActivityTime = startTime;

    JobWaitOutcome outcome = JobWaitOutcome::Completed;
    for (;;) {
        if (cancelFlag != nullptr && cancelFlag->load()) {
            outcome = JobWaitOutcome::Cancelled;
            break;
        }

        const auto now = clock::now();
        if (onTick(now - startTime)) {
            lastActivityTime = now;
        }

        VARIANT_BOOL done = VARIANT_FALSE;
        const HRESULT hrDone = job->get_IsCompleted(&done);
        if (SUCCEEDED(hrDone) && done == VARIANT_TRUE) {
            outcome = JobWaitOutcome::Completed;
            break;
        }
        if (FAILED(hrDone)) {
            outcome = JobWaitOutcome::TimedOut;
            break;
        }

        if (cb != nullptr && cb->EventHandle() != nullptr) {
            if (WaitForSingleObject(cb->EventHandle(), 400) == WAIT_OBJECT_0) {
                outcome = JobWaitOutcome::Completed;
                break;
            }
        } else {
            Sleep(400);
        }

        if (cancelFlag != nullptr && cancelFlag->load()) {
            outcome = JobWaitOutcome::Cancelled;
            break;
        }

        const auto nowAfter = clock::now();
        if (nowAfter - startTime >= maxTotalTimeout ||
            (stallTimeout.count() > 0 && nowAfter - lastActivityTime >= stallTimeout)) {
            outcome = JobWaitOutcome::TimedOut;
            break;
        }
    }

    if (outcome == JobWaitOutcome::Completed) {
        if (outAbortedCleanly != nullptr) {
            *outAbortedCleanly = true;
        }
        return outcome;
    }

    job->RequestAbort();
    const auto abortDeadline = clock::now() + kAbortGraceTimeout;
    bool aborted = false;
    while (clock::now() < abortDeadline) {
        VARIANT_BOOL done = VARIANT_FALSE;
        if (SUCCEEDED(job->get_IsCompleted(&done)) && done == VARIANT_TRUE) {
            aborted = true;
            break;
        }
        if (cb != nullptr && cb->EventHandle() != nullptr) {
            if (WaitForSingleObject(cb->EventHandle(), 200) == WAIT_OBJECT_0) {
                aborted = true;
                break;
            }
        } else {
            Sleep(200);
        }
    }
    if (outAbortedCleanly != nullptr) {
        *outAbortedCleanly = aborted;
    }
    return outcome;
}

bool RecreateWuSessionAndSearcher(
    ComPtr<IUpdateSession>& session,
    ComPtr<IUpdateSearcher>& searcher) {
    session.Reset();
    searcher.Reset();
    HRESULT hr = CoCreateInstance(
        CLSID_UpdateSession,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IUpdateSession,
        reinterpret_cast<void**>(session.Put()));
    if (FAILED(hr) || !session) {
        return false;
    }
    BstrGuard appId(L"DriverLoom");
    session->put_ClientApplicationID(appId.Get());
    hr = session->CreateUpdateSearcher(searcher.Put());
    return SUCCEEDED(hr) && searcher;
}

HRESULT RunAsyncSearch(
    ComPtr<IUpdateSession>& session,
    ComPtr<IUpdateSearcher>& searcher,
    const wchar_t* criteriaW,
    bool online,
    std::chrono::seconds timeout,
    const std::atomic<bool>* cancelFlag,
    ComPtr<ISearchResult>& outResult,
    bool* outAbortedCleanly,
    JobWaitOutcome* outOutcome) {
    outResult.Reset();
    if (outAbortedCleanly != nullptr) *outAbortedCleanly = true;
    if (outOutcome != nullptr) *outOutcome = JobWaitOutcome::Completed;

    HRESULT hrOnline = searcher->put_Online(online ? VARIANT_TRUE : VARIANT_FALSE);
    if (FAILED(hrOnline)) {
        return hrOnline;
    }

    BstrGuard criteria(criteriaW);
    VARIANT emptyState;
    VariantInit(&emptyState);

    ComPtr<WuCompletedCallback> searchCb;
    *searchCb.Put() = new WuCompletedCallback(kIidSearchCompletedCallback);

    ComPtr<ISearchJob> searchJob;
    HRESULT hr = searcher->BeginSearch(
        criteria.Get(), searchCb.Get(), emptyState, searchJob.Put());
    if (FAILED(hr) || !searchJob) {
        return FAILED(hr) ? hr : E_FAIL;
    }

    bool abortedCleanly = true;
    const JobWaitOutcome outcome = WaitForWuJob(
        searchJob.Get(),
        searchCb.Get(),
        timeout,
        std::chrono::seconds(0),
        cancelFlag,
        [](std::chrono::steady_clock::duration) { return false; },
        &abortedCleanly);

    if (outAbortedCleanly != nullptr) *outAbortedCleanly = abortedCleanly;
    if (outOutcome != nullptr) *outOutcome = outcome;

    if (outcome != JobWaitOutcome::Completed && !abortedCleanly) {
        AbandonWuJobAsync(
            session,
            searcher,
            searchJob,
            searchCb,
            [](IUpdateSearcher* s, ISearchJob* j) {
                ISearchResult* r = nullptr;
                if (SUCCEEDED(s->EndSearch(j, &r)) && r != nullptr) {
                    r->Release();
                }
            });
        return outcome == JobWaitOutcome::Cancelled ? E_ABORT : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }

    hr = searcher->EndSearch(searchJob.Get(), outResult.Put());
    searchJob->CleanUp();

    if (outcome == JobWaitOutcome::Cancelled) {
        return E_ABORT;
    }
    if (outcome == JobWaitOutcome::TimedOut) {
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    return hr;
}

bool WaitWhileInstallerBusy(
    IUpdateInstaller* installer,
    const std::atomic<bool>* cancelFlag,
    const std::function<void(const std::string&)>& onPhase,
    const LogCallback& onLog) {
    if (installer == nullptr) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + kBusyWaitTimeout;
    bool loggedWait = false;
    for (;;) {
        if (cancelFlag != nullptr && cancelFlag->load()) {
            return false;
        }
        VARIANT_BOOL busy = VARIANT_FALSE;
        if (FAILED(installer->get_IsBusy(&busy)) || busy == VARIANT_FALSE) {
            return true;
        }
        if (!loggedWait) {
            loggedWait = true;
            if (onPhase) {
                onPhase("Ожидание освобождения службы Windows Update...");
            }
            if (onLog) {
                onLog(LogLevel::Info,
                      "Служба Windows Update сейчас занята другой задачей — ожидание до 25 с...");
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            if (onLog) {
                onLog(LogLevel::Error,
                      "Служба Windows Update остаётся занятой (IsBusy=TRUE). "
                      "Закройте «Центр обновления Windows» или повторите позже.");
            }
            return false;
        }
        Sleep(500);
    }
}

void ConfigureInstallerSilent(IUpdateInstaller* installer, bool quiet) {
    if (installer == nullptr) {
        return;
    }
    installer->put_AllowSourcePrompts(VARIANT_FALSE);
    // Prevent Windows Installer / OEM wrappers from opening hidden interactive prompts.
    TryPutDispatchBoolProperty(installer, L"ForceQuiet", quiet);
}

ComPtr<IUpdateCollection> MakeSingleUpdateCollection(IUpdate* upd) {
    ComPtr<IUpdateCollection> coll;
    if (upd == nullptr) {
        return coll;
    }
    if (FAILED(CoCreateInstance(
            kClsidUpdateCollection,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_IUpdateCollection,
            reinterpret_cast<void**>(coll.Put()))) ||
        !coll) {
        coll.Reset();
        return coll;
    }
    LONG addedIdx = 0;
    if (FAILED(coll->Add(upd, &addedIdx))) {
        coll.Reset();
        return coll;
    }
    return coll;
}

} // namespace

struct WindowsUpdateManager::Impl {
    Impl() {
        searchDoneEvent.Reset(CreateEventW(nullptr, TRUE, TRUE, nullptr));
        installDoneEvent.Reset(CreateEventW(nullptr, TRUE, TRUE, nullptr));
    }

    std::atomic<bool> searching{false};
    std::atomic<bool> installing{false};
    std::atomic<bool> cancelRequested{false};
    std::atomic<float> progress{0.0f};
    std::atomic<uint64_t> generation{0};

    mutable std::mutex dataMutex;
    std::mutex threadMutex;
    std::mutex callbackMutex;
    bool ownerAlive{true};
    std::vector<DriverUpdateItem> updates;
    std::string phaseMessage;

    HandleGuard searchDoneEvent;
    HandleGuard installDoneEvent;
    std::thread searchThread;
    std::thread installThread;
};

WindowsUpdateManager::WindowsUpdateManager()
    : impl_(std::make_shared<Impl>()) {}

WindowsUpdateManager::~WindowsUpdateManager() {
    if (!impl_) {
        return;
    }
    impl_->cancelRequested.store(true);
    {
        std::lock_guard<std::mutex> cbLock(impl_->callbackMutex);
        impl_->ownerAlive = false;
    }

    std::thread sThread;
    std::thread iThread;
    {
        std::lock_guard<std::mutex> tLock(impl_->threadMutex);
        HANDLE handles[2]{};
        DWORD count = 0;
        if (impl_->searchThread.joinable() && impl_->searchDoneEvent.IsValid()) {
            handles[count++] = impl_->searchDoneEvent.Get();
        }
        if (impl_->installThread.joinable() && impl_->installDoneEvent.IsValid()) {
            handles[count++] = impl_->installDoneEvent.Get();
        }
        if (count > 0) {
            WaitForMultipleObjects(count, handles, TRUE, 2500);
        }

        if (impl_->searchThread.joinable()) {
            if (impl_->searchDoneEvent.IsValid() &&
                WaitForSingleObject(impl_->searchDoneEvent.Get(), 0) == WAIT_OBJECT_0) {
                sThread = std::move(impl_->searchThread);
            } else {
                impl_->searchThread.detach();
            }
        }
        if (impl_->installThread.joinable()) {
            if (impl_->installDoneEvent.IsValid() &&
                WaitForSingleObject(impl_->installDoneEvent.Get(), 0) == WAIT_OBJECT_0) {
                iThread = std::move(impl_->installThread);
            } else {
                impl_->installThread.detach();
            }
        }
    }
    if (sThread.joinable()) {
        sThread.join();
    }
    if (iThread.joinable()) {
        iThread.join();
    }
}

void WindowsUpdateManager::RequestCancel() {
    impl_->cancelRequested.store(true);
    std::lock_guard<std::mutex> lock(impl_->dataMutex);
    if (impl_->searching.load() || impl_->installing.load()) {
        impl_->phaseMessage = "Отмена операции Windows Update...";
    }
}

bool WindowsUpdateManager::IsSearching() const noexcept {
    return impl_->searching.load();
}

bool WindowsUpdateManager::IsInstalling() const noexcept {
    return impl_->installing.load();
}

bool WindowsUpdateManager::IsBusy() const noexcept {
    return impl_->searching.load() || impl_->installing.load();
}

float WindowsUpdateManager::Progress() const noexcept {
    return impl_->progress.load();
}

uint64_t WindowsUpdateManager::Generation() const noexcept {
    return impl_->generation.load();
}

std::string WindowsUpdateManager::PhaseMessage() const {
    std::lock_guard<std::mutex> lock(impl_->dataMutex);
    return impl_->phaseMessage;
}

std::vector<DriverUpdateItem> WindowsUpdateManager::GetUpdatesSnapshot() const {
    std::lock_guard<std::mutex> lock(impl_->dataMutex);
    return impl_->updates;
}

void WindowsUpdateManager::SetUpdateSelected(const std::string& updateId, bool selected) {
    std::lock_guard<std::mutex> lock(impl_->dataMutex);
    for (auto& item : impl_->updates) {
        if (item.updateId == updateId) {
            item.selected = selected;
            break;
        }
    }
}

void WindowsUpdateManager::SelectAll(bool selected, bool includeHidden) {
    std::lock_guard<std::mutex> lock(impl_->dataMutex);
    for (auto& item : impl_->updates) {
        if (!selected) {
            item.selected = false;
        } else if (includeHidden || !item.isHidden) {
            item.selected = true;
        }
    }
}

void WindowsUpdateManager::StartSearchAsync(LogCallback onLog, bool driversOnly) {
    std::thread oldSearch;
    bool busy = false;
    {
        std::lock_guard<std::mutex> tLock(impl_->threadMutex);
        std::lock_guard<std::mutex> cbLock(impl_->callbackMutex);
        if (!impl_->ownerAlive) {
            return;
        }
        if (impl_->searching.load() || impl_->installing.load()) {
            busy = true;
        } else {
            impl_->cancelRequested.store(false);
            impl_->searching.store(true);
            impl_->progress.store(0.0f);
            if (impl_->searchDoneEvent.IsValid()) {
                ResetEvent(impl_->searchDoneEvent.Get());
            }
            if (impl_->searchThread.joinable()) {
                oldSearch = std::move(impl_->searchThread);
            }
        }
    }
    if (busy) {
        if (onLog) {
            onLog(LogLevel::Warning, "Windows Update уже выполняет операцию.");
        }
        return;
    }
    if (oldSearch.joinable()) {
        oldSearch.join();
    }

    auto setPhase = [state = impl_](const std::string& msg) {
        std::lock_guard<std::mutex> lock(state->dataMutex);
        state->phaseMessage = msg;
    };
    setPhase("Подключение к службе Windows Update...");

    std::lock_guard<std::mutex> tLock(impl_->threadMutex);
    {
        std::lock_guard<std::mutex> cbLock(impl_->callbackMutex);
        if (!impl_->ownerAlive) {
            impl_->searching.store(false);
            if (impl_->searchDoneEvent.IsValid()) SetEvent(impl_->searchDoneEvent.Get());
            return;
        }
    }

    std::shared_ptr<Impl> state = impl_;
    impl_->searchThread = std::thread([state, setPhase, driversOnly, rawLog = std::move(onLog)]() {
        auto log = [&state, &rawLog](LogLevel lvl, const std::string& msg) {
            std::lock_guard<std::mutex> cbLock(state->callbackMutex);
            if (state->ownerAlive && rawLog) {
                rawLog(lvl, msg);
            }
        };

        struct DoneSignal {
            HANDLE ev;
            std::atomic<bool>& flag;
            ~DoneSignal() {
                flag.store(false);
                if (ev != nullptr) SetEvent(ev);
            }
        } guard{state->searchDoneEvent.Get(), state->searching};

        CoInitGuard comInit(COINIT_MULTITHREADED);
        if (!comInit.Succeeded()) {
            log(LogLevel::Error,
                "Ошибка CoInitializeEx (MTA): " + FormatHResult(comInit.Result()));
            return;
        }

        const wchar_t* queryW =
            driversOnly
                ? L"IsInstalled=0 and Type='Driver' and IsHidden=0 or IsInstalled=0 and Type='Driver' and IsHidden=1"
                : L"IsInstalled=0 and IsHidden=0 or IsInstalled=0 and IsHidden=1";
        log(LogLevel::Info,
            driversOnly
                ? "Поиск доступных драйверов через Windows Update API (Type='Driver')..."
                : "Расширенный поиск драйверов и системных обновлений Windows Update...");

        ComPtr<IUpdateSession> session;
        HRESULT hr = CoCreateInstance(
            CLSID_UpdateSession,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_IUpdateSession,
            reinterpret_cast<void**>(session.Put()));
        if (FAILED(hr) || !session) {
            log(LogLevel::Error,
                "Не удалось создать IUpdateSession: " + FormatHResult(hr));
            setPhase("Ошибка создания IUpdateSession");
            return;
        }

        BstrGuard appId(L"DriverLoom");
        session->put_ClientApplicationID(appId.Get());

        ComPtr<IUpdateSearcher> searcher;
        hr = session->CreateUpdateSearcher(searcher.Put());
        if (FAILED(hr) || !searcher) {
            log(LogLevel::Error,
                "Не удалось создать IUpdateSearcher: " + FormatHResult(hr));
            setPhase("Ошибка создания IUpdateSearcher");
            return;
        }

        setPhase("Поиск обновлений на сервере Windows Update...");
        ComPtr<ISearchResult> searchResult;
        bool abortedCleanly = true;
        JobWaitOutcome outcome = JobWaitOutcome::Completed;
        hr = RunAsyncSearch(
            session,
            searcher,
            queryW,
            true,
            kSearchOnlineTimeout,
            &state->cancelRequested,
            searchResult,
            &abortedCleanly,
            &outcome);

        if (outcome == JobWaitOutcome::Cancelled) {
            log(LogLevel::Warning, "Поиск обновлений Windows Update отменён.");
            setPhase("Поиск отменён");
            return;
        }
        if (outcome == JobWaitOutcome::TimedOut) {
            log(LogLevel::Error, "Тайм-аут поиска обновлений Windows Update.");
            setPhase("Тайм-аут поиска обновлений");
            return;
        }
        if (FAILED(hr) || !searchResult) {
            log(LogLevel::Error,
                "Ошибка EndSearch Windows Update: " + FormatHResult(hr));
            setPhase("Ошибка получения результатов поиска");
            return;
        }

        OperationResultCode resultCode = orcNotStarted;
        searchResult->get_ResultCode(&resultCode);
        if (resultCode != orcSucceeded && resultCode != orcSucceededWithErrors) {
            log(LogLevel::Warning,
                "Поиск Windows Update завершился со статусом: " + ResultCodeToText(resultCode));
        }

        std::vector<DriverUpdateItem> found;
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
                item.title = GetUpdateTitle(upd.Get());

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
                VARIANT_BOOL hidden = VARIANT_FALSE;
                if (SUCCEEDED(upd->get_IsHidden(&hidden))) {
                    item.isHidden = (hidden == VARIANT_TRUE);
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

                item.statusText =
                    item.isHidden
                        ? "Скрыто (заблокировано)"
                        : (item.isDownloaded ? "Загружен (готов к установке)" : "Доступен");
                item.selected = !item.isHidden;
                found.push_back(std::move(item));
            }
        }

        const size_t total = found.size();
        {
            std::lock_guard<std::mutex> lock(state->dataMutex);
            state->updates = std::move(found);
        }
        state->generation.fetch_add(1);
        state->progress.store(1.0f);

        if (total == 0) {
            log(LogLevel::Success, "Новых обновлений драйверов в Windows Update не найдено.");
        } else {
            log(LogLevel::Success,
                "Найдено обновлений в Windows Update: " + std::to_string(total));
        }
        setPhase(total == 0 ? "Обновления не найдены"
                            : "Найдено обновлений: " + std::to_string(total));
    });
}

void WindowsUpdateManager::StartDownloadAndInstallAsync(
    const std::vector<std::string>& selectedUpdateIds,
    bool createRestorePoint,
    LogCallback onLog,
    InstallDoneCallback onComplete,
    bool driversOnly) {
    if (selectedUpdateIds.empty()) {
        if (onLog) {
            onLog(LogLevel::Warning, "Не выбрано ни одного обновления для установки.");
        }
        return;
    }

    std::thread oldInstall;
    bool busy = false;
    {
        std::lock_guard<std::mutex> tLock(impl_->threadMutex);
        std::lock_guard<std::mutex> cbLock(impl_->callbackMutex);
        if (!impl_->ownerAlive) {
            return;
        }
        if (impl_->searching.load() || impl_->installing.load()) {
            busy = true;
        } else {
            impl_->cancelRequested.store(false);
            impl_->installing.store(true);
            impl_->progress.store(0.0f);
            if (impl_->installDoneEvent.IsValid()) {
                ResetEvent(impl_->installDoneEvent.Get());
            }
            if (impl_->installThread.joinable()) {
                oldInstall = std::move(impl_->installThread);
            }
        }
    }
    if (busy) {
        if (onLog) {
            onLog(LogLevel::Warning, "Windows Update уже выполняет операцию.");
        }
        return;
    }
    if (oldInstall.joinable()) {
        oldInstall.join();
    }

    auto setPhase = [state = impl_](const std::string& msg) {
        std::lock_guard<std::mutex> lock(state->dataMutex);
        state->phaseMessage = msg;
    };
    auto updateItem = [state = impl_](
                          const std::string& updateId,
                          bool installedOk,
                          bool rebootReq,
                          const std::string& statusText) {
        {
            std::lock_guard<std::mutex> lock(state->dataMutex);
            for (auto& item : state->updates) {
                if (item.updateId == updateId) {
                    item.installedOk = installedOk;
                    item.rebootRequired = rebootReq;
                    if (!statusText.empty()) {
                        item.statusText = statusText;
                    }
                    break;
                }
            }
        }
        state->generation.fetch_add(1);
    };

    setPhase("Подготовка к загрузке и установке...");

    std::lock_guard<std::mutex> tLock(impl_->threadMutex);
    {
        std::lock_guard<std::mutex> cbLock(impl_->callbackMutex);
        if (!impl_->ownerAlive) {
            impl_->installing.store(false);
            if (impl_->installDoneEvent.IsValid()) SetEvent(impl_->installDoneEvent.Get());
            return;
        }
    }

    std::shared_ptr<Impl> state = impl_;
    impl_->installThread = std::thread([state,
                                        setPhase,
                                        updateItem,
                                        selectedUpdateIds,
                                        createRestorePoint,
                                        driversOnly,
                                        rawLog = std::move(onLog),
                                        rawComplete = std::move(onComplete)]() {
        auto log = [&state, &rawLog](LogLevel lvl, const std::string& msg) {
            std::lock_guard<std::mutex> cbLock(state->callbackMutex);
            if (state->ownerAlive && rawLog) {
                rawLog(lvl, msg);
            }
        };

        struct DoneSignal {
            HANDLE ev;
            std::atomic<bool>& flag;
            ~DoneSignal() {
                flag.store(false);
                if (ev != nullptr) SetEvent(ev);
            }
        } guard{state->installDoneEvent.Get(), state->installing};

        WuInstallSummary summary;
        summary.requestedCount = selectedUpdateIds.size();

        if (IsProcessElevated()) {
            summary = DownloadAndInstallSync(
                selectedUpdateIds,
                createRestorePoint,
                log,
                &state->progress,
                &state->cancelRequested,
                setPhase,
                updateItem,
                driversOnly);
        } else {
            log(LogLevel::Info,
                "Запрос прав администратора (UAC) для установки драйверов Windows Update...");
            setPhase("Ожидание подтверждения UAC...");

            const std::filesystem::path tempLog = CreateTempLogPath(L"hdm_wu");
            const std::filesystem::path cancelMarker =
                tempLog.empty()
                    ? std::filesystem::path{}
                    : std::filesystem::path(tempLog.wstring() + L".cancel");
            std::error_code ec;
            if (!cancelMarker.empty()) {
                std::filesystem::remove(cancelMarker, ec);
            }

            std::wstring args =
                L"--cli-wu-install " + QuoteWin32Arg(Utf8ToWide(JoinUpdateIds(selectedUpdateIds)));
            if (createRestorePoint) {
                args += L" --restore-before";
            }
            if (driversOnly) {
                args += L" --drivers-only";
            }
            if (!tempLog.empty()) {
                args += L" --log " + QuoteWin32Arg(tempLog.wstring());
            }
            if (!cancelMarker.empty()) {
                args += L" --cancel-file " + QuoteWin32Arg(cancelMarker.wstring());
            }

            setPhase("Запуск установки с правами администратора...");
            state->progress.store(0.03f);

            std::unordered_set<std::string> reportedIds;
            auto handleMeta = [&](const std::string& payload) {
                if (payload.rfind("PROG|", 0) == 0) {
                    const size_t sep = payload.find('|', 5);
                    if (sep != std::string::npos) {
                        const long milli = std::strtol(payload.substr(5, sep - 5).c_str(), nullptr, 10);
                        const float p = std::clamp(static_cast<float>(milli) / 1000.0f, 0.0f, 1.0f);
                        state->progress.store(p);
                        setPhase(payload.substr(sep + 1));
                    }
                } else if (payload.rfind("ITEM|", 0) == 0) {
                    const size_t p1 = payload.find('|', 5);
                    const size_t p2 = p1 != std::string::npos ? payload.find('|', p1 + 1) : std::string::npos;
                    const size_t p3 = p2 != std::string::npos ? payload.find('|', p2 + 1) : std::string::npos;
                    if (p1 != std::string::npos && p2 != std::string::npos && p3 != std::string::npos) {
                        const std::string uid = payload.substr(5, p1 - 5);
                        const bool ok = payload.substr(p1 + 1, p2 - p1 - 1) == "1";
                        const bool reb = payload.substr(p2 + 1, p3 - p2 - 1) == "1";
                        const std::string stText = payload.substr(p3 + 1);
                        reportedIds.insert(uid);
                        if (ok) {
                            ++summary.installedCount;
                            if (reb) summary.rebootRequired = true;
                        } else {
                            ++summary.failedCount;
                        }
                        updateItem(uid, ok, reb, stText);
                    }
                }
            };

            const ElevatedRunResult run = ExecuteElevatedSelf(
                args,
                true,
                tempLog.empty() ? nullptr : &tempLog,
                log,
                handleMeta,
                &state->cancelRequested,
                cancelMarker.empty() ? nullptr : &cancelMarker);
            if (!tempLog.empty()) {
                std::filesystem::remove(tempLog, ec);
            }
            if (!cancelMarker.empty()) {
                std::filesystem::remove(cancelMarker, ec);
            }

            if (!run.launched) {
                summary.failedCount = selectedUpdateIds.size();
                if (run.launchError == ERROR_CANCELLED) {
                    log(LogLevel::Warning, "Установка отменена пользователем в окне UAC.");
                } else {
                    log(LogLevel::Error,
                        "Не удалось запустить elevated-процесс: " + FormatWin32Error(run.launchError));
                }
            } else {
                summary.cancelled = run.cancelled || (run.exitCode == 3);
                if (run.exitCode == 2) {
                    summary.rebootRequired = true;
                }
                for (const auto& uid : selectedUpdateIds) {
                    if (reportedIds.count(uid) > 0) {
                        continue;
                    }
                    const bool ok = !summary.cancelled &&
                                    reportedIds.empty() &&
                                    (run.exitCode == 0 || run.exitCode == 2);
                    const bool reb = ok && (run.exitCode == 2);
                    if (ok) {
                        ++summary.installedCount;
                    } else {
                        ++summary.failedCount;
                    }
                    updateItem(
                        uid,
                        ok,
                        reb,
                        ok ? (reb ? "Установлено (нужна перезагрузка)" : "Установлено")
                           : (summary.cancelled ? "Отменено" : "Ошибка установки"));
                }
            }
            state->progress.store(1.0f);
        }

        if (summary.cancelled) {
            setPhase("Установка Windows Update отменена");
        } else if (summary.rebootRequired) {
            setPhase("Установка завершена (требуется перезагрузка)");
        } else if (summary.failedCount > 0 && summary.installedCount == 0) {
            setPhase("Ошибка установки обновлений Windows Update");
        } else {
            setPhase("Операция Windows Update завершена");
        }

        {
            std::lock_guard<std::mutex> cbLock(state->callbackMutex);
            if (state->ownerAlive && rawComplete) {
                rawComplete(summary);
            }
        }
    });
}

WuInstallSummary WindowsUpdateManager::DownloadAndInstallSync(
    const std::vector<std::string>& updateIds,
    bool createRestorePoint,
    const LogCallback& onLog,
    std::atomic<float>* outProgress,
    std::atomic<bool>* cancelFlag,
    std::function<void(const std::string&)> onPhase,
    ItemUpdateCallback onItemUpdate,
    bool driversOnly) {
    WuInstallSummary summary;
    summary.requestedCount = updateIds.size();
    if (updateIds.empty()) {
        return summary;
    }

    auto setProg = [&](float value, const std::string& phase) {
        if (outProgress != nullptr) {
            outProgress->store(std::clamp(value, 0.0f, 1.0f));
        }
        if (onPhase) {
            onPhase(phase);
        }
    };

    if (createRestorePoint) {
        setProg(0.02f, "Создание точки восстановления системы...");
        SystemRestore::CreateRestorePointSync(
            "DriverLoom: перед установкой драйверов Windows Update", onLog);
    }

    if (cancelFlag != nullptr && cancelFlag->load()) {
        summary.cancelled = true;
        return summary;
    }

    CoInitGuard comInit(COINIT_MULTITHREADED);
    if (comInit.Result() == RPC_E_CHANGED_MODE) {
        WuInstallSummary mtaSummary;
        std::thread mtaThread([&]() {
            mtaSummary = DownloadAndInstallSync(
                updateIds,
                false,
                onLog,
                outProgress,
                cancelFlag,
                onPhase,
                onItemUpdate,
                driversOnly);
        });
        mtaThread.join();
        return mtaSummary;
    }
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

    setProg(0.05f, "Поиск выбранных обновлений в локальном кэше...");
    if (onLog) {
        onLog(LogLevel::Info, "Подготовка выбранных обновлений Windows Update...");
    }

    // Keep COM update references in ordered target list matching requested updateIds.
    struct TargetUpdate {
        std::string id;
        std::string title;
        ComPtr<IUpdate> upd;
        bool isDriver{true};
    };

    std::set<std::string> requestedSet(updateIds.begin(), updateIds.end());
    std::map<std::string, ComPtr<IUpdate>> matchedById;

    auto collectMatches = [&](ISearchResult* sr) {
        if (sr == nullptr) return;
        ComPtr<IUpdateCollection> coll;
        if (FAILED(sr->get_Updates(coll.Put())) || !coll) return;
        LONG cnt = 0;
        coll->get_Count(&cnt);
        for (LONG i = 0; i < cnt; ++i) {
            ComPtr<IUpdate> upd;
            if (FAILED(coll->get_Item(i, upd.Put())) || !upd) continue;
            const std::string uid = GetUpdateIdentityId(upd.Get());
            if (!uid.empty() && requestedSet.count(uid) > 0 && matchedById.find(uid) == matchedById.end()) {
                matchedById.emplace(uid, std::move(upd));
            }
        }
    };

    bool anySearchSucceeded = false;
    bool onlineAttempted = false;
    bool onlineSucceeded = false;

    // Pass 1: Fast offline search against the local WUA datastore already populated by scan.
    {
        ComPtr<ISearchResult> offlineResult;
        bool abortedCleanly = true;
        JobWaitOutcome outcome = JobWaitOutcome::Completed;
        hr = RunAsyncSearch(
            session,
            searcher,
            L"IsInstalled=0 and IsHidden=0 or IsInstalled=0 and IsHidden=1",
            false,
            kSearchOfflineTimeout,
            cancelFlag,
            offlineResult,
            &abortedCleanly,
            &outcome);
        if (outcome == JobWaitOutcome::Cancelled) {
            summary.cancelled = true;
            return summary;
        }
        if (SUCCEEDED(hr) && offlineResult) {
            anySearchSucceeded = true;
            collectMatches(offlineResult.Get());
        }
    }

    // Pass 2: Online fallback only if some requested UpdateIDs are absent from local cache.
    if (matchedById.size() < requestedSet.size() &&
        ((session && searcher) || RecreateWuSessionAndSearcher(session, searcher))) {
        onlineAttempted = true;
        setProg(0.07f, "Запрос метаданных на сервере Windows Update...");
        if (onLog) {
            onLog(LogLevel::Info, "Запрос недостающих метаданных с сервера Windows Update...");
        }
        const wchar_t* criteriaW =
            driversOnly
                ? L"IsInstalled=0 and Type='Driver' and IsHidden=0 or IsInstalled=0 and Type='Driver' and IsHidden=1"
                : L"IsInstalled=0 and IsHidden=0 or IsInstalled=0 and IsHidden=1";
        ComPtr<ISearchResult> onlineResult;
        bool abortedCleanly = true;
        JobWaitOutcome outcome = JobWaitOutcome::Completed;
        hr = RunAsyncSearch(
            session,
            searcher,
            criteriaW,
            true,
            kSearchOnlineTimeout,
            cancelFlag,
            onlineResult,
            &abortedCleanly,
            &outcome);
        if (outcome == JobWaitOutcome::Cancelled) {
            summary.cancelled = true;
            return summary;
        }
        if (SUCCEEDED(hr) && onlineResult) {
            anySearchSucceeded = true;
            onlineSucceeded = true;
            collectMatches(onlineResult.Get());
        }
        if (driversOnly && matchedById.size() < requestedSet.size() && session && searcher) {
            ComPtr<ISearchResult> fullResult;
            hr = RunAsyncSearch(
                session,
                searcher,
                L"IsInstalled=0 and IsHidden=0 or IsInstalled=0 and IsHidden=1",
                true,
                kSearchOnlineTimeout,
                cancelFlag,
                fullResult,
                &abortedCleanly,
                &outcome);
            if (outcome == JobWaitOutcome::Cancelled) {
                summary.cancelled = true;
                return summary;
            }
            if (SUCCEEDED(hr) && fullResult) {
                anySearchSucceeded = true;
                onlineSucceeded = true;
                collectMatches(fullResult.Get());
            }
        }
    }

    if (!anySearchSucceeded && matchedById.empty()) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Не удалось получить список обновлений Windows Update (ошибка локального и сетевого поиска).");
        }
        for (const auto& uid : updateIds) {
            ++summary.failedCount;
            if (onItemUpdate) {
                onItemUpdate(uid, false, false, "Ошибка поиска Windows Update");
            }
        }
        setProg(1.0f, "Ошибка поиска Windows Update");
        return summary;
    }

    std::vector<TargetUpdate> targets;
    for (const auto& uid : updateIds) {
        auto it = matchedById.find(uid);
        if (it == matchedById.end()) {
            if (onlineAttempted && !onlineSucceeded) {
                ++summary.failedCount;
                if (onLog) {
                    onLog(LogLevel::Error,
                          "Не удалось получить метаданные обновления " + uid + " с сервера Windows Update.");
                }
                if (onItemUpdate) {
                    onItemUpdate(uid, false, false, "Ошибка сетевого поиска Windows Update");
                }
            } else {
                if (onLog) {
                    onLog(LogLevel::Warning,
                          "Обновление " + uid + " уже установлено или больше не найдено в Windows Update.");
                }
                if (onItemUpdate) {
                    onItemUpdate(uid, true, false, "Актуален / не требуется");
                }
            }
            continue;
        }

        IUpdate* rawUpd = it->second.Get();
        VARIANT_BOOL alreadyInstalled = VARIANT_FALSE;
        if (SUCCEEDED(rawUpd->get_IsInstalled(&alreadyInstalled)) &&
            alreadyInstalled == VARIANT_TRUE) {
            const std::string t = GetUpdateTitle(rawUpd);
            if (onLog) {
                onLog(LogLevel::Success, "Уже установлено: " + t);
            }
            ++summary.installedCount;
            if (onItemUpdate) {
                onItemUpdate(uid, true, false, "Установлено");
            }
            continue;
        }

        VARIANT_BOOL eulaAccepted = VARIANT_TRUE;
        if (SUCCEEDED(rawUpd->get_EulaAccepted(&eulaAccepted)) &&
            eulaAccepted == VARIANT_FALSE) {
            rawUpd->AcceptEula();
        }

        TargetUpdate tu;
        tu.id = uid;
        tu.title = GetUpdateTitle(rawUpd);
        if (tu.title.empty()) tu.title = uid;
        tu.isDriver = IsDriverUpdate(rawUpd);
        tu.upd = std::move(it->second);
        targets.push_back(std::move(tu));
    }

    if (targets.empty()) {
        setProg(1.0f, "Нет обновлений, требующих установки");
        return summary;
    }

    if (!session) {
        hr = CoCreateInstance(
            CLSID_UpdateSession,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_IUpdateSession,
            reinterpret_cast<void**>(session.Put()));
        if (FAILED(hr) || !session) {
            if (onLog) {
                onLog(LogLevel::Error,
                      "Не удалось пересоздать IUpdateSession: " + FormatHResult(hr));
            }
            for (const auto& t : targets) {
                ++summary.failedCount;
                if (onItemUpdate) {
                    onItemUpdate(t.id, false, false, "Ошибка сессии WU");
                }
            }
            return summary;
        }
        session->put_ClientApplicationID(appId.Get());
    }

    ComPtr<IUpdateDownloader> downloader;
    hr = session->CreateUpdateDownloader(downloader.Put());
    if (FAILED(hr) || !downloader) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Не удалось создать IUpdateDownloader: " + FormatHResult(hr));
        }
        for (const auto& t : targets) {
            ++summary.failedCount;
            if (onItemUpdate) {
                onItemUpdate(t.id, false, false, "Ошибка загрузки");
            }
        }
        return summary;
    }

    ComPtr<IUpdateInstaller> installer;
    hr = session->CreateUpdateInstaller(installer.Put());
    if (FAILED(hr) || !installer) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Не удалось создать IUpdateInstaller: " + FormatHResult(hr));
        }
        for (const auto& t : targets) {
            ++summary.failedCount;
            if (onItemUpdate) {
                onItemUpdate(t.id, false, false, "Ошибка установки");
            }
        }
        return summary;
    }

    ConfigureInstallerSilent(installer.Get(), true);

    const float baseProg = 0.10f;
    const float spanPerItem = 0.90f / static_cast<float>(targets.size());
    auto markRemaining = [&](size_t startIndex,
                             bool rebootReqFlag,
                             const std::string& status,
                             bool countAsFailed = false) {
        if (countAsFailed && startIndex < targets.size()) {
            summary.failedCount += (targets.size() - startIndex);
        }
        if (!onItemUpdate) return;
        for (size_t r = startIndex; r < targets.size(); ++r) {
            onItemUpdate(targets[r].id, false, rebootReqFlag, status);
        }
    };

    for (size_t idx = 0; idx < targets.size(); ++idx) {
        auto& target = targets[idx];
        const std::string stepPrefix =
            "[" + std::to_string(idx + 1) + "/" + std::to_string(targets.size()) + "] ";
        const float itemStart = baseProg + spanPerItem * static_cast<float>(idx);
        const float dlWeight = 0.45f * spanPerItem;
        const float instWeight = 0.55f * spanPerItem;

        if (cancelFlag != nullptr && cancelFlag->load()) {
            summary.cancelled = true;
            markRemaining(idx, false, "Отменено");
            break;
        }

        // Check reboot requirement before starting the next package so WUA/CBS doesn't stall.
        VARIANT_BOOL rebootBefore = VARIANT_FALSE;
        if (SUCCEEDED(installer->get_RebootRequiredBeforeInstallation(&rebootBefore)) &&
            rebootBefore == VARIANT_TRUE) {
            summary.rebootRequired = true;
            if (onLog) {
                onLog(LogLevel::Warning,
                      "Требуется перезагрузка Windows перед установкой: " + target.title);
            }
            summary.failedCount += (targets.size() - idx);
            markRemaining(idx, true, "Нужна перезагрузка");
            break;
        }

        ComPtr<IUpdateCollection> singleColl = MakeSingleUpdateCollection(target.upd.Get());
        if (!singleColl) {
            ++summary.failedCount;
            if (onItemUpdate) onItemUpdate(target.id, false, false, "Ошибка подготовки");
            continue;
        }

        // --- Step 1: Download if needed ---
        VARIANT_BOOL isDl = VARIANT_FALSE;
        target.upd->get_IsDownloaded(&isDl);
        if (isDl == VARIANT_TRUE) {
            if (onLog) {
                onLog(LogLevel::Info, stepPrefix + "Пакет уже загружен: " + target.title);
            }
            setProg(itemStart + dlWeight, stepPrefix + "Пакет уже загружен: " + target.title);
        } else {
            if (onLog) {
                onLog(LogLevel::Info, stepPrefix + "Скачивание: " + target.title + "...");
            }
            setProg(itemStart, stepPrefix + "Скачивание: " + target.title + "...");

            if (!downloader) {
                hr = session->CreateUpdateDownloader(downloader.Put());
                if (FAILED(hr) || !downloader) {
                    if (onLog) {
                        onLog(LogLevel::Error,
                              stepPrefix + "Не удалось создать IUpdateDownloader: " + FormatHResult(hr));
                    }
                    ++summary.failedCount;
                    if (onItemUpdate) onItemUpdate(target.id, false, false, "Ошибка загрузки");
                    continue;
                }
            }
            hr = downloader->put_Updates(singleColl.Get());
            if (FAILED(hr)) {
                if (onLog) {
                    onLog(LogLevel::Error,
                          stepPrefix + "Ошибка назначения списка загрузки (put_Updates): " +
                              FormatWuErrorDiagnostic(hr));
                }
                ++summary.failedCount;
                if (onItemUpdate) onItemUpdate(target.id, false, false, "Ошибка загрузки");
                continue;
            }

            VARIANT emptyState;
            VariantInit(&emptyState);
            ComPtr<WuProgressCallback> dlProgCb;
            *dlProgCb.Put() = new WuProgressCallback(kIidDownloadProgressChangedCallback);
            ComPtr<WuCompletedCallback> dlCb;
            *dlCb.Put() = new WuCompletedCallback(kIidDownloadCompletedCallback);

            ComPtr<IDownloadJob> dlJob;
            hr = downloader->BeginDownload(dlProgCb.Get(), dlCb.Get(), emptyState, dlJob.Put());
            if (FAILED(hr) || !dlJob) {
                if (onLog) {
                    onLog(LogLevel::Error,
                          stepPrefix + "Ошибка BeginDownload: " + FormatWuErrorDiagnostic(hr));
                }
                ++summary.failedCount;
                if (onItemUpdate) onItemUpdate(target.id, false, false, "Ошибка загрузки");
                continue;
            }

            uint64_t lastBytes = 0;
            LONG lastPct = -1;
            bool abortedCleanly = true;
            const JobWaitOutcome dlWait = WaitForWuJob(
                dlJob.Get(),
                dlCb.Get(),
                kDownloadTotalTimeout,
                kDownloadStallTimeout,
                cancelFlag,
                [&](std::chrono::steady_clock::duration elapsed) -> bool {
                    bool moved = false;
                    ComPtr<IDownloadProgress> dlProg;
                    LONG pct = 0;
                    uint64_t doneB = 0;
                    uint64_t totB = 0;
                    if (SUCCEEDED(dlJob->GetProgress(dlProg.Put())) && dlProg) {
                        LONG curPct = -1;
                        if (SUCCEEDED(dlProg->get_CurrentUpdatePercentComplete(&curPct)) && curPct >= 0) {
                            pct = curPct;
                        } else {
                            dlProg->get_PercentComplete(&pct);
                        }
                        DECIMAL d1{}, d2{};
                        if (SUCCEEDED(dlProg->get_TotalBytesDownloaded(&d1))) {
                            doneB = DecimalToUint64(d1);
                        }
                        if (SUCCEEDED(dlProg->get_TotalBytesToDownload(&d2))) {
                            totB = DecimalToUint64(d2);
                        }
                    }
                    if (doneB > lastBytes || pct != lastPct) {
                        lastBytes = doneB;
                        lastPct = pct;
                        moved = true;
                    }
                    float frac = std::clamp(static_cast<float>(pct) / 100.0f, 0.0f, 1.0f);
                    if (totB > 0) {
                        frac = std::max(
                            frac,
                            std::clamp(static_cast<float>(doneB) / static_cast<float>(totB), 0.0f, 1.0f));
                    }
                    const auto secs =
                        std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
                    std::string msg = stepPrefix + "Скачивание " + target.title + ": " +
                                      std::to_string(static_cast<int>(frac * 100.0f)) + "%";
                    if (totB > 0) {
                        msg += " (" + FormatBytes(doneB) + " / " + FormatBytes(totB) + ")";
                    } else {
                        msg += " (" + std::to_string(secs) + " с)";
                    }
                    setProg(itemStart + dlWeight * frac, msg);
                    return moved;
                },
                &abortedCleanly);

            if (dlWait != JobWaitOutcome::Completed) {
                summary.cancelled = (dlWait == JobWaitOutcome::Cancelled);
                if (!abortedCleanly) {
                    AbandonWuJobAsync(
                        session,
                        downloader,
                        dlJob,
                        dlCb,
                        [](IUpdateDownloader* d, IDownloadJob* j) {
                            IDownloadResult* r = nullptr;
                            if (SUCCEEDED(d->EndDownload(j, &r)) && r != nullptr) r->Release();
                        });
                    if (onLog) {
                        onLog(LogLevel::Error,
                              stepPrefix + (dlWait == JobWaitOutcome::Cancelled
                                                ? "Загрузка отменена (Windows Update завершает операцию в фоне)."
                                                : "Тайм-аут скачивания Windows Update: " + target.title));
                    }
                    ++summary.failedCount;
                    if (onItemUpdate) {
                        onItemUpdate(
                            target.id,
                            false,
                            false,
                            dlWait == JobWaitOutcome::Cancelled ? "Отменено" : "Тайм-аут загрузки");
                    }
                    markRemaining(
                        idx + 1,
                        false,
                        dlWait == JobWaitOutcome::Cancelled ? "Отменено" : "Пропущено",
                        dlWait != JobWaitOutcome::Cancelled);
                    break;
                }

                ComPtr<IDownloadResult> abortedRes;
                downloader->EndDownload(dlJob.Get(), abortedRes.Put());
                dlJob->CleanUp();
                if (onLog) {
                    onLog(LogLevel::Warning,
                          stepPrefix + (dlWait == JobWaitOutcome::Cancelled
                                            ? "Скачивание отменено пользователем."
                                            : "Тайм-аут скачивания: " + target.title));
                }
                ++summary.failedCount;
                if (onItemUpdate) {
                    onItemUpdate(
                        target.id,
                        false,
                        false,
                        dlWait == JobWaitOutcome::Cancelled ? "Отменено" : "Тайм-аут загрузки");
                }
                if (dlWait == JobWaitOutcome::Cancelled) {
                    markRemaining(idx + 1, false, "Отменено");
                    break;
                }
                continue;
            }

            ComPtr<IDownloadResult> dlResult;
            hr = downloader->EndDownload(dlJob.Get(), dlResult.Put());
            dlJob->CleanUp();

            if (FAILED(hr) || !dlResult) {
                if (onLog) {
                    onLog(LogLevel::Error,
                          stepPrefix + "Ошибка EndDownload: " + FormatWuErrorDiagnostic(hr));
                }
                ++summary.failedCount;
                if (onItemUpdate) onItemUpdate(target.id, false, false, "Ошибка загрузки");
                continue;
            }

            OperationResultCode dlCode = orcNotStarted;
            dlResult->get_ResultCode(&dlCode);
            target.upd->get_IsDownloaded(&isDl);
            if (isDl != VARIANT_TRUE) {
                LONG dlHr = 0;
                ComPtr<IUpdateDownloadResult> updDlRes;
                if (SUCCEEDED(dlResult->GetUpdateResult(0, updDlRes.Put())) && updDlRes) {
                    updDlRes->get_HResult(&dlHr);
                } else {
                    dlResult->get_HResult(&dlHr);
                }
                if (onLog) {
                    onLog(LogLevel::Error,
                          stepPrefix + "Не удалось скачать «" + target.title + "» [" +
                              ResultCodeToText(dlCode) + ", " +
                              FormatWuErrorDiagnostic(static_cast<HRESULT>(dlHr)) + "]");
                }
                ++summary.failedCount;
                if (onItemUpdate) onItemUpdate(target.id, false, false, "Ошибка загрузки");
                continue;
            }
        }

        // --- Step 2: Install single update safely ---
        if (!WaitWhileInstallerBusy(installer.Get(), cancelFlag, onPhase, onLog)) {
            if (cancelFlag != nullptr && cancelFlag->load()) {
                summary.cancelled = true;
            }
            ++summary.failedCount;
            if (onItemUpdate) {
                onItemUpdate(
                    target.id,
                    false,
                    false,
                    summary.cancelled ? "Отменено" : "Служба занята");
            }
            if (summary.cancelled) {
                markRemaining(idx + 1, false, "Отменено");
                break;
            }
            continue;
        }

        if (CheckUpdateCanRequestUserInput(target.upd.Get()) && onLog) {
            onLog(LogLevel::Info,
                  stepPrefix + "Пакет «" + target.title +
                      "» допускает диалог — применяется тихий неблокирующий режим.");
        }

        ConfigureInstallerSilent(installer.Get(), true);
        hr = installer->put_Updates(singleColl.Get());
        if (FAILED(hr)) {
            if (onLog) {
                onLog(LogLevel::Error,
                      stepPrefix + "Ошибка назначения списка установки (put_Updates): " +
                          FormatWuErrorDiagnostic(hr));
            }
            ++summary.failedCount;
            if (onItemUpdate) onItemUpdate(target.id, false, false, "Ошибка установки");
            continue;
        }

        if (onLog) {
            onLog(LogLevel::Info, stepPrefix + "Установка: " + target.title + "...");
        }
        setProg(itemStart + dlWeight, stepPrefix + "Установка: " + target.title + "...");

        VARIANT emptyState;
        VariantInit(&emptyState);
        ComPtr<WuProgressCallback> instProgCb;
        *instProgCb.Put() = new WuProgressCallback(kIidInstallationProgressChangedCallback);
        ComPtr<WuCompletedCallback> instCb;
        *instCb.Put() = new WuCompletedCallback(kIidInstallationCompletedCallback);

        ComPtr<IInstallationJob> instJob;
        hr = installer->BeginInstall(instProgCb.Get(), instCb.Get(), emptyState, instJob.Put());
        if (FAILED(hr) || !instJob) {
            if (onLog) {
                onLog(LogLevel::Error,
                      stepPrefix + "Ошибка BeginInstall для «" + target.title + "»: " +
                          FormatWuErrorDiagnostic(hr));
            }
            ++summary.failedCount;
            if (onItemUpdate) onItemUpdate(target.id, false, false, "Ошибка установки");
            continue;
        }

        const auto installTimeout = target.isDriver ? kInstallDriverTimeout : kInstallSoftwareTimeout;
        bool instAbortedCleanly = true;
        LONG lastInstPct = -1;
        auto lastPhaseUpdate = std::chrono::steady_clock::time_point{};
        const JobWaitOutcome instWait = WaitForWuJob(
            instJob.Get(),
            instCb.Get(),
            installTimeout,
            std::chrono::seconds(0),
            cancelFlag,
            [&](std::chrono::steady_clock::duration elapsed) -> bool {
                LONG pct = 0;
                ComPtr<IInstallationProgress> instProg;
                if (SUCCEEDED(instJob->GetProgress(instProg.Put())) && instProg) {
                    LONG curPct = -1;
                    if (SUCCEEDED(instProg->get_CurrentUpdatePercentComplete(&curPct)) && curPct >= 0) {
                        pct = curPct;
                    } else {
                        LONG totalPct = 0;
                        if (SUCCEEDED(instProg->get_PercentComplete(&totalPct)) && totalPct >= 0) {
                            pct = totalPct;
                        }
                    }
                }
                const auto now = std::chrono::steady_clock::now();
                const auto secs =
                    std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
                if (pct != lastInstPct ||
                    now - lastPhaseUpdate >= std::chrono::seconds(1)) {
                    lastInstPct = pct;
                    lastPhaseUpdate = now;
                    const float frac = std::clamp(static_cast<float>(pct) / 100.0f, 0.0f, 1.0f);
                    setProg(
                        itemStart + dlWeight + instWeight * frac,
                        stepPrefix + "Установка " + target.title + ": " +
                            std::to_string(pct) + "% (" + std::to_string(secs) + " с)");
                }
                return false;
            },
            &instAbortedCleanly);

        if (instWait != JobWaitOutcome::Completed) {
            summary.cancelled = (instWait == JobWaitOutcome::Cancelled);
            const std::string statusMsg =
                (instWait == JobWaitOutcome::Cancelled) ? "Отменено" : "Тайм-аут установки";

            if (!instAbortedCleanly) {
                AbandonWuJobAsync(
                    session,
                    installer,
                    instJob,
                    instCb,
                    [](IUpdateInstaller* inst, IInstallationJob* j) {
                        IInstallationResult* r = nullptr;
                        if (SUCCEEDED(inst->EndInstall(j, &r)) && r != nullptr) r->Release();
                    });
                if (onLog) {
                    onLog(LogLevel::Error,
                          stepPrefix +
                              (instWait == JobWaitOutcome::Cancelled
                                   ? "Установка отменена. Фоновая служба Windows Update ещё завершает пакет «" +
                                         target.title + "»."
                                   : "Превышен тайм-аут установки («" + target.title +
                                         "»). Возможна блокировка со-установщика драйвера; проверьте "
                                         "C:\\Windows\\INF\\setupapi.dev.log или установите вручную через "
                                         "«Параметры Windows -> Центр обновления Windows»."));
                }
                ++summary.failedCount;
                if (onItemUpdate) onItemUpdate(target.id, false, false, statusMsg);
                const bool skippedByTimeout = (instWait != JobWaitOutcome::Cancelled);
                markRemaining(
                    idx + 1,
                    false,
                    skippedByTimeout ? "Пропущено" : "Отменено",
                    skippedByTimeout);
                break;
            }

            ComPtr<IInstallationResult> abortedInstRes;
            installer->EndInstall(instJob.Get(), abortedInstRes.Put());
            instJob->CleanUp();
            if (onLog) {
                onLog(LogLevel::Warning,
                      stepPrefix +
                          (instWait == JobWaitOutcome::Cancelled
                               ? "Установка «" + target.title + "» отменена пользователем."
                               : "Тайм-аут установки «" + target.title + "» — задача прервана."));
            }
            ++summary.failedCount;
            if (onItemUpdate) onItemUpdate(target.id, false, false, statusMsg);
            if (instWait == JobWaitOutcome::Cancelled) {
                markRemaining(idx + 1, false, "Отменено");
                break;
            }
            continue;
        }

        ComPtr<IInstallationResult> instResult;
        hr = installer->EndInstall(instJob.Get(), instResult.Put());
        instJob->CleanUp();

        if (FAILED(hr) || !instResult) {
            if (onLog) {
                onLog(LogLevel::Error,
                      stepPrefix + "Ошибка EndInstall: " + FormatWuErrorDiagnostic(hr));
            }
            ++summary.failedCount;
            if (onItemUpdate) onItemUpdate(target.id, false, false, "Ошибка установки");
            continue;
        }

        OperationResultCode instCode = orcNotStarted;
        instResult->get_ResultCode(&instCode);
        VARIANT_BOOL rebootReq = VARIANT_FALSE;
        instResult->get_RebootRequired(&rebootReq);

        LONG itemHr = 0;
        OperationResultCode itemCode = instCode;
        VARIANT_BOOL itemReboot = rebootReq;
        ComPtr<IUpdateInstallationResult> itemRes;
        if (SUCCEEDED(instResult->GetUpdateResult(0, itemRes.Put())) && itemRes) {
            itemRes->get_ResultCode(&itemCode);
            itemRes->get_HResult(&itemHr);
            itemRes->get_RebootRequired(&itemReboot);
        } else {
            instResult->get_HResult(&itemHr);
        }

        const bool itemNeedsReboot = (itemReboot == VARIANT_TRUE || rebootReq == VARIANT_TRUE);
        if (itemNeedsReboot) {
            summary.rebootRequired = true;
        }

        if (itemCode == orcSucceeded || itemCode == orcSucceededWithErrors) {
            ++summary.installedCount;
            const std::string okStatus =
                itemNeedsReboot ? "Установлено (нужна перезагрузка)" : "Установлено";
            if (onLog) {
                onLog(LogLevel::Success,
                      stepPrefix + "Установлен: " + target.title + " [" + ResultCodeToText(itemCode) + "]");
            }
            if (onItemUpdate) {
                onItemUpdate(target.id, true, itemNeedsReboot, okStatus);
            }
        } else {
            ++summary.failedCount;
            if (onLog) {
                onLog(LogLevel::Error,
                      stepPrefix + "Ошибка установки: " + target.title + " [" +
                          ResultCodeToText(itemCode) + ", " +
                          FormatWuErrorDiagnostic(static_cast<HRESULT>(itemHr)) + "]");
            }
            if (onItemUpdate) {
                onItemUpdate(target.id, false, false, "Ошибка установки");
            }
        }

        setProg(itemStart + spanPerItem, stepPrefix + "Завершено: " + target.title);
    }

    if (summary.rebootRequired && onLog) {
        onLog(LogLevel::Warning,
              "Для завершения установки драйверов требуется перезагрузка компьютера!");
    }
    if (outProgress != nullptr && !summary.cancelled) {
        outProgress->store(1.0f);
    }
    return summary;
}

bool WindowsUpdateManager::SetUpdatesHiddenSync(
    const std::vector<std::string>& updateIds,
    bool hide,
    const LogCallback& onLog) {
    if (updateIds.empty()) {
        return false;
    }

    CoInitGuard comInit(COINIT_MULTITHREADED);
    if (comInit.Result() == RPC_E_CHANGED_MODE) {
        bool mtaOk = false;
        std::thread mtaThread([&]() {
            mtaOk = SetUpdatesHiddenSync(updateIds, hide, onLog);
        });
        mtaThread.join();
        return mtaOk;
    }
    if (!comInit.Succeeded()) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Ошибка CoInitializeEx (MTA): " + FormatHResult(comInit.Result()));
        }
        return false;
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
        return false;
    }

    BstrGuard appId(L"DriverLoom");
    session->put_ClientApplicationID(appId.Get());

    ComPtr<IUpdateSearcher> searcher;
    hr = session->CreateUpdateSearcher(searcher.Put());
    if (FAILED(hr) || !searcher) {
        if (onLog) {
            onLog(LogLevel::Error, "Не удалось создать IUpdateSearcher: " + FormatHResult(hr));
        }
        return false;
    }

    std::set<std::string> requestedSet(updateIds.begin(), updateIds.end());
    std::map<std::string, ComPtr<IUpdate>> matchedById;
    auto collectMatches = [&](ISearchResult* sr) {
        if (sr == nullptr) return;
        ComPtr<IUpdateCollection> coll;
        if (FAILED(sr->get_Updates(coll.Put())) || !coll) return;
        LONG cnt = 0;
        coll->get_Count(&cnt);
        for (LONG i = 0; i < cnt; ++i) {
            ComPtr<IUpdate> upd;
            if (FAILED(coll->get_Item(i, upd.Put())) || !upd) continue;
            const std::string uid = GetUpdateIdentityId(upd.Get());
            if (!uid.empty() && requestedSet.count(uid) > 0 &&
                matchedById.find(uid) == matchedById.end()) {
                matchedById.emplace(uid, std::move(upd));
            }
        }
    };

    const wchar_t* criteriaW = L"IsInstalled=0 and IsHidden=0 or IsInstalled=0 and IsHidden=1";
    {
        ComPtr<ISearchResult> offlineRes;
        bool aborted = true;
        JobWaitOutcome outcome = JobWaitOutcome::Completed;
        hr = RunAsyncSearch(
            session,
            searcher,
            criteriaW,
            false,
            kSearchOfflineTimeout,
            nullptr,
            offlineRes,
            &aborted,
            &outcome);
        if (SUCCEEDED(hr) && offlineRes) {
            collectMatches(offlineRes.Get());
        }
    }
    if (matchedById.size() < requestedSet.size() &&
        ((session && searcher) || RecreateWuSessionAndSearcher(session, searcher))) {
        ComPtr<ISearchResult> onlineRes;
        bool aborted = true;
        JobWaitOutcome outcome = JobWaitOutcome::Completed;
        hr = RunAsyncSearch(
            session,
            searcher,
            criteriaW,
            true,
            kSearchOnlineTimeout,
            nullptr,
            onlineRes,
            &aborted,
            &outcome);
        if (SUCCEEDED(hr) && onlineRes) {
            collectMatches(onlineRes.Get());
        }
    }

    size_t changedCount = 0;
    size_t failedCount = 0;
    for (const auto& uid : updateIds) {
        auto it = matchedById.find(uid);
        if (it == matchedById.end()) {
            ++failedCount;
            if (onLog) {
                onLog(LogLevel::Warning,
                      "Обновление " + uid + " больше не найдено в кэше Windows Update.");
            }
            continue;
        }
        IUpdate* upd = it->second.Get();
        const std::string title = GetUpdateTitle(upd);
        const HRESULT hrPut = upd->put_IsHidden(hide ? VARIANT_TRUE : VARIANT_FALSE);
        if (SUCCEEDED(hrPut)) {
            ++changedCount;
            if (onLog) {
                onLog(LogLevel::Success,
                      std::string(hide ? "Скрыто обновление Windows Update: "
                                       : "Возвращено обновление Windows Update: ") +
                          (title.empty() ? uid : title));
            }
        } else {
            ++failedCount;
            if (onLog) {
                onLog(LogLevel::Error,
                      "Не удалось изменить статус скрытия («" +
                          (title.empty() ? uid : title) + "»): " +
                          FormatWuErrorDiagnostic(hrPut));
            }
        }
    }
    return changedCount > 0 && failedCount == 0;
}

void WindowsUpdateManager::SetUpdatesHiddenAsync(
    const std::vector<std::string>& updateIds,
    bool hide,
    LogCallback onLog) {
    if (updateIds.empty()) {
        return;
    }
    std::thread oldInstall;
    {
        std::lock_guard<std::mutex> tLock(impl_->threadMutex);
        std::lock_guard<std::mutex> cbLock(impl_->callbackMutex);
        if (!impl_->ownerAlive || impl_->searching.load() || impl_->installing.load()) {
            return;
        }
        impl_->cancelRequested.store(false);
        impl_->installing.store(true);
        impl_->progress.store(0.20f);
        if (impl_->installDoneEvent.IsValid()) {
            ResetEvent(impl_->installDoneEvent.Get());
        }
        if (impl_->installThread.joinable()) {
            oldInstall = std::move(impl_->installThread);
        }
    }
    if (oldInstall.joinable()) {
        oldInstall.join();
    }

    auto setPhase = [state = impl_](const std::string& msg) {
        std::lock_guard<std::mutex> lock(state->dataMutex);
        state->phaseMessage = msg;
    };
    setPhase(hide ? "Скрытие выбранных обновлений..." : "Возврат скрытых обновлений...");

    std::lock_guard<std::mutex> tLock(impl_->threadMutex);
    {
        std::lock_guard<std::mutex> cbLock(impl_->callbackMutex);
        if (!impl_->ownerAlive) {
            impl_->installing.store(false);
            if (impl_->installDoneEvent.IsValid()) SetEvent(impl_->installDoneEvent.Get());
            return;
        }
    }

    std::shared_ptr<Impl> state = impl_;
    impl_->installThread = std::thread(
        [state, setPhase, updateIds, hide, rawLog = std::move(onLog)]() {
            auto log = [&state, &rawLog](LogLevel lvl, const std::string& msg) {
                std::lock_guard<std::mutex> cbLock(state->callbackMutex);
                if (state->ownerAlive && rawLog) {
                    rawLog(lvl, msg);
                }
            };

            struct DoneSignal {
                HANDLE ev;
                std::atomic<bool>& flag;
                ~DoneSignal() {
                    flag.store(false);
                    if (ev != nullptr) SetEvent(ev);
                }
            } guard{state->installDoneEvent.Get(), state->installing};

            bool ok = false;
            if (IsProcessElevated()) {
                ok = SetUpdatesHiddenSync(updateIds, hide, log);
            } else {
                const std::filesystem::path tempLog = CreateTempLogPath(L"hdm_wuhide");
                std::wstring args =
                    std::wstring(hide ? L"--cli-wu-hide " : L"--cli-wu-unhide ") +
                    QuoteWin32Arg(Utf8ToWide(JoinUpdateIds(updateIds)));
                if (!tempLog.empty()) {
                    args += L" --log " + QuoteWin32Arg(tempLog.wstring());
                }

                const ElevatedRunResult run = ExecuteElevatedSelf(
                    args, true, tempLog.empty() ? nullptr : &tempLog, log, nullptr, &state->cancelRequested);
                std::error_code ec;
                if (!tempLog.empty()) {
                    std::filesystem::remove(tempLog, ec);
                }
                if (!run.launched) {
                    log(run.launchError == ERROR_CANCELLED ? LogLevel::Warning : LogLevel::Error,
                        run.launchError == ERROR_CANCELLED
                            ? "Операция скрытия отменена пользователем (UAC)."
                            : "Не удалось запустить elevated-процесс: " +
                                  FormatWin32Error(run.launchError));
                } else {
                    ok = (!run.cancelled && !run.timedOutWaiting && run.exitCode == 0);
                }
            }

            if (ok) {
                std::set<std::string> idSet(updateIds.begin(), updateIds.end());
                {
                    std::lock_guard<std::mutex> lock(state->dataMutex);
                    for (auto& item : state->updates) {
                        if (idSet.count(item.updateId) > 0) {
                            item.isHidden = hide;
                            item.selected = !hide;
                            item.statusText =
                                hide ? "Скрыто (заблокировано)"
                                     : (item.isDownloaded ? "Загружен (готов к установке)" : "Доступен");
                        }
                    }
                }
                state->generation.fetch_add(1);
                setPhase(hide ? "Выбранные обновления скрыты" : "Обновления возвращены в список");
            } else {
                setPhase(hide ? "Не удалось скрыть обновления" : "Не удалось вернуть обновления");
            }
            state->progress.store(1.0f);
        });
}

} // namespace hdm
