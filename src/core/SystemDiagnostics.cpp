#include "core/SystemDiagnostics.hpp"

#include <shobjidl.h>
#include <winsvc.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

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

std::string TrimWhitespace(const std::string& s) {
    size_t first = 0;
    while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first])) != 0) {
        ++first;
    }
    size_t last = s.size();
    while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1])) != 0) {
        --last;
    }
    return s.substr(first, last - first);
}

std::string UrlEncodeComponent(const std::string& s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        const bool unreserved =
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0F]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::string ResolveMotherboardSupportUrl(const std::string& mfg, const std::string& product) {
    const std::string cleanProduct = TrimWhitespace(product);
    const bool hasProduct = !cleanProduct.empty() && !IsPlaceholderSystemString(cleanProduct);
    const std::string combined = mfg + " " + cleanProduct;
    if (ContainsCaseInsensitive(combined, "ASUS")) {
        if (hasProduct) {
            return "https://www.asus.com/supportonly/" +
                   UrlEncodeComponent(cleanProduct) +
                   "/helpdesk_download/";
        }
        return "https://www.asus.com/support/download-center/";
    }
    if (ContainsCaseInsensitive(combined, "MSI") || ContainsCaseInsensitive(combined, "Micro-Star")) {
        if (hasProduct) {
            return "https://www.msi.com/search/" + UrlEncodeComponent(cleanProduct);
        }
        return "https://www.msi.com/support/download";
    }
    if (ContainsCaseInsensitive(combined, "Gigabyte")) {
        if (hasProduct) {
            return "https://www.gigabyte.com/Search?kw=" + UrlEncodeComponent(cleanProduct);
        }
        return "https://www.gigabyte.com/Support";
    }
    if (ContainsCaseInsensitive(combined, "ASRock")) {
        if (hasProduct) {
            return "https://www.asrock.com/support/index.asp?Model=" +
                   UrlEncodeComponent(cleanProduct);
        }
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
    {
        RegKeyGuard fsKey;
        if (RegOpenKeyExW(
                HKEY_LOCAL_MACHINE,
                L"SYSTEM\\CurrentControlSet\\Control\\FileSystem",
                0,
                KEY_READ,
                fsKey.Put()) == ERROR_SUCCESS) {
            DWORD disableTrim = 0;
            if (ReadRegDwordVal(fsKey.Get(), L"DisableDeleteNotify", disableTrim)) {
                r.trimEnabled = (disableTrim == 0);
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
           ContainsCaseInsensitive(d.name, "NVIDIA Broadcast") ||
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
                   !ContainsCaseInsensitive(d.infName, "hdaudbus.inf")) {
            if (d.pciVendorId == "1022" || d.pciVendorId == "8086" ||
                ContainsCaseInsensitive(d.name, "SMBus") ||
                ContainsCaseInsensitive(d.name, "Chipset") ||
                ContainsCaseInsensitive(d.name, "GPIO") ||
                ContainsCaseInsensitive(d.name, "PSP") ||
                ContainsCaseInsensitive(d.name, "Management Engine") ||
                clsLower == "scsiadapter" ||
                clsLower == "hdc") {
                if (clsLower == "scsiadapter") {
                    role = (ContainsCaseInsensitive(d.infName, "stornvme") ||
                            ContainsCaseInsensitive(d.name, "NVM"))
                               ? "NVMe контроллер"
                               : "Контроллер накопителей";
                } else if (clsLower == "hdc") {
                    role = "SATA / AHCI";
                } else {
                    role = "Чипсет / плата";
                }
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
        item.deviceName =
            (clsLower == "display" && d.gpuVramBytes >= (64ull * 1024ull * 1024ull))
                ? (d.name + "  [" + FormatBytes(d.gpuVramBytes) + "]")
                : d.name;
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
        const bool isStandardStorageOrChipsetBridge =
            (role == "SATA / AHCI" &&
             (ContainsCaseInsensitive(d.name, "AHCI") || infLower == "mshdc.inf")) ||
            (role == "NVMe контроллер" && infLower == "stornvme.inf") ||
            (role == "Чипсет / плата") || isUsbAudioOrBt;

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
        } else if (drvYear > 0 && !isMicrosoftGeneric2006 && !isStandardStorageOrChipsetBridge &&
                   (currentYear - drvYear) >= 5) {
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

std::string DecodeConsoleChunk(
    const std::string& raw,
    bool& isUtf16Mode,
    std::string& byteCarry,
    bool flush = false) {
    if (raw.empty() && byteCarry.empty()) {
        return {};
    }

    std::string data;
    data.reserve(byteCarry.size() + raw.size());
    data.append(byteCarry);
    data.append(raw);
    byteCarry.clear();

    if (!isUtf16Mode && data.size() >= 4) {
        size_t evenZeros = 0;
        size_t oddZeros = 0;
        const size_t pairs = std::min<size_t>(data.size() / 2, 64);
        for (size_t i = 0; i < pairs; ++i) {
            if (data[i * 2] == '\0') ++evenZeros;
            if (data[i * 2 + 1] == '\0') ++oddZeros;
        }
        if (pairs >= 2 && oddZeros >= pairs / 3 && evenZeros == 0) {
            isUtf16Mode = true;
        }
    }

    if (isUtf16Mode) {
        if (!flush && (data.size() % 2) != 0) {
            byteCarry.assign(1, data.back());
            data.pop_back();
        }
        const size_t wcharCount = data.size() / sizeof(wchar_t);
        if (wcharCount == 0) {
            return {};
        }
        std::wstring wide(wcharCount, L'\0');
        std::memcpy(wide.data(), data.data(), wcharCount * sizeof(wchar_t));
        return WideToUtf8(wide);
    }

    const int utf8Chars = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, data.data(), static_cast<int>(data.size()), nullptr, 0);
    if (utf8Chars > 0) {
        return data;
    }

    const int oemChars = MultiByteToWideChar(
        CP_OEMCP, 0, data.data(), static_cast<int>(data.size()), nullptr, 0);
    if (oemChars <= 0) {
        return data;
    }
    std::wstring wide(static_cast<size_t>(oemChars), L'\0');
    MultiByteToWideChar(CP_OEMCP, 0, data.data(), static_cast<int>(data.size()), wide.data(), oemChars);
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

std::string SanitizePersonalPaths(std::string text) {
    const std::string lower = ToLowerAscii(text);
    std::string out;
    out.reserve(text.size());

    size_t i = 0;
    while (i < text.size()) {
        if (i + 6 <= lower.size() &&
            lower.compare(i, 5, "users") == 0 &&
            (lower[i + 5] == '\\' || lower[i + 5] == '/')) {
            const bool boundaryBefore =
                (i == 0 || text[i - 1] == '\\' || text[i - 1] == '/' ||
                 text[i - 1] == ':' || std::isspace(static_cast<unsigned char>(text[i - 1])) != 0 ||
                 text[i - 1] == '"' || text[i - 1] == '\'' || text[i - 1] == '(' ||
                 text[i - 1] == ',' || text[i - 1] == ';');
            if (boundaryBefore) {
                const char sep = text[i + 5];
                size_t segStart = i + 6;
                size_t segEnd = segStart;
                while (segEnd < text.size()) {
                    const char c = text[segEnd];
                    if (c == '\\' || c == '/' || c == '"' || c == '\'' ||
                        c == '|' || c == ')' || c == ',' || c == ';' ||
                        std::isspace(static_cast<unsigned char>(c)) != 0) {
                        break;
                    }
                    ++segEnd;
                }
                if (segEnd > segStart) {
                    out.append(text, i, 5);
                    out.push_back(sep);
                    out.append("<user>");
                    i = segEnd;
                    continue;
                }
            }
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

struct EventLogGuard {
    HANDLE handle{nullptr};
    EventLogGuard() noexcept = default;
    explicit EventLogGuard(HANDLE h) noexcept : handle(h) {}
    ~EventLogGuard() {
        if (handle != nullptr) {
            CloseEventLog(handle);
        }
    }
    EventLogGuard(const EventLogGuard&) = delete;
    EventLogGuard& operator=(const EventLogGuard&) = delete;
};

std::string ExtractExecutableBasename(std::string rawCmd) {
    while (!rawCmd.empty() &&
           (rawCmd.back() == ' ' || rawCmd.back() == '\t' ||
            rawCmd.back() == '\r' || rawCmd.back() == '\n')) {
        rawCmd.pop_back();
    }
    const std::string lower = ToLowerAscii(rawCmd);
    const size_t exePos = lower.find(".exe");
    std::string pathPart = (exePos != std::string::npos) ? rawCmd.substr(0, exePos + 4) : rawCmd;
    const size_t slashPos = pathPart.find_last_of("\\/");
    std::string base = (slashPos != std::string::npos) ? pathPart.substr(slashPos + 1) : pathPart;
    const size_t spPos = base.find_first_of(" \t\"");
    if (spPos != std::string::npos) {
        base.resize(spPos);
    }
    return base.empty() ? "system" : base;
}

std::vector<RestorePointItem> QueryRecentRestorePoints() {
    std::vector<RestorePointItem> items;
    EventLogGuard log(OpenEventLogW(nullptr, L"Application"));
    if (log.handle == nullptr) {
        return items;
    }

    std::vector<BYTE> buf(65536);
    DWORD bytesRead = 0;
    DWORD minBytes = 0;
    constexpr size_t kMaxPoints = 25;
    constexpr DWORD kMaxEventBuf = 1024u * 1024u;

    while (items.size() < kMaxPoints) {
        bytesRead = 0;
        minBytes = 0;
        if (!ReadEventLogW(
                log.handle,
                EVENTLOG_BACKWARDS_READ | EVENTLOG_SEQUENTIAL_READ,
                0,
                buf.data(),
                static_cast<DWORD>(buf.size()),
                &bytesRead,
                &minBytes)) {
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER &&
                minBytes > buf.size() &&
                minBytes <= kMaxEventBuf) {
                buf.resize(minBytes);
                continue;
            }
            break;
        }
        if (bytesRead == 0) {
            break;
        }

        DWORD off = 0;
        while (bytesRead - off >= sizeof(EVENTLOGRECORD) && items.size() < kMaxPoints) {
            const auto* rec = reinterpret_cast<const EVENTLOGRECORD*>(buf.data() + off);
            if (rec->Length < sizeof(EVENTLOGRECORD) || rec->Length > bytesRead - off) {
                break;
            }
            const DWORD evId = rec->EventID & 0xFFFFu;
            const auto* recEnd = reinterpret_cast<const BYTE*>(rec) + rec->Length;
            const auto* srcStart = reinterpret_cast<const wchar_t*>(
                reinterpret_cast<const BYTE*>(rec) + sizeof(EVENTLOGRECORD));
            const size_t maxSrcChars =
                static_cast<size_t>(recEnd - reinterpret_cast<const BYTE*>(srcStart)) / sizeof(wchar_t);
            const size_t srcLen = wcsnlen(srcStart, maxSrcChars);
            if (evId == 8194 && srcLen > 0 && srcLen < maxSrcChars &&
                _wcsicmp(srcStart, L"System Restore") == 0 &&
                rec->NumStrings >= 2 &&
                rec->StringOffset >= sizeof(EVENTLOGRECORD) &&
                rec->StringOffset < rec->Length &&
                (rec->StringOffset % sizeof(wchar_t)) == 0) {
                const auto* strCursor = reinterpret_cast<const wchar_t*>(
                    reinterpret_cast<const BYTE*>(rec) + rec->StringOffset);
                const auto* wEnd = reinterpret_cast<const wchar_t*>(recEnd);

                std::wstring rawProcW;
                std::wstring descW;
                if (strCursor < wEnd) {
                    const size_t avail0 = static_cast<size_t>(wEnd - strCursor);
                    const size_t n0 = wcsnlen(strCursor, avail0);
                    rawProcW.assign(strCursor, n0);
                    if (n0 < avail0) {
                        strCursor += n0 + 1;
                        if (strCursor < wEnd) {
                            const size_t n1 = wcsnlen(strCursor, static_cast<size_t>(wEnd - strCursor));
                            descW.assign(strCursor, n1);
                        }
                    }
                }

                RestorePointItem rp;
                ULARGE_INTEGER uli{};
                uli.QuadPart =
                    (static_cast<ULONGLONG>(rec->TimeGenerated) + 11644473600ULL) * 10000000ULL;
                FILETIME ftUtc{uli.LowPart, uli.HighPart};
                FILETIME ftLocal{};
                SYSTEMTIME st{};
                if (FileTimeToLocalFileTime(&ftUtc, &ftLocal) &&
                    FileTimeToSystemTime(&ftLocal, &st)) {
                    char timeBuf[32];
                    std::snprintf(
                        timeBuf,
                        sizeof(timeBuf),
                        "%04u-%02u-%02u %02u:%02u:%02u",
                        st.wYear,
                        st.wMonth,
                        st.wDay,
                        st.wHour,
                        st.wMinute,
                        st.wSecond);
                    rp.timestamp = timeBuf;
                }

                const std::string rawProc = WideToUtf8(rawProcW);
                if (ContainsCaseInsensitive(rawProc, "DriverLoom")) {
                    rp.sourceProcess = "DriverLoom.exe";
                    rp.categoryLabel = "DriverLoom";
                    rp.isDriverLoom = true;
                } else if (ContainsCaseInsensitive(rawProc, "wuauserv")) {
                    rp.sourceProcess = "wuauserv (Windows Update)";
                    rp.categoryLabel = "Windows Update";
                } else if (ContainsCaseInsensitive(rawProc, "srtasks")) {
                    rp.sourceProcess = "srtasks.exe";
                    rp.categoryLabel = "Плановая точка";
                } else if (ContainsCaseInsensitive(rawProc, "msiexec")) {
                    rp.sourceProcess = "msiexec.exe";
                    rp.categoryLabel = "Windows Installer";
                } else {
                    rp.sourceProcess = ExtractExecutableBasename(rawProc);
                    rp.categoryLabel = "Системная точка";
                }

                rp.description = SanitizePersonalPaths(WideToUtf8(descW));
                while (!rp.description.empty() &&
                       (rp.description.back() == '\r' || rp.description.back() == '\n' ||
                        rp.description.back() == ' ')) {
                    rp.description.pop_back();
                }
                if (rp.description.empty()) {
                    rp.description = "Контрольная точка восстановления";
                }
                rp.sourceProcess = SanitizePersonalPaths(rp.sourceProcess);
                items.push_back(std::move(rp));
            }
            off += rec->Length;
        }
    }
    return items;
}

std::vector<StorageDiagnosticItem> AnalyzeStorageSubsystem(
    const std::vector<DeviceInfo>& devices,
    bool trimEnabled) {
    std::vector<StorageDiagnosticItem> items;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const int currentYear = static_cast<int>(now.wYear);

    for (const auto& d : devices) {
        if (IsVirtualOrSoftwareAdapter(d)) {
            continue;
        }
        const std::string clsLower = ToLowerAscii(d.deviceClass);
        const bool isPci = ContainsCaseInsensitive(d.hardwareId, "PCI\\VEN_") ||
                           ContainsCaseInsensitive(d.instanceId, "PCI\\VEN_");

        if ((clsLower == "scsiadapter" || clsLower == "hdc") && isPci) {
            StorageDiagnosticItem st;
            st.isController = true;
            st.role = (ContainsCaseInsensitive(d.infName, "stornvme") ||
                       ContainsCaseInsensitive(d.name, "NVM"))
                          ? "NVMe контроллер"
                          : "SATA / AHCI контроллер";
            st.deviceName = d.name;
            st.manufacturer = d.manufacturer;
            st.driverVersion = d.driverVersion;
            st.driverDate = d.driverDate;
            st.infName = d.infName;
            st.hardwareId = d.hardwareId;
            st.searchKey = d.searchHardwareKey;

            const std::string infLower = ToLowerAscii(d.infName);
            const bool isStandardAhciOrNvme =
                ContainsCaseInsensitive(d.name, "AHCI") ||
                infLower == "mshdc.inf" ||
                infLower == "stornvme.inf";

            const int drvYear = ParseDriverYear(d.driverDate);
            const bool isMicrosoftGeneric2006 =
                (drvYear == 2006) &&
                (ContainsCaseInsensitive(d.driverProvider, "Microsoft") ||
                 ContainsCaseInsensitive(d.manufacturer, "Стандарт") ||
                 ContainsCaseInsensitive(d.manufacturer, "Standard"));

            if (d.health != DeviceHealth::Ok) {
                st.health = ControllerHealth::Problem;
                st.statusText = d.statusText;
            } else if (drvYear > 0 && !isMicrosoftGeneric2006 && !isStandardAhciOrNvme &&
                       (currentYear - drvYear) >= 5) {
                st.health = ControllerHealth::Attention;
                st.statusText = "Устаревший драйвер (" + std::to_string(drvYear) + " г.)";
            } else {
                st.health = ControllerHealth::Ok;
                st.statusText = d.infName.empty() ? "Актуален" : ("Актуален (" + d.infName + ")");
            }
            items.push_back(std::move(st));
        } else if (clsLower == "diskdrive") {
            const bool isNvmeDisk =
                ContainsCaseInsensitive(d.hardwareId, "DiskNVMe") ||
                ContainsCaseInsensitive(d.name, "NVMe") ||
                ContainsCaseInsensitive(d.name, "990") ||
                ContainsCaseInsensitive(d.name, "980");

            StorageDiagnosticItem st;
            st.isController = false;
            st.role = isNvmeDisk ? "Накопитель (NVMe)" : "Накопитель (SATA / Диск)";
            st.deviceName = d.name;
            st.manufacturer = d.manufacturer;
            st.driverVersion = d.driverVersion;
            st.driverDate = d.driverDate;
            st.infName = d.infName;
            st.hardwareId = d.hardwareId;
            st.searchKey = d.searchHardwareKey;
            if (d.health != DeviceHealth::Ok) {
                st.health = ControllerHealth::Problem;
                st.statusText = d.statusText;
            } else {
                st.health = ControllerHealth::Ok;
                st.statusText = trimEnabled ? "Работает (TRIM активен)" : "Работает (TRIM отключён)";
            }
            items.push_back(std::move(st));
        }
    }

    std::sort(items.begin(), items.end(), [](const StorageDiagnosticItem& a, const StorageDiagnosticItem& b) {
        if (a.isController != b.isController) {
            return a.isController > b.isController;
        }
        const bool aNvme = (a.role.find("NVMe") != std::string::npos);
        const bool bNvme = (b.role.find("NVMe") != std::string::npos);
        if (aNvme != bNvme) {
            return aNvme > bNvme;
        }
        return a.deviceName < b.deviceName;
    });
    return items;
}

using PowerGetActiveSchemeFn = DWORD(WINAPI*)(HKEY, GUID**);
using PowerEnumerateFn = DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, ULONG, ULONG, UCHAR*, DWORD*);
using PowerReadFriendlyNameFn =
    DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, const GUID*, UCHAR*, DWORD*);
using PowerSetActiveSchemeFn = DWORD(WINAPI*)(HKEY, const GUID*);

std::vector<PowerSchemeItem> QueryPowerSchemes(std::string& outActiveName) {
    outActiveName.clear();
    std::vector<PowerSchemeItem> schemes;

    hdm::ModuleGuard pwr(LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    if (!pwr) {
        return schemes;
    }

    auto getActive = reinterpret_cast<PowerGetActiveSchemeFn>(
        reinterpret_cast<void*>(GetProcAddress(pwr.Get(), "PowerGetActiveScheme")));
    auto enumerate = reinterpret_cast<PowerEnumerateFn>(
        reinterpret_cast<void*>(GetProcAddress(pwr.Get(), "PowerEnumerate")));
    auto readName = reinterpret_cast<PowerReadFriendlyNameFn>(
        reinterpret_cast<void*>(GetProcAddress(pwr.Get(), "PowerReadFriendlyName")));
    if (getActive == nullptr || enumerate == nullptr || readName == nullptr) {
        return schemes;
    }

    GUID* activeGuid = nullptr;
    const bool hasActive = (getActive(nullptr, &activeGuid) == ERROR_SUCCESS && activeGuid != nullptr);
    hdm::LocalFreeGuard activeGuard(activeGuid);

    constexpr ULONG kAccessScheme = 16;
    for (ULONG idx = 0; idx < 32; ++idx) {
        GUID schemeGuid{};
        DWORD bufSize = sizeof(schemeGuid);
        if (enumerate(
                nullptr,
                nullptr,
                nullptr,
                kAccessScheme,
                idx,
                reinterpret_cast<UCHAR*>(&schemeGuid),
                &bufSize) != ERROR_SUCCESS) {
            break;
        }

        PowerSchemeItem item;
        wchar_t guidW[64]{};
        if (StringFromGUID2(schemeGuid, guidW, static_cast<int>(std::size(guidW))) > 0) {
            item.guidString = WideToUtf8(guidW);
        }

        DWORD nameBytes = 0;
        readName(nullptr, &schemeGuid, nullptr, nullptr, nullptr, &nameBytes);
        if (nameBytes > 0 && nameBytes < 4096) {
            std::vector<wchar_t> nameBuf(nameBytes / sizeof(wchar_t) + 2, L'\0');
            DWORD bufBytes = static_cast<DWORD>(nameBuf.size() * sizeof(wchar_t));
            if (readName(
                    nullptr,
                    &schemeGuid,
                    nullptr,
                    nullptr,
                    reinterpret_cast<UCHAR*>(nameBuf.data()),
                    &bufBytes) == ERROR_SUCCESS) {
                item.friendlyName = WideToUtf8(nameBuf.data());
            }
        }
        if (item.friendlyName.empty()) {
            item.friendlyName = item.guidString;
        }

        item.isActive = hasActive && IsEqualGUID(schemeGuid, *activeGuid);
        if (item.isActive) {
            outActiveName = item.friendlyName;
        }
        schemes.push_back(std::move(item));
    }

    return schemes;
}

} // namespace

SystemDiagnostics::~SystemDiagnostics() {
    stopRequested_.store(true);
    cancelRequested_.store(true);
    std::thread scanT;
    std::thread actionT;
    std::thread exportT;
    {
        std::lock_guard<std::mutex> lock(threadMutex_);
        scanT = std::move(scanThread_);
        actionT = std::move(actionThread_);
        exportT = std::move(exportThread_);
    }
    if (exportT.joinable()) {
        exportT.join();
    }
    if (actionT.joinable()) {
        actionT.join();
    }
    if (scanT.joinable()) {
        scanT.join();
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
    snap.restorePoints = QueryRecentRestorePoints();
    snap.storageItems = AnalyzeStorageSubsystem(devicesSnapshot, snap.readiness.trimEnabled);
    snap.powerSchemes = QueryPowerSchemes(snap.activePowerSchemeName);
    snap.valid = true;
    return snap;
}

void SystemDiagnostics::StartScanAsync(
    std::vector<DeviceInfo> devicesSnapshot,
    LogCallback onLog) {
    std::lock_guard<std::mutex> lock(threadMutex_);
    if (stopRequested_.load() || scanning_.exchange(true)) {
        return;
    }
    if (scanThread_.joinable()) {
        scanThread_.join();
    }

    scanThread_ = std::thread([this, devs = std::move(devicesSnapshot), log = std::move(onLog)]() {
        auto snap = CollectSynchronously(devs);
        const size_t disabledSvc = snap.disabledCriticalServices;
        const size_t attCtrl = snap.attentionControllers;
        if (!stopRequested_.load()) {
            {
                std::lock_guard<std::mutex> dataLock(dataMutex_);
                snapshot_ = std::move(snap);
            }
            generation_.fetch_add(1);
        }
        scanning_.store(false);
        if (!stopRequested_.load() && log && (disabledSvc > 0 || attCtrl > 0)) {
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
    std::lock_guard<std::mutex> lock(threadMutex_);
    if (stopRequested_.load()) {
        return;
    }
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
        auto logUacFailure = [&](const ElevatedRunResult& run, const char* opLabel) {
            if (run.launched || stopRequested_.load() || !log) {
                return;
            }
            if (run.launchError == ERROR_CANCELLED) {
                log(LogLevel::Warning,
                    std::string("Операция «") + opLabel + "» отменена в окне UAC.");
            } else {
                log(LogLevel::Error,
                    std::string("Ошибка запуска UAC (") + opLabel + "): " +
                        FormatWin32Error(run.launchError));
            }
        };

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
                std::wstring args = L"--cli-enable-service " + QuoteWin32Arg(Utf8ToWide(targetArg));
                if (preferAuto) args += L" --auto-start";
                args += L" --log " + QuoteWin32Arg(tempLog.wstring());
                const ElevatedRunResult run = ExecuteElevatedSelf(
                    args, true, &tempLog, log, nullptr, &cancelRequested_, nullptr);
                std::error_code ec;
                std::filesystem::remove(tempLog, ec);
                logUacFailure(run, "Включение службы");
                ok = run.launched && run.exitCode == 0;
            }
        } else if (action == MaintenanceAction::ResetWuDownloadCache) {
            setPhase("Очистка кэша загрузок Windows Update...");
            if (IsProcessElevated()) {
                ok = ResetWuDownloadCacheSync(log);
            } else {
                const std::filesystem::path tempLog = CreateTempLogPath(L"hdm_wuc");
                const std::wstring args =
                    L"--cli-reset-wucache --log " + QuoteWin32Arg(tempLog.wstring());
                const ElevatedRunResult run = ExecuteElevatedSelf(
                    args, true, &tempLog, log, nullptr, &cancelRequested_, nullptr);
                std::error_code ec;
                std::filesystem::remove(tempLog, ec);
                logUacFailure(run, "Очистка кэша Windows Update");
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
                                          L" --log " + QuoteWin32Arg(tempLog.wstring()) +
                                          L" --cancel-file " + QuoteWin32Arg(cancelMarker.wstring());
                const ElevatedRunResult run = ExecuteElevatedSelf(
                    args, true, &tempLog, log, nullptr, &cancelRequested_, &cancelMarker);
                std::filesystem::remove(tempLog, ec);
                std::filesystem::remove(cancelMarker, ec);
                logUacFailure(run, label.c_str());
                ok = run.launched && run.exitCode == 0;
            }
        }

        if (!stopRequested_.load()) {
            auto refreshed = CollectSynchronously(devs);
            {
                std::lock_guard<std::mutex> dataLock(dataMutex_);
                snapshot_ = std::move(refreshed);
            }
            generation_.fetch_add(1);
            setPhase(ok ? "Операция завершена" : "Операция завершена с предупреждениями");
        }
        runningAction_.store(false);
        if (!stopRequested_.load() && complete) {
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
    const UINT winLen = GetWindowsDirectoryW(winDir, MAX_PATH);
    if (winLen == 0 || winLen >= MAX_PATH) {
        if (onLog) {
            onLog(LogLevel::Error, "Не удалось определить папку Windows.");
        }
        return false;
    }
    const std::filesystem::path dlDir =
        std::filesystem::path(winDir) / L"SoftwareDistribution" / L"Download";

    std::vector<std::filesystem::path> entriesToRemove;
    std::error_code ec;
    if (std::filesystem::exists(dlDir, ec)) {
        for (std::filesystem::directory_iterator it(
                 dlDir, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::directory_iterator();
             it.increment(ec)) {
            entriesToRemove.push_back(it->path());
        }
    }

    size_t removedCount = 0;
    for (const auto& entryPath : entriesToRemove) {
        std::error_code rmEc;
        std::filesystem::remove_all(entryPath, rmEc);
        if (!rmEc) {
            ++removedCount;
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
    const UINT sysLen = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (sysLen == 0 || sysLen >= MAX_PATH) {
        if (onLog) {
            onLog(LogLevel::Error, "Не удалось определить системную папку System32.");
        }
        return false;
    }
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

    bool isUtf16Mode = false;
    std::string utf16Carry;
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
                const std::string decoded =
                    DecodeConsoleChunk(std::string(buf, readBytes), isUtf16Mode, utf16Carry, false);
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
                const std::string decoded =
                    DecodeConsoleChunk(std::string(buf, readBytes), isUtf16Mode, utf16Carry, false);
                EmitDecodedLines(carry, decoded, onLog, lastPctLine);
            }
            break;
        }
    }

    if (!utf16Carry.empty()) {
        const std::string tailDecoded = DecodeConsoleChunk({}, isUtf16Mode, utf16Carry, true);
        if (!tailDecoded.empty()) {
            EmitDecodedLines(carry, tailDecoded, onLog, lastPctLine);
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

bool SystemDiagnostics::SetActivePowerSchemeSync(
    const std::string& schemeGuidStr,
    const LogCallback& onLog) {
    if (schemeGuidStr.empty()) {
        return false;
    }
    GUID schemeGuid{};
    const HRESULT hrParse = CLSIDFromString(Utf8ToWide(schemeGuidStr).c_str(), &schemeGuid);
    if (FAILED(hrParse)) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Некорректный GUID схемы электропитания: " + schemeGuidStr);
        }
        return false;
    }

    hdm::ModuleGuard pwr(LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    PowerSetActiveSchemeFn setScheme = nullptr;
    if (pwr) {
        setScheme = reinterpret_cast<PowerSetActiveSchemeFn>(
            reinterpret_cast<void*>(GetProcAddress(pwr.Get(), "PowerSetActiveScheme")));
    }
    if (setScheme == nullptr) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Не удалось загрузить PowerSetActiveScheme из powrprof.dll.");
        }
        return false;
    }

    const DWORD rc = setScheme(nullptr, &schemeGuid);
    if (rc != ERROR_SUCCESS) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Не удалось переключить схему электропитания: " + FormatWin32Error(rc));
        }
        return false;
    }

    std::string activeName;
    (void)QueryPowerSchemes(activeName);
    if (onLog) {
        onLog(LogLevel::Success,
              "Активная схема электропитания: " +
                  (activeName.empty() ? schemeGuidStr : activeName) + ".");
    }
    return true;
}

void SystemDiagnostics::SetActivePowerSchemeAsync(
    const std::string& schemeGuidStr,
    LogCallback onLog) {
    std::lock_guard<std::mutex> lock(threadMutex_);
    if (stopRequested_.load() || IsBusy() || schemeGuidStr.empty()) {
        return;
    }
    runningAction_.store(true);
    SetActionPhase("Переключение схемы электропитания...");
    if (actionThread_.joinable()) {
        actionThread_.join();
    }

    actionThread_ = std::thread([this, schemeGuidStr, log = std::move(onLog)]() {
        (void)SetActivePowerSchemeSync(schemeGuidStr, log);
        if (!stopRequested_.load()) {
            std::string activeName;
            auto refreshedSchemes = QueryPowerSchemes(activeName);
            {
                std::lock_guard<std::mutex> dataLock(dataMutex_);
                snapshot_.powerSchemes = std::move(refreshedSchemes);
                snapshot_.activePowerSchemeName = std::move(activeName);
            }
            generation_.fetch_add(1);
            SetActionPhase("");
        }
        runningAction_.store(false);
    });
}

namespace {

std::string CleanSingleLine(std::string_view sv) {
    std::string cleaned = SanitizePersonalPaths(std::string(sv));
    for (char& ch : cleaned) {
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            ch = ' ';
        }
    }
    return cleaned;
}

std::string MdCell(std::string_view sv) {
    std::string s = CleanSingleLine(sv);
    if (s.empty()) {
        return "—";
    }
    for (char& ch : s) {
        if (ch == '|') {
            ch = '/';
        }
    }
    return s;
}

std::string TxtVal(std::string_view sv) {
    std::string s = CleanSingleLine(sv);
    return s.empty() ? "—" : s;
}

std::string JsonEscape(std::string_view sv) {
    const std::string clean = SanitizePersonalPaths(std::string(sv));
    std::string out;
    out.reserve(clean.size() + 8);
    for (unsigned char c : clean) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned>(c));
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

std::string FormatLocalTimestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[32];
    std::snprintf(
        buf,
        sizeof(buf),
        "%04u-%02u-%02u %02u:%02u:%02u",
        st.wYear,
        st.wMonth,
        st.wDay,
        st.wHour,
        st.wMinute,
        st.wSecond);
    return buf;
}

std::string FormatMotherboardTitle(const MotherboardInfo& mb) {
    std::string title;
    if (!mb.manufacturer.empty() && mb.manufacturer != "—") {
        title = mb.manufacturer;
    } else if (!mb.systemManufacturer.empty() && mb.systemManufacturer != "—") {
        title = mb.systemManufacturer;
    }
    if (!mb.product.empty() && mb.product != "—") {
        if (!title.empty()) title += " ";
        title += mb.product;
    } else if (!mb.systemProduct.empty() && mb.systemProduct != "—") {
        if (!title.empty()) title += " ";
        title += mb.systemProduct;
    }
    return title.empty()
               ? Tr("Не определена в SMBIOS", "Не визначено в SMBIOS", "Not defined in SMBIOS")
               : title;
}

std::string FormatVersionAndDate(std::string_view ver, std::string_view date) {
    std::string out = ver.empty() ? "—" : std::string(ver);
    if (!date.empty()) {
        out += " (" + std::string(date) + ")";
    }
    return out;
}

const char* ControllerHealthLabel(ControllerHealth h) noexcept {
    switch (h) {
    case ControllerHealth::Ok: return "OK";
    case ControllerHealth::Attention: return Tr("Внимание", "Увага", "Attention");
    case ControllerHealth::Problem: return Tr("Проблема", "Проблема", "Problem");
    }
    return "OK";
}

const char* ServiceHealthLabel(ServiceHealthState s) noexcept {
    switch (s) {
    case ServiceHealthState::Healthy: return "OK";
    case ServiceHealthState::Disabled: return Tr("Отключена", "Вимкнена", "Disabled");
    case ServiceHealthState::MissingOrError:
        return Tr("Ошибка / Недоступна", "Помилка / Недоступна", "Error / Unavailable");
    }
    return "OK";
}

struct ReportSummaryCounts {
    std::string generatedAt;
    std::string boardTitle;
    std::string biosSummary;
    std::vector<const DeviceInfo*> problemDevices;
    std::vector<const DeviceInfo*> monitors;
};

ReportSummaryCounts GatherReportSummary(
    const SystemDiagnosticsSnapshot& diag,
    const std::vector<DeviceInfo>& devices) {
    ReportSummaryCounts summary;
    summary.generatedAt = FormatLocalTimestamp();
    summary.boardTitle = FormatMotherboardTitle(diag.motherboard);
    summary.biosSummary =
        (diag.motherboard.biosVersion.empty() ? "—" : diag.motherboard.biosVersion) +
        " (" + (diag.motherboard.biosDate.empty() ? "—" : diag.motherboard.biosDate) + ")";
    for (const auto& d : devices) {
        if (d.health != DeviceHealth::Ok) {
            summary.problemDevices.push_back(&d);
        }
        if (d.category == DeviceCategory::Monitor) {
            summary.monitors.push_back(&d);
        }
    }
    return summary;
}

std::string BuildMarkdownReport(
    const SystemDiagnosticsSnapshot& diag,
    const std::vector<DeviceInfo>& devices,
    const std::vector<SystemComponentItem>& components,
    const ReportSummaryCounts& s) {
    std::ostringstream oss;
    oss << "# " << Tr("Отчёт о системе DriverLoom", "Звіт про систему DriverLoom", "DriverLoom System Report") << "\n\n";
    oss << "- **" << Tr("Сформирован", "Сформовано", "Generated") << "**: " << s.generatedAt << "\n";
    oss << "- **" << Tr("Примечание", "Примітка", "Note") << "**: "
        << Tr("отчёт очищен от имени пользователя и личных путей.",
              "звіт очищено від імені користувача та особистих шляхів.",
              "this report is stripped of usernames and personal file paths.")
        << "\n\n";

    oss << "## 1. "
        << Tr("Материнская плата, BIOS и готовность Windows",
              "Материнська плата, BIOS та готовність Windows",
              "Motherboard, BIOS & Windows Readiness")
        << "\n\n";
    oss << "| " << Tr("Параметр", "Параметр", "Parameter")
        << " | " << Tr("Значение", "Значення", "Value") << " |\n";
    oss << "| --- | --- |\n";
    oss << "| " << Tr("Материнская плата", "Материнська плата", "Motherboard") << " | " << MdCell(s.boardTitle) << " |\n";
    oss << "| BIOS | " << MdCell(s.biosSummary) << " |\n";
    oss << "| " << Tr("Сайт поддержки платы", "Сайт підтримки плати", "Motherboard Support URL") << " | " << MdCell(diag.motherboard.supportUrl) << " |\n";
    oss << "| " << Tr("Ожидается перезагрузка (CBS / WU)", "Очікується перезавантаження (CBS / WU)", "Reboot Pending (CBS / WU)") << " | "
        << ((diag.readiness.cbsRebootPending || diag.readiness.wuRebootRequired)
                ? Tr("Да", "Так", "Yes")
                : Tr("Нет", "Ні", "No"))
        << " |\n";
    oss << "| " << Tr("Отложенная замена файлов (PendingFileRename)", "Відкладена заміна файлів (PendingFileRename)", "Pending File Renames") << " | "
        << (diag.readiness.pendingFileRenames ? Tr("Да", "Так", "Yes") : Tr("Нет", "Ні", "No")) << " |\n";
    oss << "| " << Tr("Защита системы (точки восстановления)", "Захист системи (точки відновлення)", "System Protection (Restore Points)") << " | "
        << (diag.readiness.systemRestorePolicyDisabled
                ? Tr("Запрещена политикой (DisableSR)", "Заборонено політикою (DisableSR)", "Disabled by policy (DisableSR)")
                : (diag.readiness.systemRestoreEnabled
                       ? Tr("Активна", "Активна", "Enabled")
                       : Tr("Отключена", "Вимкнено", "Disabled")))
        << " |\n";
    oss << "| " << Tr("Всего PnP-устройств", "Усього PnP-пристроїв", "Total PnP Devices") << " | " << devices.size() << " |\n";
    oss << "| " << Tr("Мониторов", "Моніторів", "Monitors") << " | " << s.monitors.size() << " |\n";
    oss << "| " << Tr("Устройств с замечаниями", "Пристроїв із зауваженнями", "Devices with Issues") << " | " << s.problemDevices.size() << " |\n";
    oss << "| " << Tr("Отключённых критичных служб", "Вимкнених критичних служб", "Disabled Critical Services") << " | " << diag.disabledCriticalServices << " |\n";
    oss << "| " << Tr("Контроллеров с замечаниями", "Контролерів із зауваженнями", "Controllers Needing Attention") << " | " << diag.attentionControllers << " |\n\n";

    oss << "## 2. "
        << Tr("Ключевые контроллеры платы", "Ключові контролери плати", "Key Board Controllers")
        << " (" << diag.controllers.size() << ")\n\n";
    if (diag.controllers.empty()) {
        oss << "_" << Tr("Ключевые контроллеры не обнаружены.", "Ключові контролери не виявлено.", "No key controllers detected.") << "_\n\n";
    } else {
        oss << "| " << Tr("Роль", "Роль", "Role")
            << " | " << Tr("Состояние", "Стан", "Status")
            << " | " << Tr("Устройство", "Пристрій", "Device")
            << " | " << Tr("Поставщик", "Виробник", "Vendor")
            << " | " << Tr("Драйвер / Дата", "Драйвер / Дата", "Driver / Date")
            << " | INF | Hardware ID |\n";
        oss << "| --- | --- | --- | --- | --- | --- | --- |\n";
        for (const auto& c : diag.controllers) {
            oss << "| " << MdCell(TrDyn(c.role))
                << " | " << MdCell(TrDyn(c.statusText))
                << " | " << MdCell(c.deviceName)
                << " | " << MdCell(c.manufacturer)
                << " | " << MdCell(FormatVersionAndDate(c.driverVersion, c.driverDate))
                << " | " << MdCell(c.infName)
                << " | " << MdCell(c.hardwareId) << " |\n";
        }
        oss << "\n";
    }

    oss << "## 3. "
        << Tr("Устройства, требующие внимания", "Пристрої, що потребують уваги", "Devices Requiring Attention")
        << " (" << s.problemDevices.size() << ")\n\n";
    if (s.problemDevices.empty()) {
        oss << "_" << Tr("Проблемных устройств и устройств без драйверов не обнаружено.",
                         "Проблемних пристроїв і пристроїв без драйверів не виявлено.",
                         "No problematic devices or devices missing drivers detected.")
            << "_\n\n";
    } else {
        oss << "| " << Tr("Статус", "Статус", "Status")
            << " | " << Tr("Устройство", "Пристрій", "Device")
            << " | " << Tr("Класс", "Клас", "Class")
            << " | " << Tr("Поставщик", "Виробник", "Vendor")
            << " | " << Tr("Версия / Дата", "Версія / Дата", "Version / Date")
            << " | INF | Hardware ID |\n";
        oss << "| --- | --- | --- | --- | --- | --- | --- |\n";
        for (const DeviceInfo* d : s.problemDevices) {
            oss << "| " << MdCell(TrDyn(d->statusText))
                << " | " << MdCell(d->name)
                << " | " << MdCell(d->deviceClass)
                << " | " << MdCell(d->manufacturer)
                << " | " << MdCell(FormatVersionAndDate(d->driverVersion, d->driverDate))
                << " | " << MdCell(d->infName)
                << " | " << MdCell(d->hardwareId) << " |\n";
        }
        oss << "\n";
    }

    oss << "## 4. " << Tr("Мониторы (EDID)", "Монітори (EDID)", "Monitors (EDID)") << " (" << s.monitors.size() << ")\n\n";
    if (s.monitors.empty()) {
        oss << "_" << Tr("Мониторы не обнаружены.", "Монітори не виявлено.", "No monitors detected.") << "_\n\n";
    } else {
        oss << "| " << Tr("Название", "Назва", "Name")
            << " | " << Tr("Модель (EDID)", "Модель (EDID)", "Model (EDID)")
            << " | " << Tr("Режим", "Режим", "Mode")
            << " | GPU"
            << " | " << Tr("Производитель", "Виробник", "Manufacturer")
            << " | " << Tr("Драйвер / Дата", "Драйвер / Дата", "Driver / Date")
            << " | INF | " << Tr("Статус", "Статус", "Status") << " |\n";
        oss << "| --- | --- | --- | --- | --- | --- | --- | --- |\n";
        for (const DeviceInfo* m : s.monitors) {
            oss << "| " << MdCell(m->name)
                << " | " << MdCell(m->edidModel)
                << " | " << MdCell(m->displayModeText)
                << " | " << MdCell(m->connectedGpuName)
                << " | " << MdCell(m->manufacturer)
                << " | " << MdCell(FormatVersionAndDate(m->driverVersion, m->driverDate))
                << " | " << MdCell(m->infName)
                << " | " << MdCell(TrDyn(m->statusText)) << " |\n";
        }
        oss << "\n";
    }

    oss << "## 5. "
        << Tr("Системные библиотеки, компоненты и ПО периферии",
              "Системні бібліотеки, компоненти та ПЗ периферії",
              "System Runtimes, Components & Peripheral Software")
        << " (" << components.size() << ")\n\n";
    if (components.empty()) {
        oss << "_" << Tr("Список компонентов ещё не заполнен.", "Список компонентів ще не заповнено.", "Component list is not populated yet.") << "_\n\n";
    } else {
        oss << "| " << Tr("Статус", "Статус", "Status")
            << " | " << Tr("Компонент", "Компонент", "Component")
            << " | " << Tr("Устройство / Назначение", "Пристрій / Призначення", "Hardware / Purpose")
            << " | " << Tr("Установлено", "Встановлено", "Installed")
            << " | " << Tr("Актуальная", "Актуальна", "Latest") << " |\n";
        oss << "| --- | --- | --- | --- | --- |\n";
        for (const auto& c : components) {
            const std::string& hwOrDesc =
                c.detectedHardware.empty() ? c.purposeDescription : c.detectedHardware;
            oss << "| " << MdCell(TrDyn(c.statusText))
                << " | " << MdCell(c.name)
                << " | " << MdCell(TrDyn(hwOrDesc))
                << " | " << MdCell(c.installedVersion)
                << " | " << MdCell(TrDyn(c.latestVersion)) << " |\n";
        }
        oss << "\n";
    }

    oss << "## 6. "
        << Tr("Службы Windows и обновления",
              "Служби Windows та оновлення",
              "Windows & Update Services")
        << " (" << diag.services.size() << ")\n\n";
    if (diag.services.empty()) {
        oss << "_" << Tr("Сведения о службах отсутствуют.", "Відомості про служби відсутні.", "No service diagnostics available.") << "_\n";
    } else {
        oss << "| " << Tr("Служба", "Служба", "Service")
            << " | " << Tr("Название", "Назва", "Display Name")
            << " | " << Tr("Статус", "Статус", "State")
            << " | " << Tr("Тип запуска", "Тип запуску", "Startup")
            << " | " << Tr("Назначение", "Призначення", "Purpose") << " |\n";
        oss << "| --- | --- | --- | --- | --- |\n";
        for (const auto& svc : diag.services) {
            oss << "| " << MdCell(svc.serviceName)
                << " | " << MdCell(TrDyn(svc.displayName))
                << " | " << MdCell(TrDyn(svc.stateText))
                << " | " << MdCell(TrDyn(svc.startTypeText))
                << " | " << MdCell(TrDyn(svc.purpose)) << " |\n";
        }
    }

    return SanitizePersonalPaths(oss.str());
}

std::string BuildTextReport(
    const SystemDiagnosticsSnapshot& diag,
    const std::vector<DeviceInfo>& devices,
    const std::vector<SystemComponentItem>& components,
    const ReportSummaryCounts& s) {
    std::ostringstream oss;
    oss << "====================================================================\n";
    oss << Tr("DriverLoom — Отчёт о состоянии ПК и драйверов (без личных данных)",
              "DriverLoom — Звіт про стан ПК та драйверів (без особистих даних)",
              "DriverLoom — Privacy-Clean System & Driver Report")
        << "\n";
    oss << Tr("Сформирован: ", "Сформовано: ", "Generated: ") << s.generatedAt << "\n";
    oss << "====================================================================\n\n";

    oss << "[1] " << Tr("МАТЕРИНСКАЯ ПЛАТА, BIOS И ГОТОВНОСТЬ WINDOWS",
                         "МАТЕРИНСЬКА ПЛАТА, BIOS ТА ГОТОВНІСТЬ WINDOWS",
                         "MOTHERBOARD, BIOS & WINDOWS READINESS") << "\n";
    oss << "  " << Tr("Плата: ", "Плата: ", "Board: ") << TxtVal(s.boardTitle) << "\n";
    oss << "  BIOS: " << TxtVal(s.biosSummary) << "\n";
    oss << "  " << Tr("Поддержка платы: ", "Підтримка плати: ", "Support URL: ") << TxtVal(diag.motherboard.supportUrl) << "\n";
    oss << "  " << Tr("Перезагрузка Windows (CBS/WU): ", "Перезавантаження Windows (CBS/WU): ", "Reboot Pending (CBS/WU): ")
        << ((diag.readiness.cbsRebootPending || diag.readiness.wuRebootRequired)
                ? Tr("Требуется", "Потрібне", "Required")
                : Tr("Нет", "Ні", "No"))
        << "\n";
    oss << "  " << Tr("Отложенная замена файлов: ", "Відкладена заміна файлів: ", "Pending File Renames: ")
        << (diag.readiness.pendingFileRenames ? Tr("Да", "Так", "Yes") : Tr("Нет", "Ні", "No")) << "\n";
    oss << "  " << Tr("Защита системы: ", "Захист системи: ", "System Protection: ")
        << (diag.readiness.systemRestorePolicyDisabled
                ? Tr("Запрещена политикой (DisableSR)", "Заборонено політикою (DisableSR)", "Disabled by policy (DisableSR)")
                : (diag.readiness.systemRestoreEnabled
                       ? Tr("Активна", "Активна", "Enabled")
                       : Tr("Отключена", "Вимкнено", "Disabled")))
        << "\n";
    oss << "  " << Tr("Сводка: устройств=", "Зведення: пристроїв=", "Summary: devices=") << devices.size()
        << Tr(", мониторов=", ", моніторів=", ", monitors=") << s.monitors.size()
        << Tr(", проблемных=", ", проблемних=", ", issues=") << s.problemDevices.size()
        << Tr(", откл. служб=", ", вимкн. служб=", ", disabled services=") << diag.disabledCriticalServices
        << Tr(", контроллеров с замечаниями=", ", контролерів із зауваженнями=", ", attention controllers=") << diag.attentionControllers << "\n\n";

    oss << "[2] " << Tr("КЛЮЧЕВЫЕ КОНТРОЛЛЕРЫ ПЛАТЫ", "КЛЮЧОВІ КОНТРОЛЕРИ ПЛАТИ", "KEY BOARD CONTROLLERS")
        << " (" << diag.controllers.size() << ")\n";
    if (diag.controllers.empty()) {
        oss << "  (" << Tr("нет данных", "немає даних", "no data") << ")\n";
    } else {
        for (const auto& c : diag.controllers) {
            oss << "  - [" << ControllerHealthLabel(c.health) << "] " << TxtVal(TrDyn(c.role))
                << ": " << TxtVal(c.deviceName)
                << " | " << Tr("Статус: ", "Статус: ", "Status: ") << TxtVal(TrDyn(c.statusText))
                << " | " << Tr("Поставщик: ", "Виробник: ", "Vendor: ") << TxtVal(c.manufacturer)
                << " | " << Tr("Версия: ", "Версія: ", "Version: ") << TxtVal(FormatVersionAndDate(c.driverVersion, c.driverDate))
                << " | INF: " << TxtVal(c.infName)
                << " | HWID: " << TxtVal(c.hardwareId) << "\n";
        }
    }
    oss << "\n";

    oss << "[3] " << Tr("УСТРОЙСТВА, ТРЕБУЮЩИЕ ВНИМАНИЯ", "ПРИСТРОЇ, ЩО ПОТРЕБУЮТЬ УВАГИ", "DEVICES REQUIRING ATTENTION")
        << " (" << s.problemDevices.size() << ")\n";
    if (s.problemDevices.empty()) {
        oss << "  " << Tr("Проблемных устройств не обнаружено.", "Проблемних пристроїв не виявлено.", "No problematic devices detected.") << "\n";
    } else {
        for (const DeviceInfo* d : s.problemDevices) {
            oss << "  - [" << TxtVal(TrDyn(d->statusText)) << "] " << TxtVal(d->name)
                << " | " << Tr("Класс: ", "Клас: ", "Class: ") << TxtVal(d->deviceClass)
                << " | " << Tr("Поставщик: ", "Виробник: ", "Vendor: ") << TxtVal(d->manufacturer)
                << " | " << Tr("Версия: ", "Версія: ", "Version: ") << TxtVal(FormatVersionAndDate(d->driverVersion, d->driverDate))
                << " | INF: " << TxtVal(d->infName)
                << " | HWID: " << TxtVal(d->hardwareId) << "\n";
        }
    }
    oss << "\n";

    oss << "[4] " << Tr("МОНИТОРЫ (EDID)", "МОНІТОРИ (EDID)", "MONITORS (EDID)") << " (" << s.monitors.size() << ")\n";
    if (s.monitors.empty()) {
        oss << "  " << Tr("Мониторы не обнаружены.", "Монітори не виявлено.", "No monitors detected.") << "\n";
    } else {
        for (const DeviceInfo* m : s.monitors) {
            oss << "  - " << TxtVal(m->name)
                << " | EDID: " << TxtVal(m->edidModel)
                << " | " << Tr("Режим: ", "Режим: ", "Mode: ") << TxtVal(m->displayModeText)
                << " | GPU: " << TxtVal(m->connectedGpuName)
                << " | " << Tr("Производитель: ", "Виробник: ", "Manufacturer: ") << TxtVal(m->manufacturer)
                << " | " << Tr("Версия: ", "Версія: ", "Version: ") << TxtVal(FormatVersionAndDate(m->driverVersion, m->driverDate))
                << " | INF: " << TxtVal(m->infName)
                << " | " << Tr("Статус: ", "Статус: ", "Status: ") << TxtVal(TrDyn(m->statusText)) << "\n";
        }
    }
    oss << "\n";

    oss << "[5] " << Tr("СИСТЕМНЫЕ БИБЛИОТЕКИ, КОМПОНЕНТЫ И ПО ПЕРИФЕРИИ",
                         "СИСТЕМНІ БІБЛІОТЕКИ, КОМПОНЕНТИ ТА ПЗ ПЕРИФЕРІЇ",
                         "SYSTEM RUNTIMES, COMPONENTS & PERIPHERALS") << " (" << components.size() << ")\n";
    if (components.empty()) {
        oss << "  (" << Tr("нет данных", "немає даних", "no data") << ")\n";
    } else {
        for (const auto& c : components) {
            const std::string& hwOrDesc =
                c.detectedHardware.empty() ? c.purposeDescription : c.detectedHardware;
            oss << "  - [" << TxtVal(TrDyn(c.statusText)) << "] " << TxtVal(c.name)
                << " | " << Tr("Назначение: ", "Призначення: ", "Target: ") << TxtVal(TrDyn(hwOrDesc))
                << " | " << Tr("Установлено: ", "Встановлено: ", "Installed: ") << TxtVal(c.installedVersion)
                << " | " << Tr("Актуальная: ", "Актуальна: ", "Latest: ") << TxtVal(TrDyn(c.latestVersion)) << "\n";
        }
    }
    oss << "\n";

    oss << "[6] " << Tr("СЛУЖБЫ WINDOWS И ОБНОВЛЕНИЯ", "СЛУЖБИ WINDOWS ТА ОНОВЛЕННЯ", "WINDOWS & UPDATE SERVICES")
        << " (" << diag.services.size() << ")\n";
    for (const auto& svc : diag.services) {
        oss << "  - [" << ServiceHealthLabel(svc.health) << "] " << TxtVal(svc.serviceName)
            << " (" << TxtVal(TrDyn(svc.displayName)) << ")"
            << " | " << Tr("Состояние: ", "Стан: ", "State: ") << TxtVal(TrDyn(svc.stateText))
            << " | " << Tr("Запуск: ", "Запуск: ", "Startup: ") << TxtVal(TrDyn(svc.startTypeText))
            << " | " << Tr("Назначение: ", "Призначення: ", "Purpose: ") << TxtVal(TrDyn(svc.purpose)) << "\n";
    }

    return SanitizePersonalPaths(oss.str());
}

std::string BuildJsonReport(
    const SystemDiagnosticsSnapshot& diag,
    const std::vector<DeviceInfo>& devices,
    const std::vector<SystemComponentItem>& components,
    const ReportSummaryCounts& s) {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"generatedAt\": \"" << JsonEscape(s.generatedAt) << "\",\n";
    oss << "  \"privacyCleaned\": true,\n";

    oss << "  \"motherboard\": {\n";
    oss << "    \"title\": \"" << JsonEscape(s.boardTitle) << "\",\n";
    oss << "    \"manufacturer\": \"" << JsonEscape(diag.motherboard.manufacturer) << "\",\n";
    oss << "    \"product\": \"" << JsonEscape(diag.motherboard.product) << "\",\n";
    oss << "    \"biosVersion\": \"" << JsonEscape(diag.motherboard.biosVersion) << "\",\n";
    oss << "    \"biosDate\": \"" << JsonEscape(diag.motherboard.biosDate) << "\",\n";
    oss << "    \"supportUrl\": \"" << JsonEscape(diag.motherboard.supportUrl) << "\"\n";
    oss << "  },\n";

    oss << "  \"windowsReadiness\": {\n";
    oss << "    \"cbsRebootPending\": " << (diag.readiness.cbsRebootPending ? "true" : "false") << ",\n";
    oss << "    \"wuRebootRequired\": " << (diag.readiness.wuRebootRequired ? "true" : "false") << ",\n";
    oss << "    \"pendingFileRenames\": " << (diag.readiness.pendingFileRenames ? "true" : "false") << ",\n";
    oss << "    \"systemRestoreEnabled\": " << (diag.readiness.systemRestoreEnabled ? "true" : "false") << ",\n";
    oss << "    \"systemRestorePolicyDisabled\": " << (diag.readiness.systemRestorePolicyDisabled ? "true" : "false") << "\n";
    oss << "  },\n";

    oss << "  \"counts\": {\n";
    oss << "    \"totalDevices\": " << devices.size() << ",\n";
    oss << "    \"monitors\": " << s.monitors.size() << ",\n";
    oss << "    \"problemDevices\": " << s.problemDevices.size() << ",\n";
    oss << "    \"disabledCriticalServices\": " << diag.disabledCriticalServices << ",\n";
    oss << "    \"attentionControllers\": " << diag.attentionControllers << "\n";
    oss << "  },\n";

    oss << "  \"controllers\": [";
    for (size_t i = 0; i < diag.controllers.size(); ++i) {
        const auto& c = diag.controllers[i];
        oss << (i == 0 ? "\n" : ",\n")
            << "    {\n"
            << "      \"role\": \"" << JsonEscape(c.role) << "\",\n"
            << "      \"health\": \"" << ControllerHealthLabel(c.health) << "\",\n"
            << "      \"status\": \"" << JsonEscape(c.statusText) << "\",\n"
            << "      \"deviceName\": \"" << JsonEscape(c.deviceName) << "\",\n"
            << "      \"manufacturer\": \"" << JsonEscape(c.manufacturer) << "\",\n"
            << "      \"driverVersion\": \"" << JsonEscape(c.driverVersion) << "\",\n"
            << "      \"driverDate\": \"" << JsonEscape(c.driverDate) << "\",\n"
            << "      \"infName\": \"" << JsonEscape(c.infName) << "\",\n"
            << "      \"hardwareId\": \"" << JsonEscape(c.hardwareId) << "\"\n"
            << "    }";
    }
    oss << (diag.controllers.empty() ? "],\n" : "\n  ],\n");

    oss << "  \"problemDevices\": [";
    for (size_t i = 0; i < s.problemDevices.size(); ++i) {
        const DeviceInfo* d = s.problemDevices[i];
        oss << (i == 0 ? "\n" : ",\n")
            << "    {\n"
            << "      \"status\": \"" << JsonEscape(d->statusText) << "\",\n"
            << "      \"name\": \"" << JsonEscape(d->name) << "\",\n"
            << "      \"deviceClass\": \"" << JsonEscape(d->deviceClass) << "\",\n"
            << "      \"manufacturer\": \"" << JsonEscape(d->manufacturer) << "\",\n"
            << "      \"driverVersion\": \"" << JsonEscape(d->driverVersion) << "\",\n"
            << "      \"driverDate\": \"" << JsonEscape(d->driverDate) << "\",\n"
            << "      \"infName\": \"" << JsonEscape(d->infName) << "\",\n"
            << "      \"hardwareId\": \"" << JsonEscape(d->hardwareId) << "\"\n"
            << "    }";
    }
    oss << (s.problemDevices.empty() ? "],\n" : "\n  ],\n");

    oss << "  \"monitors\": [";
    for (size_t i = 0; i < s.monitors.size(); ++i) {
        const DeviceInfo* m = s.monitors[i];
        oss << (i == 0 ? "\n" : ",\n")
            << "    {\n"
            << "      \"name\": \"" << JsonEscape(m->name) << "\",\n"
            << "      \"edidModel\": \"" << JsonEscape(m->edidModel) << "\",\n"
            << "      \"edidManufacturer\": \"" << JsonEscape(m->edidManufacturer) << "\",\n"
            << "      \"displayMode\": \"" << JsonEscape(m->displayModeText) << "\",\n"
            << "      \"connectedGpu\": \"" << JsonEscape(m->connectedGpuName) << "\",\n"
            << "      \"manufacturer\": \"" << JsonEscape(m->manufacturer) << "\",\n"
            << "      \"driverVersion\": \"" << JsonEscape(m->driverVersion) << "\",\n"
            << "      \"driverDate\": \"" << JsonEscape(m->driverDate) << "\",\n"
            << "      \"infName\": \"" << JsonEscape(m->infName) << "\",\n"
            << "      \"status\": \"" << JsonEscape(m->statusText) << "\"\n"
            << "    }";
    }
    oss << (s.monitors.empty() ? "],\n" : "\n  ],\n");

    oss << "  \"components\": [";
    for (size_t i = 0; i < components.size(); ++i) {
        const auto& c = components[i];
        const std::string& hwOrDesc =
            c.detectedHardware.empty() ? c.purposeDescription : c.detectedHardware;
        oss << (i == 0 ? "\n" : ",\n")
            << "    {\n"
            << "      \"id\": \"" << JsonEscape(c.id) << "\",\n"
            << "      \"name\": \"" << JsonEscape(c.name) << "\",\n"
            << "      \"vendor\": \"" << JsonEscape(c.vendor) << "\",\n"
            << "      \"target\": \"" << JsonEscape(hwOrDesc) << "\",\n"
            << "      \"status\": \"" << JsonEscape(c.statusText) << "\",\n"
            << "      \"installed\": " << (c.installed ? "true" : "false") << ",\n"
            << "      \"updateAvailable\": " << (c.updateAvailable ? "true" : "false") << ",\n"
            << "      \"installedVersion\": \"" << JsonEscape(c.installedVersion) << "\",\n"
            << "      \"latestVersion\": \"" << JsonEscape(c.latestVersion) << "\"\n"
            << "    }";
    }
    oss << (components.empty() ? "],\n" : "\n  ],\n");

    oss << "  \"services\": [";
    for (size_t i = 0; i < diag.services.size(); ++i) {
        const auto& svc = diag.services[i];
        oss << (i == 0 ? "\n" : ",\n")
            << "    {\n"
            << "      \"serviceName\": \"" << JsonEscape(svc.serviceName) << "\",\n"
            << "      \"displayName\": \"" << JsonEscape(svc.displayName) << "\",\n"
            << "      \"health\": \"" << ServiceHealthLabel(svc.health) << "\",\n"
            << "      \"state\": \"" << JsonEscape(svc.stateText) << "\",\n"
            << "      \"startType\": \"" << JsonEscape(svc.startTypeText) << "\",\n"
            << "      \"purpose\": \"" << JsonEscape(svc.purpose) << "\"\n"
            << "    }";
    }
    oss << (diag.services.empty() ? "]\n" : "\n  ]\n");
    oss << "}\n";
    return oss.str();
}

} // namespace

std::string SystemDiagnostics::BuildCleanSystemReport(
    ReportFormat format,
    const SystemDiagnosticsSnapshot& diag,
    const std::vector<DeviceInfo>& devices,
    const std::vector<SystemComponentItem>& components) {
    const ReportSummaryCounts summary = GatherReportSummary(diag, devices);
    switch (format) {
    case ReportFormat::Markdown:
        return BuildMarkdownReport(diag, devices, components, summary);
    case ReportFormat::Text:
        return BuildTextReport(diag, devices, components, summary);
    case ReportFormat::Json:
        return BuildJsonReport(diag, devices, components, summary);
    }
    return BuildMarkdownReport(diag, devices, components, summary);
}

bool SystemDiagnostics::WriteSystemReportToFile(
    const std::filesystem::path& filePath,
    ReportFormat format,
    const SystemDiagnosticsSnapshot& diag,
    const std::vector<DeviceInfo>& devices,
    const std::vector<SystemComponentItem>& components,
    const LogCallback& onLog) {
    const std::string content =
        BuildCleanSystemReport(format, diag, devices, components);

    std::ofstream ofs(filePath, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Не удалось создать файл отчёта: " +
                      WideToUtf8(filePath.filename().wstring()));
        }
        return false;
    }

    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    ofs.flush();
    if (!ofs.good()) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Ошибка записи файла отчёта: " +
                      WideToUtf8(filePath.filename().wstring()));
        }
        return false;
    }

    if (onLog) {
        onLog(LogLevel::Success,
              "Отчёт о системе сохранён: " + WideToUtf8(filePath.filename().wstring()));
    }
    return true;
}

void SystemDiagnostics::ExportReportInteractiveAsync(
    HWND ownerWindow,
    SystemDiagnosticsSnapshot diagSnapshot,
    std::vector<DeviceInfo> devicesSnapshot,
    std::vector<SystemComponentItem> componentsSnapshot,
    LogCallback onLog) {
    std::lock_guard<std::mutex> lock(threadMutex_);
    if (stopRequested_.load() || exportingReport_.exchange(true)) {
        return;
    }
    if (exportThread_.joinable()) {
        exportThread_.join();
    }

    exportThread_ = std::thread(
        [this,
         ownerWindow,
         diag = std::move(diagSnapshot),
         devices = std::move(devicesSnapshot),
         components = std::move(componentsSnapshot),
         log = std::move(onLog)]() {
            (void)ownerWindow;
            if (stopRequested_.load()) {
                exportingReport_.store(false);
                return;
            }
            CoInitGuard comInit(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            if (!comInit.Succeeded()) {
                if (!stopRequested_.load() && log) {
                    log(LogLevel::Error,
                        "Не удалось инициализировать COM STA для сохранения отчёта: " +
                            FormatHResult(comInit.Result()));
                }
                exportingReport_.store(false);
                return;
            }

            {
                ComPtr<IFileSaveDialog> dialog;
                HRESULT hr = CoCreateInstance(
                    CLSID_FileSaveDialog,
                    nullptr,
                    CLSCTX_INPROC_SERVER,
                    IID_IFileSaveDialog,
                    reinterpret_cast<void**>(dialog.Put()));
                if (FAILED(hr) || !dialog) {
                    if (!stopRequested_.load() && log) {
                        log(LogLevel::Error,
                            "Не удалось создать диалог сохранения файла: " +
                                FormatHResult(hr));
                    }
                } else {
                    const std::wstring mdFilter = Utf8ToWide(Tr("Отчёт Markdown (*.md)", "Звіт Markdown (*.md)", "Markdown Report (*.md)"));
                    const std::wstring txtFilter = Utf8ToWide(Tr("Текстовый отчёт (*.txt)", "Текстовий звіт (*.txt)", "Text Report (*.txt)"));
                    const std::wstring jsonFilter = Utf8ToWide(Tr("Отчёт JSON (*.json)", "Звіт JSON (*.json)", "JSON Report (*.json)"));
                    const COMDLG_FILTERSPEC filters[] = {
                        {mdFilter.c_str(), L"*.md"},
                        {txtFilter.c_str(), L"*.txt"},
                        {jsonFilter.c_str(), L"*.json"},
                    };
                    dialog->SetFileTypes(3, filters);
                    dialog->SetFileTypeIndex(1);
                    dialog->SetDefaultExtension(L"md");
                    dialog->SetFileName(L"DriverLoom_Report.md");
                    const std::wstring dlgTitle = Utf8ToWide(Tr(
                        "Сохранить отчёт о системе (без личных данных)",
                        "Зберегти звіт про систему (без особистих даних)",
                        "Save System Report (Privacy Clean)"));
                    dialog->SetTitle(dlgTitle.c_str());

                    hr = dialog->Show(nullptr);
                    if (SUCCEEDED(hr) && !stopRequested_.load()) {
                        ComPtr<IShellItem> item;
                        if (SUCCEEDED(dialog->GetResult(item.Put())) && item) {
                            CoTaskMemGuard<wchar_t> rawPath;
                            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, rawPath.Put())) &&
                                rawPath.Get() != nullptr) {
                                const std::filesystem::path outPath(rawPath.Get());
                                const std::string extLower =
                                    ToLowerAscii(WideToUtf8(outPath.extension().wstring()));

                                ReportFormat fmt = ReportFormat::Markdown;
                                if (extLower == ".json") {
                                    fmt = ReportFormat::Json;
                                } else if (extLower == ".txt") {
                                    fmt = ReportFormat::Text;
                                } else if (extLower == ".md" || extLower == ".markdown") {
                                    fmt = ReportFormat::Markdown;
                                } else {
                                    UINT typeIdx = 1;
                                    if (SUCCEEDED(dialog->GetFileTypeIndex(&typeIdx))) {
                                        if (typeIdx == 2) fmt = ReportFormat::Text;
                                        else if (typeIdx == 3) fmt = ReportFormat::Json;
                                    }
                                }

                                WriteSystemReportToFile(
                                    outPath, fmt, diag, devices, components, log);
                            }
                        }
                    } else if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED) &&
                               !stopRequested_.load() && log) {
                        log(LogLevel::Warning,
                            "Ошибка диалога сохранения отчёта: " + FormatHResult(hr));
                    }
                }
            }

            exportingReport_.store(false);
        });
}

} // namespace hdm
