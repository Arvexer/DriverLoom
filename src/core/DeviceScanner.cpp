#include "core/DeviceScanner.hpp"

#include <cfgmgr32.h>
#include <setupapi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace hdm {

namespace {

bool IsHexChar(char ch) noexcept {
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'A' && ch <= 'F') ||
           (ch >= 'a' && ch <= 'f');
}

char ToUpperAsciiChar(char ch) noexcept {
    return (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch;
}

std::string ToUpperAscii(std::string_view sv) {
    std::string out(sv);
    for (char& ch : out) {
        ch = ToUpperAsciiChar(ch);
    }
    return out;
}

std::string ExtractHex4AfterTag(std::string_view upperHwId, std::string_view tag) {
    const size_t pos = upperHwId.find(tag);
    if (pos == std::string_view::npos) {
        return {};
    }
    const size_t valPos = pos + tag.size();
    if (valPos + 4 > upperHwId.size()) {
        return {};
    }
    for (size_t i = 0; i < 4; ++i) {
        if (!IsHexChar(upperHwId[valPos + i])) {
            return {};
        }
    }
    return std::string(upperHwId.substr(valPos, 4));
}

std::string ExtractVenDevOrVidPid(std::string_view hwId) {
    if (hwId.empty()) {
        return {};
    }
    const std::string upper = ToUpperAscii(hwId);
    const std::string ven = ExtractHex4AfterTag(upper, "VEN_");
    const std::string dev = ExtractHex4AfterTag(upper, "DEV_");
    if (!ven.empty() && !dev.empty()) {
        return "VEN_" + ven + "&DEV_" + dev;
    }
    const std::string vid = ExtractHex4AfterTag(upper, "VID_");
    const std::string pid = ExtractHex4AfterTag(upper, "PID_");
    if (!vid.empty() && !pid.empty()) {
        return "VID_" + vid + "&PID_" + pid;
    }
    return std::string(hwId);
}

std::wstring GetDeviceRegistryPropertyString(
    HDEVINFO devInfoSet,
    PSP_DEVINFO_DATA devInfoData,
    DWORD property) {
    DWORD regType = 0;
    DWORD requiredSize = 0;
    SetupDiGetDeviceRegistryPropertyW(
        devInfoSet, devInfoData, property, &regType, nullptr, 0, &requiredSize);
    if (requiredSize == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return {};
    }

    std::vector<wchar_t> buf(requiredSize / sizeof(wchar_t) + 2, L'\0');
    if (!SetupDiGetDeviceRegistryPropertyW(
            devInfoSet,
            devInfoData,
            property,
            &regType,
            reinterpret_cast<PBYTE>(buf.data()),
            requiredSize,
            nullptr)) {
        return {};
    }
    return std::wstring(buf.data());
}

std::wstring GetDeviceInstanceIdString(HDEVINFO devInfoSet, PSP_DEVINFO_DATA devInfoData) {
    DWORD requiredChars = 0;
    SetupDiGetDeviceInstanceIdW(devInfoSet, devInfoData, nullptr, 0, &requiredChars);
    if (requiredChars == 0) {
        return {};
    }
    std::wstring buf(requiredChars + 1, L'\0');
    if (!SetupDiGetDeviceInstanceIdW(devInfoSet, devInfoData, buf.data(), requiredChars, nullptr)) {
        return {};
    }
    return std::wstring(buf.c_str());
}

std::wstring ReadRegString(HKEY hKey, const wchar_t* valueName) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(hKey, valueName, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes == 0 || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return {};
    }
    std::wstring buf(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(hKey, valueName, nullptr, &type,
                         reinterpret_cast<LPBYTE>(buf.data()), &bytes) != ERROR_SUCCESS) {
        return {};
    }
    return std::wstring(buf.c_str());
}

void ReadDriverRegistryMetadata(const std::wstring& driverKeyRelative, DeviceInfo& out) {
    if (driverKeyRelative.empty()) {
        return;
    }
    const std::wstring fullSubKey =
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\" + driverKeyRelative;

    RegKeyGuard key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fullSubKey.c_str(), 0, KEY_READ, key.Put()) != ERROR_SUCCESS) {
        return;
    }

    out.driverVersion = WideToUtf8(ReadRegString(key.Get(), L"DriverVersion"));
    out.driverDate = WideToUtf8(ReadRegString(key.Get(), L"DriverDate"));
    out.driverProvider = WideToUtf8(ReadRegString(key.Get(), L"ProviderName"));
    out.infName = WideToUtf8(ReadRegString(key.Get(), L"InfPath"));
}

std::string MapEdidPnpCodeToBrand(std::string_view pnp) {
    if (pnp == "DEL") return "Dell";
    if (pnp == "SAM") return "Samsung";
    if (pnp == "GSM" || pnp == "LGD") return "LG";
    if (pnp == "ACI" || pnp == "AUS") return "ASUS";
    if (pnp == "MSI") return "MSI";
    if (pnp == "AOC") return "AOC";
    if (pnp == "BNQ") return "BenQ / ZOWIE";
    if (pnp == "GIG" || pnp == "GBT") return "Gigabyte";
    if (pnp == "ACR") return "Acer";
    if (pnp == "PHL") return "Philips";
    if (pnp == "VSC") return "ViewSonic";
    if (pnp == "LEN") return "Lenovo";
    if (pnp == "HPN" || pnp == "HWP") return "HP";
    if (pnp == "ENC") return "EIZO";
    if (pnp == "NEC") return "NEC";
    if (pnp == "IVM") return "iiyama";
    if (pnp == "HKC") return "HKC";
    if (pnp == "XMI") return "Xiaomi";
    if (pnp == "HWV") return "Huawei";
    if (pnp == "SNY") return "Sony";
    return std::string(pnp);
}

void ReadMonitorEdid(const std::wstring& instanceId, DeviceInfo& out) {
    if (instanceId.empty()) {
        return;
    }
    const std::wstring edidKeyPath =
        L"SYSTEM\\CurrentControlSet\\Enum\\" + instanceId + L"\\Device Parameters";
    RegKeyGuard key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, edidKeyPath.c_str(), 0, KEY_READ, key.Put()) != ERROR_SUCCESS) {
        return;
    }

    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key.Get(), L"EDID", nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < 128 || type != REG_BINARY) {
        return;
    }

    std::vector<BYTE> edid(bytes);
    if (RegQueryValueExW(key.Get(), L"EDID", nullptr, &type, edid.data(), &bytes) != ERROR_SUCCESS) {
        return;
    }

    // Verify EDID header: 00 FF FF FF FF FF FF 00
    static constexpr BYTE kEdidHeader[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    if (std::memcmp(edid.data(), kEdidHeader, sizeof(kEdidHeader)) != 0) {
        return;
    }

    // Manufacturer 3-letter PNP ID packed in big-endian 16-bit at bytes 8..9
    const uint16_t mfgWord = (static_cast<uint16_t>(edid[8]) << 8) | edid[9];
    char pnp[4]{};
    pnp[0] = static_cast<char>('A' + ((mfgWord >> 10) & 0x1F) - 1);
    pnp[1] = static_cast<char>('A' + ((mfgWord >> 5) & 0x1F) - 1);
    pnp[2] = static_cast<char>('A' + (mfgWord & 0x1F) - 1);
    pnp[3] = '\0';
    if (pnp[0] >= 'A' && pnp[0] <= 'Z' && pnp[1] >= 'A' && pnp[1] <= 'Z' &&
        pnp[2] >= 'A' && pnp[2] <= 'Z') {
        out.edidManufacturer = pnp;
    }

    // Parse 4 descriptor blocks (18 bytes each, starting at offset 54) for tag 0xFC (Monitor Name)
    for (size_t off = 54; off + 18 <= 126; off += 18) {
        if (edid[off] == 0x00 && edid[off + 1] == 0x00 &&
            edid[off + 2] == 0x00 && edid[off + 3] == 0xFC) {
            std::string model;
            for (size_t k = 5; k < 18; ++k) {
                const char ch = static_cast<char>(edid[off + k]);
                if (ch == '\n' || ch == '\r' || ch == '\0') {
                    break;
                }
                if (static_cast<unsigned char>(ch) >= 0x20 && static_cast<unsigned char>(ch) < 0x7F) {
                    model.push_back(ch);
                }
            }
            while (!model.empty() && model.back() == ' ') {
                model.pop_back();
            }
            out.edidModel = std::move(model);
            break;
        }
    }

    const std::string infLower = ToLowerAscii(out.infName);
    out.isGenericMonitorDriver = (infLower == "monitor.inf" || infLower.empty());

    const std::string brand = MapEdidPnpCodeToBrand(out.edidManufacturer);
    if (!brand.empty() && (out.manufacturer.empty() ||
                           out.manufacturer.find("Стандарт") != std::string::npos ||
                           out.manufacturer.find("Standard") != std::string::npos ||
                           out.manufacturer.find("Generic") != std::string::npos)) {
        out.manufacturer = brand;
    }
    if (!out.edidModel.empty()) {
        const std::string lowerName = ToLowerAscii(out.name);
        if (lowerName.find("pnp") != std::string::npos ||
            out.name.find("Универсальный") != std::string::npos ||
            out.name.find("Generic") != std::string::npos) {
            out.name = out.edidModel;
        }
    }
}

std::string MapUsbVendorBrand(std::string_view vidUpper, std::string_view devName) {
    if (vidUpper == "1532") return "Razer";
    if (vidUpper == "046D") return "Logitech";
    if (vidUpper == "1038") return "SteelSeries";
    if (vidUpper == "1B1C") return "Corsair";
    if (vidUpper == "0951") return "HyperX";
    if (vidUpper == "03F0") {
        if (ContainsCaseInsensitive(devName, "HyperX")) return "HyperX";
        return "HP";
    }
    if (vidUpper == "0B05") return "ASUS ROG";
    if (vidUpper == "1462") return "MSI";
    if (vidUpper == "1458") return "Gigabyte";
    if (vidUpper == "3434") return "Keychron";
    if (vidUpper == "31E3") return "Wooting";
    if (vidUpper == "258A") return "Glorious";
    if (vidUpper == "3710") return "Pulsar";
    if (vidUpper == "3554") return "Lamzu";
    if (vidUpper == "3367") return "Endgame Gear";
    if (vidUpper == "041E") return "Creative";
    if (vidUpper == "1395") return "EPOS / Sennheiser";
    if (vidUpper == "0909") return "Audio-Technica";
    if (vidUpper == "226D") return "Beyerdynamic";
    if (vidUpper == "054C") return "Sony";
    if (vidUpper == "0ECB") return "JBL";
    if (vidUpper == "3142") return "Fifine";
    if (vidUpper == "09DA") return "A4Tech / Bloody";
    if (vidUpper == "1E7D") return "Turtle Beach / ROCCAT";
    if (vidUpper == "0FD9") return "Elgato";

    if (ContainsCaseInsensitive(devName, "Razer")) return "Razer";
    if (ContainsCaseInsensitive(devName, "Logitech")) return "Logitech";
    if (ContainsCaseInsensitive(devName, "SteelSeries")) return "SteelSeries";
    if (ContainsCaseInsensitive(devName, "HyperX")) return "HyperX";
    if (ContainsCaseInsensitive(devName, "Corsair")) return "Corsair";
    if (ContainsCaseInsensitive(devName, "Wooting")) return "Wooting";
    if (ContainsCaseInsensitive(devName, "Keychron")) return "Keychron";
    return {};
}

DeviceCategory ClassifyDevice(std::string_view deviceClass) {
    const std::string cls = ToLowerAscii(deviceClass);
    if (cls == "monitor") {
        return DeviceCategory::Monitor;
    }
    if (cls == "keyboard" || cls == "mouse" || cls == "hidclass" ||
        cls == "audioendpoint" || cls == "media" || cls == "camera") {
        return DeviceCategory::Peripheral;
    }
    if (cls == "display" || cls == "system" || cls == "net" ||
        cls == "bluetooth" || cls == "usb") {
        return DeviceCategory::DisplayAndChipset;
    }
    return DeviceCategory::Other;
}

std::string DescribeProblemCode(ULONG status, ULONG problemCode, bool hasDriver) {
    if ((status & DN_HAS_PROBLEM) != 0 || problemCode != 0) {
        switch (problemCode) {
        case CM_PROB_NOT_CONFIGURED:
            return "Не настроено (Код 1)";
        case CM_PROB_FAILED_INSTALL:
            return "Нет драйвера (Код 28)";
        case CM_PROB_DISABLED:
            return "Отключено (Код 22)";
        case CM_PROB_NEED_RESTART:
            return "Требуется перезагрузка (Код 14)";
        case CM_PROB_REINSTALL:
            return "Требуется переустановка (Код 18)";
        case CM_PROB_FAILED_START:
            return "Запуск невозможен (Код 10)";
        case CM_PROB_DEVICE_NOT_THERE:
            return "Устройство отсутствует (Код 24)";
        case CM_PROB_UNSIGNED_DRIVER:
            return "Неподписанный драйвер (Код 52)";
        default: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Ошибка (Код %lu)", problemCode);
            return buf;
        }
        }
    }
    if ((status & DN_NEED_RESTART) != 0) {
        return "Требуется перезагрузка";
    }
    if (!hasDriver) {
        return "Без драйвера";
    }
    return "Работает";
}

std::wstring BuildLowerSearchIndex(const DeviceInfo& d) {
    const std::string combined =
        d.name + "\n" + d.manufacturer + "\n" + d.deviceClass + "\n" +
        d.driverVersion + "\n" + d.hardwareId + "\n" + d.edidModel + "\n" +
        d.peripheralBrand + "\n" + d.infName;
    std::wstring wide = Utf8ToWide(combined);
    if (!wide.empty()) {
        CharLowerBuffW(wide.data(), static_cast<DWORD>(wide.size()));
    }
    return wide;
}

} // namespace

DeviceScanner::~DeviceScanner() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

void DeviceScanner::StartScanAsync(LogCallback onLog) {
    if (scanning_.exchange(true)) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    worker_ = std::thread([this, log = std::move(onLog)]() {
        auto result = ScanSynchronously(log);
        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            devices_ = std::move(result);
        }
        generation_.fetch_add(1);
        scanning_.store(false);
    });
}

std::vector<DeviceInfo> DeviceScanner::GetDevicesSnapshot() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    return devices_;
}

std::vector<DeviceInfo> DeviceScanner::ScanSynchronously(const LogCallback& onLog) {
    std::vector<DeviceInfo> list;
    if (onLog) {
        onLog(LogLevel::Info, "Запуск нативного сканирования PnP-устройств и EDID (SetupAPI)...");
    }

    DevInfoSetGuard devInfoSet(
        SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT));
    if (!devInfoSet.IsValid()) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Ошибка SetupDiGetClassDevsW: " + FormatWin32Error(GetLastError()));
        }
        return list;
    }

    SP_DEVINFO_DATA devInfoData{};
    devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

    size_t problemCount = 0;
    size_t monitorCount = 0;

    for (DWORD idx = 0; SetupDiEnumDeviceInfo(devInfoSet.Get(), idx, &devInfoData); ++idx) {
        DeviceInfo info;

        std::wstring friendly =
            GetDeviceRegistryPropertyString(devInfoSet.Get(), &devInfoData, SPDRP_FRIENDLYNAME);
        std::wstring desc =
            GetDeviceRegistryPropertyString(devInfoSet.Get(), &devInfoData, SPDRP_DEVICEDESC);
        info.name = WideToUtf8(!friendly.empty() ? friendly : desc);
        if (info.name.empty()) {
            info.name = "Неизвестное устройство";
        }

        info.manufacturer = WideToUtf8(
            GetDeviceRegistryPropertyString(devInfoSet.Get(), &devInfoData, SPDRP_MFG));
        info.deviceClass = WideToUtf8(
            GetDeviceRegistryPropertyString(devInfoSet.Get(), &devInfoData, SPDRP_CLASS));
        info.hardwareId = WideToUtf8(
            GetDeviceRegistryPropertyString(devInfoSet.Get(), &devInfoData, SPDRP_HARDWAREID));
        info.searchHardwareKey = ExtractVenDevOrVidPid(info.hardwareId);

        const std::string upperHw = ToUpperAscii(info.hardwareId);
        info.usbVendorId = ExtractHex4AfterTag(upperHw, "VID_");
        info.pciVendorId = ExtractHex4AfterTag(upperHw, "VEN_");

        const std::wstring wInstanceId =
            GetDeviceInstanceIdString(devInfoSet.Get(), &devInfoData);
        info.instanceId = WideToUtf8(wInstanceId);

        const std::wstring driverKey =
            GetDeviceRegistryPropertyString(devInfoSet.Get(), &devInfoData, SPDRP_DRIVER);
        info.hasDriver = !driverKey.empty();
        ReadDriverRegistryMetadata(driverKey, info);

        info.category = ClassifyDevice(info.deviceClass);
        if (info.category == DeviceCategory::Monitor) {
            ++monitorCount;
            ReadMonitorEdid(wInstanceId, info);
        } else if (info.category == DeviceCategory::Peripheral) {
            info.peripheralBrand = MapUsbVendorBrand(info.usbVendorId, info.name);
        }

        ULONG status = 0;
        ULONG problem = 0;
        const CONFIGRET cr = CM_Get_DevNode_Status(&status, &problem, devInfoData.DevInst, 0);
        if (cr == CR_SUCCESS) {
            info.devNodeStatus = status;
            info.problemCode = problem;
        }

        const bool hasProblem = ((info.devNodeStatus & DN_HAS_PROBLEM) != 0) || (info.problemCode != 0);
        if (hasProblem) {
            if (info.problemCode == CM_PROB_DISABLED || info.problemCode == CM_PROB_NEED_RESTART) {
                info.health = DeviceHealth::Warning;
            } else {
                info.health = DeviceHealth::Error;
            }
            ++problemCount;
        } else if ((info.devNodeStatus & DN_NEED_RESTART) != 0 || !info.hasDriver) {
            info.health = DeviceHealth::Warning;
            if (!info.hasDriver) {
                ++problemCount;
            }
        } else {
            info.health = DeviceHealth::Ok;
        }

        info.statusText = DescribeProblemCode(info.devNodeStatus, info.problemCode, info.hasDriver);
        if (info.category == DeviceCategory::Monitor && info.isGenericMonitorDriver &&
            info.health == DeviceHealth::Ok) {
            info.statusText = "Базовый монитор (monitor.inf)";
        }

        info.searchIndexLower = BuildLowerSearchIndex(info);
        list.push_back(std::move(info));
    }

    std::sort(list.begin(), list.end(), [](const DeviceInfo& a, const DeviceInfo& b) {
        auto rank = [](DeviceHealth h) {
            if (h == DeviceHealth::Error) return 0;
            if (h == DeviceHealth::Warning) return 1;
            return 2;
        };
        const int ra = rank(a.health);
        const int rb = rank(b.health);
        if (ra != rb) {
            return ra < rb;
        }
        return a.name < b.name;
    });

    if (onLog) {
        onLog(LogLevel::Success,
              "Сканирование завершено: устройств — " + std::to_string(list.size()) +
                  ", мониторов — " + std::to_string(monitorCount) +
                  ", требуют внимания — " + std::to_string(problemCount) + ".");
    }
    return list;
}

} // namespace hdm
