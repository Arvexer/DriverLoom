#include "core/SystemRestore.hpp"

#include <oleauto.h>
#include <srrestoreptapi.h>
#include <wbemidl.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>

namespace hdm {

namespace {

struct VariantGuard {
    VARIANT v;
    VariantGuard() noexcept { VariantInit(&v); }
    ~VariantGuard() { VariantClear(&v); }
    VariantGuard(const VariantGuard&) = delete;
    VariantGuard& operator=(const VariantGuard&) = delete;
};

bool ReadDwordRegValue(HKEY key, const wchar_t* valueName, DWORD& outVal) noexcept {
    DWORD type = 0;
    DWORD bytes = sizeof(DWORD);
    DWORD val = 0;
    if (RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(&val), &bytes) ==
            ERROR_SUCCESS &&
        type == REG_DWORD) {
        outVal = val;
        return true;
    }
    return false;
}

} // namespace

SystemProtectionState SystemRestore::ClassifySystemProtectionState(
    bool hasPolicyDisable,
    DWORD policyDisableVal,
    bool hasRpInterval,
    DWORD rpIntervalVal) noexcept {
    if (hasPolicyDisable && policyDisableVal != 0) {
        return SystemProtectionState::DisabledByPolicy;
    }
    if (hasRpInterval && rpIntervalVal != 0) {
        return SystemProtectionState::Enabled;
    }
    return SystemProtectionState::Disabled;
}

SystemProtectionState SystemRestore::QuerySystemProtectionState() noexcept {
    bool hasPolicyDisable = false;
    DWORD policyDisableVal = 0;
    {
        RegKeyGuard polKey;
        if (RegOpenKeyExW(
                HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Policies\\Microsoft\\Windows NT\\SystemRestore",
                0,
                KEY_READ,
                polKey.Put()) == ERROR_SUCCESS) {
            hasPolicyDisable = ReadDwordRegValue(polKey.Get(), L"DisableSR", policyDisableVal);
        }
    }

    bool hasRpInterval = false;
    DWORD rpIntervalVal = 0;
    {
        RegKeyGuard srKey;
        if (RegOpenKeyExW(
                HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\SystemRestore",
                0,
                KEY_READ,
                srKey.Put()) == ERROR_SUCCESS) {
            hasRpInterval = ReadDwordRegValue(srKey.Get(), L"RPSessionInterval", rpIntervalVal);
        }
    }

    return ClassifySystemProtectionState(
        hasPolicyDisable, policyDisableVal, hasRpInterval, rpIntervalVal);
}

std::wstring SystemRestore::GetSystemDriveRootW() {
    wchar_t winDir[MAX_PATH]{};
    const UINT len = GetSystemWindowsDirectoryW(winDir, MAX_PATH);
    if (len >= 2 && std::iswalpha(static_cast<wint_t>(winDir[0])) != 0 && winDir[1] == L':') {
        std::wstring root;
        root.push_back(static_cast<wchar_t>(std::towupper(static_cast<wint_t>(winDir[0]))));
        root += L":\\";
        return root;
    }
    return L"C:\\";
}

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

void SystemRestore::EnableSystemProtectionAsync(
    bool createRestorePointAfter,
    const std::string& restorePointDescription,
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
                onLog(LogLevel::Warning, "Операция с защитой системы уже выполняется.");
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

    worker_ = std::thread([this,
                           createRestorePointAfter,
                           restorePointDescription,
                           log = std::move(onLog),
                           complete = std::move(onComplete)]() {
        std::string outMsg;
        bool ok = false;

        if (IsProcessElevated()) {
            ok = EnableSystemProtectionSync(log, &outMsg);
            if (ok && createRestorePointAfter && !stopRequested_.load()) {
                CreateRestorePointSync(
                    restorePointDescription.empty() ? "DriverLoom" : restorePointDescription, log);
            }
        } else {
            if (log) {
                log(LogLevel::Info,
                    "Запрос прав администратора (UAC) для включения защиты системы...");
            }
            const std::filesystem::path tempLog = CreateTempLogPath(L"hdm_sr_en");
            std::wstring args = L"--cli-enable-system-protection";
            if (createRestorePointAfter) {
                const std::wstring wideDesc = Utf8ToWide(
                    restorePointDescription.empty() ? "DriverLoom" : restorePointDescription);
                args += L" --restore-desc " + QuoteWin32Arg(wideDesc);
            }
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
                outMsg = (run.launchError == ERROR_CANCELLED)
                             ? "Пользователь отменил запрос UAC для включения защиты системы."
                             : ("Не удалось запустить процесс с правами администратора: " +
                                FormatWin32Error(run.launchError));
                if (log) {
                    log(LogLevel::Warning, outMsg);
                }
            } else {
                ok = (!run.cancelled && !run.timedOutWaiting && run.exitCode == 0);
                outMsg = ok ? "Защита системы успешно включена."
                            : (run.cancelled
                                   ? "Включение защиты системы отменено."
                                   : "Не удалось включить защиту системы (код " +
                                         std::to_string(run.exitCode) + ").");
                if (!ok && log) {
                    log(LogLevel::Warning, outMsg);
                }
            }
        }

        busy_.store(false);
        if (complete && !stopRequested_.load()) {
            complete(ok, outMsg);
        }
    });
}

bool SystemRestore::EnableSystemProtectionSync(
    const LogCallback& onLog,
    std::string* outMessage) {
    if (QuerySystemProtectionState() == SystemProtectionState::DisabledByPolicy) {
        const std::string errMsg = "Защита системы запрещена групповой политикой Windows.";
        if (onLog) {
            onLog(LogLevel::Error, errMsg);
        }
        if (outMessage != nullptr) {
            *outMessage = errMsg;
        }
        return false;
    }

    const std::wstring driveRoot = GetSystemDriveRootW();
    if (onLog) {
        onLog(LogLevel::Info, "Включение защиты системы для диска " + WideToUtf8(driveRoot) + "...");
    }

    CoInitGuard comInit(COINIT_MULTITHREADED);
    if (!comInit.Succeeded()) {
        const std::string errMsg = "Ошибка инициализации COM (WMI): " +
                                   FormatWin32Error(static_cast<DWORD>(comInit.Result()));
        if (onLog) {
            onLog(LogLevel::Error, errMsg);
        }
        if (outMessage != nullptr) {
            *outMessage = errMsg;
        }
        return false;
    }

    const HRESULT secHr = CoInitializeSecurity(
        nullptr,
        -1,
        nullptr,
        nullptr,
        RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr,
        EOAC_NONE,
        nullptr);
    if (FAILED(secHr) && secHr != RPC_E_TOO_LATE) {
        const std::string errMsg =
            "Ошибка CoInitializeSecurity: " + FormatWin32Error(static_cast<DWORD>(secHr));
        if (onLog) {
            onLog(LogLevel::Error, errMsg);
        }
        if (outMessage != nullptr) {
            *outMessage = errMsg;
        }
        return false;
    }

    ComPtr<IWbemLocator> locator;
    HRESULT hr = CoCreateInstance(
        CLSID_WbemLocator,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IWbemLocator,
        reinterpret_cast<void**>(locator.Put()));
    if (FAILED(hr) || !locator) {
        const std::string errMsg =
            "Не удалось создать WMI Locator: " + FormatWin32Error(static_cast<DWORD>(hr));
        if (onLog) {
            onLog(LogLevel::Error, errMsg);
        }
        if (outMessage != nullptr) {
            *outMessage = errMsg;
        }
        return false;
    }

    ComPtr<IWbemServices> services;
    {
        BstrGuard ns(L"ROOT\\DEFAULT");
        hr = locator->ConnectServer(
            ns.Get(), nullptr, nullptr, nullptr, 0, nullptr, nullptr, services.Put());
    }
    if (FAILED(hr) || !services) {
        const std::string errMsg =
            "Не удалось подключиться к WMI ROOT\\DEFAULT: " + FormatWin32Error(static_cast<DWORD>(hr));
        if (onLog) {
            onLog(LogLevel::Error, errMsg);
        }
        if (outMessage != nullptr) {
            *outMessage = errMsg;
        }
        return false;
    }

    hr = CoSetProxyBlanket(
        services.Get(),
        RPC_C_AUTHN_WINNT,
        RPC_C_AUTHZ_NONE,
        nullptr,
        RPC_C_AUTHN_LEVEL_CALL,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr,
        EOAC_NONE);
    if (FAILED(hr)) {
        const std::string errMsg =
            "Ошибка CoSetProxyBlanket (WMI): " + FormatWin32Error(static_cast<DWORD>(hr));
        if (onLog) {
            onLog(LogLevel::Error, errMsg);
        }
        if (outMessage != nullptr) {
            *outMessage = errMsg;
        }
        return false;
    }

    BstrGuard clsName(L"SystemRestore");
    BstrGuard methodName(L"Enable");
    ComPtr<IWbemClassObject> srClass;
    hr = services->GetObject(clsName.Get(), 0, nullptr, srClass.Put(), nullptr);
    if (FAILED(hr) || !srClass) {
        const std::string errMsg =
            "Класс WMI SystemRestore недоступен: " + FormatWin32Error(static_cast<DWORD>(hr));
        if (onLog) {
            onLog(LogLevel::Error, errMsg);
        }
        if (outMessage != nullptr) {
            *outMessage = errMsg;
        }
        return false;
    }

    ComPtr<IWbemClassObject> inParamsDef;
    hr = srClass->GetMethod(methodName.Get(), 0, inParamsDef.Put(), nullptr);
    if (FAILED(hr) || !inParamsDef) {
        const std::string errMsg =
            "Метод WMI SystemRestore::Enable недоступен: " + FormatWin32Error(static_cast<DWORD>(hr));
        if (onLog) {
            onLog(LogLevel::Error, errMsg);
        }
        if (outMessage != nullptr) {
            *outMessage = errMsg;
        }
        return false;
    }

    ComPtr<IWbemClassObject> inParams;
    hr = inParamsDef->SpawnInstance(0, inParams.Put());
    if (FAILED(hr) || !inParams) {
        const std::string errMsg =
            "Ошибка подготовки параметров WMI: " + FormatWin32Error(static_cast<DWORD>(hr));
        if (onLog) {
            onLog(LogLevel::Error, errMsg);
        }
        if (outMessage != nullptr) {
            *outMessage = errMsg;
        }
        return false;
    }

    {
        VariantGuard varDrive;
        BSTR bstrDrive = SysAllocString(driveRoot.c_str());
        if (bstrDrive == nullptr) {
            const std::string errMsg = "Недостаточно памяти при передаче имени диска в WMI.";
            if (onLog) {
                onLog(LogLevel::Error, errMsg);
            }
            if (outMessage != nullptr) {
                *outMessage = errMsg;
            }
            return false;
        }
        varDrive.v.vt = VT_BSTR;
        varDrive.v.bstrVal = bstrDrive;
        hr = inParams->Put(L"Drive", 0, &varDrive.v, 0);
    }
    if (FAILED(hr)) {
        const std::string errMsg =
            "Ошибка задания параметра диска WMI: " + FormatWin32Error(static_cast<DWORD>(hr));
        if (onLog) {
            onLog(LogLevel::Error, errMsg);
        }
        if (outMessage != nullptr) {
            *outMessage = errMsg;
        }
        return false;
    }

    ComPtr<IWbemClassObject> outParams;
    hr = services->ExecMethod(
        clsName.Get(), methodName.Get(), 0, nullptr, inParams.Get(), outParams.Put(), nullptr);
    if (FAILED(hr)) {
        const std::string errMsg =
            "Не удалось включить защиту системы (WMI ExecMethod): " +
            FormatWin32Error(static_cast<DWORD>(hr));
        if (onLog) {
            onLog(LogLevel::Error, errMsg);
        }
        if (outMessage != nullptr) {
            *outMessage = errMsg;
        }
        return false;
    }

    if (outParams) {
        VariantGuard retVal;
        if (SUCCEEDED(outParams->Get(L"ReturnValue", 0, &retVal.v, nullptr, nullptr))) {
            DWORD wmiCode = 0;
            bool hasCode = false;
            if (retVal.v.vt == VT_I4) {
                wmiCode = static_cast<DWORD>(retVal.v.lVal);
                hasCode = true;
            } else if (retVal.v.vt == VT_UI4) {
                wmiCode = retVal.v.ulVal;
                hasCode = true;
            }
            if (hasCode && wmiCode != 0) {
                const std::string errMsg =
                    "Не удалось включить защиту системы: " + FormatWin32Error(wmiCode);
                if (onLog) {
                    onLog(LogLevel::Error, errMsg);
                }
                if (outMessage != nullptr) {
                    *outMessage = errMsg;
                }
                return false;
            }
        }
    }

    const std::string okMsg = "Защита системы включена для диска " + WideToUtf8(driveRoot) + ".";
    if (onLog) {
        onLog(LogLevel::Success, okMsg);
    }
    if (outMessage != nullptr) {
        *outMessage = okMsg;
    }
    return true;
}

} // namespace hdm
