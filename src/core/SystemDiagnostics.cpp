#include "core/SystemDiagnostics.hpp"

#include <winsvc.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>

namespace hdm {

namespace {

class ScHandleGuard {
public:
    explicit ScHandleGuard(SC_HANDLE h = nullptr) noexcept : handle_(h) {}
    ~ScHandleGuard() { Reset(); }

    ScHandleGuard(const ScHandleGuard&) = delete;
    ScHandleGuard& operator=(const ScHandleGuard&) = delete;

    void Reset(SC_HANDLE h = nullptr) noexcept {
        if (handle_ != nullptr) {
            CloseServiceHandle(handle_);
        }
        handle_ = h;
    }

    [[nodiscard]] SC_HANDLE Get() const noexcept { return handle_; }
    [[nodiscard]] bool IsValid() const noexcept { return handle_ != nullptr; }

private:
    SC_HANDLE handle_{nullptr};
};

std::wstring ReadRegStringVal(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes == 0 || (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ)) {
        return {};
    }
    std::wstring buf(bytes / sizeof(wchar_t) + 2, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(buf.data()), &bytes) != ERROR_SUCCESS) {
        return {};
    }
    if (type == REG_MULTI_SZ) {
        std::string joined;
        const wchar_t* p = buf.c_str();
        while (*p != L'\0') {
            if (!joined.empty()) joined += " ";
            joined += WideToUtf8(p);
            p += std::wcslen(p) + 1;
        }
        return Utf8ToWide(joined);
    }
    return std::wstring(buf.c_str());
}

bool ReadRegDwordVal(HKEY key, const wchar_t* name, DWORD& outVal) {
    DWORD type = 0;
    DWORD bytes = sizeof(DWORD);
    DWORD val = 0;
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(&val), &bytes) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        outVal = val;
        return true;
    }
    return false;
}

bool RegKeyExists(HKEY root, const wchar_t* subKey) {
    RegKeyGuard k;
    return RegOpenKeyExW(root, subKey, 0, KEY_READ, k.Put()) == ERROR_SUCCESS;
}

bool IsPlaceholderSystemString(const std::string& s) {
    if (s.empty()) return true;
    return ContainsCaseInsensitive(s, "To Be Filled") ||
           ContainsCaseInsensitive(s, "Default string") ||
           ContainsCaseInsensitive(s, "System Product Name") ||
           ContainsCaseInsensitive(s, "System Manufacturer") ||
           ContainsCaseInsensitive(s, "Not Specified");
}

std::string ResolveMotherboardSupportUrl(const std::string& mfg, const std::string& product) {
    const std::string combined = mfg + " " + product;
    if (ContainsCaseInsensitive(combined, "ASUS")) {
        return "https://www.asus.com/support/download-center/";
    }
    if (ContainsCaseInsensitive(combined, "MSI") || ContainsCaseInsensitive(combined, "Micro-Star")) {
        return "https://www.msi.com/support/download";
    }
    if (ContainsCaseInsensitive(combined, "Gigabyte")) {
        return "https://www.gigabyte.com/Support";
    }
    if (ContainsCaseInsensitive(combined, "ASRock")) {
        return "https://www.asrock.com/support/index.asp";
    }
    if (ContainsCaseInsensitive(combined, "Dell") || ContainsCaseInsensitive(combined, "Alienware")) {
        return "https://www.dell.com/support/home";
    }
    if (ContainsCaseInsensitive(combined, "Lenovo")) {
        return "https://pcsupport.lenovo.com/";
    }
    if (ContainsCaseInsensitive(combined, "HP") || ContainsCaseInsensitive(combined, "Hewlett")) {
        return "https://support.hp.com/drivers";
    }
    if (ContainsCaseInsensitive(combined, "Acer")) {
        return "https://www.acer.com/us-en/support/drivers-and-manuals";
    }
    return {};
}

MotherboardInfo ReadMotherboardInfo() {
    MotherboardInfo info;
    RegKeyGuard key;
    if (RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DESCRIPTION\\System\\BIOS",
            0,
            KEY_READ,
            key.Put()) == ERROR_SUCCESS) {
        info.manufacturer = WideToUtf8(ReadRegStringVal(key.Get(), L"BaseBoardManufacturer"));
        info.product = WideToUtf8(ReadRegStringVal(key.Get(), L"BaseBoardProduct"));
        info.systemManufacturer = WideToUtf8(ReadRegStringVal(key.Get(), L"SystemManufacturer"));
        info.systemProduct = WideToUtf8(ReadRegStringVal(key.Get(), L"SystemProductName"));
        info.biosVersion = WideToUtf8(ReadRegStringVal(key.Get(), L"BIOSVersion"));
        info.biosDate = WideToUtf8(ReadRegStringVal(key.Get(), L"BIOSReleaseDate"));
    }
    if (IsPlaceholderSystemString(info.manufacturer) &&
        !IsPlaceholderSystemString(info.systemManufacturer)) {
        info.manufacturer = info.systemManufacturer;
    }
    if (IsPlaceholderSystemString(info.product) &&
        !IsPlaceholderSystemString(info.systemProduct)) {
        info.product = info.systemProduct;
    }
    info.supportUrl = ResolveMotherboardSupportUrl(info.manufacturer, info.product);
    return info;
}

WindowsReadinessInfo ReadWindowsReadiness() {
    WindowsReadinessInfo r;
    r.cbsRebootPending = RegKeyExists(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\RebootPending");
    r.wuRebootRequired = RegKeyExists(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired");

    {
        RegKeyGuard smKey;
        if (RegOpenKeyExW(
                HKEY_LOCAL_MACHINE,
                L"SYSTEM\\CurrentControlSet\\Control\\Session Manager",
                0,
                KEY_READ,
                smKey.Put()) == ERROR_SUCCESS) {
            DWORD type = 0;
            DWORD bytes = 0;
            if (RegQueryValueExW(smKey.Get(), L"PendingFileRenameOperations",
                                 nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
                bytes > 4) {
                r.pendingFileRenames = true;
            }
        }
    }

    {
        RegKeyGuard srKey;
        if (RegOpenKeyExW(
                HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\SystemRestore",
                0,
                KEY_READ,
                srKey.Put()) == ERROR_SUCCESS) {
            DWORD rpInterval = 0;
            if (ReadRegDwordVal(srKey.Get(), L"RPSessionInterval", rpInterval)) {
                r.systemRestoreEnabled = (rpInterval != 0);
            }
        }
        RegKeyGuard polKey;
        if (RegOpenKeyExW(
                HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Policies\\Microsoft\\Windows NT\\SystemRestore",
                0,
                KEY_READ,
                polKey.Put()) == ERROR_SUCCESS) {
            DWORD disableSr = 0;
            if (ReadRegDwordVal(polKey.Get(), L"DisableSR", disableSr) && disableSr != 0) {
                r.systemRestorePolicyDisabled = true;
                r.systemRestoreEnabled = false;
            }
        }
    }
    return r;
}

struct MonitoredServiceDef {
    const wchar_t* name;
    const char* nameUtf8;
    const char* displayName;
    const char* purpose;
    bool criticalForUpdates;
    bool preferAutoStart;
};

constexpr MonitoredServiceDef kMonitoredServices[] = {
    {L"wuauserv", "wuauserv", "Центр обновления Windows",
     "Поиск, скачивание и установка драйверов и обновлений Windows Update.", true, false},
    {L"TrustedInstaller", "TrustedInstaller", "Установщик модулей Windows",
     "Установка системных компонентов, обновлений ОС, .NET и хранилища CBS.", true, false},
    {L"UsoSvc", "UsoSvc", "Оркестратор обновлений (UsoSvc)",
     "Координирует сеансы скачивания и установки через Windows Update.", true, true},
    {L"bits", "bits", "Фоновая служба передачи (BITS)",
     "Отвечает за надёжную докачку пакетов Windows Update и системных файлов.", true, false},
    {L"cryptsvc", "cryptsvc", "Службы криптографии",
     "Проверяет цифровые подписи драйверов (.cat), сертификаты и каталоги обновлений.", true, true},
    {L"DeviceInstall", "DeviceInstall", "Служба установки устройств",
     "Устанавливает и настраивает PnP-драйверы (.inf) и периферию.", true, false},
    {L"PlugPlay", "PlugPlay", "Plug and Play (PnP)",
     "Обнаружение подключаемого оборудования, мониторов, USB и PCI-контроллеров.", true, true},
    {L"VSS", "VSS", "Теневое копирование тома (VSS)",
     "Необходимо для создания точек восстановления системы перед установкой драйверов.", false, false},
    {L"WinDefend", "WinDefend", "Microsoft Defender",
     "Встроенная защита Windows (может быть отключена сторонним антивирусом).", false, true},
};

std::string ServiceStateToText(DWORD state) {
    switch (state) {
    case SERVICE_RUNNING: return "Работает";
    case SERVICE_STOPPED: return "Остановлена";
    case SERVICE_START_PENDING: return "Запускается";
    case SERVICE_STOP_PENDING: return "Останавливается";
    case SERVICE_PAUSED: return "Приостановлена";
    default: return "Неизвестно";
    }
}

std::string ServiceStartTypeToText(DWORD startType) {
    switch (startType) {
    case SERVICE_AUTO_START: return "Авто";
    case SERVICE_DEMAND_START: return "Вручную";
    case SERVICE_DISABLED: return "Отключена";
    case SERVICE_BOOT_START:
    case SERVICE_SYSTEM_START: return "Системная";
    default: return "—";
    }
}

std::vector<ServiceHealthItem> QueryMonitoredServices(size_t& outDisabledCritical) {
    outDisabledCritical = 0;
    std::vector<ServiceHealthItem> list;
    ScHandleGuard scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.IsValid()) {
        return list;
    }

    for (const auto& def : kMonitoredServices) {
        ServiceHealthItem item;
        item.serviceName = def.nameUtf8;
        item.displayName = def.displayName;
        item.purpose = def.purpose;
        item.criticalForUpdates = def.criticalForUpdates;
        item.preferAutoStart = def.preferAutoStart;

        ScHandleGuard svc(OpenServiceW(
            scm.Get(), def.name, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG));
        if (!svc.IsValid()) {
            item.health = ServiceHealthState::MissingOrError;
            item.stateText = "Недоступна";
            item.startTypeText = "—";
            list.push_back(std::move(item));
            continue;
        }

        SERVICE_STATUS_PROCESS ssp{};
        DWORD bytesNeeded = 0;
        DWORD curState = SERVICE_STOPPED;
        if (QueryServiceStatusEx(
                svc.Get(),
                SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&ssp),
                sizeof(ssp),
                &bytesNeeded)) {
            curState = ssp.dwCurrentState;
            item.stateText = ServiceStateToText(curState);
        }

        DWORD cfgBytes = 0;
        QueryServiceConfigW(svc.Get(), nullptr, 0, &cfgBytes);
        DWORD startType = SERVICE_DEMAND_START;
        if (cfgBytes > 0) {
            std::vector<BYTE> buf(cfgBytes);
            auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
            if (QueryServiceConfigW(svc.Get(), cfg, cfgBytes, &cfgBytes)) {
                startType = cfg->dwStartType;
            }
        }
        item.startTypeText = ServiceStartTypeToText(startType);

        if (startType == SERVICE_DISABLED) {
            item.health = ServiceHealthState::Disabled;
            if (def.criticalForUpdates) {
                ++outDisabledCritical;
            }
        } else {
            item.health = ServiceHealthState::Healthy;
            if (curState == SERVICE_STOPPED && startType == SERVICE_DEMAND_START) {
                item.stateText = "Готова (по требованию)";
            }
        }
        list.push_back(std::move(item));
    }
    return list;
}

int ParseDriverYear(const std::string& driverDate) {
    // Windows registry DriverDate format is typically M-D-YYYY or MM-DD-YYYY.
    const size_t dash = driverDate.rfind('-');
    if (dash == std::string::npos || dash + 1 >= driverDate.size()) {
        return 0;
    }
    int year = 0;
    for (size_t i = dash + 1; i < driverDate.size(); ++i) {
        const char ch = driverDate[i];
        if (ch < '0' || ch > '9') break;
        year = year * 10 + (ch - '0');
    }
    return (year >= 1995 && year <= 2100) ? year : 0;
}

bool IsVirtualOrSoftwareAdapter(const DeviceInfo& d) {
    return ContainsCaseInsensitive(d.name, "WAN Miniport") ||
           ContainsCaseInsensitive(d.name, "Kernel Debug") ||
           ContainsCaseInsensitive(d.name, "VirtualBox") ||
           ContainsCaseInsensitive(d.name, "VMware") ||
           ContainsCaseInsensitive(d.name, "Radmin") ||
           ContainsCaseInsensitive(d.name, "Hyper-V") ||
           ContainsCaseInsensitive(d.name, "TAP-Windows") ||
           ContainsCaseInsensitive(d.name, "Loopback") ||
           ContainsCaseInsensitive(d.name, "Personal Area Network") ||
           ContainsCaseInsensitive(d.name, "SteelSeries Sonar") ||
           ContainsCaseInsensitive(d.name, "Razer THX") ||
           ContainsCaseInsensitive(d.name, "NVIDIA Virtual Audio");
}

std::vector<ControllerDiagnosticItem> AnalyzeKeyControllers(
    const std::vector<DeviceInfo>& devices,
    size_t& outAttention) {
    outAttention = 0;
    std::vector<ControllerDiagnosticItem> list;
    std::set<std::string> seenIds;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    const int currentYear = static_cast<int>(now.wYear);

    for (const auto& d : devices) {
        const std::string clsLower = ToLowerAscii(d.deviceClass);
        const bool isPci = ContainsCaseInsensitive(d.hardwareId, "PCI\\VEN_") ||
                           ContainsCaseInsensitive(d.instanceId, "PCI\\VEN_");
        const bool isUsbAudioOrBt =
            (clsLower == "bluetooth" || clsLower == "media") &&
            ContainsCaseInsensitive(d.hardwareId, "USB\\VID_");

        std::string role;
        if (clsLower == "display") {
            role = "Видеокарта";
        } else if (clsLower == "net" && isPci) {
            if (ContainsCaseInsensitive(d.name, "Wi-Fi") ||
                ContainsCaseInsensitive(d.name, "Wireless") ||
                ContainsCaseInsensitive(d.name, "WLAN") ||
                ContainsCaseInsensitive(d.name, "AX")) {
                role = "Wi-Fi адаптер";
            } else {
                role = "Сеть (LAN)";
            }
        } else if (clsLower == "media" &&
                   (ContainsCaseInsensitive(d.hardwareId, "HDAUDIO\\") ||
                    ContainsCaseInsensitive(d.hardwareId, "INTELAUDIO\\") || isPci ||
                    (isUsbAudioOrBt && !IsVirtualOrSoftwareAdapter(d)))) {
            role = "Аудио";
        } else if (clsLower == "bluetooth" && (isPci || isUsbAudioOrBt)) {
            role = "Bluetooth";
        } else if ((clsLower == "system" || clsLower == "hdc" || clsLower == "scsiadapter") &&
                   isPci &&
                   !ContainsCaseInsensitive(d.infName, "machine.inf") &&
                   !ContainsCaseInsensitive(d.infName, "pci.inf") &&
                   !ContainsCaseInsensitive(d.infName, "volume.inf") &&
                   !ContainsCaseInsensitive(d.infName, "hdaudbus.inf") &&
                   !ContainsCaseInsensitive(d.infName, "stornvme.inf")) {
            if (d.pciVendorId == "1022" || d.pciVendorId == "8086" ||
                ContainsCaseInsensitive(d.name, "SMBus") ||
                ContainsCaseInsensitive(d.name, "Chipset") ||
                ContainsCaseInsensitive(d.name, "GPIO") ||
                ContainsCaseInsensitive(d.name, "PSP") ||
                ContainsCaseInsensitive(d.name, "Management Engine") ||
                clsLower == "scsiadapter") {
                role = (clsLower == "scsiadapter") ? "Контроллер накопителей" : "Чипсет / плата";
            }
        }

        if (IsVirtualOrSoftwareAdapter(d)) {
            continue;
        }
        if (role.empty() && d.health == DeviceHealth::Ok) {
            continue;
        }
        if (role.empty()) {
            // Also include any failing PCI/system/unknown hardware controller
            if (isPci || d.category == DeviceCategory::DisplayAndChipset) {
                role = "Контроллер платы";
            } else {
                continue;
            }
        }

        const std::string dedupKey = role + "|" + d.name + "|" + d.searchHardwareKey;
        if (!seenIds.insert(dedupKey).second) {
            continue;
        }

        ControllerDiagnosticItem item;
        item.role = role;
        item.deviceName = d.name;
        item.manufacturer = d.manufacturer;
        item.driverVersion = d.driverVersion;
        item.driverDate = d.driverDate;
        item.infName = d.infName;
        item.hardwareId = d.hardwareId;
        item.searchKey = d.searchHardwareKey;

        const std::string infLower = ToLowerAscii(d.infName);
        const bool isFallbackMsDriver =
            (clsLower == "media" && infLower == "hdaudio.inf") ||
            (clsLower == "display" && infLower == "basicdisplay.inf");

        const int drvYear = ParseDriverYear(d.driverDate);
        const bool isMicrosoftGeneric2006 =
            (drvYear == 2006) &&
            (ContainsCaseInsensitive(d.driverProvider, "Microsoft") ||
             ContainsCaseInsensitive(d.manufacturer, "Стандарт") ||
             ContainsCaseInsensitive(d.manufacturer, "Standard"));

        if (d.health != DeviceHealth::Ok) {
            item.health = ControllerHealth::Problem;
            item.statusText = d.statusText;
            ++outAttention;
        } else if (isFallbackMsDriver) {
            item.health = ControllerHealth::Attention;
            item.statusText = "Базовый драйвер Windows (" + d.infName + ")";
            ++outAttention;
        } else if (drvYear > 0 && !isMicrosoftGeneric2006 && (currentYear - drvYear) >= 5) {
            item.health = ControllerHealth::Attention;
            item.statusText = "Устаревший драйвер (" + std::to_string(drvYear) + " г.)";
            ++outAttention;
        } else {
            item.health = ControllerHealth::Ok;
            item.statusText = "Актуален";
        }
        list.push_back(std::move(item));
    }

    std::sort(list.begin(), list.end(), [](const ControllerDiagnosticItem& a, const ControllerDiagnosticItem& b) {
        if (a.health != b.health) {
            return static_cast<int>(a.health) > static_cast<int>(b.health);
        }
        return a.role < b.role;
    });
    return list;
}

std::string DecodeConsoleChunk(const std::string& raw) {
    if (raw.empty()) return {};

    // Check for UTF-16LE output (common for sfc.exe when redirected to a pipe).
    if (raw.size() >= 4 && (raw.size() % 2) == 0) {
        size_t evenZeros = 0;
        size_t oddZeros = 0;
        const size_t pairs = std::min<size_t>(raw.size() / 2, 64);
        for (size_t i = 0; i < pairs; ++i) {
            if (raw[i * 2] == '\0') ++evenZeros;
            if (raw[i * 2 + 1] == '\0') ++oddZeros;
        }
        if (oddZeros >= pairs / 3 && evenZeros == 0) {
            std::wstring wide(raw.size() / 2, L'\0');
            std::memcpy(wide.data(), raw.data(), raw.size());
            return WideToUtf8(wide);
        }
    }

    const int utf8Chars = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    if (utf8Chars > 0) {
        return raw;
    }

    const int oemChars = MultiByteToWideChar(
        CP_OEMCP, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    if (oemChars <= 0) {
        return raw;
    }
    std::wstring wide(static_cast<size_t>(oemChars), L'\0');
    MultiByteToWideChar(CP_OEMCP, 0, raw.data(), static_cast<int>(raw.size()), wide.data(), oemChars);
    return WideToUtf8(wide);
}

void EmitDecodedLines(
    std::string& carry,
    const std::string& decoded,
    const LogCallback& onLog,
    std::string& lastPercentLine) {
    carry += decoded;
    size_t pos = 0;
    while ((pos = carry.find_first_of("\r\n")) != std::string::npos) {
        std::string line = carry.substr(0, pos);
        carry.erase(0, pos + 1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        size_t lead = 0;
        while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) {
            ++lead;
        }
        if (lead > 0) line.erase(0, lead);
        if (line.empty()) continue;

        // Deduplicate noisy console progress bar redraws (e.g. "[=== 12.0% ]").
        if (line == lastPercentLine) continue;
        if (line.find('%') != std::string::npos && line.size() < 90 &&
            lastPercentLine.find('%') != std::string::npos && line.find('[') != std::string::npos) {
            lastPercentLine = line;
            continue;
        }
        lastPercentLine = line;
        if (onLog) {
            onLog(LogLevel::Info, line);
        }
    }
}

bool StopServiceWithWait(SC_HANDLE scm, const wchar_t* svcName, const LogCallback& onLog) {
    ScHandleGuard svc(OpenServiceW(scm, svcName, SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!svc.IsValid()) {
        return false;
    }
    SERVICE_STATUS status{};
    if (!ControlService(svc.Get(), SERVICE_CONTROL_STOP, &status)) {
        const DWORD err = GetLastError();
        if (err == ERROR_SERVICE_NOT_ACTIVE) {
            return true;
        }
        if (onLog) {
            onLog(LogLevel::Warning,
                  "Не удалось остановить службу " + WideToUtf8(svcName) + ": " +
                      FormatWin32Error(err));
        }
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        SERVICE_STATUS_PROCESS ssp{};
        DWORD bytes = 0;
        if (!QueryServiceStatusEx(
                svc.Get(), SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &bytes) ||
            ssp.dwCurrentState == SERVICE_STOPPED) {
            return true;
        }
        Sleep(250);
    }
    return false;
}

} // namespace

SystemDiagnostics::~SystemDiagnostics() {
    cancelRequested_.store(true);
    if (scanThread_.joinable()) {
        scanThread_.join();
    }
    if (actionThread_.joinable()) {
        actionThread_.join();
    }
}

void SystemDiagnostics::SetActionPhase(const std::string& msg) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    actionPhase_ = msg;
}

std::string SystemDiagnostics::ActionPhase() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    return actionPhase_;
}

SystemDiagnosticsSnapshot SystemDiagnostics::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    return snapshot_;
}

SystemDiagnosticsSnapshot SystemDiagnostics::CollectSynchronously(
    const std::vector<DeviceInfo>& devicesSnapshot) {
    SystemDiagnosticsSnapshot snap;
    snap.motherboard = ReadMotherboardInfo();
    snap.readiness = ReadWindowsReadiness();
    snap.services = QueryMonitoredServices(snap.disabledCriticalServices);
    snap.controllers = AnalyzeKeyControllers(devicesSnapshot, snap.attentionControllers);
    snap.valid = true;
    return snap;
}

void SystemDiagnostics::StartScanAsync(
    std::vector<DeviceInfo> devicesSnapshot,
    LogCallback onLog) {
    if (scanning_.exchange(true)) {
        return;
    }
    if (scanThread_.joinable()) {
        scanThread_.join();
    }

    scanThread_ = std::thread([this, devs = std::move(devicesSnapshot), log = std::move(onLog)]() {
        auto snap = CollectSynchronously(devs);
        const size_t disabledSvc = snap.disabledCriticalServices;
        const size_t attCtrl = snap.attentionControllers;
        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            snapshot_ = std::move(snap);
        }
        generation_.fetch_add(1);
        scanning_.store(false);
        if (log && (disabledSvc > 0 || attCtrl > 0)) {
            log(LogLevel::Warning,
                "Диагностика системы: отключённых критических служб — " +
                    std::to_string(disabledSvc) + ", контроллеров с замечаниями — " +
                    std::to_string(attCtrl) + ".");
        }
    });
}

void SystemDiagnostics::RunMaintenanceAsync(
    MaintenanceAction action,
    const std::string& targetArg,
    std::vector<DeviceInfo> devicesSnapshot,
    LogCallback onLog,
    ActionDoneCallback onComplete) {
    if (runningAction_.exchange(true)) {
        if (onLog) {
            onLog(LogLevel::Warning, "Операция обслуживания уже выполняется.");
        }
        return;
    }
    if (actionThread_.joinable()) {
        actionThread_.join();
    }

    cancelRequested_.store(false);
    actionThread_ = std::thread([this,
                                 action,
                                 targetArg,
                                 devs = std::move(devicesSnapshot),
                                 log = std::move(onLog),
                                 complete = std::move(onComplete)]() {
        bool ok = false;
        auto setPhase = [&](const std::string& m) { SetActionPhase(m); };

        if (action == MaintenanceAction::EnableService) {
            setPhase("Включение службы " + targetArg + "...");
            bool preferAuto = false;
            for (const auto& def : kMonitoredServices) {
                if (targetArg == def.nameUtf8) {
                    preferAuto = def.preferAutoStart;
                    break;
                }
            }
            if (IsProcessElevated()) {
                ok = EnableServiceSync(targetArg, preferAuto, log);
            } else {
                const std::filesystem::path tempLog = CreateTempLogPath(L"hdm_svc");
                std::wstring args = L"--cli-enable-service \"" + Utf8ToWide(targetArg) + L"\"";
                if (preferAuto) args += L" --auto-start";
                args += L" --log \"" + tempLog.wstring() + L"\"";
                const ElevatedRunResult run = ExecuteElevatedSelf(
                    args, true, &tempLog, log, nullptr, &cancelRequested_, nullptr);
                std::error_code ec;
                std::filesystem::remove(tempLog, ec);
                ok = run.launched && run.exitCode == 0;
            }
        } else if (action == MaintenanceAction::ResetWuDownloadCache) {
            setPhase("Очистка кэша загрузок Windows Update...");
            if (IsProcessElevated()) {
                ok = ResetWuDownloadCacheSync(log);
            } else {
                const std::filesystem::path tempLog = CreateTempLogPath(L"hdm_wuc");
                const std::wstring args = L"--cli-reset-wucache --log \"" + tempLog.wstring() + L"\"";
                const ElevatedRunResult run = ExecuteElevatedSelf(
                    args, true, &tempLog, log, nullptr, &cancelRequested_, nullptr);
                std::error_code ec;
                std::filesystem::remove(tempLog, ec);
                ok = run.launched && run.exitCode == 0;
            }
        } else {
            const wchar_t* cliSwitch = L"--cli-sfc";
            std::string label = "Проверка системных файлов (SFC /scannow)...";
            if (action == MaintenanceAction::DismCheckHealth) {
                cliSwitch = L"--cli-dism-check";
                label = "Проверка хранилища компонентов (DISM /CheckHealth)...";
            } else if (action == MaintenanceAction::DismRestoreHealth) {
                cliSwitch = L"--cli-dism-restore";
                label = "Восстановление образа Windows (DISM /RestoreHealth)...";
            }
            setPhase(label);

            if (IsProcessElevated()) {
                if (action == MaintenanceAction::SfcScan) {
                    ok = RunSystemToolSync(L"sfc.exe", L"/scannow", "SFC /scannow", log, &cancelRequested_);
                } else if (action == MaintenanceAction::DismCheckHealth) {
                    ok = RunSystemToolSync(
                        L"Dism.exe", L"/Online /Cleanup-Image /CheckHealth",
                        "DISM /CheckHealth", log, &cancelRequested_);
                } else {
                    ok = RunSystemToolSync(
                        L"Dism.exe", L"/Online /Cleanup-Image /RestoreHealth /NoRestart",
                        "DISM /RestoreHealth", log, &cancelRequested_);
                }
            } else {
                const std::filesystem::path tempLog = CreateTempLogPath(L"hdm_sys");
                const std::filesystem::path cancelMarker =
                    std::filesystem::path(tempLog.wstring() + L".cancel");
                std::error_code ec;
                std::filesystem::remove(cancelMarker, ec);
                const std::wstring args = std::wstring(cliSwitch) +
                                          L" --log \"" + tempLog.wstring() +
                                          L"\" --cancel-file \"" + cancelMarker.wstring() + L"\"";
                const ElevatedRunResult run = ExecuteElevatedSelf(
                    args, true, &tempLog, log, nullptr, &cancelRequested_, &cancelMarker);
                std::filesystem::remove(tempLog, ec);
                std::filesystem::remove(cancelMarker, ec);
                ok = run.launched && run.exitCode == 0;
            }
        }

        auto refreshed = CollectSynchronously(devs);
        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            snapshot_ = std::move(refreshed);
        }
        generation_.fetch_add(1);
        setPhase(ok ? "Операция завершена" : "Операция завершена с предупреждениями");
        runningAction_.store(false);
        if (complete) {
            complete(ok);
        }
    });
}

bool SystemDiagnostics::EnableServiceSync(
    const std::string& serviceName,
    bool autoStart,
    const LogCallback& onLog) {
    if (serviceName.empty()) {
        return false;
    }
    ScHandleGuard scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm.IsValid()) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Не удалось открыть диспетчер служб (SCM): " + FormatWin32Error(GetLastError()));
        }
        return false;
    }
    const std::wstring wName = Utf8ToWide(serviceName);
    ScHandleGuard svc(OpenServiceW(
        scm.Get(), wName.c_str(), SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_QUERY_STATUS));
    if (!svc.IsValid()) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Не удалось открыть службу «" + serviceName + "»: " + FormatWin32Error(GetLastError()));
        }
        return false;
    }

    const DWORD startType = autoStart ? SERVICE_AUTO_START : SERVICE_DEMAND_START;
    if (!ChangeServiceConfigW(
            svc.Get(),
            SERVICE_NO_CHANGE,
            startType,
            SERVICE_NO_CHANGE,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Ошибка ChangeServiceConfigW для «" + serviceName + "»: " +
                      FormatWin32Error(GetLastError()));
        }
        return false;
    }

    if (autoStart) {
        if (!StartServiceW(svc.Get(), 0, nullptr)) {
            const DWORD err = GetLastError();
            if (err != ERROR_SERVICE_ALREADY_RUNNING && onLog) {
                onLog(LogLevel::Warning,
                      "Тип запуска службы изменён, но автозапуск вернул: " + FormatWin32Error(err));
            }
        }
    }

    if (onLog) {
        onLog(LogLevel::Success,
              "Служба «" + serviceName + "» переведена в режим: " +
                  (autoStart ? "Автоматически" : "Вручную (по требованию)") + ".");
    }
    return true;
}

bool SystemDiagnostics::ResetWuDownloadCacheSync(const LogCallback& onLog) {
    if (onLog) {
        onLog(LogLevel::Info, "Остановка служб wuauserv и BITS...");
    }
    ScHandleGuard scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (scm.IsValid()) {
        StopServiceWithWait(scm.Get(), L"bits", onLog);
        StopServiceWithWait(scm.Get(), L"wuauserv", onLog);
    }

    wchar_t winDir[MAX_PATH]{};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    const std::filesystem::path dlDir =
        std::filesystem::path(winDir) / L"SoftwareDistribution" / L"Download";

    std::error_code ec;
    size_t removedCount = 0;
    if (std::filesystem::exists(dlDir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(dlDir, ec)) {
            std::filesystem::remove_all(entry.path(), ec);
            if (!ec) {
                ++removedCount;
            }
        }
    }

    if (scm.IsValid()) {
        ScHandleGuard wuSvc(OpenServiceW(scm.Get(), L"wuauserv", SERVICE_START));
        if (wuSvc.IsValid()) {
            StartServiceW(wuSvc.Get(), 0, nullptr);
        }
    }

    if (onLog) {
        onLog(LogLevel::Success,
              "Кэш загрузок Windows Update очищен (удалено объектов: " +
                  std::to_string(removedCount) + ").");
    }
    return true;
}

bool SystemDiagnostics::RunSystemToolSync(
    const std::wstring& exeName,
    const std::wstring& arguments,
    const std::string& title,
    const LogCallback& onLog,
    const std::atomic<bool>* cancelFlag) {
    wchar_t sysDir[MAX_PATH]{};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    const std::filesystem::path toolPath = std::filesystem::path(sysDir) / exeName;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rawRead = nullptr;
    HANDLE rawWrite = nullptr;
    if (!CreatePipe(&rawRead, &rawWrite, &sa, 0)) {
        if (onLog) {
            onLog(LogLevel::Error, "Ошибка CreatePipe: " + FormatWin32Error(GetLastError()));
        }
        return false;
    }
    HandleGuard pipeRead(rawRead);
    HandleGuard pipeWrite(rawWrite);
    SetHandleInformation(pipeRead.Get(), HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = pipeWrite.Get();
    si.hStdError = pipeWrite.Get();

    std::wstring cmdLine = L"\"" + toolPath.wstring() + L"\" " + arguments;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(
            toolPath.c_str(),
            cmdLine.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi)) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Не удалось запустить " + title + ": " + FormatWin32Error(GetLastError()));
        }
        return false;
    }

    HandleGuard proc(pi.hProcess);
    HandleGuard thread(pi.hThread);
    pipeWrite.Reset();

    if (onLog) {
        onLog(LogLevel::Info, "Запущена утилита: " + title + "...");
    }

    std::string carry;
    std::string lastPctLine;
    bool cancelled = false;
    char buf[4096];

    for (;;) {
        if (cancelFlag != nullptr && cancelFlag->load()) {
            cancelled = true;
            TerminateProcess(proc.Get(), 1);
            break;
        }

        DWORD avail = 0;
        if (PeekNamedPipe(pipeRead.Get(), nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD readBytes = 0;
            if (ReadFile(
                    pipeRead.Get(),
                    buf,
                    static_cast<DWORD>(std::min<DWORD>(avail, sizeof(buf))),
                    &readBytes,
                    nullptr) &&
                readBytes > 0) {
                const std::string decoded = DecodeConsoleChunk(std::string(buf, readBytes));
                EmitDecodedLines(carry, decoded, onLog, lastPctLine);
            }
            continue;
        }

        if (WaitForSingleObject(proc.Get(), 150) == WAIT_OBJECT_0) {
            while (PeekNamedPipe(pipeRead.Get(), nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                DWORD readBytes = 0;
                if (!ReadFile(
                        pipeRead.Get(),
                        buf,
                        static_cast<DWORD>(std::min<DWORD>(avail, sizeof(buf))),
                        &readBytes,
                        nullptr) ||
                    readBytes == 0) {
                    break;
                }
                const std::string decoded = DecodeConsoleChunk(std::string(buf, readBytes));
                EmitDecodedLines(carry, decoded, onLog, lastPctLine);
            }
            break;
        }
    }

    if (!carry.empty() && onLog) {
        onLog(LogLevel::Info, carry);
    }

    if (cancelled) {
        if (onLog) {
            onLog(LogLevel::Warning, title + " прервана пользователем.");
        }
        return false;
    }

    DWORD exitCode = 1;
    GetExitCodeProcess(proc.Get(), &exitCode);
    if (onLog) {
        onLog(exitCode == 0 ? LogLevel::Success : LogLevel::Warning,
              title + " завершилась с кодом " + std::to_string(exitCode) + ".");
    }
    return exitCode == 0;
}

} // namespace hdm
