#include "core/DeviceScanner.hpp"

#include <cfgmgr32.h>
#include <dxgi.h>
#include <newdev.h>
#include <setupapi.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

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

    // Physical screen size in centimeters at EDID bytes 21..22 -> diagonal in inches
    int diagInches = 0;
    if (edid[21] >= 15 && edid[21] <= 200 && edid[22] >= 10 && edid[22] <= 150) {
        const double diag =
            std::hypot(static_cast<double>(edid[21]), static_cast<double>(edid[22])) / 2.54;
        diagInches = static_cast<int>(std::lround(diag));
    }
    const std::string diagSuffix =
        (diagInches > 0) ? (" (" + std::to_string(diagInches) + "\")") : std::string();

    // Preferred timing descriptor at offset 54..71 (pixel clock != 0)
    if (edid[54] != 0 || edid[55] != 0) {
        const unsigned hActive =
            static_cast<unsigned>(edid[56]) | ((static_cast<unsigned>(edid[58] & 0xF0u)) << 4);
        const unsigned vActive =
            static_cast<unsigned>(edid[59]) | ((static_cast<unsigned>(edid[61] & 0xF0u)) << 4);
        if (hActive >= 640 && hActive <= 16384 && vActive >= 480 && vActive <= 8640) {
            out.displayModeText =
                std::to_string(hActive) + "x" + std::to_string(vActive) + diagSuffix;
        }
    }
    if (out.displayModeText.empty() && diagInches > 0) {
        out.displayModeText = std::to_string(diagInches) + "\"";
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
        d.peripheralBrand + "\n" + d.infName + "\n" + d.displayModeText + "\n" +
        d.connectedGpuName;
    std::wstring wide = Utf8ToWide(combined);
    if (!wide.empty()) {
        CharLowerBuffW(wide.data(), static_cast<DWORD>(wide.size()));
    }
    return wide;
}

std::string ExtractInstanceIdFromDisplayInterface(std::string_view ifPath) {
    if (ifPath.empty()) {
        return {};
    }
    if (ifPath.rfind("\\\\?\\", 0) == 0) {
        ifPath.remove_prefix(4);
    }
    const size_t guidSep = ifPath.rfind("#{");
    if (guidSep != std::string_view::npos) {
        ifPath = ifPath.substr(0, guidSep);
    }
    std::string out(ifPath);
    for (char& ch : out) {
        if (ch == '#') {
            ch = '\\';
        }
    }
    return out;
}

void ApplyActiveDisplayMode(
    DeviceInfo& d,
    DWORD width,
    DWORD height,
    DWORD freqHz,
    const std::string& gpuName) {
    std::string diagPart;
    const size_t openParen = d.displayModeText.find('(');
    if (openParen != std::string::npos && d.displayModeText.back() == ')') {
        diagPart = " " + d.displayModeText.substr(openParen);
    } else if (!d.displayModeText.empty() && d.displayModeText.back() == '"') {
        diagPart = " (" + d.displayModeText + ")";
    }

    if (freqHz > 0) {
        d.displayModeText = std::to_string(width) + "x" + std::to_string(height) +
                            " @ " + std::to_string(freqHz) + " Hz" + diagPart;
    } else {
        d.displayModeText = std::to_string(width) + "x" + std::to_string(height) + diagPart;
    }
    if (!gpuName.empty()) {
        d.connectedGpuName = gpuName;
    }
}

void EnrichMonitorsAndGpus(std::vector<DeviceInfo>& devices) {
    CoInitGuard coInit(COINIT_MULTITHREADED);

    // 1. DXGI Dedicated Video Memory (VRAM) for GPU devices
    {
        ComPtr<IDXGIFactory1> factory;
        if (SUCCEEDED(CreateDXGIFactory1(
                __uuidof(IDXGIFactory1),
                reinterpret_cast<void**>(factory.Put()))) &&
            factory) {
            for (UINT i = 0;; ++i) {
                ComPtr<IDXGIAdapter1> adapter;
                if (factory->EnumAdapters1(i, adapter.Put()) == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                DXGI_ADAPTER_DESC1 desc{};
                if (FAILED(adapter->GetDesc1(&desc))) {
                    continue;
                }
                if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                    continue;
                }
                const uint64_t vram = static_cast<uint64_t>(desc.DedicatedVideoMemory);
                if (vram < (64ull * 1024ull * 1024ull)) {
                    continue;
                }

                const std::string adapterDescUtf8 = WideToUtf8(desc.Description);
                bool matchedByPciId = false;
                for (auto& d : devices) {
                    if (ToLowerAscii(d.deviceClass) != "display" || d.pciVendorId.empty()) {
                        continue;
                    }
                    const std::string upperHw = ToUpperAscii(d.hardwareId);
                    const std::string devHex = ExtractHex4AfterTag(upperHw, "DEV_");
                    if (devHex.empty()) {
                        continue;
                    }
                    const unsigned long pnpVen = std::strtoul(d.pciVendorId.c_str(), nullptr, 16);
                    const unsigned long pnpDev = std::strtoul(devHex.c_str(), nullptr, 16);
                    if (pnpVen == desc.VendorId && pnpDev == desc.DeviceId) {
                        if (d.gpuVramBytes == 0) {
                            d.gpuVramBytes = vram;
                        }
                        matchedByPciId = true;
                    }
                }
                if (!matchedByPciId && !adapterDescUtf8.empty()) {
                    for (auto& d : devices) {
                        if (ToLowerAscii(d.deviceClass) == "display" && d.gpuVramBytes == 0 &&
                            (ContainsCaseInsensitive(d.name, adapterDescUtf8) ||
                             ContainsCaseInsensitive(adapterDescUtf8, d.name))) {
                            d.gpuVramBytes = vram;
                            break;
                        }
                    }
                }
            }
        }
    }

    // 2. Active GDI display modes (resolution, Hz, connected GPU) for Monitor devices
    DISPLAY_DEVICEW adapterDev{};
    adapterDev.cb = sizeof(DISPLAY_DEVICEW);
    for (DWORD dispIdx = 0; EnumDisplayDevicesW(nullptr, dispIdx, &adapterDev, 0); ++dispIdx) {
        if ((adapterDev.StateFlags & DISPLAY_DEVICE_ACTIVE) != 0) {
            DEVMODEW dm{};
            dm.dmSize = sizeof(DEVMODEW);
            if (EnumDisplaySettingsExW(adapterDev.DeviceName, ENUM_CURRENT_SETTINGS, &dm, 0) &&
                dm.dmPelsWidth > 0 && dm.dmPelsHeight > 0) {
                const std::string gpuStr = WideToUtf8(adapterDev.DeviceString);
                DISPLAY_DEVICEW monDev{};
                monDev.cb = sizeof(DISPLAY_DEVICEW);
                for (DWORD monIdx = 0;
                     EnumDisplayDevicesW(
                         adapterDev.DeviceName,
                         monIdx,
                         &monDev,
                         EDD_GET_DEVICE_INTERFACE_NAME);
                     ++monIdx) {
                    if ((monDev.StateFlags & DISPLAY_DEVICE_ACTIVE) != 0) {
                        const std::string ifPath = WideToUtf8(monDev.DeviceID);
                        const std::string monInstance = ExtractInstanceIdFromDisplayInterface(ifPath);
                        const std::string upperInst = ToUpperAscii(monInstance);

                        bool matched = false;
                        if (!upperInst.empty()) {
                            for (auto& d : devices) {
                                if (d.category != DeviceCategory::Monitor || d.instanceId.empty()) {
                                    continue;
                                }
                                if (ToUpperAscii(d.instanceId) == upperInst) {
                                    ApplyActiveDisplayMode(
                                        d, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency, gpuStr);
                                    matched = true;
                                    break;
                                }
                            }
                        }
                        if (!matched && upperInst.rfind("DISPLAY\\", 0) == 0) {
                            const size_t secondSlash = upperInst.find('\\', 8);
                            const std::string hwPrefix =
                                (secondSlash != std::string::npos)
                                    ? upperInst.substr(0, secondSlash)
                                    : upperInst;
                            if (hwPrefix.size() > 8) {
                                for (auto& d : devices) {
                                    if (d.category == DeviceCategory::Monitor &&
                                        d.connectedGpuName.empty() &&
                                        ContainsCaseInsensitive(d.hardwareId, hwPrefix)) {
                                        ApplyActiveDisplayMode(
                                            d,
                                            dm.dmPelsWidth,
                                            dm.dmPelsHeight,
                                            dm.dmDisplayFrequency,
                                            gpuStr);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    std::memset(&monDev, 0, sizeof(monDev));
                    monDev.cb = sizeof(DISPLAY_DEVICEW);
                }
            }
        }
        std::memset(&adapterDev, 0, sizeof(adapterDev));
        adapterDev.cb = sizeof(DISPLAY_DEVICEW);
    }
}

bool OpenDeviceByInstanceId(
    const std::wstring& instanceId,
    DevInfoSetGuard& outSet,
    SP_DEVINFO_DATA& outData) {
    if (instanceId.empty()) {
        return false;
    }

    outSet.Reset(SetupDiCreateDeviceInfoList(nullptr, nullptr));
    if (outSet.IsValid()) {
        SP_DEVINFO_DATA data{};
        data.cbSize = sizeof(SP_DEVINFO_DATA);
        if (SetupDiOpenDeviceInfoW(
                outSet.Get(), instanceId.c_str(), nullptr, 0, &data)) {
            outData = data;
            return true;
        }
    }

    outSet.Reset(
        SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT));
    if (!outSet.IsValid()) {
        return false;
    }

    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(SP_DEVINFO_DATA);
    for (DWORD idx = 0; SetupDiEnumDeviceInfo(outSet.Get(), idx, &data); ++idx) {
        const std::wstring curId = GetDeviceInstanceIdString(outSet.Get(), &data);
        if (!curId.empty() && _wcsicmp(curId.c_str(), instanceId.c_str()) == 0) {
            outData = data;
            return true;
        }
    }

    outSet.Reset();
    return false;
}

bool CheckDevNodeNeedsReboot(HDEVINFO devInfoSet, PSP_DEVINFO_DATA devInfoData) {
    SP_DEVINSTALL_PARAMS_W params{};
    params.cbSize = sizeof(SP_DEVINSTALL_PARAMS_W);
    if (!SetupDiGetDeviceInstallParamsW(devInfoSet, devInfoData, &params)) {
        return false;
    }
    return (params.Flags & (DI_NEEDRESTART | DI_NEEDREBOOT)) != 0;
}

struct NewdevModuleGuard {
    HMODULE mod{nullptr};
    ~NewdevModuleGuard() {
        if (mod != nullptr) {
            FreeLibrary(mod);
        }
    }
};

using DiRollbackDriverFn = BOOL(WINAPI*)(HDEVINFO, PSP_DEVINFO_DATA, HWND, DWORD, PBOOL);
using DiUninstallDeviceFn = BOOL(WINAPI*)(HWND, HDEVINFO, PSP_DEVINFO_DATA, DWORD, PBOOL);

const char* DeviceActionDisplayLabel(DeviceAction action) noexcept {
    switch (action) {
    case DeviceAction::Restart: return "Перезапуск устройства";
    case DeviceAction::Enable: return "Включение устройства";
    case DeviceAction::Disable: return "Отключение устройства";
    case DeviceAction::RollbackDriver: return "Откат драйвера";
    case DeviceAction::UninstallDevice: return "Удаление устройства";
    case DeviceAction::Reenumerate: return "Опрос шины PnP";
    }
    return "Операция с устройством";
}

} // namespace

DeviceScanner::~DeviceScanner() {
    if (worker_.joinable()) {
        worker_.join();
    }
    if (actionWorker_.joinable()) {
        actionWorker_.join();
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

        list.push_back(std::move(info));
    }

    EnrichMonitorsAndGpus(list);
    for (auto& d : list) {
        d.searchIndexLower = BuildLowerSearchIndex(d);
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

std::string DeviceScanner::DeviceActionToCliVerb(DeviceAction action) {
    switch (action) {
    case DeviceAction::Restart: return "restart";
    case DeviceAction::Enable: return "enable";
    case DeviceAction::Disable: return "disable";
    case DeviceAction::RollbackDriver: return "rollback";
    case DeviceAction::UninstallDevice: return "uninstall";
    case DeviceAction::Reenumerate: return "rescan";
    }
    return {};
}

bool DeviceScanner::ParseDeviceActionCliVerb(const std::string& verb, DeviceAction& outAction) {
    const std::string lower = ToLowerAscii(verb);
    if (lower == "restart") {
        outAction = DeviceAction::Restart;
        return true;
    }
    if (lower == "enable") {
        outAction = DeviceAction::Enable;
        return true;
    }
    if (lower == "disable") {
        outAction = DeviceAction::Disable;
        return true;
    }
    if (lower == "rollback") {
        outAction = DeviceAction::RollbackDriver;
        return true;
    }
    if (lower == "uninstall") {
        outAction = DeviceAction::UninstallDevice;
        return true;
    }
    if (lower == "rescan") {
        outAction = DeviceAction::Reenumerate;
        return true;
    }
    return false;
}

bool DeviceScanner::RunDeviceActionSync(
    DeviceAction action,
    const std::string& instanceId,
    const LogCallback& onLog,
    bool* outRebootRequired) {
    if (outRebootRequired != nullptr) {
        *outRebootRequired = false;
    }

    if (action == DeviceAction::Reenumerate) {
        DEVINST rootInst = 0;
        CONFIGRET cr = CM_Locate_DevNodeW(&rootInst, nullptr, CM_LOCATE_DEVNODE_NORMAL);
        if (cr == CR_SUCCESS) {
            cr = CM_Reenumerate_DevNode(rootInst, CM_REENUMERATE_NORMAL);
        }
        if (cr != CR_SUCCESS) {
            if (onLog) {
                if (cr == CR_ACCESS_DENIED) {
                    onLog(LogLevel::Error,
                          "Ошибка опроса шины PnP: требуются права администратора (CR_ACCESS_DENIED).");
                } else {
                    char buf[96];
                    std::snprintf(
                        buf,
                        sizeof(buf),
                        "Ошибка опроса шины PnP (CM_Reenumerate_DevNode: 0x%08lX).",
                        static_cast<unsigned long>(cr));
                    onLog(LogLevel::Error, buf);
                }
            }
            return false;
        }
        if (onLog) {
            onLog(LogLevel::Success, "Повторный опрос шины PnP (CM_Reenumerate_DevNode) завершён.");
        }
        return true;
    }

    const std::wstring wInstance = Utf8ToWide(instanceId);
    if (wInstance.empty()) {
        if (onLog) {
            onLog(LogLevel::Error, "Не указан Instance ID устройства.");
        }
        return false;
    }

    DevInfoSetGuard devInfoSet;
    SP_DEVINFO_DATA devInfoData{};
    if (!OpenDeviceByInstanceId(wInstance, devInfoSet, devInfoData)) {
        if (onLog) {
            onLog(LogLevel::Error, "Устройство с Instance ID «" + instanceId + "» не найдено.");
        }
        return false;
    }

    if (action == DeviceAction::Restart ||
        action == DeviceAction::Enable ||
        action == DeviceAction::Disable) {
        SP_PROPCHANGE_PARAMS pcp{};
        pcp.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        pcp.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        pcp.Scope = DICS_FLAG_GLOBAL;
        pcp.HwProfile = 0;
        if (action == DeviceAction::Enable) {
            pcp.StateChange = DICS_ENABLE;
        } else if (action == DeviceAction::Disable) {
            pcp.StateChange = DICS_DISABLE;
        } else {
            pcp.StateChange = DICS_PROPCHANGE;
        }

        if (!SetupDiSetClassInstallParamsW(
                devInfoSet.Get(), &devInfoData, &pcp.ClassInstallHeader, sizeof(pcp)) ||
            !SetupDiCallClassInstaller(
                DIF_PROPERTYCHANGE, devInfoSet.Get(), &devInfoData)) {
            const DWORD err = GetLastError();
            if (onLog) {
                onLog(LogLevel::Error,
                      std::string(DeviceActionDisplayLabel(action)) + " (" + instanceId +
                          ") завершилось ошибкой: " + FormatWin32Error(err));
            }
            return false;
        }

        const bool rebootNeeded = CheckDevNodeNeedsReboot(devInfoSet.Get(), &devInfoData);
        if (outRebootRequired != nullptr && rebootNeeded) {
            *outRebootRequired = true;
        }
        if (onLog) {
            std::string okMsg;
            if (action == DeviceAction::Restart) {
                okMsg = "Устройство перезапущено (" + instanceId + ").";
            } else if (action == DeviceAction::Enable) {
                okMsg = "Устройство включено (" + instanceId + ").";
            } else {
                okMsg = "Устройство отключено (" + instanceId + ").";
            }
            if (rebootNeeded) {
                okMsg += " Требуется перезагрузка Windows.";
            }
            onLog(LogLevel::Success, okMsg);
        }
        return true;
    }

    if (action == DeviceAction::RollbackDriver) {
        NewdevModuleGuard newdev{
            LoadLibraryExW(L"newdev.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
        DiRollbackDriverFn diRollbackDriver = nullptr;
        if (newdev.mod != nullptr) {
            diRollbackDriver = reinterpret_cast<DiRollbackDriverFn>(
                reinterpret_cast<void*>(GetProcAddress(newdev.mod, "DiRollbackDriver")));
        }
        if (diRollbackDriver == nullptr) {
            const DWORD err = GetLastError();
            if (onLog) {
                onLog(LogLevel::Error,
                      "Не удалось загрузить DiRollbackDriver из newdev.dll: " +
                          FormatWin32Error(err));
            }
            return false;
        }

        BOOL needReboot = FALSE;
        if (!diRollbackDriver(
                devInfoSet.Get(), &devInfoData, nullptr, ROLLBACK_FLAG_NO_UI, &needReboot)) {
            const DWORD err = GetLastError();
            if (onLog) {
                if (err == ERROR_NO_MORE_ITEMS) {
                    onLog(LogLevel::Warning,
                          "Для устройства «" + instanceId +
                              "» нет сохранённой предыдущей версии драйвера для отката.");
                } else {
                    onLog(LogLevel::Error,
                          "Не удалось откатить драйвер (" + instanceId + "): " +
                              FormatWin32Error(err));
                }
            }
            return false;
        }
        const bool rebootNeeded =
            (needReboot != FALSE) || CheckDevNodeNeedsReboot(devInfoSet.Get(), &devInfoData);
        if (outRebootRequired != nullptr && rebootNeeded) {
            *outRebootRequired = true;
        }
        if (onLog) {
            onLog(LogLevel::Success,
                  "Выполнен откат драйвера для «" + instanceId + "»" +
                      (rebootNeeded ? " (требуется перезагрузка Windows)." : "."));
        }
        return true;
    }

    if (action == DeviceAction::UninstallDevice) {
        NewdevModuleGuard newdev{
            LoadLibraryExW(L"newdev.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
        DiUninstallDeviceFn diUninstallDevice = nullptr;
        if (newdev.mod != nullptr) {
            diUninstallDevice = reinterpret_cast<DiUninstallDeviceFn>(
                reinterpret_cast<void*>(GetProcAddress(newdev.mod, "DiUninstallDevice")));
        }
        if (diUninstallDevice == nullptr) {
            const DWORD err = GetLastError();
            if (onLog) {
                onLog(LogLevel::Error,
                      "Не удалось загрузить DiUninstallDevice из newdev.dll: " +
                          FormatWin32Error(err));
            }
            return false;
        }

        BOOL needReboot = FALSE;
        if (!diUninstallDevice(nullptr, devInfoSet.Get(), &devInfoData, 0, &needReboot)) {
            const DWORD err = GetLastError();
            if (onLog) {
                onLog(LogLevel::Error,
                      "Не удалось удалить устройство (" + instanceId + "): " +
                          FormatWin32Error(err));
            }
            return false;
        }
        const bool rebootNeeded =
            (needReboot != FALSE) || CheckDevNodeNeedsReboot(devInfoSet.Get(), &devInfoData);
        if (outRebootRequired != nullptr && rebootNeeded) {
            *outRebootRequired = true;
        }
        if (onLog) {
            onLog(LogLevel::Success,
                  "Устройство удалено (" + instanceId +
                      "). Для повторного обнаружения нажмите «Обновить PnP»" +
                      (rebootNeeded ? " (требуется перезагрузка)." : "."));
        }
        return true;
    }

    return false;
}

bool DeviceScanner::RestoreInboxDriverByHardwareKeySync(
    const std::string& searchKey,
    const std::string& deviceName,
    const LogCallback& onLog,
    bool* outRebootRequired) {
    if (outRebootRequired != nullptr) {
        *outRebootRequired = false;
    }
    if (searchKey.empty()) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Не указан Hardware ID для восстановления системного драйвера Windows.");
        }
        return false;
    }

    DevInfoSetGuard devInfoSet(
        SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT));
    if (!devInfoSet.IsValid()) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Ошибка SetupDiGetClassDevsW: " + FormatWin32Error(GetLastError()));
        }
        return false;
    }

    SP_DEVINFO_DATA devInfoData{};
    devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
    bool foundDevice = false;
    for (DWORD idx = 0; SetupDiEnumDeviceInfo(devInfoSet.Get(), idx, &devInfoData); ++idx) {
        const std::string hwId = WideToUtf8(
            GetDeviceRegistryPropertyString(devInfoSet.Get(), &devInfoData, SPDRP_HARDWAREID));
        const std::string instId =
            WideToUtf8(GetDeviceInstanceIdString(devInfoSet.Get(), &devInfoData));
        if (ContainsCaseInsensitive(hwId, searchKey) ||
            ContainsCaseInsensitive(instId, searchKey)) {
            foundDevice = true;
            break;
        }
    }
    if (!foundDevice) {
        if (onLog) {
            onLog(LogLevel::Warning,
                  "Устройство с ключом «" + searchKey + "» не найдено в PnP.");
        }
        return false;
    }

    if (!SetupDiBuildDriverInfoList(devInfoSet.Get(), &devInfoData, SPDIT_COMPATDRIVER)) {
        if (onLog) {
            onLog(LogLevel::Error,
                  "Не удалось построить список совместимых драйверов: " +
                      FormatWin32Error(GetLastError()));
        }
        return false;
    }

    struct CompatDriverListGuard {
        HDEVINFO set{INVALID_HANDLE_VALUE};
        PSP_DEVINFO_DATA dev{nullptr};
        ~CompatDriverListGuard() {
            if (set != INVALID_HANDLE_VALUE && dev != nullptr) {
                SetupDiDestroyDriverInfoList(set, dev, SPDIT_COMPATDRIVER);
            }
        }
    } drvListGuard{devInfoSet.Get(), &devInfoData};

    bool foundInbox = false;
    SP_DRVINFO_DATA_W bestDrvData{};
    std::string bestInfBase;

    SP_DRVINFO_DATA_W drvData{};
    drvData.cbSize = sizeof(SP_DRVINFO_DATA_W);
    for (DWORD dIdx = 0;
         SetupDiEnumDriverInfoW(
             devInfoSet.Get(), &devInfoData, SPDIT_COMPATDRIVER, dIdx, &drvData);
         ++dIdx) {
        DWORD reqBytes = 0;
        SetupDiGetDriverInfoDetailW(devInfoSet.Get(), &devInfoData, &drvData, nullptr, 0, &reqBytes);
        if (reqBytes < sizeof(SP_DRVINFO_DETAIL_DATA_W)) {
            continue;
        }
        std::vector<BYTE> detailBuf(reqBytes);
        auto* detail = reinterpret_cast<SP_DRVINFO_DETAIL_DATA_W*>(detailBuf.data());
        detail->cbSize = sizeof(SP_DRVINFO_DETAIL_DATA_W);
        if (!SetupDiGetDriverInfoDetailW(
                devInfoSet.Get(), &devInfoData, &drvData, detail, reqBytes, nullptr)) {
            continue;
        }

        const std::string infBase =
            WideToUtf8(std::filesystem::path(detail->InfFileName).filename().wstring());
        const std::string infLower = ToLowerAscii(infBase);
        if (infLower.empty() || infLower.rfind("oem", 0) == 0) {
            continue;
        }

        if (!foundInbox || drvData.DriverVersion > bestDrvData.DriverVersion) {
            foundInbox = true;
            bestDrvData = drvData;
            bestInfBase = infBase;
        }
    }

    if (!foundInbox) {
        if (onLog) {
            onLog(LogLevel::Warning,
                  "Для «" + deviceName +
                      "» не найден встроенный системный драйвер Windows в совместимых INF.");
        }
        return false;
    }

    const DWORDLONG v = bestDrvData.DriverVersion;
    char verBuf[64];
    std::snprintf(
        verBuf,
        sizeof(verBuf),
        "%u.%u.%u.%u",
        static_cast<unsigned>((v >> 48) & 0xFFFFu),
        static_cast<unsigned>((v >> 32) & 0xFFFFu),
        static_cast<unsigned>((v >> 16) & 0xFFFFu),
        static_cast<unsigned>(v & 0xFFFFu));
    const std::string inboxVer = verBuf;

    using DiInstallDeviceFn = BOOL(WINAPI*)(
        HWND, HDEVINFO, PSP_DEVINFO_DATA, PSP_DRVINFO_DATA_W, DWORD, PBOOL);
    NewdevModuleGuard newdev{
        LoadLibraryExW(L"newdev.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
    DiInstallDeviceFn diInstallDevice = nullptr;
    if (newdev.mod != nullptr) {
        diInstallDevice = reinterpret_cast<DiInstallDeviceFn>(
            reinterpret_cast<void*>(GetProcAddress(newdev.mod, "DiInstallDevice")));
    }
    if (diInstallDevice == nullptr) {
        const DWORD err = GetLastError();
        if (onLog) {
            onLog(LogLevel::Error,
                  "Не удалось загрузить DiInstallDevice из newdev.dll: " +
                      FormatWin32Error(err));
        }
        return false;
    }

    BOOL needReboot = FALSE;
    if (!diInstallDevice(nullptr, devInfoSet.Get(), &devInfoData, &bestDrvData, 0, &needReboot)) {
        const DWORD err = GetLastError();
        if (onLog) {
            onLog(LogLevel::Error,
                  "Не удалось установить системный драйвер " + bestInfBase +
                      " для «" + deviceName + "»: " + FormatWin32Error(err));
        }
        return false;
    }

    const bool rebootNeeded =
        (needReboot != FALSE) || CheckDevNodeNeedsReboot(devInfoSet.Get(), &devInfoData);
    if (outRebootRequired != nullptr && rebootNeeded) {
        *outRebootRequired = true;
    }
    if (onLog) {
        onLog(LogLevel::Success,
              "Для «" + deviceName + "» установлен актуальный системный драйвер Windows (" +
                  bestInfBase + ", версия " + inboxVer + ")" +
                  (rebootNeeded ? " — требуется перезагрузка." : "."));
    }
    return true;
}

void DeviceScanner::RunDeviceActionAsync(
    DeviceAction action,
    const std::string& instanceId,
    const std::string& deviceName,
    LogCallback onLog,
    ActionCallback onComplete) {
    if (IsBusy()) {
        return;
    }
    runningAction_.store(true);
    if (actionWorker_.joinable()) {
        actionWorker_.join();
    }

    actionWorker_ = std::thread(
        [this,
         action,
         instanceId,
         deviceName,
         log = std::move(onLog),
         cb = std::move(onComplete)]() {
            const std::string label = DeviceActionDisplayLabel(action);
            if (log) {
                log(LogLevel::Info,
                    label + (deviceName.empty() ? std::string("...") : (": " + deviceName + "...")));
            }

            bool ok = false;
            bool rebootReq = false;
            if (IsProcessElevated()) {
                ok = RunDeviceActionSync(action, instanceId, log, &rebootReq);
            } else {
                const std::filesystem::path tempLog = CreateTempLogPath(L"hdm_dev_");
                std::wstring args =
                    L"--cli-device-action " + Utf8ToWide(DeviceActionToCliVerb(action));
                if (!instanceId.empty()) {
                    args += L" --instance \"" + Utf8ToWide(instanceId) + L"\"";
                }
                args += L" --log \"" + tempLog.wstring() + L"\"";

                const ElevatedRunResult res =
                    ExecuteElevatedSelf(args, true, &tempLog, log);
                std::error_code ec;
                std::filesystem::remove(tempLog, ec);

                if (!res.launched) {
                    if (log) {
                        log(res.launchError == ERROR_CANCELLED ? LogLevel::Warning : LogLevel::Error,
                            res.launchError == ERROR_CANCELLED
                                ? "Операция с устройством отменена пользователем (UAC)."
                                : "Не удалось запустить модуль управления устройствами: " +
                                      FormatWin32Error(res.launchError));
                    }
                } else {
                    rebootReq = (res.exitCode == 2);
                    ok = (res.exitCode == 0 || res.exitCode == 2);
                }
            }

            auto refreshed = ScanSynchronously(nullptr);
            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                devices_ = std::move(refreshed);
            }
            generation_.fetch_add(1);
            runningAction_.store(false);

            if (cb) {
                cb(ok, rebootReq);
            }
        });
}

} // namespace hdm
