#include "core/SystemComponentsScanner.hpp"
#include "core/DeviceScanner.hpp"
#include "core/HttpClient.hpp"
#include "core/LocalDriverInstaller.hpp"
#include "core/OnlineCatalogClient.hpp"
#include "core/PackageSecurity.hpp"
#include "core/SystemRestore.hpp"

#include <setupapi.h>
#include <winver.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <future>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

namespace hdm {

namespace {

struct InstalledAppEntry {
    std::string displayName;
    std::string displayVersion;
    std::string publisher;
};

std::wstring ReadRegStr(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes == 0 || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return {};
    }
    std::wstring buf(bytes / sizeof(wchar_t) + 2, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(buf.data()), &bytes) != ERROR_SUCCESS) {
        return {};
    }
    return std::wstring(buf.c_str());
}

bool ReadRegDword(HKEY key, const wchar_t* name, DWORD& outVal) {
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

void CollectUninstallAppsFromRoot(
    HKEY rootKey,
    const wchar_t* subKeyPath,
    REGSAM extraFlags,
    std::vector<InstalledAppEntry>& outList) {
    RegKeyGuard baseKey;
    if (RegOpenKeyExW(rootKey, subKeyPath, 0, KEY_READ | extraFlags, baseKey.Put()) != ERROR_SUCCESS) {
        return;
    }
    for (DWORD idx = 0;; ++idx) {
        wchar_t childName[256]{};
        DWORD childLen = static_cast<DWORD>(std::size(childName));
        if (RegEnumKeyExW(baseKey.Get(), idx, childName, &childLen,
                          nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            break;
        }
        RegKeyGuard itemKey;
        if (RegOpenKeyExW(baseKey.Get(), childName, 0, KEY_READ | extraFlags, itemKey.Put()) !=
            ERROR_SUCCESS) {
            continue;
        }
        const std::wstring dispName = ReadRegStr(itemKey.Get(), L"DisplayName");
        if (dispName.empty()) {
            continue;
        }
        InstalledAppEntry entry;
        entry.displayName = WideToUtf8(dispName);
        entry.displayVersion = WideToUtf8(ReadRegStr(itemKey.Get(), L"DisplayVersion"));
        entry.publisher = WideToUtf8(ReadRegStr(itemKey.Get(), L"Publisher"));
        outList.push_back(std::move(entry));
    }
}

std::vector<InstalledAppEntry> CollectAllInstalledApps() {
    std::vector<InstalledAppEntry> apps;
    apps.reserve(256);
    const wchar_t* kUninstallPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    CollectUninstallAppsFromRoot(HKEY_LOCAL_MACHINE, kUninstallPath, KEY_WOW64_64KEY, apps);
    CollectUninstallAppsFromRoot(HKEY_LOCAL_MACHINE, kUninstallPath, KEY_WOW64_32KEY, apps);
    CollectUninstallAppsFromRoot(HKEY_CURRENT_USER, kUninstallPath, 0, apps);
    return apps;
}

std::string GetFileVersionUtf8(const std::filesystem::path& filePath) {
    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec)) {
        return {};
    }
    DWORD dummy = 0;
    const std::wstring wPath = filePath.wstring();
    const DWORD size = GetFileVersionInfoSizeW(wPath.c_str(), &dummy);
    if (size == 0) {
        return {};
    }
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(wPath.c_str(), 0, size, data.data())) {
        return {};
    }
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT ffiLen = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &ffiLen) ||
        ffi == nullptr) {
        return {};
    }
    char buf[64];
    std::snprintf(
        buf,
        sizeof(buf),
        "%u.%u.%u.%u",
        HIWORD(ffi->dwFileVersionMS),
        LOWORD(ffi->dwFileVersionMS),
        HIWORD(ffi->dwFileVersionLS),
        LOWORD(ffi->dwFileVersionLS));
    return buf;
}

const InstalledAppEntry* FindInstalledApp(
    const std::vector<InstalledAppEntry>& apps,
    std::initializer_list<const char*> keywords) {
    const InstalledAppEntry* best = nullptr;
    for (const auto& app : apps) {
        bool matched = false;
        for (const char* kw : keywords) {
            if (kw != nullptr && ContainsCaseInsensitive(app.displayName, kw)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            continue;
        }
        if (best == nullptr ||
            (!app.displayVersion.empty() &&
             (best->displayVersion.empty() ||
              IsVersionNewer(app.displayVersion, best->displayVersion)))) {
            best = &app;
        }
    }
    return best;
}

std::string CheckVcRuntimeKey(const wchar_t* subKey, REGSAM wowFlags) {
    RegKeyGuard key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, KEY_READ | wowFlags, key.Put()) != ERROR_SUCCESS) {
        return {};
    }
    DWORD installed = 0;
    if (!ReadRegDword(key.Get(), L"Installed", installed) || installed == 0) {
        return {};
    }
    std::wstring ver = ReadRegStr(key.Get(), L"Version");
    if (!ver.empty() && (ver[0] == L'v' || ver[0] == L'V')) {
        ver.erase(0, 1);
    }
    if (!ver.empty()) {
        return WideToUtf8(ver);
    }
    DWORD major = 0, minor = 0, bld = 0;
    if (ReadRegDword(key.Get(), L"Major", major) &&
        ReadRegDword(key.Get(), L"Minor", minor) &&
        ReadRegDword(key.Get(), L"Bld", bld)) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%lu.%lu.%lu", major, minor, bld);
        return buf;
    }
    return "Установлен";
}

std::string FindBestVcUninstallVersion(
    const std::vector<InstalledAppEntry>& apps,
    const char* archTag) {
    std::string best;
    for (const auto& app : apps) {
        if (ContainsCaseInsensitive(app.displayName, "Microsoft Visual C++ 2015") &&
            ContainsCaseInsensitive(app.displayName, archTag) &&
            !app.displayVersion.empty()) {
            if (best.empty() || IsVersionNewer(app.displayVersion, best)) {
                best = app.displayVersion;
            }
        }
    }
    return best;
}

std::string DetectHighestDotNetDesktop(const std::string& majorPrefix) {
    wchar_t progFiles[MAX_PATH]{};
    const DWORD len = GetEnvironmentVariableW(L"ProgramFiles", progFiles, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }
    const std::filesystem::path dir =
        std::filesystem::path(progFiles) / L"dotnet" / L"shared" / L"Microsoft.WindowsDesktop.App";
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        return {};
    }
    std::string best;
    for (std::filesystem::directory_iterator it(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
        std::error_code entryEc;
        if (!it->is_directory(entryEc)) {
            continue;
        }
        const std::string ver = WideToUtf8(it->path().filename().wstring());
        if (ver.rfind(majorPrefix + ".", 0) == 0) {
            if (best.empty() || IsVersionNewer(ver, best)) {
                best = ver;
            }
        }
    }
    return best;
}

std::filesystem::path GetSystemDirPath(bool wow64) {
    wchar_t buf[MAX_PATH]{};
    const UINT len = wow64 ? GetSystemWow64DirectoryW(buf, MAX_PATH) : GetSystemDirectoryW(buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(buf);
}

std::filesystem::path GetEnvPath(const wchar_t* varName) {
    wchar_t buf[MAX_PATH]{};
    const DWORD len = GetEnvironmentVariableW(varName, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(buf);
}

std::string FormatNvidiaDriverVersion(const std::string& infDriverVer) {
    std::string digits;
    for (char ch : infDriverVer) {
        if (ch >= '0' && ch <= '9') {
            digits.push_back(ch);
        }
    }
    if (digits.size() >= 5) {
        const std::string tail = digits.substr(digits.size() - 5);
        return tail.substr(0, 3) + "." + tail.substr(3, 2);
    }
    return infDriverVer;
}

void ApplyResolvedVersionToItem(
    SystemComponentItem& item,
    const std::string& latestVer,
    const std::string& missingPrefix = "Не установлен") {
    item.onlineChecked = true;
    if (latestVer.empty()) {
        return;
    }
    item.latestVersion = latestVer;
    if (!item.installed) {
        item.updateAvailable = true;
        item.statusText = missingPrefix + " (" + latestVer + ")";
        return;
    }
    if (!item.compareVersions || item.installedVersion.empty() ||
        !std::isdigit(static_cast<unsigned char>(latestVer[0]))) {
        return;
    }
    const bool newer = (item.category == ComponentCategory::MonitorAndGpu)
                           ? IsDriverVersionNewer(latestVer, item.installedVersion)
                           : IsVersionNewer(latestVer, item.installedVersion);
    if (newer) {
        item.updateAvailable = true;
        item.statusText = "Доступно обновление (" + latestVer + ")";
    } else {
        item.updateAvailable = false;
        item.statusText = "Актуален (" + item.installedVersion + ")";
    }
}

void MergeOnlineMetadataIntoRefreshed(
    std::vector<SystemComponentItem>& refreshed,
    const std::vector<SystemComponentItem>& previous) {
    for (auto& newItem : refreshed) {
        for (const auto& oldItem : previous) {
            if (newItem.id != oldItem.id) {
                continue;
            }
            if (oldItem.onlineChecked) {
                newItem.onlineChecked = true;
                if (!oldItem.latestVersion.empty()) {
                    newItem.latestVersion = oldItem.latestVersion;
                }
                if (oldItem.latestDateYmd > 0) {
                    newItem.latestDateYmd = oldItem.latestDateYmd;
                }
                if (!oldItem.directInstallerUrl.empty()) {
                    newItem.directInstallerUrl = oldItem.directInstallerUrl;
                }
                if (!oldItem.expectedSha256.empty()) {
                    newItem.expectedSha256 = oldItem.expectedSha256;
                }
                if (!oldItem.installerArgs.empty()) {
                    newItem.installerArgs = oldItem.installerArgs;
                }
                if (!oldItem.nestedInstallerHint.empty()) {
                    newItem.nestedInstallerHint = oldItem.nestedInstallerHint;
                }
            }
            if (!newItem.installed) {
                newItem.updateAvailable = true;
                if (!newItem.latestVersion.empty() &&
                    std::isdigit(static_cast<unsigned char>(newItem.latestVersion[0]))) {
                    newItem.statusText = "Не установлен (" + newItem.latestVersion + ")";
                }
            } else if (newItem.compareVersions &&
                       !newItem.latestVersion.empty() &&
                       std::isdigit(static_cast<unsigned char>(newItem.latestVersion[0])) &&
                       !newItem.installedVersion.empty()) {
                const bool catalogIsOlderByDate =
                    (newItem.latestDateYmd > 0 && newItem.installedDateYmd > 0 &&
                     newItem.latestDateYmd < newItem.installedDateYmd);
                const bool newer = (newItem.category == ComponentCategory::MonitorAndGpu)
                                       ? IsDriverVersionNewer(newItem.latestVersion, newItem.installedVersion)
                                       : IsVersionNewer(newItem.latestVersion, newItem.installedVersion);
                if (!catalogIsOlderByDate && newer) {
                    newItem.updateAvailable = true;
                    newItem.statusText = "Доступно обновление (" + newItem.latestVersion + ")";
                } else {
                    newItem.updateAvailable = false;
                    newItem.statusText = "Актуален (" + newItem.installedVersion + ")";
                }
            } else {
                // Installed and no numeric newer version known -> mark up-to-date
                if (newItem.category != ComponentCategory::MonitorAndGpu || !newItem.updateAvailable) {
                    newItem.updateAvailable = false;
                }
            }
            break;
        }
    }
}

struct PeripheralVendorRule {
    const char* id;
    const char* brand;
    const char* softwareName;
    const char* purpose;
    const char* pageUrl;
    const char* directUrl;
    const char* wingetPath;
    const char* nestedHint;
    const char* silentArgs;
    const char* defaultLatestLabel;
    const char* uninstallKeywords[3];
};

constexpr PeripheralVendorRule kPeripheralRules[] = {
    {"sw_razer", "Razer", "Razer Synapse (Официальное ПО)",
     "Настройка мышей, клавиатур, гарнитур, веб-камер и микрофонов Razer: DPI, частота опроса, макросы, подсветка Chroma и обновление прошивки.",
     "https://www.razer.com/synapse-4",
     "https://rzr.to/synapse-4-pc-download", nullptr, nullptr, "", "Synapse 4 (онлайн)",
     {"Razer Synapse", "Razer Central", "Razer App Engine"}},
    {"sw_logitech", "Logitech", "Logitech G HUB",
     "Управление периферией Logitech G: настройка сенсора HERO, переназначение кнопок, игровые профили, эквалайзер гарнитур и обновление прошивки.",
     "https://www.logitechg.com/en-us/innovation/g-hub.html",
     "https://download01.logi.com/web/ftp/pub/techsupport/gaming/lghub_installer.exe",
     "l/Logitech/GHUB", nullptr, "--silent", "Поиск...",
     {"Logitech G HUB", "Logitech Gaming Software", "LGHUB"}},
    {"sw_steelseries", "SteelSeries", "SteelSeries GG",
     "Официальный пакет SteelSeries: пространственный звук Sonar (игры/чат/микрофон), настройка клавиатур/мышей, OLED-дисплеев и прошивки.",
     "https://steelseries.com/gg",
     "https://steelseries.com/gg/downloads/gg/latest/windows",
     "s/SteelSeries/GG", nullptr, "/S", "Поиск...",
     {"SteelSeries GG", "SteelSeries Engine", nullptr}},
    {"sw_hyperx", "HyperX", "HyperX NGENUITY",
     "Официальное ПО для периферии HyperX: настройка DPI мышей, эквалайзера/микрофона гарнитур, макросов и подсветки.",
     "https://hyperx.com/pages/ngenuity",
     nullptr, nullptr, nullptr, "", "Microsoft Store / сайт",
     {"HyperX NGENUITY", "NGENUITY", nullptr}},
    {"sw_corsair", "Corsair", "Corsair iCUE",
     "Центр управления устройствами Corsair: профили клавиатур и мышей, синхронизация подсветки, мониторинг и обновление прошивки.",
     "https://www.corsair.com/us/en/s/icue",
     "https://www3.corsair.com/software/CUE_V5/public/modules/windows/installer/Install%20iCUE.exe",
     "c/Corsair/iCUE/5", nullptr, "--quiet", "Поиск...",
     {"CORSAIR iCUE", "Corsair Utility Engine", nullptr}},
    {"sw_asus", "ASUS ROG", "ASUS Armoury Crate",
     "Фирменный софт ASUS для материнских плат и периферии: подсветка Aura Sync, управление вентиляторами и профили (по желанию).",
     "https://www.asus.com/supportonly/armoury%20crate/helpdesk_download/",
     "https://dlcdnets.asus.com/pub/ASUS/mb/14Utilities/ArmouryCrateInstallTool.zip",
     "a/Asus/ArmouryCrate", nullptr, "/SILENT", "Поиск...",
     {"Armoury Crate", "ASUS Framework", nullptr}},
    {"sw_wooting", "Wooting", "Wootility (клавиатуры Wooting)",
     "Конфигуратор аналоговых клавиатур Wooting: настройка Rapid Trigger, точки срабатывания клавиш, профилей и прошивки.",
     "https://wooting.io/wootility",
     nullptr, nullptr, nullptr, "", "Веб / Сайт Wooting",
     {"Wootility", nullptr, nullptr}},
    {"sw_keychron", "Keychron", "Keychron Launcher (веб-конфигуратор)",
     "Веб-конфигуратор клавиатур Keychron (QMK/VIA): переназначение клавиш, макросы, подсветка и обновление прошивки прямо из браузера.",
     "https://launcher.keychron.com/",
     nullptr, nullptr, nullptr, "", "Веб-конфигуратор",
     {"Keychron", nullptr, nullptr}},
    {"sw_glorious", "Glorious", "Glorious CORE",
     "Официальное ПО Glorious: настройка DPI, времени отклика (debounce), подсветки, макросов и прошивки мышей/клавиатур.",
     "https://www.gloriousgaming.com/pages/glorious-core",
     nullptr, nullptr, nullptr, "", "Сайт Glorious",
     {"Glorious CORE", nullptr, nullptr}},
    {"sw_pulsar", "Pulsar", "Pulsar Gaming Gear Software",
     "Утилита настройки мышей и клавиатур Pulsar: частота опроса до 4K/8K, DPI, LoD и обновление прошивки.",
     "https://www.pulsar.gg/pages/downloads",
     nullptr, nullptr, nullptr, "", "Сайт Pulsar",
     {"Pulsar", nullptr, nullptr}},
    {"sw_lamzu", "Lamzu", "Lamzu Software",
     "Официальная утилита Lamzu для настройки параметров сенсора, частоты опроса, макросов и обновления прошивки.",
     "https://lamzu.com/pages/download",
     nullptr, nullptr, nullptr, "", "Сайт Lamzu",
     {"Lamzu", nullptr, nullptr}},
    {"sw_endgame", "Endgame Gear", "Endgame Gear Software",
     "Конфигуратор периферии Endgame Gear: точная настройка DPI, дебаунса, Motion Sync и прошивки.",
     "https://www.endgamegear.com/en-us/downloads",
     nullptr, nullptr, nullptr, "", "Сайт Endgame Gear",
     {"Endgame Gear", nullptr, nullptr}},
    {"sw_creative", "Creative", "Creative App / Sound Blaster",
     "Управление звуковыми картами и гарнитурами Creative: эквалайзер, Acoustic Engine, Scout Mode и драйверы.",
     "https://support.creative.com/",
     nullptr, nullptr, nullptr, "", "Сайт Creative",
     {"Creative App", "Sound Blaster", nullptr}},
    {"sw_epos", "EPOS / Sennheiser", "EPOS Gaming Suite / EPOS Connect",
     "Настройка игровых гарнитур и ЦАП EPOS / Sennheiser: объемный звук 7.1, шумоподавление микрофона и прошивка.",
     "https://www.eposaudio.com/en/us/gaming/downloads",
     nullptr, "e/EPOS/EPOSConnect", nullptr, "", "Поиск...",
     {"EPOS Connect", "EPOS Gaming Suite", "Sennheiser Gaming"}},
    {"sw_bloody", "A4Tech / Bloody", "Bloody Official Software",
     "Официальное ПО для мышей и клавиатур Bloody: профили чувствительности, макросы и настройка отклика.",
     "https://www.bloody.com/en/download.php",
     nullptr, nullptr, nullptr, "", "Сайт Bloody",
     {"Bloody", nullptr, nullptr}},
    {"sw_roccat", "Turtle Beach / ROCCAT", "Turtle Beach Swarm II",
     "Единый центр управления гарнитурами, мышами и клавиатурами Turtle Beach / ROCCAT.",
     "https://www.turtlebeach.com/pages/swarm-ii",
     nullptr, nullptr, nullptr, "", "Сайт Turtle Beach",
     {"Swarm II", "ROCCAT Swarm", nullptr}},
    {"sw_elgato", "Elgato", "Elgato Stream Deck / Wave Link",
     "ПО для микрофонов Wave, контроллеров Stream Deck и карт захвата Elgato: микширование звука и профили сцен.",
     "https://www.elgato.com/us/en/s/downloads",
     nullptr, "e/Elgato/StreamDeck", nullptr, "", "Поиск...",
     {"Elgato Stream Deck", "Elgato Wave Link", nullptr}},
};

struct MonitorBrandInfo {
    std::string softwareName;
    std::string url;
    const char* uninstallKeyword;
    const char* wingetPath{nullptr};
    const char* fallbackDirectUrl{nullptr};
    const char* fallbackNestedHint{nullptr};
    const char* fallbackArgs{""};
};

MonitorBrandInfo GetMonitorBrandSupport(const std::string& pnpOrBrand, const std::string& model) {
    if (pnpOrBrand == "MSI" || ContainsCaseInsensitive(model, "MSI")) {
        return {"MSI Gaming Intelligence / Драйвер монитора",
                "https://www.msi.com/support/download",
                "Gaming Intelligence",
                "m/MSI/GamingIntelligence",
                "https://download.msi.com/uti_exe/monitor/Gaming-Intelligence-x64.zip",
                "setup.exe",
                "/S /v /qn"};
    }
    if (pnpOrBrand == "GSM" || pnpOrBrand == "LGD" || pnpOrBrand == "LG" ||
        ContainsCaseInsensitive(model, "LG")) {
        return {"LG OnScreen Control / Драйвер монитора",
                "https://www.lg.com/global/support", "OnScreen Control"};
    }
    if (pnpOrBrand == "SAM" || pnpOrBrand == "Samsung" ||
        ContainsCaseInsensitive(model, "Samsung") || ContainsCaseInsensitive(model, "SMS")) {
        return {"Samsung Easy Setting Box / Драйвер монитора",
                "https://www.samsung.com/us/support/downloads/",
                "Easy Setting Box",
                "s/Samsung/EasySettingBox",
                "https://downloadcenter.samsung.com/content/SW/202405/20240514170303450/EasySettingMSIX64_V3.0.11.msi",
                nullptr,
                ""};
    }
    if (pnpOrBrand == "ACI" || pnpOrBrand == "AUS" || pnpOrBrand == "ASUS" ||
        ContainsCaseInsensitive(model, "ASUS") || ContainsCaseInsensitive(model, "ROG")) {
        return {"ASUS DisplayWidget Center",
                "https://www.asus.com/content/asus-displaywidget-center/",
                "DisplayWidget",
                "a/Asus/DisplayWidgetCenter",
                "https://dlcdnets.asus.com/pub/ASUS/LCD%20Monitors/App/DisplayWidgetCenter/DisplayWidgetCenter.zip",
                "DisplayWidgetCenterSetup.msi",
                ""};
    }
    if (pnpOrBrand == "DEL" || pnpOrBrand == "Dell" ||
        ContainsCaseInsensitive(model, "DELL") || ContainsCaseInsensitive(model, "Alienware")) {
        return {"Dell Display Manager / Драйвер монитора",
                "https://www.dell.com/support/home",
                "Dell Display",
                "d/Dell/DisplayAndPeripheralManager",
                nullptr,
                nullptr,
                "/Silent"};
    }
    if (pnpOrBrand == "AOC" || ContainsCaseInsensitive(model, "AOC")) {
        return {"AOC G-Menu / Драйвер монитора",
                "https://aoc.com/uk/gaming/products/monitors", "G-Menu"};
    }
    if (pnpOrBrand == "BNQ" || ContainsCaseInsensitive(model, "BenQ") ||
        ContainsCaseInsensitive(model, "ZOWIE")) {
        return {"BenQ Display Pilot 2 / XL Setting to Share",
                "https://www.benq.com/en-us/support/downloads-faq.html", "Display Pilot"};
    }
    if (pnpOrBrand == "GIG" || pnpOrBrand == "GBT" || ContainsCaseInsensitive(model, "Gigabyte")) {
        return {"Gigabyte Control Center / OSD Sidekick",
                "https://www.gigabyte.com/Support", "OSD Sidekick"};
    }
    if (pnpOrBrand == "ACR" || ContainsCaseInsensitive(model, "Acer") ||
        ContainsCaseInsensitive(model, "Nitro") || ContainsCaseInsensitive(model, "Predator")) {
        return {"Acer Display Widget",
                "https://www.acer.com/us-en/support/drivers-and-manuals", "Acer Display Widget"};
    }
    if (pnpOrBrand == "PHL" || ContainsCaseInsensitive(model, "Philips")) {
        return {"Philips SmartControl / Evnia Precision Center",
                "https://www.philips.com/c-w/support-home",
                "SmartControl",
                "p/Philips/SmartControl",
                "https://gz-dlsw.tpv-tech.com/MNT/PHILIPS/files/Software/SmartControl.exe.zip",
                nullptr,
                "/S"};
    }
    if (pnpOrBrand == "VSC" || ContainsCaseInsensitive(model, "ViewSonic")) {
        return {"ViewSonic Display Portal / vDisplayManager",
                "https://www.viewsonic.com/us/support",
                "Display Portal",
                "v/ViewSonic/DisplayPortal",
                "https://www.viewsonicglobal.com/public/products_download/software/display/Display_Portal_win.zip",
                "Display_Portal_win\\setup.exe",
                ""};
    }
    return {"Официальная поддержка монитора",
            "https://www.catalog.update.microsoft.com/Search.aspx?q=" + UrlEncodeCatalogParam(model),
            nullptr};
}

} // namespace

SystemComponentsScanner::SystemComponentsScanner()
    : onlineHttpRegistry_(std::make_shared<HttpRequestRegistry>()),
      installHttpRegistry_(std::make_shared<HttpRequestRegistry>()) {}

SystemComponentsScanner::~SystemComponentsScanner() {
    stopRequested_.store(true);
    cancelInstallRequested_.store(true);
    if (onlineHttpRegistry_) {
        onlineHttpRegistry_->AbortAll();
    }
    if (installHttpRegistry_) {
        installHttpRegistry_->AbortAll();
    }
    for (;;) {
        std::thread scanT;
        std::thread onlineT;
        std::thread installT;
        {
            std::lock_guard<std::mutex> lock(threadMutex_);
            scanT = std::move(scanThread_);
            onlineT = std::move(onlineThread_);
            installT = std::move(installThread_);
        }
        if (!scanT.joinable() && !onlineT.joinable() && !installT.joinable()) {
            break;
        }
        if (installT.joinable()) installT.join();
        if (onlineT.joinable()) onlineT.join();
        if (scanT.joinable()) scanT.join();
    }
}

void SystemComponentsScanner::RequestCancelInstall() noexcept {
    cancelInstallRequested_.store(true);
    if (installHttpRegistry_) {
        installHttpRegistry_->AbortAll();
    }
}

void SystemComponentsScanner::SetPhaseMessage(const std::string& msg) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    phaseMessage_ = msg;
}

std::string SystemComponentsScanner::PhaseMessage() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    return phaseMessage_;
}

std::vector<SystemComponentItem> SystemComponentsScanner::GetItemsSnapshot() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    return items_;
}

void SystemComponentsScanner::StartLocalScanAsync(
    std::vector<DeviceInfo> devicesSnapshot,
    LogCallback onLog,
    bool autoCheckOnline) {
    std::thread oldThread;
    std::unique_lock<std::mutex> lock(threadMutex_);
    if (stopRequested_.load() || scanning_.exchange(true)) {
        return;
    }
    if (scanThread_.joinable()) {
        oldThread = std::move(scanThread_);
    }
    if (oldThread.joinable()) {
        lock.unlock();
        oldThread.join();
        lock.lock();
    }
    if (stopRequested_.load()) {
        scanning_.store(false);
        return;
    }
    SetPhaseMessage("Анализ библиотек Windows, мониторов и периферии...");

    scanThread_ = std::thread([this,
                               devs = std::move(devicesSnapshot),
                               log = std::move(onLog),
                               autoCheckOnline]() {
        auto list = ScanLocalSynchronously(devs);
        if (!autoCheckOnline) {
            for (auto& it : list) {
                if (it.latestVersion == "Поиск...") {
                    it.latestVersion = "По запросу";
                }
            }
        }
        if (!stopRequested_.load()) {
            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                items_ = std::move(list);
            }
            generation_.fetch_add(1);
            SetPhaseMessage("Локальная проверка завершена");
        }
        if (!stopRequested_.load() && log) {
            log(LogLevel::Success,
                "Проверка игровых библиотек, мониторов и ПО периферии завершена.");
        }
        if (autoCheckOnline && !stopRequested_.load()) {
            StartOnlineVersionCheckAsync(log);
        }
        scanning_.store(false);
    });
}

void SystemComponentsScanner::RefreshLocalPreserveOnlineAsync(
    std::vector<DeviceInfo> devicesSnapshot,
    bool rescanDevices) {
    std::thread oldThread;
    std::unique_lock<std::mutex> lock(threadMutex_);
    if (stopRequested_.load() || scanning_.exchange(true)) {
        return;
    }
    if (scanThread_.joinable()) {
        oldThread = std::move(scanThread_);
    }
    if (oldThread.joinable()) {
        lock.unlock();
        oldThread.join();
        lock.lock();
    }
    if (stopRequested_.load()) {
        scanning_.store(false);
        return;
    }

    scanThread_ = std::thread([this, devs = std::move(devicesSnapshot), rescanDevices]() {
        const auto actualDevs =
            (rescanDevices || devs.empty()) ? DeviceScanner::ScanSynchronously() : devs;
        auto refreshed = ScanLocalSynchronously(actualDevs);
        if (!stopRequested_.load()) {
            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                if (!items_.empty()) {
                    MergeOnlineMetadataIntoRefreshed(refreshed, items_);
                }
                items_ = std::move(refreshed);
            }
            generation_.fetch_add(1);
        }
        scanning_.store(false);
    });
}

void SystemComponentsScanner::StartOnlineVersionCheckAsync(LogCallback onLog) {
    std::thread oldThread;
    std::unique_lock<std::mutex> lock(threadMutex_);
    if (stopRequested_.load() || installing_.load() || checkingOnline_.exchange(true)) {
        return;
    }
    if (onlineThread_.joinable()) {
        oldThread = std::move(onlineThread_);
    }
    if (oldThread.joinable()) {
        lock.unlock();
        oldThread.join();
        lock.lock();
    }
    if (stopRequested_.load()) {
        checkingOnline_.store(false);
        return;
    }
    if (onlineHttpRegistry_ && !stopRequested_.load()) {
        onlineHttpRegistry_->Reset();
    }

    SetPhaseMessage("Поиск актуальных версий...");
    onlineThread_ = std::thread([this, log = std::move(onLog)]() {
        ScopedHttpRegistry onlineScope(onlineHttpRegistry_.get());
        auto updateItem = [&](const std::string& compId, auto&& fn) {
            std::lock_guard<std::mutex> lock(dataMutex_);
            for (auto& item : items_) {
                if (item.id == compId) {
                    fn(item);
                    break;
                }
            }
        };

        auto hasItem = [&](const std::string& compId) {
            std::lock_guard<std::mutex> lock(dataMutex_);
            for (const auto& it : items_) {
                if (it.id == compId) return true;
            }
            return false;
        };

        // Snapshot distinct (wingetRepoPath, preferredArch) and MS Catalog lookups needed right now.
        std::vector<std::pair<std::string, std::string>> wingetTargets;
        std::vector<std::pair<std::string, std::string>> catalogTargets;
        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            std::set<std::pair<std::string, std::string>> seen;
            std::set<std::pair<std::string, std::string>> seenCatalog;
            for (const auto& it : items_) {
                if (!it.wingetRepoPath.empty() && it.id != "sw_steelseries") {
                    const std::pair<std::string, std::string> key{
                        it.wingetRepoPath, it.preferredArch};
                    if (seen.insert(key).second) {
                        wingetTargets.push_back(key);
                    }
                }
                if (!it.catalogSearchKey.empty()) {
                    const std::pair<std::string, std::string> catPair{
                        it.catalogSearchKey, it.catalogFallbackKey};
                    if (seenCatalog.insert(catPair).second) {
                        catalogTargets.push_back(catPair);
                    }
                }
            }
        }

        // Group 1: .NET Desktop Runtime 8.0, 9.0 & 10.0
        auto fDotnet = std::async(std::launch::async, [&]() {
            ScopedHttpRegistry onlineScope(onlineHttpRegistry_.get());
            const auto checkDotnet = [&](const std::string& compId, const std::wstring& metaUrl) {
                const HttpResponse resp = WinHttpRequest(metaUrl, L"GET", true, 4 * 1024 * 1024);
                if (!resp.ok || resp.truncated || resp.body.empty()) {
                    updateItem(compId, [](SystemComponentItem& item) {
                        item.latestVersion = "Недоступно";
                        item.onlineChecked = true;
                    });
                    return;
                }
                const std::string latest = ExtractJsonStringField(resp.body, "latest-runtime");
                bool validDotnetVer = !latest.empty() &&
                                      std::isdigit(static_cast<unsigned char>(latest.front())) != 0 &&
                                      std::isdigit(static_cast<unsigned char>(latest.back())) != 0 &&
                                      latest.find('.') != std::string::npos &&
                                      latest.find("..") == std::string::npos;
                for (unsigned char ch : latest) {
                    const bool ok = std::isdigit(ch) || ch == '.';
                    if (!ok) {
                        validDotnetVer = false;
                        break;
                    }
                }
                if (!validDotnetVer) {
                    return;
                }
                const std::string dlUrl =
                    "https://builds.dotnet.microsoft.com/dotnet/WindowsDesktop/" + latest +
                    "/windowsdesktop-runtime-" + latest + "-win-x64.exe";

                updateItem(compId, [&](SystemComponentItem& item) {
                    item.directInstallerUrl = dlUrl;
                    item.installerArgs = "/install /passive /norestart";
                    ApplyResolvedVersionToItem(item, latest);
                });
            };
            checkDotnet("rt_dotnet8",
                        L"https://dotnetcli.blob.core.windows.net/dotnet/release-metadata/8.0/releases.json");
            checkDotnet("rt_dotnet9",
                        L"https://dotnetcli.blob.core.windows.net/dotnet/release-metadata/9.0/releases.json");
            checkDotnet("rt_dotnet10",
                        L"https://dotnetcli.blob.core.windows.net/dotnet/release-metadata/10.0/releases.json");
        });

        // Group 2: Microsoft Edge WebView2 + LunarG Vulkan Runtime
        auto fRuntimes = std::async(std::launch::async, [&]() {
            ScopedHttpRegistry onlineScope(onlineHttpRegistry_.get());
            {
                const HttpResponse resp = WinHttpRequest(
                    L"https://edgeupdates.microsoft.com/api/products?view=enterprise",
                    L"GET",
                    true,
                    2 * 1024 * 1024);
                std::string latestWv2;
                if (resp.ok && !resp.truncated && !resp.body.empty()) {
                    const size_t stablePos = resp.body.find("\"Product\":\"Stable\"");
                    if (stablePos != std::string::npos) {
                        latestWv2 = ExtractJsonStringField(resp.body, "ProductVersion", stablePos);
                    }
                }
                updateItem("rt_webview2", [&](SystemComponentItem& item) {
                    if (!latestWv2.empty()) {
                        ApplyResolvedVersionToItem(item, latestWv2);
                    } else {
                        item.onlineChecked = true;
                        item.latestVersion = "Онлайн-установщик";
                    }
                });
            }

            {
                const HttpResponse resp = WinHttpRequest(
                    L"https://vulkan.lunarg.com/sdk/files", L"GET", true, 2 * 1024 * 1024);
                const auto [vkLatest, vkUrl] =
                    (resp.ok && !resp.truncated)
                        ? ParseLatestVulkanRuntimeFromJson(resp.body)
                        : std::pair<std::string, std::string>{};
                updateItem("rt_vulkan", [&](SystemComponentItem& item) {
                    if (!vkLatest.empty() && !vkUrl.empty() &&
                        IsAllowedHttpsHost(vkUrl, {"lunarg.com"})) {
                        item.directInstallerUrl = vkUrl;
                        item.expectedSha256.clear();
                        item.installerArgs = "";
                        ApplyResolvedVersionToItem(item, vkLatest, "Не найден");
                    } else {
                        item.onlineChecked = true;
                        item.latestVersion = "Сайт LunarG";
                    }
                });
            }
        });

        // Group 3: SteelSeries CDN + Razer verification + NVIDIA/AMD GPU checks
        auto fVendors = std::async(std::launch::async, [&]() {
            ScopedHttpRegistry onlineScope(onlineHttpRegistry_.get());
            if (hasItem("sw_steelseries")) {
                const HttpResponse resp = WinHttpRequest(
                    L"https://steelseries.com/gg/downloads/gg/latest/windows", L"HEAD", false);
                std::string ver;
                std::string url =
                    IsAllowedHttpsHost(resp.locationHeader, {"steelseries.com", "steelseriescdn.com"})
                        ? resp.locationHeader
                        : std::string{};
                std::string sha256;
                if (!url.empty()) {
                    const size_t ggPos = url.find("SteelSeriesGG");
                    if (ggPos != std::string::npos) {
                        const size_t numStart = ggPos + 13;
                        const size_t setupPos = url.find("Setup", numStart);
                        if (setupPos != std::string::npos && setupPos > numStart) {
                            ver = url.substr(numStart, setupPos - numStart);
                        }
                    }
                }
                if (ver.empty()) {
                    const WingetManifestInfo wm = ResolveWingetPkgManifest("s/SteelSeries/GG", "x64");
                    if (wm.ok) {
                        ver = wm.version;
                        if (url.empty()) {
                            url = wm.url;
                            sha256 = wm.sha256;
                        } else if (wm.url == url) {
                            sha256 = wm.sha256;
                        }
                    }
                }
                updateItem("sw_steelseries", [&](SystemComponentItem& item) {
                    if (!url.empty()) {
                        item.directInstallerUrl = url;
                        item.expectedSha256 = sha256;
                    }
                    if (!ver.empty()) {
                        ApplyResolvedVersionToItem(item, ver, "Не установлено");
                    } else {
                        item.onlineChecked = true;
                        if (item.latestVersion == "Поиск...") item.latestVersion = "Онлайн";
                    }
                });
            }

            if (hasItem("sw_razer")) {
                const HttpResponse rzHead = WinHttpRequest(
                    L"https://rzr.to/synapse-4-pc-download", L"HEAD", true);
                updateItem("sw_razer", [&](SystemComponentItem& item) {
                    item.onlineChecked = true;
                    item.latestVersion = rzHead.ok ? "Synapse 4 (актуальный)" : "Сайт Razer";
                });
            }

            if (hasItem("gpu_nvidia")) {
                const HttpResponse resp = WinHttpRequest(
                    L"https://gfwsl.geforce.com/services_toolkit/services/com/nvidia/services/AjaxDriverService.php?func=DriverManualLookup&psid=127&pfid=985&osID=57&languageCode=1033&isWHQL=1&dch=1&upcrd=0&numberOfResults=1",
                    L"GET",
                    true,
                    64 * 1024);
                if (resp.ok && !resp.truncated && !resp.body.empty()) {
                    const std::string nvVer = ExtractJsonStringField(resp.body, "Version");
                    const std::string nvUrl = ExtractJsonStringField(resp.body, "DownloadURL");
                    updateItem("gpu_nvidia", [&](SystemComponentItem& item) {
                        if (!nvUrl.empty() && IsAllowedHttpsHost(nvUrl, {"nvidia.com"})) {
                            item.directInstallerUrl = nvUrl;
                            item.expectedSha256.clear();
                            item.installerArgs = "";
                        }
                        if (!nvVer.empty()) {
                            ApplyResolvedVersionToItem(item, nvVer, "Не найден");
                        } else {
                            item.onlineChecked = true;
                        }
                    });
                }
            }

            if (hasItem("gpu_amd")) {
                const HttpResponse resp = WinHttpRequest(
                    L"https://www.amd.com/en/support/download/drivers.html",
                    L"GET",
                    true,
                    512 * 1024);
                const auto [amdVer, amdUrl] =
                    (resp.ok && !resp.truncated) ? ParseAmdAdrenalinFromHtml(resp.body)
                                                 : std::pair<std::string, std::string>{};
                updateItem("gpu_amd", [&](SystemComponentItem& item) {
                    item.onlineChecked = true;
                    if (!amdUrl.empty() && IsAllowedHttpsHost(amdUrl, {"amd.com"})) {
                        item.directInstallerUrl = amdUrl;
                        item.expectedSha256.clear();
                        item.installerArgs = "";
                    }
                    if (!amdVer.empty()) {
                        ApplyResolvedVersionToItem(item, amdVer, "Доступен");
                    } else {
                        item.latestVersion = "Портал AMD";
                    }
                });
            }

            if (hasItem("chip_amd")) {
                const HttpResponse resp = WinHttpRequest(
                    L"https://www.amd.com/en/support/downloads/drivers.html/chipsets/am4/b550.html",
                    L"GET",
                    true,
                    512 * 1024);
                const auto [chipVer, chipUrl] =
                    (resp.ok && !resp.truncated) ? ParseAmdChipsetFromHtml(resp.body)
                                                 : std::pair<std::string, std::string>{};
                updateItem("chip_amd", [&](SystemComponentItem& item) {
                    item.onlineChecked = true;
                    if (!chipUrl.empty() && IsAllowedHttpsHost(chipUrl, {"amd.com"})) {
                        item.directInstallerUrl = chipUrl;
                        item.expectedSha256.clear();
                    }
                    if (!chipVer.empty()) {
                        ApplyResolvedVersionToItem(item, chipVer, "Не установлен");
                    } else if (!item.directInstallerUrl.empty()) {
                        ApplyResolvedVersionToItem(item, "8.09.28.1124", "Не установлен");
                    }
                });
            }
        });

        // Group 4: Parallel official winget-pkgs manifest lookups (VC++ 2015-2022,
        // ASUS DisplayWidget Center, Armoury Crate, Logitech G HUB, Corsair iCUE,
        // Dell Display Manager, MSI Gaming Intelligence, Samsung Easy Setting Box,
        // Philips SmartControl, ViewSonic DisplayPortal, Elgato, EPOS, Intel DSA).
        auto fWinget = std::async(std::launch::async, [&]() {
            const size_t n = wingetTargets.size();
            if (n == 0 || stopRequested_.load()) {
                return;
            }
            std::vector<WingetManifestInfo> resolved(n);
            const size_t kWorkers = std::min<size_t>(4, n);
            std::atomic<size_t> nextIdx{0};
            std::vector<std::thread> pool;
            pool.reserve(kWorkers);
            for (size_t w = 0; w < kWorkers; ++w) {
                pool.emplace_back([&]() {
                    ScopedHttpRegistry onlineScope(onlineHttpRegistry_.get());
                    for (;;) {
                        if (stopRequested_.load()) break;
                        const size_t idx = nextIdx.fetch_add(1);
                        if (idx >= n) break;
                        resolved[idx] = ResolveWingetPkgManifest(
                            wingetTargets[idx].first, wingetTargets[idx].second);
                    }
                });
            }
            for (auto& t : pool) t.join();
            if (stopRequested_.load()) {
                return;
            }

            std::lock_guard<std::mutex> lock(dataMutex_);
            for (auto& item : items_) {
                if (item.wingetRepoPath.empty() || item.id == "sw_steelseries") {
                    continue;
                }
                for (size_t idx = 0; idx < n; ++idx) {
                    if (wingetTargets[idx].first == item.wingetRepoPath &&
                        wingetTargets[idx].second == item.preferredArch) {
                        const auto& wm = resolved[idx];
                        item.onlineChecked = true;
                        if (wm.ok) {
                            if (!wm.url.empty()) {
                                item.directInstallerUrl = wm.url;
                                item.expectedSha256 = wm.sha256;
                            }
                            if (!wm.nestedRelativePath.empty()) {
                                item.nestedInstallerHint = wm.nestedRelativePath;
                            }
                            if (!wm.silentArgs.empty() && item.installerArgs.empty()) {
                                item.installerArgs = wm.silentArgs;
                            }
                            ApplyResolvedVersionToItem(item, wm.version);
                        } else if (item.latestVersion == "Поиск..." ||
                                   item.latestVersion == "Портал поддержки") {
                            if (item.id == "rt_vcredist_x64" || item.id == "rt_vcredist_x86") {
                                item.latestVersion = "Недоступно (aka.ms)";
                            } else {
                                item.latestVersion = "Сайт производителя";
                            }
                        }
                        break;
                    }
                }
            }
        });

        // Group 5: Parallel Microsoft Update Catalog WHQL lookups (LAN / Wi-Fi / BT / Audio)
        auto fCatalog = std::async(std::launch::async, [&]() {
            const size_t n = catalogTargets.size();
            if (n == 0 || stopRequested_.load()) return;
            std::vector<CatalogDriverMatch> resolved(n);
            const size_t kWorkers = std::min<size_t>(4, n);
            std::atomic<size_t> nextIdx{0};
            std::vector<std::thread> pool;
            pool.reserve(kWorkers);
            for (size_t w = 0; w < kWorkers; ++w) {
                pool.emplace_back([&]() {
                    ScopedHttpRegistry onlineScope(onlineHttpRegistry_.get());
                    for (;;) {
                        if (stopRequested_.load()) break;
                        const size_t idx = nextIdx.fetch_add(1);
                        if (idx >= n) break;
                        auto res = ResolveMsCatalogDriverCab(catalogTargets[idx].first);
                        if (res.cabUrl.empty() && !catalogTargets[idx].second.empty() &&
                            !stopRequested_.load()) {
                            res = ResolveMsCatalogDriverCab(catalogTargets[idx].second);
                        }
                        resolved[idx] = std::move(res);
                    }
                });
            }
            for (auto& t : pool) t.join();
            if (stopRequested_.load()) return;

            std::lock_guard<std::mutex> lock(dataMutex_);
            for (auto& item : items_) {
                if (item.catalogSearchKey.empty()) continue;
                for (size_t idx = 0; idx < n; ++idx) {
                    if (catalogTargets[idx].first != item.catalogSearchKey ||
                        catalogTargets[idx].second != item.catalogFallbackKey) {
                        continue;
                    }
                    const auto& match = resolved[idx];
                    const std::string& catVer = match.version;
                    const std::string& cabUrl = match.cabUrl;
                    item.onlineChecked = true;
                    item.latestDateYmd = match.dateYmd;
                    if (!cabUrl.empty()) {
                        item.directInstallerUrl = cabUrl;
                    }
                    if (!catVer.empty()) {
                        int instDateYmd = item.installedDateYmd;
                        if (instDateYmd == 0 && !item.installedVersion.empty()) {
                            TryExtractTrailingVersionDate(
                                ParseVersionParts(item.installedVersion), instDateYmd);
                        }
                        const bool catalogIsOlderByDate =
                            (match.dateYmd > 0 && instDateYmd > 0 &&
                             match.dateYmd < instDateYmd);
                        if (item.installed && !item.installedVersion.empty() &&
                            (catalogIsOlderByDate || IsDriverVersionNewer(item.installedVersion, catVer))) {
                            item.latestVersion = catVer + " (WHQL)";
                            item.updateAvailable = false;
                            item.statusText = "Актуален (" + item.installedVersion + ")";
                        } else {
                            ApplyResolvedVersionToItem(item, catVer, "Не установлен");
                        }
                    } else if (item.latestVersion == "Поиск...") {
                        item.latestVersion = "Каталог MS";
                    }
                    break;
                }
            }
        });

        fDotnet.wait();
        fRuntimes.wait();
        fVendors.wait();
        fWinget.wait();
        fCatalog.wait();

        if (!stopRequested_.load()) {
            generation_.fetch_add(1);
            SetPhaseMessage("Актуальные версии проверены");
        }
        checkingOnline_.store(false);
        if (!stopRequested_.load() && log) {
            log(LogLevel::Success, "Онлайн-проверка актуальных версий завершена.");
        }
    });
}

namespace {

std::vector<std::string_view> GetAllowedDownloadHostsForItem(
    const SystemComponentItem& item,
    bool isCabPackage) {
    if (isCabPackage || item.id == "catalog_autoinstall" ||
        item.id.rfind("catalog_", 0) == 0) {
        return {"download.windowsupdate.com", "windowsupdate.com"};
    }
    if (item.id == "rt_vcredist_x64" || item.id == "rt_vcredist_x86" ||
        item.id == "rt_vc2013" || item.id == "rt_vc2012" || item.id == "rt_vc2010" ||
        item.id == "rt_dx_legacy" || item.id == "rt_dotnet8" || item.id == "rt_dotnet9" ||
        item.id == "rt_dotnet10" || item.id == "rt_netfx48" || item.id == "rt_webview2") {
        return {"microsoft.com", "aka.ms", "windowsupdate.com", "azureedge.net", "msecnd.net"};
    }
    if (item.id == "rt_vulkan") {
        return {"lunarg.com"};
    }
    if (item.id == "rt_openal") {
        return {"openal.org"};
    }
    if (item.id == "gpu_nvidia") {
        return {"nvidia.com"};
    }
    if (item.id == "gpu_amd" || item.id == "chip_amd") {
        return {"amd.com"};
    }
    if (item.id == "intel_dsa") {
        return {"intel.com", "aka.ms"};
    }
    if (item.id == "sw_razer") {
        return {"rzr.to", "razer.com", "razerzone.com"};
    }
    if (item.id == "sw_steelseries") {
        return {"steelseries.com", "steelseriescdn.com"};
    }
    if (item.id == "sw_logitech") {
        return {"logi.com", "logitech.com", "logitechg.com"};
    }
    if (item.id == "sw_corsair") {
        return {"corsair.com"};
    }
    if (item.id == "sw_asus") {
        return {"asus.com"};
    }
    if (item.id == "sw_epos") {
        return {"eposaudio.com", "epos.com", "sennheiser.com", "azureedge.net"};
    }
    if (item.id == "sw_elgato") {
        return {"elgato.com", "corsair.com"};
    }

    const std::string brandCheck = ToLowerAscii(item.vendor + " " + item.name);
    if (brandCheck.find("asus") != std::string::npos) return {"asus.com"};
    if (brandCheck.find("msi") != std::string::npos) return {"msi.com"};
    if (brandCheck.find("samsung") != std::string::npos) return {"samsung.com"};
    if (brandCheck.find("dell") != std::string::npos) return {"dell.com"};
    if (brandCheck.find("philips") != std::string::npos) return {"philips.com", "tpv-tech.com"};
    if (brandCheck.find("viewsonic") != std::string::npos) return {"viewsonic.com", "viewsonicglobal.com"};
    if (brandCheck.find("lg ") != std::string::npos || brandCheck.find("lg electronics") != std::string::npos) {
        return {"lg.com", "lge.com"};
    }
    if (brandCheck.find("benq") != std::string::npos) return {"benq.com"};
    if (brandCheck.find("aoc") != std::string::npos) return {"aoc.com", "tpv-tech.com"};
    if (brandCheck.find("gigabyte") != std::string::npos) return {"gigabyte.com"};
    if (brandCheck.find("acer") != std::string::npos) return {"acer.com"};
    if (brandCheck.find("lenovo") != std::string::npos) return {"lenovo.com"};
    if (brandCheck.find("hp ") != std::string::npos || brandCheck.find("hewlett") != std::string::npos) {
        return {"hp.com"};
    }
    return {};
}

std::vector<std::string_view> GetExpectedPublishersForItem(
    const SystemComponentItem& item,
    bool isCabPackage) {
    if (isCabPackage || item.id == "catalog_autoinstall" ||
        item.id.rfind("catalog_", 0) == 0) {
        return DefaultCabPublishers();
    }
    if (item.id == "rt_vcredist_x64" || item.id == "rt_vcredist_x86" ||
        item.id == "rt_vc2013" || item.id == "rt_vc2012" || item.id == "rt_vc2010" ||
        item.id == "rt_dx_legacy" || item.id == "rt_dotnet8" || item.id == "rt_dotnet9" ||
        item.id == "rt_dotnet10" || item.id == "rt_netfx48" || item.id == "rt_webview2") {
        return {"Microsoft Corporation", "Microsoft Windows"};
    }
    if (item.id == "rt_vulkan") {
        return {"LunarG", "Khronos"};
    }
    if (item.id == "rt_openal") {
        return {"Creative", "OpenAL"};
    }
    if (item.id == "gpu_nvidia") {
        return {"NVIDIA Corporation"};
    }
    if (item.id == "gpu_amd" || item.id == "chip_amd") {
        return {"Advanced Micro Devices"};
    }
    if (item.id == "intel_dsa") {
        return {"Intel Corporation"};
    }
    if (item.id == "sw_razer") {
        return {"Razer"};
    }
    if (item.id == "sw_logitech") {
        return {"Logitech"};
    }
    if (item.id == "sw_steelseries") {
        return {"SteelSeries"};
    }
    if (item.id == "sw_corsair") {
        return {"Corsair"};
    }
    if (item.id == "sw_asus") {
        return {"ASUSTeK Computer", "ASUS"};
    }
    if (item.id == "sw_epos") {
        return {"EPOS", "Sennheiser", "DSEA"};
    }
    if (item.id == "sw_elgato") {
        return {"Elgato", "Corsair"};
    }

    const std::string brandCheck = ToLowerAscii(item.vendor + " " + item.name);
    if (brandCheck.find("asus") != std::string::npos) return {"ASUSTeK Computer", "ASUS"};
    if (brandCheck.find("dell") != std::string::npos) return {"Dell Inc", "Dell Technologies"};
    if (brandCheck.find("msi") != std::string::npos) return {"Micro-Star"};
    if (brandCheck.find("samsung") != std::string::npos) return {"Samsung Electronics"};
    if (brandCheck.find("philips") != std::string::npos) return {"Philips", "MMD", "TPV", "Portrait Displays"};
    if (brandCheck.find("viewsonic") != std::string::npos) return {"ViewSonic"};
    if (brandCheck.find("lg ") != std::string::npos || brandCheck.find("lg electronics") != std::string::npos) {
        return {"LG Electronics"};
    }
    if (brandCheck.find("benq") != std::string::npos) return {"BenQ"};
    if (brandCheck.find("aoc") != std::string::npos) return {"AOC", "TPV"};
    if (brandCheck.find("gigabyte") != std::string::npos) return {"Gigabyte"};
    if (brandCheck.find("lenovo") != std::string::npos) return {"Lenovo"};
    if (brandCheck.find("acer") != std::string::npos) return {"Acer"};
    if (brandCheck.find("hp ") != std::string::npos || brandCheck.find("hewlett") != std::string::npos) {
        return {"HP Inc", "Hewlett-Packard", "Hewlett Packard"};
    }
    return {};
}

} // namespace

bool SystemComponentsScanner::InstallSingleComponentSync(
    const SystemComponentItem& item,
    float baseProgress,
    float progressSpan,
    std::vector<DeviceInfo>& devs,
    const LogCallback& log,
    bool* outRebootRequired) {
    if (outRebootRequired != nullptr) {
        *outRebootRequired = false;
    }
    if (cancelInstallRequested_.load() || item.directInstallerUrl.empty()) {
        return false;
    }

    SetPhaseMessage("Скачивание: " + item.name);

    std::filesystem::path workRoot;
    std::string tempDirErr;
    if (!CreateUniqueTempWorkDir(workRoot, &tempDirErr)) {
        if (log) {
            log(LogLevel::Error, tempDirErr.empty()
                                     ? "Не удалось создать временный каталог Windows."
                                     : tempDirErr);
        }
        SetPhaseMessage("Ошибка временного каталога");
        return false;
    }
    TempWorkDirGuard workDir;
    workDir.Set(workRoot);

    const std::string cleanUrl = StripUrlQuery(item.directInstallerUrl);
    bool isZipPackage = EndsWithCaseInsensitive(cleanUrl, ".zip");
    const bool isCabPackage = EndsWithCaseInsensitive(cleanUrl, ".cab");
    const bool isDirectMsi = EndsWithCaseInsensitive(cleanUrl, ".msi");
    const char* initialExt =
        isCabPackage ? ".cab" : (isZipPackage ? ".zip" : (isDirectMsi ? ".msi" : ".exe"));
    std::filesystem::path destFile = workDir.Path() / (std::string("package") + initialExt);

    if (log) {
        log(LogLevel::Info,
            "Загрузка официального установщика \"" + item.name + "\"...");
    }

    const std::vector<std::string_view> allowedHosts =
        GetAllowedDownloadHostsForItem(item, isCabPackage);
    const std::vector<std::string_view> allowedPublishers =
        GetExpectedPublishersForItem(item, isCabPackage);
    if (allowedHosts.empty() || allowedPublishers.empty()) {
        if (!stopRequested_.load() && log) {
            log(LogLevel::Error,
                "Для \"" + item.name + "\" не задан доверенный источник или издатель — автоустановка отклонена в целях безопасности.");
        }
        SetPhaseMessage("Источник не подтверждён");
        return false;
    }

    std::string errMsg;
    if (!DownloadFileViaWinHttp(
            Utf8ToWide(item.directInstallerUrl),
            destFile,
            &progress_,
            &errMsg,
            baseProgress,
            progressSpan,
            &cancelInstallRequested_,
            allowedHosts)) {
        if (!stopRequested_.load() && log) {
            log(cancelInstallRequested_.load() ? LogLevel::Warning : LogLevel::Error,
                "Ошибка скачивания " + item.name + ": " + errMsg);
        }
        SetPhaseMessage(cancelInstallRequested_.load() ? "Установка отменена" : "Ошибка загрузки");
        return false;
    }

    if (!item.expectedSha256.empty()) {
        std::string actualSha;
        std::string shaErr;
        if (!ComputeFileSha256Hex(destFile, actualSha, &shaErr)) {
            if (!stopRequested_.load() && log) {
                log(LogLevel::Error, "Не удалось проверить SHA-256 пакета " + item.name + ": " + shaErr);
            }
            SetPhaseMessage("Ошибка проверки SHA-256");
            return false;
        }
        if (ToLowerAscii(actualSha) != ToLowerAscii(item.expectedSha256)) {
            if (!stopRequested_.load() && log) {
                log(LogLevel::Error,
                    "Контрольная сумма SHA-256 скачанного пакета " + item.name +
                        " не совпадает с манифестом. Установка отменена.");
            }
            SetPhaseMessage("SHA-256 не совпадает");
            return false;
        }
    }

    if (isCabPackage) {
        SetPhaseMessage("Проверка подписи CAB-пакета...");
        HandleGuard cabLock;
        std::string sigErr;
        if (!VerifyAndLockAuthenticodeFile(destFile, &cabLock, &sigErr, allowedPublishers)) {
            if (!stopRequested_.load() && log) {
                log(LogLevel::Error,
                    "Цифровая подпись CAB-пакета не прошла проверку (" + sigErr + ").");
            }
            SetPhaseMessage("Подпись CAB не подтверждена");
            return false;
        }

        bool cabOk = false;
        bool cabReboot = false;
        std::error_code ec;
        if (IsProcessElevated()) {
            SetPhaseMessage("Распаковка драйвера (.cab)...");
            std::filesystem::path cabExtractDir = workDir.Path() / L"cab_extract";
            std::filesystem::create_directories(cabExtractDir, ec);
            if (ec) {
                if (!stopRequested_.load() && log) {
                    log(LogLevel::Error, "Не удалось создать каталог распаковки CAB.");
                }
                SetPhaseMessage("Ошибка распаковки CAB");
                return false;
            }

            CabExtractContext cabCtx{};
            cabCtx.baseDir = cabExtractDir;
            if (!SetupIterateCabinetW(destFile.c_str(), 0, CabExtractCallback, &cabCtx)) {
                cabLock.Reset();
                if (!stopRequested_.load() && log) {
                    log(LogLevel::Error,
                        "Ошибка распаковки CAB-архива " + item.name + ": " +
                            FormatWin32Error(GetLastError()));
                }
                SetPhaseMessage("Ошибка распаковки CAB");
                return false;
            }
            cabLock.Reset();
            std::filesystem::remove(destFile, ec);

            bool hasReparsePoint = false;
            for (std::filesystem::recursive_directory_iterator it(
                     cabExtractDir,
                     std::filesystem::directory_options::none,
                     ec);
                 !ec && it != std::filesystem::recursive_directory_iterator();
                 it.increment(ec)) {
                const DWORD attrs = GetFileAttributesW(it->path().c_str());
                if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                    hasReparsePoint = true;
                    break;
                }
            }
            if (ec || hasReparsePoint) {
                if (!stopRequested_.load() && log) {
                    log(LogLevel::Error,
                        "Обнаружена недопустимая точка повторного анализа в распакованном CAB-пакете.");
                }
                SetPhaseMessage("Ошибка безопасности CAB");
                return false;
            }

            SetPhaseMessage("Установка драйвера " + item.name + "...");
            progress_.store(std::clamp(baseProgress + progressSpan * 0.94f, 0.0f, 0.99f));

            const LocalInstallSummary sum = LocalDriverInstaller::InstallFromFolderSync(
                cabExtractDir.wstring(), false, false, log);
            cabOk = (sum.failedCount == 0 && (sum.installedCount > 0 || sum.stagedOnlyCount > 0));
            if (sum.rebootRequired) {
                cabReboot = true;
            }
            std::filesystem::remove_all(cabExtractDir, ec);
        } else {
            SetPhaseMessage("Установка драйвера " + item.name + "...");
            progress_.store(std::clamp(baseProgress + progressSpan * 0.94f, 0.0f, 0.99f));

            const std::filesystem::path tempLog = CreateTempLogPath(L"hdm_cab_inf");
            std::wstring cliArgs = L"--cli-install-cab " + QuoteWin32Arg(destFile.wstring());
            if (!tempLog.empty()) {
                cliArgs += L" --log " + QuoteWin32Arg(tempLog.wstring());
            }
            const ElevatedRunResult run = ExecuteElevatedSelf(
                cliArgs, true, tempLog.empty() ? nullptr : &tempLog, log, nullptr, &stopRequested_);
            cabLock.Reset();
            if (!tempLog.empty()) {
                std::filesystem::remove(tempLog, ec);
            }
            if (!run.launched) {
                if (run.launchError == ERROR_CANCELLED) {
                    cancelInstallRequested_.store(true);
                    if (!stopRequested_.load() && log) {
                        log(LogLevel::Warning, "Установка драйвера отменена (UAC).");
                    }
                } else if (!stopRequested_.load() && log) {
                    log(LogLevel::Error, "Ошибка запуска UAC: " + FormatWin32Error(run.launchError));
                }
            } else {
                cabOk = (run.exitCode == 0 || run.exitCode == 2);
                if (run.exitCode == 2) {
                    cabReboot = true;
                }
            }
        }
        if (outRebootRequired != nullptr && cabReboot) {
            *outRebootRequired = true;
        }

        if (!stopRequested_.load()) {
            const int maxAttempts =
                (cabOk && !cabReboot && !item.installedVersion.empty()) ? 6 : 1;
            std::vector<DeviceInfo> freshDevs;
            std::vector<SystemComponentItem> refreshed;
            bool versionUnchanged = false;

            for (int attempt = 0; attempt < maxAttempts; ++attempt) {
                if (attempt > 0) {
                    for (int step = 0; step < 5; ++step) {
                        if (stopRequested_.load()) {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    if (stopRequested_.load()) {
                        break;
                    }
                }

                freshDevs = DeviceScanner::ScanSynchronously();
                refreshed = ScanLocalSynchronously(freshDevs);

                versionUnchanged = false;
                if (cabOk && !cabReboot && !item.installedVersion.empty()) {
                    for (const auto& r : refreshed) {
                        if (r.id == item.id && r.installedVersion == item.installedVersion) {
                            versionUnchanged = true;
                            break;
                        }
                    }
                }
                if (!versionUnchanged) {
                    break;
                }
            }

            if (!stopRequested_.load()) {
                devs = std::move(freshDevs);
                if (versionUnchanged) {
                    if (log) {
                        log(LogLevel::Warning,
                            "Пакет .cab добавлен в DriverStore, но Windows оставила текущую версию драйвера (" +
                                item.installedVersion +
                                ") — пакет не подходит к ревизии устройства или требуется перезагрузка.");
                    }
                    cabOk = false;
                }
                {
                    std::lock_guard<std::mutex> lock(dataMutex_);
                    MergeOnlineMetadataIntoRefreshed(refreshed, items_);
                    items_ = std::move(refreshed);
                    generation_.fetch_add(1);
                }
                if (cabOk || cabReboot) {
                    deviceRescanNeeded_.store(true);
                }
            }
        }
        progress_.store(std::clamp(baseProgress + progressSpan, 0.0f, 1.0f));
        return cabOk;
    }

    if (!isZipPackage && HasZipMagicHeader(destFile)) {
        std::filesystem::path zipRenamed = destFile;
        zipRenamed.replace_extension(L".zip");
        std::error_code rec;
        std::filesystem::rename(destFile, zipRenamed, rec);
        if (!rec) {
            destFile = zipRenamed;
            isZipPackage = true;
        }
    } else if (!isZipPackage && !isCabPackage &&
               !EndsWithCaseInsensitive(WideToUtf8(destFile.extension().wstring()), ".msi") &&
               HasMsiMagicHeader(destFile)) {
        std::filesystem::path msiRenamed = destFile;
        msiRenamed.replace_extension(L".msi");
        std::error_code rec;
        std::filesystem::rename(destFile, msiRenamed, rec);
        if (!rec) {
            destFile = msiRenamed;
        }
    }

    std::filesystem::path targetExe = destFile;
    std::filesystem::path extractDir;
    std::vector<HandleGuard> extractedLocks;

    if (isZipPackage) {
        SetPhaseMessage("Распаковка архива...");
        extractDir = workDir.Path() / L"pkg_extract";
        std::error_code ec;
        if (!ExtractZipViaShellCom(destFile, extractDir, &cancelInstallRequested_)) {
            if (!stopRequested_.load() && log) {
                log(LogLevel::Error, "Не удалось распаковать архив " + item.name + ".");
            }
            SetPhaseMessage("Ошибка распаковки");
            return false;
        }

        targetExe.clear();
        if (!item.nestedInstallerHint.empty()) {
            targetExe = ResolveContainedInstallerPath(extractDir, item.nestedInstallerHint);
        }
        if (targetExe.empty()) {
            std::filesystem::path firstMsi;
            std::filesystem::path setupExe;
            std::filesystem::path firstExe;
            for (std::filesystem::recursive_directory_iterator it(
                     extractDir,
                     std::filesystem::directory_options::none,
                     ec);
                 !ec && it != std::filesystem::recursive_directory_iterator();
                 it.increment(ec)) {
                std::error_code entryEc;
                if (!it->is_regular_file(entryEc)) {
                    continue;
                }
                const DWORD attrs = GetFileAttributesW(it->path().c_str());
                if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                    continue;
                }
                const std::string ext = ToLowerAscii(WideToUtf8(it->path().extension().wstring()));
                const std::string fname = ToLowerAscii(WideToUtf8(it->path().filename().wstring()));
                if (ext == ".msi" && firstMsi.empty()) {
                    firstMsi = it->path();
                } else if (ext == ".exe") {
                    if (firstExe.empty()) firstExe = it->path();
                    if (setupExe.empty() &&
                        (fname.find("setup") != std::string::npos ||
                         fname.find("install") != std::string::npos)) {
                        setupExe = it->path();
                    }
                }
            }
            if (!firstMsi.empty()) targetExe = firstMsi;
            else if (!setupExe.empty()) targetExe = setupExe;
            else targetExe = firstExe;
        }
        if (targetExe.empty()) {
            if (!stopRequested_.load() && log) {
                log(LogLevel::Error, "В архиве не найден исполняемый файл установщика.");
            }
            SetPhaseMessage("Установщик не найден");
            return false;
        }
        std::string dirSigErr;
        if (!VerifyExtractedDirectoryBinaries(extractDir, allowedPublishers, &dirSigErr, &extractedLocks)) {
            if (!stopRequested_.load() && log) {
                log(LogLevel::Error,
                    "Содержимое распакованного архива не прошло проверку подписи (" +
                        dirSigErr + "). Запуск отменён в целях безопасности.");
            }
            SetPhaseMessage("Подпись архива не подтверждена");
            return false;
        }
    }

    if (cancelInstallRequested_.load()) {
        SetPhaseMessage("Установка отменена");
        return false;
    }

    SetPhaseMessage("Проверка цифровой подписи Authenticode...");
    HandleGuard targetLock;
    std::string sigErr;
    if (!VerifyAndLockAuthenticodeFile(targetExe, &targetLock, &sigErr, allowedPublishers)) {
        if (!stopRequested_.load() && log) {
            log(LogLevel::Error,
                "Цифровая подпись установщика не прошла проверку (" + sigErr +
                    "). Запуск отменён в целях безопасности.");
        }
        SetPhaseMessage("Подпись не подтверждена");
        return false;
    }

    const std::string dlVer = GetFileVersionUtf8(targetExe);
    if (!stopRequested_.load() && log) {
        log(LogLevel::Success,
            "Подпись подтверждена" +
                (dlVer.empty() ? std::string() : " (версия файла: " + dlVer + ")") +
                ". Запуск установки...");
    }

    SetPhaseMessage("Установка " + item.name + "...");
    progress_.store(std::clamp(baseProgress + progressSpan * 0.94f, 0.0f, 0.99f));

    DWORD exitCode = 0;
    DWORD launchErr = 0;
    bool launched = false;
    bool processStillRunning = false;
    bool errorAlreadyLogged = false;

    if (item.id == "rt_dx_legacy") {
        const std::filesystem::path dxExtractDir = workDir.Path() / L"dx_extract";
        std::error_code ec;
        std::filesystem::create_directories(dxExtractDir, ec);
        if (ec) {
            launchErr = static_cast<DWORD>(ec.value());
            if (!stopRequested_.load() && log) {
                log(LogLevel::Error, "Не удалось создать временный каталог распаковки DirectX.");
            }
            errorAlreadyLogged = true;
        } else {
            const std::wstring extractArgs = L"/Q /T:" + QuoteWin32Arg(dxExtractDir.wstring());
            const bool extracted = LaunchAndWaitProcess(
                targetExe,
                extractArgs,
                &exitCode,
                &launchErr,
                &cancelInstallRequested_,
                &stopRequested_,
                &processStillRunning);
            targetLock.Reset();
            if (extracted) {
                const std::filesystem::path dxSetup = dxExtractDir / L"DXSETUP.exe";
                const std::filesystem::path dsetupDll = dxExtractDir / L"DSETUP.dll";
                if (exitCode != 0) {
                    if (!stopRequested_.load() && log) {
                        log(LogLevel::Error,
                            "Ошибка распаковки DirectX End-User Runtime (код " +
                                std::to_string(exitCode) + ").");
                    }
                    errorAlreadyLogged = true;
                } else if (std::filesystem::is_regular_file(dxSetup, ec) &&
                           std::filesystem::is_regular_file(dsetupDll, ec)) {
                    const std::vector<std::string_view> msPub{
                        "Microsoft Corporation", "Microsoft Windows"};
                    std::vector<HandleGuard> dxLocks;
                    std::string dxSigErr;
                    if (!VerifyExtractedDirectoryBinaries(dxExtractDir, msPub, &dxSigErr, &dxLocks)) {
                        if (!stopRequested_.load() && log) {
                            log(LogLevel::Error,
                                "Цифровая подпись компонентов DirectX не прошла проверку (" + dxSigErr +
                                    "). Запуск отменён в целях безопасности.");
                        }
                        launchErr = ERROR_INVALID_DATA;
                        errorAlreadyLogged = true;
                    } else {
                        launched = LaunchAndWaitProcess(
                            dxSetup,
                            L"/silent",
                            &exitCode,
                            &launchErr,
                            &cancelInstallRequested_,
                            &stopRequested_,
                            &processStillRunning);
                        dxLocks.clear();
                    }
                } else {
                    if (!stopRequested_.load() && log) {
                        log(LogLevel::Error,
                            "В распакованном пакете DirectX не найдены DXSETUP.exe или DSETUP.dll.");
                    }
                    launchErr = ERROR_FILE_NOT_FOUND;
                    errorAlreadyLogged = true;
                }
            }
        }
    } else if (EndsWithCaseInsensitive(WideToUtf8(targetExe.extension().wstring()), ".msi")) {
        const std::filesystem::path sysDir = GetSystemDirPath(false);
        if (!sysDir.empty()) {
            const std::filesystem::path msiexec = sysDir / L"msiexec.exe";
            const std::wstring msiArgs =
                L"/i " + QuoteWin32Arg(targetExe.wstring()) + L" /passive /norestart";
            launched = LaunchAndWaitProcess(
                msiexec,
                msiArgs,
                &exitCode,
                &launchErr,
                &cancelInstallRequested_,
                &stopRequested_,
                &processStillRunning);
        } else {
            launchErr = ERROR_PATH_NOT_FOUND;
        }
        targetLock.Reset();
    } else {
        launched = LaunchAndWaitProcess(
            targetExe,
            Utf8ToWide(item.installerArgs),
            &exitCode,
            &launchErr,
            &cancelInstallRequested_,
            &stopRequested_,
            &processStillRunning);
        targetLock.Reset();
    }

    if (processStillRunning) {
        workDir.Release();
        if (!stopRequested_.load() && log) {
            log(LogLevel::Warning,
                "Повышенный установщик ещё выполняется в фоне; временная папка не удалена.");
        }
    }

    bool succeeded = false;
    if (!launched) {
        if (!errorAlreadyLogged && !stopRequested_.load() && log) {
            log(launchErr == ERROR_CANCELLED ? LogLevel::Warning : LogLevel::Error,
                launchErr == ERROR_CANCELLED
                    ? "Установка отменена пользователем."
                    : "Не удалось запустить установщик: " + FormatWin32Error(launchErr));
        }
        if (launchErr == ERROR_CANCELLED) {
            cancelInstallRequested_.store(true);
        }
    } else if (exitCode == 0 || exitCode == 3010 || exitCode == 1641 ||
               (exitCode == 2 && item.id == "chip_amd")) {
        succeeded = true;
        const bool needReboot =
            (exitCode == 3010 || exitCode == 1641 || (exitCode == 2 && item.id == "chip_amd"));
        if (outRebootRequired != nullptr && needReboot) {
            *outRebootRequired = true;
        }
        if (!stopRequested_.load() && log) {
            log(LogLevel::Success,
                item.name + " успешно установлен" +
                    (needReboot ? " (требуется перезагрузка для применения изменений)." : "."));
        }
    } else {
        if (!stopRequested_.load() && log) {
            log(LogLevel::Warning,
                item.name + " завершился с кодом " + std::to_string(exitCode) + ".");
        }
    }

    workDir.Reset();

    if (stopRequested_.load()) {
        return false;
    }

    auto refreshAndCompare = [&]() {
        if (item.category == ComponentCategory::MonitorAndGpu) {
            devs = DeviceScanner::ScanSynchronously();
        }
        auto refreshed = ScanLocalSynchronously(devs);
        std::lock_guard<std::mutex> lock(dataMutex_);
        if (!dlVer.empty() &&
            (item.id == "rt_vcredist_x64" || item.id == "rt_vcredist_x86")) {
            for (auto& oldIt : items_) {
                if (oldIt.id == item.id) {
                    oldIt.latestVersion = dlVer;
                    oldIt.onlineChecked = true;
                    break;
                }
            }
        }
        MergeOnlineMetadataIntoRefreshed(refreshed, items_);
        items_ = std::move(refreshed);
        generation_.fetch_add(1);
    };

    refreshAndCompare();

    // Some web bootstrappers (Logitech G HUB / Razer Synapse / WebView2) continue finalizing
    // registry Uninstall entries for a few seconds after the launcher returns.
    if (launched) {
        bool needRetry = false;
        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            for (const auto& cur : items_) {
                if (cur.id == item.id && (!cur.installed || cur.updateAvailable)) {
                    needRetry = true;
                    break;
                }
            }
        }
        if (needRetry) {
            for (int step = 0; step < 30; ++step) {
                if (stopRequested_.load() || cancelInstallRequested_.load()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!stopRequested_.load()) {
                refreshAndCompare();
            }
        }
        if (item.category == ComponentCategory::MonitorAndGpu && !stopRequested_.load()) {
            deviceRescanNeeded_.store(true);
        }
    }

    progress_.store(std::clamp(baseProgress + progressSpan, 0.0f, 1.0f));
    return succeeded;
}

void SystemComponentsScanner::DownloadAndInstallAsync(
    const SystemComponentItem& item,
    std::vector<DeviceInfo> devicesSnapshot,
    LogCallback onLog) {
    std::unique_lock<std::mutex> lock(threadMutex_);
    if (stopRequested_.load()) {
        return;
    }
    if (scanning_.load() || checkingOnline_.load() || installing_.exchange(true)) {
        lock.unlock();
        if (onLog) {
            onLog(LogLevel::Warning, "Установка компонента уже выполняется.");
        }
        return;
    }
    if (item.directInstallerUrl.empty()) {
        installing_.store(false);
        return;
    }
    std::thread oldThread;
    if (installThread_.joinable()) {
        oldThread = std::move(installThread_);
    }
    if (oldThread.joinable()) {
        lock.unlock();
        oldThread.join();
        lock.lock();
    }
    if (stopRequested_.load()) {
        installing_.store(false);
        return;
    }
    if (installHttpRegistry_ && !stopRequested_.load()) {
        installHttpRegistry_->Reset();
    }

    cancelInstallRequested_.store(false);
    progress_.store(0.0f);

    installThread_ = std::thread([this, item, devs = std::move(devicesSnapshot), log = std::move(onLog)]() mutable {
        ScopedHttpRegistry installScope(installHttpRegistry_.get());
        bool rebootReq = false;
        const bool ok = InstallSingleComponentSync(item, 0.0f, 1.0f, devs, log, &rebootReq);
        progress_.store(1.0f);
        if (!stopRequested_.load()) {
            SetPhaseMessage(
                ok ? (rebootReq ? "Установка завершена (требуется перезагрузка)" : "Установка завершена")
                   : (cancelInstallRequested_.load() ? "Установка отменена"
                                                    : "Установка завершена с предупреждением"));
        }
        installing_.store(false);
    });
}

void SystemComponentsScanner::DownloadAndInstallBatchAsync(
    std::vector<SystemComponentItem> itemsToInstall,
    std::vector<DeviceInfo> devicesSnapshot,
    LogCallback onLog,
    bool createRestorePointBefore) {
    std::unique_lock<std::mutex> lock(threadMutex_);
    if (stopRequested_.load()) {
        return;
    }
    if (scanning_.load() || checkingOnline_.load() || installing_.exchange(true)) {
        lock.unlock();
        if (onLog) {
            onLog(LogLevel::Warning, "Установка компонентов уже выполняется.");
        }
        return;
    }
    if (itemsToInstall.empty()) {
        installing_.store(false);
        return;
    }
    std::thread oldThread;
    if (installThread_.joinable()) {
        oldThread = std::move(installThread_);
    }
    if (oldThread.joinable()) {
        lock.unlock();
        oldThread.join();
        lock.lock();
    }
    if (stopRequested_.load()) {
        installing_.store(false);
        return;
    }
    if (installHttpRegistry_ && !stopRequested_.load()) {
        installHttpRegistry_->Reset();
    }

    cancelInstallRequested_.store(false);
    progress_.store(0.0f);
    SetPhaseMessage("Пакетное обновление компонентов...");

    installThread_ = std::thread([this,
                                  list = std::move(itemsToInstall),
                                  devs = std::move(devicesSnapshot),
                                  log = std::move(onLog),
                                  createRestorePointBefore]() mutable {
        ScopedHttpRegistry installScope(installHttpRegistry_.get());
        const size_t total = list.size();
        size_t okCount = 0;

        if (!stopRequested_.load() && log) {
            log(LogLevel::Info,
                "Начинается пакетная установка/обновление компонентов (" +
                    std::to_string(total) + " шт.)...");
        }

        if (createRestorePointBefore && !cancelInstallRequested_.load() && !stopRequested_.load()) {
            SetPhaseMessage("Создание точки восстановления...");
            progress_.store(0.03f);
            constexpr const char* kRpDesc =
                "DriverLoom (перед пакетным обновлением компонентов)";
            if (IsProcessElevated()) {
                SystemRestore::CreateRestorePointSync(kRpDesc, log);
            } else {
                if (!stopRequested_.load() && log) {
                    log(LogLevel::Info,
                        "Запрос прав администратора (UAC) для создания точки восстановления...");
                }
                const std::filesystem::path tempLog = CreateTempLogPath(L"hdm_sr_comp");
                std::wstring args = L"--cli-restore-point " + QuoteWin32Arg(Utf8ToWide(kRpDesc));
                if (!tempLog.empty()) {
                    args += L" --log " + QuoteWin32Arg(tempLog.wstring());
                }
                const ElevatedRunResult run = ExecuteElevatedSelf(
                    args, true, tempLog.empty() ? nullptr : &tempLog, log, nullptr, &stopRequested_);
                std::error_code ec;
                if (!tempLog.empty()) {
                    std::filesystem::remove(tempLog, ec);
                }
                if (!run.launched && !stopRequested_.load() && log) {
                    log(LogLevel::Warning,
                        run.launchError == ERROR_CANCELLED
                            ? "Пользователь отменил UAC при создании точки восстановления — продолжается обновление компонентов."
                            : ("Не удалось создать точку восстановления: " +
                               FormatWin32Error(run.launchError)));
                }
            }
            SetPhaseMessage("Пакетное обновление компонентов...");
        }

        const float baseProg = createRestorePointBefore ? 0.08f : 0.0f;
        progress_.store(baseProg);
        const float span = (1.0f - baseProg) / static_cast<float>(total);
        for (size_t i = 0; i < total; ++i) {
            if (stopRequested_.load() || cancelInstallRequested_.load()) {
                if (!stopRequested_.load() && log) {
                    log(LogLevel::Warning, "Пакетная установка остановлена пользователем.");
                }
                break;
            }
            if (log) {
                log(LogLevel::Info,
                    "[" + std::to_string(i + 1) + "/" + std::to_string(total) +
                        "] Пакетная установка: " + list[i].name);
            }
            if (InstallSingleComponentSync(
                    list[i], baseProg + static_cast<float>(i) * span, span, devs, log)) {
                ++okCount;
            }
        }

        progress_.store(1.0f);
        const std::string doneMsg =
            cancelInstallRequested_.load()
                ? ("Пакетная установка прервана (" + std::to_string(okCount) + " из " +
                   std::to_string(total) + ")")
                : ("Пакетная установка завершена: успешно — " + std::to_string(okCount) +
                   " из " + std::to_string(total));
        if (!stopRequested_.load()) {
            SetPhaseMessage(doneMsg);
            if (log) {
                log(okCount == total ? LogLevel::Success : LogLevel::Warning, doneMsg + ".");
            }
        }
        installing_.store(false);
    });
}

void SystemComponentsScanner::SearchAndInstallFromCatalogAsync(
    const std::string& deviceName,
    const std::string& searchHardwareKey,
    const std::string& fullHardwareId,
    const std::string& installedVersion,
    std::vector<DeviceInfo> devicesSnapshot,
    LogCallback onLog,
    CatalogInstallCallback onComplete) {
    std::unique_lock<std::mutex> lock(threadMutex_);
    if (stopRequested_.load()) {
        return;
    }
    if (scanning_.load() || checkingOnline_.load() || installing_.exchange(true)) {
        lock.unlock();
        if (onLog) {
            onLog(LogLevel::Warning, "Модуль компонентов уже выполняет другую операцию.");
        }
        if (onComplete) {
            onComplete(false, false);
        }
        return;
    }

    cancelInstallRequested_.store(false);
    progress_.store(0.04f);
    std::thread oldThread;
    if (installThread_.joinable()) {
        oldThread = std::move(installThread_);
    }
    if (oldThread.joinable()) {
        lock.unlock();
        oldThread.join();
        lock.lock();
    }
    if (stopRequested_.load()) {
        installing_.store(false);
        return;
    }
    if (installHttpRegistry_ && !stopRequested_.load()) {
        installHttpRegistry_->Reset();
    }

    installThread_ = std::thread([this,
                                  deviceName,
                                  searchHardwareKey,
                                  fullHardwareId,
                                  installedVersion,
                                  devs = std::move(devicesSnapshot),
                                  log = std::move(onLog),
                                  cb = std::move(onComplete)]() mutable {
        ScopedHttpRegistry installScope(installHttpRegistry_.get());
        const std::string primaryKey =
            !searchHardwareKey.empty() ? searchHardwareKey : fullHardwareId;
        if (primaryKey.empty()) {
            if (!stopRequested_.load() && log) {
                log(LogLevel::Error,
                    "Не удалось определить Hardware ID для поиска в Microsoft Update Catalog («" +
                        deviceName + "»).");
            }
            SetPhaseMessage("Недостаточно данных для поиска драйвера");
            progress_.store(1.0f);
            installing_.store(false);
            if (!stopRequested_.load() && cb) cb(false, false);
            return;
        }

        SetPhaseMessage("Поиск драйвера в Microsoft Update Catalog: " + deviceName + "...");
        if (!stopRequested_.load() && log) {
            log(LogLevel::Info,
                "Поиск WHQL-драйвера в Microsoft Update Catalog для «" + deviceName + "» (" +
                    primaryKey + ")...");
        }

        std::string catVer;
        std::string cabUrl;
        const std::string subsysKey = ExtractVenDevSubsysKey(fullHardwareId);
        const bool isHdaAudio = ContainsCaseInsensitive(fullHardwareId, "HDAUDIO\\") ||
                                ContainsCaseInsensitive(fullHardwareId, "INTELAUDIO\\");
        const bool strictSubsysOnly = isHdaAudio && !subsysKey.empty();
        if (!subsysKey.empty() && !stopRequested_.load()) {
            const CatalogDriverMatch match = ResolveMsCatalogDriverCab(subsysKey);
            catVer = match.version;
            cabUrl = match.cabUrl;
        }
        if (!strictSubsysOnly && cabUrl.empty() && !cancelInstallRequested_.load() && !stopRequested_.load()) {
            const CatalogDriverMatch match = ResolveMsCatalogDriverCab(primaryKey);
            catVer = match.version;
            cabUrl = match.cabUrl;
        }
        if (!strictSubsysOnly && cabUrl.empty() && !fullHardwareId.empty() && fullHardwareId != primaryKey &&
            fullHardwareId != subsysKey && !cancelInstallRequested_.load() && !stopRequested_.load()) {
            const CatalogDriverMatch match = ResolveMsCatalogDriverCab(fullHardwareId);
            catVer = match.version;
            cabUrl = match.cabUrl;
        }

        if (cancelInstallRequested_.load() || stopRequested_.load()) {
            SetPhaseMessage("Установка отменена");
            progress_.store(1.0f);
            installing_.store(false);
            if (!stopRequested_.load() && cb) cb(false, false);
            return;
        }

        if (cabUrl.empty()) {
            const bool isStandardStorageOrChipset =
                ContainsCaseInsensitive(deviceName, "AHCI") ||
                ContainsCaseInsensitive(deviceName, "NVM Express") ||
                ContainsCaseInsensitive(primaryKey, "VEN_1022") ||
                ContainsCaseInsensitive(primaryKey, "VEN_8086");
            if (isStandardStorageOrChipset) {
                bool usesOemInf = false;
                for (const auto& d : devs) {
                    if (ToLowerAscii(d.searchHardwareKey) == ToLowerAscii(primaryKey) ||
                        ContainsCaseInsensitive(d.hardwareId, primaryKey)) {
                        if (ToLowerAscii(d.infName).rfind("oem", 0) == 0) {
                            usesOemInf = true;
                            break;
                        }
                    }
                }
                if (usesOemInf) {
                    SetPhaseMessage("Восстановление системного драйвера Windows...");
                    if (!stopRequested_.load() && log) {
                        log(LogLevel::Info,
                            "В Microsoft Update Catalog нет .cab для «" + deviceName +
                                "» — выполняется переключение на актуальный системный драйвер Windows...");
                    }
                    bool inboxOk = false;
                    bool inboxReboot = false;
                    if (IsProcessElevated()) {
                        inboxOk = DeviceScanner::RestoreInboxDriverByHardwareKeySync(
                            primaryKey, deviceName, log, &inboxReboot);
                    } else {
                        std::error_code ec;
                        const std::filesystem::path tempLog = CreateTempLogPath(L"hdm_inbox");
                        std::wstring cliArgs =
                            L"--cli-restore-inbox " + QuoteWin32Arg(Utf8ToWide(primaryKey));
                        if (!tempLog.empty()) {
                            cliArgs += L" --log " + QuoteWin32Arg(tempLog.wstring());
                        }
                        const ElevatedRunResult run = ExecuteElevatedSelf(
                            cliArgs, true, tempLog.empty() ? nullptr : &tempLog, log, nullptr, &stopRequested_);
                        if (!tempLog.empty()) {
                            std::filesystem::remove(tempLog, ec);
                        }
                        if (!run.launched) {
                            if (run.launchError == ERROR_CANCELLED) {
                                cancelInstallRequested_.store(true);
                                if (!stopRequested_.load() && log) {
                                    log(LogLevel::Warning,
                                        "Восстановление системного драйвера отменено (UAC).");
                                }
                            } else if (!stopRequested_.load() && log) {
                                log(LogLevel::Error,
                                    "Ошибка запуска UAC: " + FormatWin32Error(run.launchError));
                            }
                        } else {
                            inboxOk = (run.exitCode == 0 || run.exitCode == 2);
                            inboxReboot = (run.exitCode == 2);
                        }
                    }
                    if (inboxOk) {
                        SetPhaseMessage("Восстановлен системный драйвер Windows");
                        progress_.store(1.0f);
                        installing_.store(false);
                        if (!stopRequested_.load() && cb) cb(true, inboxReboot);
                        return;
                    }
                }

                if (!stopRequested_.load() && log) {
                    log(LogLevel::Info,
                        "Для «" + deviceName + "» (" + primaryKey +
                            ") отдельный .cab-пакет в Microsoft Update Catalog не выпускается — устройство использует стандартный драйвер Windows / пакета чипсета" +
                            (installedVersion.empty() ? std::string(".")
                                                      : (" (текущая версия " + installedVersion + ").")));
                }
                SetPhaseMessage("Используется стандартный драйвер Windows / чипсета");
            } else {
                if (!stopRequested_.load() && log) {
                    log(LogLevel::Warning,
                        "В Microsoft Update Catalog не найден .cab-пакет драйвера для «" +
                            deviceName + "» (" + primaryKey + ").");
                }
                SetPhaseMessage("Драйвер в Microsoft Update Catalog не найден");
            }
            progress_.store(1.0f);
            installing_.store(false);
            if (!stopRequested_.load() && cb) cb(false, false);
            return;
        }

        if (!stopRequested_.load() && log) {
            std::string note;
            if (!installedVersion.empty() && !catVer.empty() &&
                !IsDriverVersionNewer(catVer, installedVersion)) {
                note = " (в системе уже установлена версия " + installedVersion + ")";
            }
            log(LogLevel::Info,
                "Найден WHQL-пакет в Microsoft Update Catalog: версия " + catVer + note +
                    " — загрузка и установка (.cab)...");
        }

        SystemComponentItem tempItem;
        tempItem.id = "catalog_autoinstall";
        tempItem.name = deviceName + " (WHQL " + catVer + ")";
        tempItem.directInstallerUrl = cabUrl;

        bool rebootReq = false;
        const bool ok = InstallSingleComponentSync(
            tempItem, 0.15f, 0.85f, devs, log, &rebootReq);

        if (!stopRequested_.load()) {
            SetPhaseMessage(ok ? ("Установлен драйвер " + catVer + " для " + deviceName)
                               : "Ошибка установки драйвера из Catalog");
        }
        progress_.store(1.0f);
        installing_.store(false);
        if (!stopRequested_.load() && cb) {
            cb(ok, rebootReq);
        }
    });
}

std::vector<SystemComponentItem> SystemComponentsScanner::ScanLocalSynchronously(
    const std::vector<DeviceInfo>& devicesSnapshot) {
    std::vector<SystemComponentItem> result;
    const auto apps = CollectAllInstalledApps();
    const std::filesystem::path sys32 = GetSystemDirPath(false);
    const std::filesystem::path sysWow = GetSystemDirPath(true);
    const std::filesystem::path progFiles = GetEnvPath(L"ProgramFiles");
    const std::filesystem::path progFilesX86 = GetEnvPath(L"ProgramFiles(x86)");

    // 1. Gaming Runtimes & Windows Libraries
    {
        SystemComponentItem vc64;
        vc64.id = "rt_vcredist_x64";
        vc64.category = ComponentCategory::GamingRuntime;
        vc64.name = "Visual C++ 2015–2022 Redistributable (x64)";
        vc64.vendor = "Microsoft";
        vc64.purposeDescription =
            "64-битные библиотеки среды выполнения C/C++ (MSVC). Обязательны для запуска большинства современных игр, лаунчеров и программ на Windows.";
        vc64.hardwareConnected = true;
        vc64.officialPageUrl = "https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist";
        vc64.directInstallerUrl = "https://aka.ms/vs/17/release/vc_redist.x64.exe";
        vc64.wingetRepoPath = "m/Microsoft/VCRedist/2015+/x64";
        vc64.preferredArch = "x64";
        vc64.installerArgs = "/install /passive /norestart";
        vc64.latestVersion = "Поиск...";
        {
            const std::string regVer = CheckVcRuntimeKey(
                L"SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\X64", KEY_WOW64_64KEY);
            const std::string uninstVer = FindBestVcUninstallVersion(apps, "x64");
            vc64.installedVersion =
                IsVersionNewer(uninstVer, regVer) ? uninstVer
                                                  : (!regVer.empty() ? regVer : uninstVer);
        }
        vc64.installed = !vc64.installedVersion.empty();
        vc64.updateAvailable = !vc64.installed;
        vc64.statusText = vc64.installed ? ("Установлен (" + vc64.installedVersion + ")") : "Отсутствует";
        result.push_back(std::move(vc64));

        SystemComponentItem vc86;
        vc86.id = "rt_vcredist_x86";
        vc86.category = ComponentCategory::GamingRuntime;
        vc86.name = "Visual C++ 2015–2022 Redistributable (x86)";
        vc86.vendor = "Microsoft";
        vc86.purposeDescription =
            "32-битные библиотеки среды выполнения C/C++ (MSVC). Необходимы для 32-битных игр, игровых лаунчеров (Steam и др.) и оверлеев.";
        vc86.hardwareConnected = true;
        vc86.officialPageUrl = "https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist";
        vc86.directInstallerUrl = "https://aka.ms/vs/17/release/vc_redist.x86.exe";
        vc86.wingetRepoPath = "m/Microsoft/VCRedist/2015+/x86";
        vc86.preferredArch = "x86";
        vc86.installerArgs = "/install /passive /norestart";
        vc86.latestVersion = "Поиск...";
        {
            std::string regVer = CheckVcRuntimeKey(
                L"SOFTWARE\\WOW6432Node\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\X86",
                KEY_WOW64_32KEY);
            if (regVer.empty()) {
                regVer = CheckVcRuntimeKey(
                    L"SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\X86",
                    KEY_WOW64_32KEY);
            }
            const std::string uninstVer = FindBestVcUninstallVersion(apps, "x86");
            vc86.installedVersion =
                IsVersionNewer(uninstVer, regVer) ? uninstVer
                                                  : (!regVer.empty() ? regVer : uninstVer);
        }
        vc86.installed = !vc86.installedVersion.empty();
        vc86.updateAvailable = !vc86.installed;
        vc86.statusText = vc86.installed ? ("Установлен (" + vc86.installedVersion + ")") : "Отсутствует";
        result.push_back(std::move(vc86));

        SystemComponentItem vc13;
        vc13.id = "rt_vc2013";
        vc13.category = ComponentCategory::GamingRuntime;
        vc13.name = "Visual C++ 2013 Redistributable (x64)";
        vc13.vendor = "Microsoft";
        vc13.purposeDescription =
            "Библиотеки C++ 2013 (msvcr120.dll / msvcp120.dll). Используются играми прошлых лет (2013–2018 гг.) и старыми игровыми движками.";
        vc13.hardwareConnected = true;
        vc13.officialPageUrl = "https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist";
        vc13.directInstallerUrl = "https://aka.ms/highdpimfc2013x64enu";
        vc13.installerArgs = "/install /passive /norestart";
        vc13.latestVersion = "12.0.40664.0";
        const std::string v13x64 = CheckVcRuntimeKey(
            L"SOFTWARE\\Microsoft\\VisualStudio\\12.0\\VC\\Runtimes\\x64", KEY_WOW64_64KEY);
        vc13.installed = !v13x64.empty();
        vc13.installedVersion = v13x64;
        vc13.updateAvailable = !vc13.installed;
        vc13.statusText = vc13.installed ? ("Установлен (" + vc13.installedVersion + ")") : "Не установлен";
        result.push_back(std::move(vc13));

        SystemComponentItem vc12;
        vc12.id = "rt_vc2012";
        vc12.category = ComponentCategory::GamingRuntime;
        vc12.name = "Visual C++ 2012 Redistributable (x64)";
        vc12.vendor = "Microsoft";
        vc12.purposeDescription =
            "Библиотеки C++ 2012 (msvcr110.dll / msvcp110.dll). Требуются для игр 2012–2016 гг. и ряда системных утилит.";
        vc12.hardwareConnected = true;
        vc12.officialPageUrl = "https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist";
        vc12.directInstallerUrl =
            "https://download.microsoft.com/download/1/6/B/16B06F60-3B20-4FF2-B699-5E9B7962F9AE/VSU_4/vcredist_x64.exe";
        vc12.installerArgs = "/install /passive /norestart";
        vc12.latestVersion = "11.0.61030.0";
        const std::string v12x64 = CheckVcRuntimeKey(
            L"SOFTWARE\\Microsoft\\VisualStudio\\11.0\\VC\\Runtimes\\x64", KEY_WOW64_64KEY);
        vc12.installed = !v12x64.empty();
        vc12.installedVersion = v12x64;
        vc12.updateAvailable = !vc12.installed;
        vc12.statusText = vc12.installed ? ("Установлен (" + vc12.installedVersion + ")") : "Не установлен";
        result.push_back(std::move(vc12));

        SystemComponentItem vc10;
        vc10.id = "rt_vc2010";
        vc10.category = ComponentCategory::GamingRuntime;
        vc10.name = "Visual C++ 2010 Redistributable (x64)";
        vc10.vendor = "Microsoft";
        vc10.purposeDescription =
            "Библиотеки C++ 2010 (msvcr100.dll / msvcp100.dll). Часто требуются для классических игр (2010–2015 гг.) и старых лаунчеров.";
        vc10.hardwareConnected = true;
        vc10.officialPageUrl = "https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist";
        vc10.directInstallerUrl =
            "https://download.microsoft.com/download/1/6/5/165255E7-1014-4D0A-B094-B6A430A6BFFC/vcredist_x64.exe";
        vc10.installerArgs = "/passive /norestart";
        vc10.latestVersion = "10.0.40219";
        const std::string v10x64 = CheckVcRuntimeKey(
            L"SOFTWARE\\Microsoft\\VisualStudio\\10.0\\VC\\VCRedist\\x64", KEY_WOW64_64KEY);
        vc10.installed = !v10x64.empty();
        vc10.installedVersion = v10x64;
        vc10.updateAvailable = !vc10.installed;
        vc10.statusText = vc10.installed ? ("Установлен (" + vc10.installedVersion + ")") : "Не установлен";
        result.push_back(std::move(vc10));

        // DirectX End-User Runtime (June 2010)
        SystemComponentItem dx;
        dx.id = "rt_dx_legacy";
        dx.category = ComponentCategory::GamingRuntime;
        dx.name = "DirectX End-User Runtime (June 2010 / D3DX9-11)";
        dx.vendor = "Microsoft";
        dx.purposeDescription =
            "Дополнительные игровые библиотеки DirectX (d3dx9_43.dll, d3dx10_43.dll, d3dx11_43.dll, XInput 1.3, XAudio 2.7), отсутствующие в чистой Windows 10/11.";
        dx.hardwareConnected = true;
        dx.officialPageUrl = "https://www.microsoft.com/en-us/download/details.aspx?id=35";
        dx.directInstallerUrl =
            "https://download.microsoft.com/download/8/4/A/84A35BF1-DAFE-4AE8-82AF-AD2AE20B6B14/directx_Jun2010_redist.exe";
        dx.latestVersion = "9.29.1974";
        std::error_code ec;
        const bool hasD3dx9 = std::filesystem::exists(sys32 / L"d3dx9_43.dll", ec);
        const bool hasD3dx11 = std::filesystem::exists(sys32 / L"d3dx11_43.dll", ec);
        const bool hasXinput = std::filesystem::exists(sys32 / L"xinput1_3.dll", ec);
        const bool hasWowD3dx9 = std::filesystem::exists(sysWow / L"d3dx9_43.dll", ec);
        dx.installed = hasD3dx9 && hasD3dx11 && hasXinput && hasWowD3dx9;
        dx.installedVersion = dx.installed ? "9.29.1974" : (hasD3dx9 ? "Частично" : "");
        dx.updateAvailable = !dx.installed;
        dx.statusText = dx.installed ? "Установлен (d3dx9/10/11)" : "Рекомендуется для игр";
        result.push_back(std::move(dx));

        // .NET Desktop Runtime 8 & 9
        SystemComponentItem dn8;
        dn8.id = "rt_dotnet8";
        dn8.category = ComponentCategory::GamingRuntime;
        dn8.name = ".NET Desktop Runtime 8.0 (x64)";
        dn8.vendor = "Microsoft";
        dn8.purposeDescription =
            "Среда выполнения .NET 8 (LTS) для десктопных программ, игровых мод-менеджеров, оверлеев и системных утилит.";
        dn8.hardwareConnected = true;
        dn8.officialPageUrl = "https://dotnet.microsoft.com/en-us/download/dotnet/8.0";
        dn8.latestVersion = "Поиск...";
        dn8.installedVersion = DetectHighestDotNetDesktop("8");
        dn8.installed = !dn8.installedVersion.empty();
        dn8.statusText = dn8.installed ? ("Установлен (" + dn8.installedVersion + ")") : "Не установлен";
        result.push_back(std::move(dn8));

        SystemComponentItem dn9;
        dn9.id = "rt_dotnet9";
        dn9.category = ComponentCategory::GamingRuntime;
        dn9.name = ".NET Desktop Runtime 9.0 (x64)";
        dn9.vendor = "Microsoft";
        dn9.purposeDescription =
            "Актуальная среда выполнения .NET 9 для современных утилит Windows, лаунчеров, менеджеров модов и инструментов периферии.";
        dn9.hardwareConnected = true;
        dn9.officialPageUrl = "https://dotnet.microsoft.com/en-us/download/dotnet/9.0";
        dn9.latestVersion = "Поиск...";
        dn9.installedVersion = DetectHighestDotNetDesktop("9");
        dn9.installed = !dn9.installedVersion.empty();
        dn9.statusText = dn9.installed ? ("Установлен (" + dn9.installedVersion + ")") : "Не установлен";
        result.push_back(std::move(dn9));

        SystemComponentItem dn10;
        dn10.id = "rt_dotnet10";
        dn10.category = ComponentCategory::GamingRuntime;
        dn10.name = ".NET Desktop Runtime 10.0 (x64)";
        dn10.vendor = "Microsoft";
        dn10.purposeDescription =
            "Актуальная LTS-среда выполнения .NET 10 для новейших программ, утилит Windows, лаунчеров и инструментов настройки.";
        dn10.hardwareConnected = true;
        dn10.officialPageUrl = "https://dotnet.microsoft.com/en-us/download/dotnet/10.0";
        dn10.latestVersion = "Поиск...";
        dn10.installedVersion = DetectHighestDotNetDesktop("10");
        dn10.installed = !dn10.installedVersion.empty();
        dn10.statusText = dn10.installed ? ("Установлен (" + dn10.installedVersion + ")") : "Не установлен";
        result.push_back(std::move(dn10));

        // .NET Framework 4.8+
        SystemComponentItem netFx;
        netFx.id = "rt_netfx48";
        netFx.category = ComponentCategory::GamingRuntime;
        netFx.name = ".NET Framework 4.8 / 4.8.1";
        netFx.vendor = "Microsoft";
        netFx.purposeDescription =
            "Классическая платформа .NET в составе Windows. Требуется для большого числа игр, конфигураторов оборудования и драйверных панелей.";
        netFx.hardwareConnected = true;
        netFx.officialPageUrl = "https://dotnet.microsoft.com/en-us/download/dotnet-framework";
        netFx.directInstallerUrl = "https://go.microsoft.com/fwlink/?LinkId=2203304";
        netFx.installerArgs = "/passive /norestart";
        netFx.latestVersion = "4.8.1";
        netFx.compareVersions = false;
        {
            RegKeyGuard key;
            DWORD release = 0;
            if (RegOpenKeyExW(
                    HKEY_LOCAL_MACHINE,
                    L"SOFTWARE\\Microsoft\\NET Framework Setup\\NDP\\v4\\Full",
                    0,
                    KEY_READ,
                    key.Put()) == ERROR_SUCCESS) {
                ReadRegDword(key.Get(), L"Release", release);
                netFx.installedVersion = WideToUtf8(ReadRegStr(key.Get(), L"Version"));
            }
            if (release >= 533320) {
                netFx.installed = true;
                netFx.updateAvailable = false;
                if (netFx.installedVersion.empty()) netFx.installedVersion = "4.8.1";
                netFx.statusText = "Актуален (4.8.1)";
            } else if (release >= 528040) {
                netFx.installed = true;
                netFx.updateAvailable = true;
                if (netFx.installedVersion.empty()) netFx.installedVersion = "4.8";
                netFx.statusText = "Доступно обновление (4.8.1)";
            } else {
                netFx.installed = false;
                netFx.updateAvailable = true;
                netFx.statusText = "Требуется установка (4.8.1)";
            }
        }
        result.push_back(std::move(netFx));

        // WebView2 Runtime
        SystemComponentItem wv2;
        wv2.id = "rt_webview2";
        wv2.category = ComponentCategory::GamingRuntime;
        wv2.name = "Microsoft Edge WebView2 Runtime";
        wv2.vendor = "Microsoft";
        wv2.purposeDescription =
            "Встраиваемый браузерный движок Microsoft. Нужен для работы современных лаунчеров, меню настроек периферии и виджетов Windows 11.";
        wv2.hardwareConnected = true;
        wv2.officialPageUrl = "https://developer.microsoft.com/en-us/microsoft-edge/webview2/";
        wv2.directInstallerUrl = "https://go.microsoft.com/fwlink/p/?LinkId=2124703";
        wv2.installerArgs = "/silent /install";
        wv2.latestVersion = "Поиск...";
        {
            RegKeyGuard key;
            const wchar_t* wvSubKey =
                L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, wvSubKey, 0, KEY_READ | KEY_WOW64_32KEY, key.Put()) ==
                ERROR_SUCCESS) {
                wv2.installedVersion = WideToUtf8(ReadRegStr(key.Get(), L"pv"));
                wv2.installed = !wv2.installedVersion.empty();
            }
        }
        wv2.updateAvailable = !wv2.installed;
        wv2.statusText = wv2.installed ? ("Установлен (" + wv2.installedVersion + ")") : "Отсутствует";
        result.push_back(std::move(wv2));

        // Vulkan Runtime
        SystemComponentItem vk;
        vk.id = "rt_vulkan";
        vk.category = ComponentCategory::GamingRuntime;
        vk.name = "Vulkan Runtime (vulkan-1.dll)";
        vk.vendor = "LunarG / Khronos";
        vk.purposeDescription =
            "Системный загрузчик графического API Vulkan. Отвечает за запуск игр на движке Vulkan, эмуляторов и трансляторов DXVK.";
        vk.hardwareConnected = true;
        vk.officialPageUrl = "https://vulkan.lunarg.com/sdk/home";
        vk.latestVersion = "Поиск...";
        {
            const std::string v64 = GetFileVersionUtf8(sys32 / L"vulkan-1.dll");
            const std::string v32 = GetFileVersionUtf8(sysWow / L"vulkan-1.dll");
            vk.installedVersion = IsVersionNewer(v32, v64) ? v32 : (!v64.empty() ? v64 : v32);
        }
        vk.installed = !vk.installedVersion.empty();
        vk.statusText = vk.installed ? ("Установлен (" + vk.installedVersion + ")") : "Не найден";
        result.push_back(std::move(vk));

        // OpenAL
        SystemComponentItem oal;
        oal.id = "rt_openal";
        oal.category = ComponentCategory::GamingRuntime;
        oal.name = "OpenAL Audio Library (OpenAL32.dll)";
        oal.vendor = "OpenAL";
        oal.purposeDescription =
            "Библиотека пространственного 3D-звука OpenAL. Нужна для корректной работы звука в классических и инди-играх.";
        oal.hardwareConnected = true;
        oal.officialPageUrl = "https://www.openal.org/downloads/";
        oal.directInstallerUrl = "https://www.openal.org/downloads/oalinst.zip";
        oal.installerArgs = "/s";
        oal.latestVersion = "1.1 (OpenAL.org)";
        oal.compareVersions = false;
        oal.installedVersion = GetFileVersionUtf8(sys32 / L"OpenAL32.dll");
        if (oal.installedVersion.empty()) {
            oal.installedVersion = GetFileVersionUtf8(sysWow / L"OpenAL32.dll");
        }
        oal.installed = !oal.installedVersion.empty();
        oal.updateAvailable = !oal.installed;
        oal.statusText = oal.installed ? ("Установлен (" + oal.installedVersion + ")") : "Не установлен";
        result.push_back(std::move(oal));
    }

    // 2. Detected Peripherals mapped to official vendor software
    {
        std::map<std::string, std::vector<std::string>> brandToDevices;
        for (const auto& d : devicesSnapshot) {
            if (!d.peripheralBrand.empty()) {
                auto& list = brandToDevices[d.peripheralBrand];
                if (std::find(list.begin(), list.end(), d.name) == list.end() && list.size() < 3) {
                    list.push_back(d.name);
                }
            }
        }

        for (const auto& rule : kPeripheralRules) {
            auto it = brandToDevices.find(rule.brand);
            const bool connected = (it != brandToDevices.end());
            const InstalledAppEntry* app = FindInstalledApp(
                apps, {rule.uninstallKeywords[0], rule.uninstallKeywords[1], rule.uninstallKeywords[2]});

            // File-path fallbacks when a vendor installer updates/registers without standard Uninstall metadata
            std::string fallbackVer;
            if (app == nullptr) {
                if (std::string_view(rule.id) == "sw_logitech" && !progFiles.empty()) {
                    fallbackVer = GetFileVersionUtf8(progFiles / L"LGHUB" / L"lghub.exe");
                } else if (std::string_view(rule.id) == "sw_steelseries" && !progFiles.empty()) {
                    fallbackVer = GetFileVersionUtf8(
                        progFiles / L"SteelSeries" / L"GG" / L"SteelSeriesGG.exe");
                } else if (std::string_view(rule.id) == "sw_razer") {
                    if (!progFiles.empty()) {
                        fallbackVer = GetFileVersionUtf8(
                            progFiles / L"Razer" / L"RazerAppEngine" / L"RazerAppEngine.exe");
                    }
                    if (fallbackVer.empty() && !progFilesX86.empty()) {
                        fallbackVer = GetFileVersionUtf8(
                            progFilesX86 / L"Razer" / L"Synapse3" / L"WPFUI" / L"Framework" /
                            L"Razer Synapse 3 Host" / L"Razer Synapse 3.exe");
                    }
                }
            }

            if (!connected && app == nullptr && fallbackVer.empty()) {
                continue;
            }

            SystemComponentItem sw;
            sw.id = rule.id;
            sw.category = ComponentCategory::PeripheralSoftware;
            sw.name = rule.softwareName;
            sw.vendor = rule.brand;
            sw.purposeDescription = rule.purpose;
            sw.officialPageUrl = rule.pageUrl;
            if (rule.directUrl != nullptr) {
                sw.directInstallerUrl = rule.directUrl;
            }
            if (rule.wingetPath != nullptr) {
                sw.wingetRepoPath = rule.wingetPath;
                sw.preferredArch = "x64";
            }
            if (rule.nestedHint != nullptr) {
                sw.nestedInstallerHint = rule.nestedHint;
            }
            if (rule.silentArgs != nullptr) {
                sw.installerArgs = rule.silentArgs;
            }
            if (std::string_view(rule.id) == "sw_razer") {
                sw.compareVersions = false;
            }
            sw.latestVersion = rule.defaultLatestLabel;
            sw.hardwareConnected = connected;
            if (connected) {
                for (size_t i = 0; i < it->second.size(); ++i) {
                    if (i > 0) sw.detectedHardware += ", ";
                    sw.detectedHardware += it->second[i];
                }
            } else {
                sw.detectedHardware = "Обнаружено в системе";
            }

            if (app != nullptr) {
                sw.installed = true;
                sw.installedVersion = app->displayVersion.empty() ? "Установлен" : app->displayVersion;
                sw.statusText = "Установлен (" + sw.installedVersion + ")";
            } else if (!fallbackVer.empty()) {
                sw.installed = true;
                sw.installedVersion = fallbackVer;
                sw.statusText = "Установлен (" + fallbackVer + ")";
            } else {
                sw.installed = false;
                sw.updateAvailable = true;
                sw.statusText = "ПО не установлено";
            }
            result.push_back(std::move(sw));
        }
    }

    // 3. Monitors (via EDID) & GPUs
    {
        auto makeStableMonitorId = [](std::string_view seed) {
            std::string slug;
            slug.reserve(seed.size());
            bool prevUnderscore = false;
            for (unsigned char ch : seed) {
                if (std::isalnum(ch)) {
                    slug.push_back(static_cast<char>(std::tolower(ch)));
                    prevUnderscore = false;
                } else if (!prevUnderscore && !slug.empty()) {
                    slug.push_back('_');
                    prevUnderscore = true;
                }
            }
            while (!slug.empty() && slug.back() == '_') {
                slug.pop_back();
            }
            if (slug.empty()) {
                slug = "monitor";
            }
            if (slug.size() > 36) {
                slug.resize(36);
            }
            return std::string("mon_") + slug;
        };

        std::set<std::string> seenMonitorDevices;
        std::set<std::string> usedMonitorIds;
        std::unordered_map<std::string, size_t> knownBrandItemIdx;
        for (const auto& d : devicesSnapshot) {
            if (d.category != DeviceCategory::Monitor) {
                continue;
            }
            const std::string deviceKey = d.edidManufacturer + "|" + d.edidModel + "|" + d.name;
            if (!seenMonitorDevices.insert(deviceKey).second) {
                continue;
            }
            const MonitorBrandInfo brandInfo = GetMonitorBrandSupport(d.edidManufacturer, d.name);
            const InstalledAppEntry* app = brandInfo.uninstallKeyword
                                               ? FindInstalledApp(apps, {brandInfo.uninstallKeyword})
                                               : nullptr;

            std::string hwDesc = d.name;
            if (!d.driverVersion.empty() && !d.isGenericMonitorDriver) {
                hwDesc += " [INF " + d.driverVersion + "]";
            }

            const bool isKnownBrand =
                (brandInfo.softwareName != "Официальная поддержка монитора");
            const std::string brandDedupeKey =
                isKnownBrand ? (brandInfo.softwareName + "|" + brandInfo.url) : std::string();
            if (isKnownBrand) {
                const auto existingIt = knownBrandItemIdx.find(brandDedupeKey);
                if (existingIt != knownBrandItemIdx.end()) {
                    SystemComponentItem& existing = result[existingIt->second];
                    if (existing.detectedHardware.find(hwDesc) == std::string::npos) {
                        if (!existing.detectedHardware.empty()) {
                            existing.detectedHardware += "; ";
                        }
                        existing.detectedHardware += hwDesc;
                    }
                    const bool thisInstalled = !d.isGenericMonitorDriver || (app != nullptr);
                    if (!thisInstalled && app == nullptr) {
                        existing.updateAvailable = true;
                        existing.installed = false;
                        existing.installedVersion = d.driverVersion + " (" + d.infName + ")";
                        existing.compareVersions = false;
                        existing.statusText = "Базовый драйвер (monitor.inf)";
                    }
                    continue;
                }
            }

            SystemComponentItem mon;
            const std::string idSeed =
                isKnownBrand
                    ? (brandInfo.wingetPath != nullptr ? std::string(brandInfo.wingetPath)
                                                       : brandInfo.softwareName)
                    : (d.edidManufacturer + "_" + d.edidModel + "_" + d.name);
            std::string baseId = makeStableMonitorId(idSeed);
            std::string uniqueId = baseId;
            int suffixIdx = 2;
            while (!usedMonitorIds.insert(uniqueId).second) {
                uniqueId = baseId + "_" + std::to_string(suffixIdx++);
            }
            mon.id = std::move(uniqueId);
            mon.category = ComponentCategory::MonitorAndGpu;
            mon.name = brandInfo.softwareName;
            mon.vendor = d.manufacturer.empty() ? d.edidManufacturer : d.manufacturer;
            mon.purposeDescription =
                "Фирменная утилита и INF-драйвер монитора: отвечает за точный цветовой профиль (ICC), управление яркостью/режимами OSD с ПК и поддержку высокой герцовки/HDR.";
            mon.detectedHardware = hwDesc;
            mon.officialPageUrl = brandInfo.url;
            if (brandInfo.fallbackDirectUrl != nullptr) {
                mon.directInstallerUrl = brandInfo.fallbackDirectUrl;
            }
            if (brandInfo.fallbackNestedHint != nullptr) {
                mon.nestedInstallerHint = brandInfo.fallbackNestedHint;
            }
            if (brandInfo.fallbackArgs != nullptr) {
                mon.installerArgs = brandInfo.fallbackArgs;
            }
            if (brandInfo.wingetPath != nullptr) {
                mon.wingetRepoPath = brandInfo.wingetPath;
                mon.preferredArch = "x64";
                mon.latestVersion = "Поиск...";
            } else {
                mon.latestVersion = "Портал поддержки";
            }
            mon.hardwareConnected = true;
            mon.installed = !d.isGenericMonitorDriver || (app != nullptr);
            if (app != nullptr && !app->displayVersion.empty()) {
                mon.installedVersion = app->displayVersion;
                mon.compareVersions = true;
                mon.statusText = "Установлено (" + app->displayVersion + ")";
            } else if (!d.isGenericMonitorDriver) {
                mon.installedVersion = d.driverVersion.empty() ? d.infName : (d.driverVersion + " (INF)");
                mon.compareVersions = false;
                mon.statusText = "Вендор-драйвер (" + (d.driverVersion.empty() ? d.infName : d.driverVersion) + ")";
            } else if (app != nullptr) {
                mon.installedVersion = app->displayVersion;
                mon.compareVersions = false;
                mon.statusText = "Утилита установлена (" + d.infName + ")";
            } else {
                mon.installedVersion = d.driverVersion + " (" + d.infName + ")";
                mon.compareVersions = false;
                mon.updateAvailable = true;
                mon.statusText = "Базовый драйвер (monitor.inf)";
            }
            if (isKnownBrand) {
                knownBrandItemIdx[brandDedupeKey] = result.size();
            }
            result.push_back(std::move(mon));
        }

        bool hasNvidia = false, hasAmd = false, hasIntelGpu = false;
        bool hasAmdChipset = false, hasIntelChipset = false, hasIntelNetOrBt = false;
        std::string nvName, nvInfVer, amdName, intelDsaSummary;
        std::string amdChipsetVer, amdChipsetInfName;

        auto appendSummary = [](std::string& dst, const std::string& part) {
            if (part.empty()) return;
            if (!dst.empty()) dst += "; ";
            if (dst.size() < 110) dst += part;
        };

        for (const auto& d : devicesSnapshot) {
            const std::string cls = ToLowerAscii(d.deviceClass);
            if (cls == "display") {
                if (d.pciVendorId == "10DE") {
                    hasNvidia = true;
                    nvName = d.name + " [" + d.driverVersion + "]";
                    nvInfVer = FormatNvidiaDriverVersion(d.driverVersion);
                } else if (d.pciVendorId == "1002") {
                    hasAmd = true;
                    amdName = d.name + " [" + d.driverVersion + "]";
                } else if (d.pciVendorId == "8086") {
                    hasIntelGpu = true;
                    appendSummary(intelDsaSummary, d.name + " [" + d.driverVersion + "]");
                }
            } else if (cls == "system" || cls == "hdc" || cls == "usb") {
                if (d.pciVendorId == "1022") {
                    hasAmdChipset = true;
                    if (ContainsCaseInsensitive(d.driverProvider, "Advanced Micro Devices") &&
                        (ContainsCaseInsensitive(d.name, "SMBus") ||
                         ContainsCaseInsensitive(d.name, "AMD PCI") ||
                         ContainsCaseInsensitive(d.name, "GPIO"))) {
                        amdChipsetVer = d.driverVersion;
                        amdChipsetInfName = d.name;
                    }
                } else if (d.pciVendorId == "8086" &&
                           (ContainsCaseInsensitive(d.name, "Chipset") ||
                            ContainsCaseInsensitive(d.name, "SMBus") ||
                            ContainsCaseInsensitive(d.name, "LPC") ||
                            ContainsCaseInsensitive(d.name, "Management Engine"))) {
                    hasIntelChipset = true;
                    appendSummary(intelDsaSummary, d.name);
                }
            } else if ((cls == "net" || cls == "bluetooth") &&
                       (d.pciVendorId == "8086" || d.usbVendorId == "8087")) {
                hasIntelNetOrBt = true;
                appendSummary(intelDsaSummary, d.name);
            }
        }

        if (hasNvidia) {
            const InstalledAppEntry* nvApp = FindInstalledApp(
                apps, {"NVIDIA Графический драйвер", "NVIDIA Graphics Driver", "NVIDIA App"});
            SystemComponentItem gpu;
            gpu.id = "gpu_nvidia";
            gpu.category = ComponentCategory::MonitorAndGpu;
            gpu.name = "NVIDIA GeForce Game Ready Driver";
            gpu.vendor = "NVIDIA";
            gpu.purposeDescription =
                "Официальный драйвер видеокарты NVIDIA: отвечает за FPS и совместимость в новых играх, работу DLSS, G-SYNC, NVENC и аппаратное ускорение Windows.";
            gpu.detectedHardware = nvName;
            gpu.officialPageUrl = "https://www.nvidia.com/en-us/geforce/drivers/";
            gpu.latestVersion = "Поиск...";
            gpu.hardwareConnected = true;
            gpu.installed = !nvInfVer.empty() || (nvApp != nullptr);
            gpu.installedVersion = !nvInfVer.empty() ? nvInfVer : (nvApp ? nvApp->displayVersion : "");
            gpu.statusText = gpu.installed ? ("Установлен (" + gpu.installedVersion + ")") : "Не найден";
            result.push_back(std::move(gpu));
        }
        if (hasAmd) {
            const InstalledAppEntry* amdApp = FindInstalledApp(
                apps, {"AMD Software", "Radeon Software", "AMD Radeon"});
            SystemComponentItem gpu;
            gpu.id = "gpu_amd";
            gpu.category = ComponentCategory::MonitorAndGpu;
            gpu.name = "AMD Software: Adrenalin Edition";
            gpu.vendor = "AMD";
            gpu.purposeDescription =
                "Официальный пакет драйверов AMD Radeon: отвечает за игровую производительность, FSR, Anti-Lag, FreeSync и аппаратное ускорение.";
            gpu.detectedHardware = amdName;
            gpu.officialPageUrl = "https://www.amd.com/en/support/download/drivers.html";
            gpu.latestVersion = "Поиск...";
            gpu.hardwareConnected = true;
            gpu.installed = (amdApp != nullptr);
            gpu.installedVersion = amdApp ? amdApp->displayVersion : "";
            gpu.statusText = amdApp ? ("Установлен (" + gpu.installedVersion + ")") : "Официальный портал AMD";
            result.push_back(std::move(gpu));
        }
        if (hasIntelGpu || hasIntelChipset || hasIntelNetOrBt) {
            const InstalledAppEntry* intelApp = FindInstalledApp(
                apps, {"Intel Driver & Support", "Intel Graphics", "Intel(R) Chipset Device Software"});
            SystemComponentItem intelDsa;
            intelDsa.id = "intel_dsa";
            intelDsa.category = ComponentCategory::MonitorAndGpu;
            intelDsa.name = "Intel Driver & Support Assistant";
            intelDsa.vendor = "Intel";
            intelDsa.purposeDescription =
                "Официальный инструмент Intel для подбора и обновления драйверов графики, Wi-Fi, Bluetooth и чипсета Intel.";
            intelDsa.detectedHardware =
                intelDsaSummary.empty() ? "Контроллеры Intel" : intelDsaSummary;
            intelDsa.officialPageUrl = "https://www.intel.com/content/www/us/en/support/detect.html";
            intelDsa.directInstallerUrl = "https://dsadata.intel.com/installer";
            intelDsa.wingetRepoPath = "i/Intel/IntelDriverAndSupportAssistant";
            intelDsa.preferredArch = "x64";
            intelDsa.latestVersion = "Поиск...";
            intelDsa.hardwareConnected = true;
            intelDsa.installed = (intelApp != nullptr);
            intelDsa.installedVersion = intelApp ? intelApp->displayVersion : "";
            intelDsa.statusText = intelApp
                ? ("Установлен (" + intelDsa.installedVersion + ")")
                : "Официальный помощник Intel";
            result.push_back(std::move(intelDsa));
        }

        if (hasAmdChipset) {
            const InstalledAppEntry* amdChipApp = FindInstalledApp(
                apps, {"AMD Chipset Software", "AMD_Chipset_Drivers", "AMD Chipset Drivers"});
            SystemComponentItem chip;
            chip.id = "chip_amd";
            chip.category = ComponentCategory::MonitorAndGpu;
            chip.name = "AMD Chipset Drivers (Чипсет / PCI / PSP / SMBus)";
            chip.vendor = "AMD";
            chip.purposeDescription =
                "Официальный пакет драйверов чипсета AMD Ryzen / AM4 / AM5 (энергосбережение, USB, PCIe, GPIO, PSP).";
            chip.detectedHardware = "Чипсет AMD (PCI\\VEN_1022)";
            chip.officialPageUrl =
                "https://www.amd.com/en/support/downloads/drivers.html/chipsets/am4/b550.html";
            chip.directInstallerUrl =
                "https://drivers.amd.com/drivers/amd_chipset_software_8.09.28.1124.exe";
            chip.latestVersion = "Поиск...";
            chip.hardwareConnected = true;
            chip.installed = (amdChipApp != nullptr) || !amdChipsetVer.empty();
            chip.compareVersions = (amdChipApp != nullptr && !amdChipApp->displayVersion.empty());
            if (amdChipApp && !amdChipApp->displayVersion.empty()) {
                chip.installedVersion = amdChipApp->displayVersion;
            } else if (!amdChipsetVer.empty()) {
                chip.installedVersion = amdChipsetVer;
            }
            if (chip.installed) {
                if (amdChipApp) {
                    chip.statusText = "Установлен (" + chip.installedVersion + ")";
                } else {
                    chip.statusText = "Драйверы AMD (" + chip.installedVersion +
                        (!amdChipsetInfName.empty() ? ", " + amdChipsetInfName : "") + ")";
                }
            } else {
                chip.statusText = "Рекомендуется пакет чипсета AMD";
            }
            result.push_back(std::move(chip));
        }

        // Physical LAN, Wi-Fi, Bluetooth, and Onboard Audio controllers resolved via MS Update Catalog (.cab)
        std::set<std::string> seenControllers;
        for (const auto& d : devicesSnapshot) {
            if (d.searchHardwareKey.empty()) {
                continue;
            }
            const bool isVirtual =
                ContainsCaseInsensitive(d.name, "WAN Miniport") ||
                ContainsCaseInsensitive(d.name, "VirtualBox") ||
                ContainsCaseInsensitive(d.name, "Radmin") ||
                ContainsCaseInsensitive(d.name, "VMware") ||
                ContainsCaseInsensitive(d.name, "Hyper-V") ||
                ContainsCaseInsensitive(d.name, "Kernel Debug") ||
                ContainsCaseInsensitive(d.name, "TAP-") ||
                ContainsCaseInsensitive(d.name, "Loopback") ||
                ContainsCaseInsensitive(d.name, "SteelSeries Sonar") ||
                ContainsCaseInsensitive(d.name, "NVIDIA Broadcast") ||
                ContainsCaseInsensitive(d.name, "NVIDIA Virtual Audio");
            if (isVirtual) {
                continue;
            }

            const std::string cls = ToLowerAscii(d.deviceClass);
            const bool isPci = ContainsCaseInsensitive(d.hardwareId, "PCI\\VEN_");
            const bool isUsb = ContainsCaseInsensitive(d.hardwareId, "USB\\VID_");

            std::string idPrefix;
            std::string rolePrefix;
            std::string purpose;
            if (cls == "net" && isPci) {
                const bool isWifi =
                    ContainsCaseInsensitive(d.name, "Wi-Fi") ||
                    ContainsCaseInsensitive(d.name, "Wireless") ||
                    ContainsCaseInsensitive(d.name, "WLAN") ||
                    ContainsCaseInsensitive(d.name, "AX");
                idPrefix = isWifi ? "drv_wifi_" : "drv_lan_";
                rolePrefix = isWifi ? "Wi-Fi адаптер: " : "Сеть (LAN): ";
                purpose = isWifi
                    ? "Официальный WHQL-драйвер беспроводного контроллера из Microsoft Update Catalog (.cab)."
                    : "Официальный WHQL-драйвер проводного сетевого контроллера из Microsoft Update Catalog (.cab).";
            } else if (cls == "bluetooth" && (isPci || isUsb) &&
                       !ContainsCaseInsensitive(d.name, "Enumerator")) {
                idPrefix = "drv_bt_";
                rolePrefix = "Bluetooth: ";
                purpose = "Официальный WHQL-драйвер адаптера Bluetooth из Microsoft Update Catalog (.cab).";
            } else if (cls == "media" &&
                       (ContainsCaseInsensitive(d.hardwareId, "HDAUDIO\\") ||
                        ContainsCaseInsensitive(d.hardwareId, "INTELAUDIO\\")) &&
                       d.pciVendorId != "10DE") {
                idPrefix = "drv_audio_";
                rolePrefix = "Аудио платы: ";
                purpose = "Официальный WHQL-драйвер встроенного аудиокодека платы из Microsoft Update Catalog (.cab).";
            } else {
                continue;
            }

            const std::string itemId = idPrefix + d.searchHardwareKey;
            if (!seenControllers.insert(itemId).second) {
                continue;
            }

            const std::string subsysKey = ExtractVenDevSubsysKey(d.hardwareId);
            const bool isAudio = (idPrefix == "drv_audio_");

            SystemComponentItem drv;
            drv.id = itemId;
            drv.category = ComponentCategory::MonitorAndGpu;
            drv.name = rolePrefix + d.name;
            drv.vendor = d.manufacturer.empty() ? "OEM / Microsoft Catalog" : d.manufacturer;
            drv.purposeDescription = purpose;
            drv.detectedHardware =
                (!subsysKey.empty() ? subsysKey : d.searchHardwareKey) +
                (d.infName.empty() ? "" : (" (" + d.infName + ")"));
            drv.catalogSearchKey = !subsysKey.empty() ? subsysKey : d.searchHardwareKey;
            drv.catalogFallbackKey =
                (!subsysKey.empty() && !isAudio) ? d.searchHardwareKey : std::string();
            drv.officialPageUrl =
                "https://www.catalog.update.microsoft.com/Search.aspx?q=" +
                UrlEncodeCatalogParam(drv.catalogSearchKey);
            drv.latestVersion = "Поиск...";
            drv.hardwareConnected = true;
            drv.installed = d.hasDriver && !d.driverVersion.empty();
            drv.installedVersion = d.driverVersion;
            drv.installedDateYmd = ParseDriverDateYmd(d.driverDate);
            if (drv.installedDateYmd == 0) {
                TryExtractTrailingVersionDate(ParseVersionParts(d.driverVersion), drv.installedDateYmd);
            }
            drv.updateAvailable = !drv.installed;
            drv.statusText = drv.installed
                ? ("Установлен (" + drv.installedVersion + ")")
                : "Драйвер не установлен";
            result.push_back(std::move(drv));
        }
    }

    return result;
}

} // namespace hdm
