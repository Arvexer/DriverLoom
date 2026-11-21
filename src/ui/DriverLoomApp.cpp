#include "ui/DriverLoomApp.hpp"

#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

DriverLoomApp::DriverLoomApp(HWND mainHwnd, int initialTab)
    : hwnd_(mainHwnd),
      isElevated_(hdm::IsProcessElevated()),
      pendingInitialTab_(initialTab) {
    logBuffer_.Push(
        hdm::LogLevel::Info,
        std::string(hdm::Tr(
            "DriverLoom запущен. Режим прав: ",
            "DriverLoom запущено. Режим прав: ",
            "DriverLoom started. Privilege mode: ")) +
            (isElevated_
                 ? hdm::Tr("Администратор", "Адміністратор", "Administrator")
                 : hdm::Tr("Обычный (UAC по запросу)", "Звичайний (UAC за запитом)", "Standard (UAC on demand)")));
    LoadBaselineSnapshot();
    scanner_.StartScanAsync(MakeLogger());
    if (g_autoCheckAppUpdates) {
        StartAppUpdateCheckAsync(false);
    }
}

DriverLoomApp::~DriverLoomApp() {
    if (appUpdateThread_.joinable()) {
        appUpdateThread_.join();
    }
}

void DriverLoomApp::RenderFrame() {
    SyncScannerSnapshots();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    constexpr ImGuiWindowFlags kRootFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings;

    ImGui::Begin("##MainWorkspace", nullptr, kRootFlags);

    RenderHeaderBar();
    if (rebootRequired_.load()) {
        RenderRebootBanner();
    }

    const float availHeight = ImGui::GetContentRegionAvail().y;
    const float spacingY = ImGui::GetStyle().ItemSpacing.y;
    const float logPanelHeight = std::clamp(availHeight * 0.20f, 128.0f, 195.0f);
    const float tabAreaHeight = std::max(280.0f, availHeight - logPanelHeight - spacingY);

    ImGui::BeginChild(
        "##TabsRegion",
        ImVec2(0.0f, tabAreaHeight),
        false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
    auto pushMainTabBarStyle = [uiScale]() {
        PushFontSafe(g_fontMainTab);
        ImGui::PushStyleVar(
            ImGuiStyleVar_FramePadding,
            ImVec2(std::round(14.0f * uiScale), std::round(7.0f * uiScale)));
        ImGui::PushStyleVar(ImGuiStyleVar_TabBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_TabBarBorderSize, std::round(1.0f * uiScale));
        ImGui::PushStyleVar(ImGuiStyleVar_TabBarOverlineSize, std::max(2.0f, 2.5f * uiScale));
        if (g_isLightTheme) {
            ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.89f, 0.92f, 0.96f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.93f, 0.96f, 0.99f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_TabSelected, ImVec4(0.99f, 0.995f, 1.00f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, ImVec4(0.14f, 0.45f, 0.88f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_TabDimmed, ImVec4(0.89f, 0.92f, 0.96f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, ImVec4(0.99f, 0.995f, 1.00f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_TabDimmedSelectedOverline, ImVec4(0.14f, 0.45f, 0.88f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.75f, 0.80f, 0.87f, 0.90f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.072f, 0.088f, 0.122f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.108f, 0.138f, 0.195f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_TabSelected, ImVec4(0.128f, 0.172f, 0.248f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, ImVec4(0.36f, 0.70f, 1.00f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_TabDimmed, ImVec4(0.072f, 0.088f, 0.122f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, ImVec4(0.128f, 0.172f, 0.248f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_TabDimmedSelectedOverline, ImVec4(0.36f, 0.70f, 1.00f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.15f, 0.19f, 0.26f, 0.85f));
        }
    };
    auto popMainTabBarStyle = []() {
        ImGui::PopStyleColor(8);
        ImGui::PopStyleVar(4);
        PopFontSafe(g_fontMainTab);
    };

    pushMainTabBarStyle();
    if (ImGui::BeginTabBar("##MainTabs", ImGuiTabBarFlags_DrawSelectedOverline)) {
        auto tabFlag = [this](int idx) {
            return (pendingInitialTab_ == idx)
                       ? ImGuiTabItemFlags_SetSelected
                       : ImGuiTabItemFlags_None;
        };
        auto beginMainTab = [&](const char* label, int idx) -> bool {
            const bool isSel =
                (pendingInitialTab_ >= 0) ? (pendingInitialTab_ == idx) : (activeTabIndex_ == idx);
            const ImVec4 borderCol =
                g_isLightTheme
                    ? (isSel ? ImVec4(0.32f, 0.54f, 0.84f, 0.95f) : ImVec4(0.75f, 0.80f, 0.87f, 0.90f))
                    : (isSel ? ImVec4(0.28f, 0.46f, 0.72f, 0.95f) : ImVec4(0.15f, 0.19f, 0.26f, 0.85f));
            const ImVec4 textCol =
                g_isLightTheme
                    ? (isSel ? ImVec4(0.06f, 0.14f, 0.28f, 1.00f) : ImVec4(0.36f, 0.42f, 0.52f, 1.00f))
                    : (isSel ? ImVec4(0.97f, 0.99f, 1.00f, 1.00f) : ImVec4(0.60f, 0.67f, 0.78f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, borderCol);
            ImGui::PushStyleColor(ImGuiCol_Text, textCol);
            const bool open = ImGui::BeginTabItem(label, nullptr, tabFlag(idx));
            ImGui::PopStyleColor(2);
            return open;
        };
        auto runTabCard = [&](const char* childId, auto&& drawFn) {
            popMainTabBarStyle();
            const ImVec4 cardBorder = g_isLightTheme
                                          ? ImVec4(0.68f, 0.75f, 0.85f, 0.95f)
                                          : ImVec4(0.22f, 0.31f, 0.44f, 0.95f);
            ImGui::PushStyleColor(ImGuiCol_Border, cardBorder);
            ImGui::BeginChild(
                childId,
                ImVec2(0.0f, 0.0f),
                true,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleColor();
            drawFn();
            ImGui::EndChild();
            pushMainTabBarStyle();
        };

        if (beginMainTab(
                hdm::Tr(
                    "  Устройства и мониторы  ###Tab0",
                    "  Пристрої та монітори  ###Tab0",
                    "  Devices & Monitors  ###Tab0"),
                0)) {
            activeTabIndex_ = 0;
            runTabCard("##TabCardDevices", [&]() { RenderDevicesTab(); });
            ImGui::EndTabItem();
        }
        if (beginMainTab(
                hdm::Tr(
                    "  Компоненты и периферия  ###Tab1",
                    "  Компоненти та периферія  ###Tab1",
                    "  Components & Peripherals  ###Tab1"),
                1)) {
            activeTabIndex_ = 1;
            runTabCard("##TabCardComponents", [&]() { RenderComponentsTab(); });
            ImGui::EndTabItem();
        }
        if (beginMainTab("  Windows Update  ###Tab2", 2)) {
            activeTabIndex_ = 2;
            runTabCard("##TabCardWu", [&]() { RenderWindowsUpdateTab(); });
            ImGui::EndTabItem();
        }
        if (g_isAdvancedMode &&
            beginMainTab(
                hdm::Tr(
                    "  Локальные (.inf)  ###Tab3",
                    "  Локальні (.inf)  ###Tab3",
                    "  Local (.inf)  ###Tab3"),
                3)) {
            activeTabIndex_ = 3;
            runTabCard("##TabCardLocal", [&]() { RenderLocalInstallerTab(); });
            ImGui::EndTabItem();
        }
        if (beginMainTab(
                hdm::Tr(
                    "  Система и диагностика  ###Tab4",
                    "  Система та діагностика  ###Tab4",
                    "  System & Diagnostics  ###Tab4"),
                4)) {
            activeTabIndex_ = 4;
            runTabCard("##TabCardDiag", [&]() { RenderDiagnosticsTab(); });
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    popMainTabBarStyle();
    pendingInitialTab_ = -1;
    HandleKeyboardShortcuts();
    ImGui::EndChild();

    RenderLogPanel();

    RenderAboutModal();

    ImGui::End();
}

bool DriverLoomApp::IsInitialScanSettled() const noexcept {
    return lastDeviceGen_ > 0 && lastComponentsGen_ > 0 && lastDiagGen_ > 0 &&
           lastDriverStoreGen_ > 0 &&
           !scanner_.IsScanning() && !componentsScanner_.IsScanning() &&
           !componentsScanner_.IsCheckingOnline() &&
           !diagnostics_.IsScanning() && !localInstaller_.IsScanningStore();
}
void DriverLoomApp::ConfigureCaptureState(
    int tab,
    int diagSec,
    int localSec,
    bool advancedMode,
    bool lightTheme,
    int selectDevRow) {
    g_isAdvancedMode = advancedMode;
    if (g_isLightTheme != lightTheme) {
        ApplyAppTheme(hwnd_, g_dpiScale, lightTheme);
    }
    activeTabIndex_ = tab;
    pendingInitialTab_ = tab;
    diagSectionIndex_ = diagSec;
    localSectionIndex_ = localSec;
    pendingSelectDeviceRow_ = selectDevRow;
    if (selectDevRow < 0) {
        selectedDeviceInstanceId_.clear();
    }
}

hdm::LogCallback DriverLoomApp::MakeLogger() {
    return [this](hdm::LogLevel lvl, const std::string& msg) {
        logBuffer_.Push(lvl, msg);
    };
}

std::string DriverLoomApp::FormatSnapshotEntrySummary(
    const std::string& driverVersion,
    const std::string& driverDate,
    const std::string& infName,
    const std::string& statusText) {
    std::string s;
    if (!driverVersion.empty()) {
        s = driverVersion;
        if (!driverDate.empty()) {
            s += " (" + driverDate + ")";
        }
        if (!infName.empty()) {
            s += " [" + infName + "]";
        }
    } else if (!infName.empty()) {
        s = "[" + infName + "]";
    }
    const std::string st = hdm::TrDyn(statusText);
    if (!st.empty()) {
        if (!s.empty()) {
            s += "  |  ";
        }
        s += st;
    }
    return s.empty() ? "—" : s;
}

void DriverLoomApp::LoadBaselineSnapshot() {
    snapshotLoadedFromReg_ =
        LoadBaselineSnapshotFromRegistry(baselineTimestamp_, baselineEntries_);
}

void DriverLoomApp::CaptureCurrentAsBaseline(bool logNotification) {
    baselineTimestamp_ = FormatSnapshotTimestampNow();
    baselineEntries_.clear();
    baselineEntries_.reserve(devicesCache_.size());
    for (const auto& d : devicesCache_) {
        if (d.instanceId.empty()) {
            continue;
        }
        DriverBaselineEntry e;
        e.instanceId = d.instanceId;
        e.name = d.name;
        e.deviceClass = d.deviceClass;
        e.driverVersion = d.driverVersion;
        e.driverDate = d.driverDate;
        e.infName = d.infName;
        e.statusText = d.statusText;
        e.health = static_cast<int>(d.health);
        baselineEntries_.push_back(std::move(e));
    }
    snapshotLoadedFromReg_ = true;
    SaveBaselineSnapshotToRegistry(baselineTimestamp_, baselineEntries_);
    RebuildSnapshotDiff();

    if (logNotification) {
        logBuffer_.Push(
            hdm::LogLevel::Success,
            std::string(hdm::Tr(
                "Снимок состояния драйверов «До» обновлён (",
                "Знімок стану драйверів «До» оновлено (",
                "Driver baseline snapshot 'Before' updated (")) +
                baselineTimestamp_ + ", " +
                std::to_string(baselineEntries_.size()) +
                hdm::Tr(" устройств).", " пристроїв).", " devices)."));
    }
}

void DriverLoomApp::RebuildSnapshotDiff() {
    snapshotDiffRows_.clear();
    snapshotChangedCount_ = 0;
    lastSnapshotLang_ = hdm::GetLang();

    if (baselineEntries_.empty() && devicesCache_.empty()) {
        return;
    }

    std::unordered_map<std::string, const DriverBaselineEntry*> baseMap;
    baseMap.reserve(baselineEntries_.size());
    for (const auto& b : baselineEntries_) {
        if (!b.instanceId.empty()) {
            baseMap[hdm::ToLowerAscii(b.instanceId)] = &b;
        }
    }

    std::unordered_set<std::string> seenBaseKeys;
    seenBaseKeys.reserve(devicesCache_.size());

    for (const auto& cur : devicesCache_) {
        if (cur.instanceId.empty()) {
            continue;
        }
        const std::string key = hdm::ToLowerAscii(cur.instanceId);
        const auto it = baseMap.find(key);

        DriverSnapshotDiffRow row;
        row.instanceId = cur.instanceId;
        row.name = cur.name;
        row.deviceClass = cur.deviceClass;
        row.canRollback = cur.hasDriver;
        row.afterSummary = FormatSnapshotEntrySummary(
            cur.driverVersion, cur.driverDate, cur.infName, cur.statusText);

        if (it == baseMap.end()) {
            row.kind = SnapshotDiffKind::DeviceAdded;
            row.beforeSummary = hdm::Tr("Отсутствовало", "Було відсутнє", "Not present");
            ++snapshotChangedCount_;
        } else {
            seenBaseKeys.insert(key);
            const DriverBaselineEntry& base = *(it->second);
            row.beforeSummary = FormatSnapshotEntrySummary(
                base.driverVersion, base.driverDate, base.infName, base.statusText);

            const int curHealth = static_cast<int>(cur.health);
            const bool drvChanged =
                (base.driverVersion != cur.driverVersion) ||
                (base.driverDate != cur.driverDate) ||
                (hdm::ToLowerAscii(base.infName) != hdm::ToLowerAscii(cur.infName));
            const bool statusChanged =
                (base.health != curHealth) || (base.statusText != cur.statusText);

            if (base.health != 0 && curHealth == 0) {
                row.kind = SnapshotDiffKind::IssueFixed;
                ++snapshotChangedCount_;
            } else if (base.health == 0 && curHealth != 0) {
                row.kind = SnapshotDiffKind::NewIssue;
                ++snapshotChangedCount_;
            } else if (drvChanged) {
                row.kind = SnapshotDiffKind::DriverUpdated;
                ++snapshotChangedCount_;
            } else if (statusChanged) {
                row.kind = (curHealth > base.health)
                               ? SnapshotDiffKind::NewIssue
                               : SnapshotDiffKind::IssueFixed;
                ++snapshotChangedCount_;
            } else {
                row.kind = SnapshotDiffKind::Unchanged;
            }
        }
        snapshotDiffRows_.push_back(std::move(row));
    }

    for (const auto& base : baselineEntries_) {
        if (base.instanceId.empty()) {
            continue;
        }
        const std::string key = hdm::ToLowerAscii(base.instanceId);
        if (seenBaseKeys.count(key) > 0) {
            continue;
        }
        DriverSnapshotDiffRow row;
        row.kind = SnapshotDiffKind::DeviceRemoved;
        row.instanceId = base.instanceId;
        row.name = base.name;
        row.deviceClass = base.deviceClass;
        row.beforeSummary = FormatSnapshotEntrySummary(
            base.driverVersion, base.driverDate, base.infName, base.statusText);
        row.afterSummary = hdm::Tr("Устройство удалено / отключено", "Пристрій видалено / відключено", "Device removed / disconnected");
        row.canRollback = false;
        ++snapshotChangedCount_;
        snapshotDiffRows_.push_back(std::move(row));
    }

    auto kindRank = [](SnapshotDiffKind k) {
        switch (k) {
        case SnapshotDiffKind::NewIssue: return 0;
        case SnapshotDiffKind::IssueFixed: return 1;
        case SnapshotDiffKind::DriverUpdated: return 2;
        case SnapshotDiffKind::DeviceAdded: return 3;
        case SnapshotDiffKind::DeviceRemoved: return 4;
        case SnapshotDiffKind::Unchanged: return 5;
        }
        return 5;
    };
    std::sort(
        snapshotDiffRows_.begin(),
        snapshotDiffRows_.end(),
        [&](const DriverSnapshotDiffRow& a, const DriverSnapshotDiffRow& b) {
            const int ra = kindRank(a.kind);
            const int rb = kindRank(b.kind);
            if (ra != rb) {
                return ra < rb;
            }
            return a.name < b.name;
        });
}

void DriverLoomApp::SyncScannerSnapshots() {
    const uint64_t devGen = scanner_.Generation();
    if (devGen != lastDeviceGen_) {
        devicesCache_ = scanner_.GetDevicesSnapshot();
        lastDeviceGen_ = devGen;
        totalDevicesCount_ = devicesCache_.size();
        problemDevicesCount_ = 0;
        monitorDevicesCount_ = 0;
        peripheralDevicesCount_ = 0;
        for (const auto& d : devicesCache_) {
            if (d.health != hdm::DeviceHealth::Ok) ++problemDevicesCount_;
            if (d.category == hdm::DeviceCategory::Monitor) ++monitorDevicesCount_;
            if (d.category == hdm::DeviceCategory::Peripheral) ++peripheralDevicesCount_;
        }
        deviceViewDirty_ = true;

        if (baselineEntries_.empty() && !devicesCache_.empty()) {
            CaptureCurrentAsBaseline(false);
        } else {
            RebuildSnapshotDiff();
        }

        // Automatically refresh gaming runtimes / peripheral software when device list updates.
        if (!componentsScanner_.IsScanning() && !componentsScanner_.IsInstalling()) {
            componentsScanner_.StartLocalScanAsync(devicesCache_, MakeLogger());
        }
        if (!diagnostics_.IsBusy()) {
            diagnostics_.StartScanAsync(devicesCache_, MakeLogger());
        }
        if (!localInstaller_.IsScanningStore() && !localInstaller_.IsRemovingStorePackages()) {
            localInstaller_.ScanDriverStoreAsync(devicesCache_, nullptr);
        }
    }

    const uint64_t compGen = componentsScanner_.Generation();
    if (compGen != lastComponentsGen_) {
        componentsCache_ = componentsScanner_.GetItemsSnapshot();
        lastComponentsGen_ = compGen;
        compViewDirty_ = true;
    }

    const uint64_t diagGen = diagnostics_.Generation();
    if (diagGen != lastDiagGen_) {
        diagCache_ = diagnostics_.GetSnapshot();
        lastDiagGen_ = diagGen;
    }

    const uint64_t storeGen = localInstaller_.StoreGeneration();
    if (storeGen != lastDriverStoreGen_) {
        driverStoreCache_ = localInstaller_.GetDriverStoreSnapshot();
        lastDriverStoreGen_ = storeGen;
        storeViewDirty_ = true;
    }

    if (pendingDeviceRescan_.exchange(false) && !scanner_.IsScanning()) {
        scanner_.StartScanAsync(MakeLogger());
    }

    if (g_windowRegainedFocus.exchange(false)) {
        const double now = ImGui::GetTime();
        if (now - lastFocusRefreshTime_ > 2.5 &&
            !devicesCache_.empty() &&
            !componentsScanner_.IsScanning() &&
            !componentsScanner_.IsInstalling() &&
            !componentsScanner_.IsCheckingOnline()) {
            lastFocusRefreshTime_ = now;
            componentsScanner_.RefreshLocalPreserveOnlineAsync(devicesCache_);
            if (!diagnostics_.IsBusy()) {
                diagnostics_.StartScanAsync(devicesCache_, nullptr);
            }
        }
    }
}

void DriverLoomApp::RenderHeaderBar() {
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool isAppForeground = (GetForegroundWindow() == hwnd_);
    const float padY = std::round(9.0f * uiScale);
    const float baseFrameH = ImGui::GetFrameHeight();
    const float hdrGap = std::round(8.0f * uiScale);
    const float minSectionGap = 12.0f * uiScale;
    const float innerAvailW = std::max(280.0f, ImGui::GetContentRegionAvail().x - style.WindowPadding.x * 2.0f);

    AppUpdateState updState = AppUpdateState::Idle;
    std::string latestVer;
    {
        std::lock_guard<std::mutex> lock(appUpdateMutex_);
        updState = appUpdateState_;
        latestVer = appLatestVersion_;
    }
    const bool hasUpdate =
        (updState == AppUpdateState::UpdateAvailable && !latestVer.empty());
    char aboutLabel[112]{};
    float aboutBtnW = baseFrameH;
    if (hasUpdate) {
        char aboutMeasure[96];
        std::snprintf(
            aboutMeasure,
            sizeof(aboutMeasure),
            hdm::Tr("Обновление v%s", "Оновлення v%s", "Update v%s"),
            latestVer.c_str());
        std::snprintf(aboutLabel, sizeof(aboutLabel), "%s###AboutVerBtn", aboutMeasure);
        aboutBtnW = CalcButtonWidth(aboutMeasure) + 6.0f * uiScale;
    } else {
        std::snprintf(aboutLabel, sizeof(aboutLabel), "###AboutVerBtn");
    }

    const char* srLabel = systemRestore_.IsBusy()
                              ? hdm::Tr(
                                    "Создание точки восстановления...###SrBtn",
                                    "Створення точки відновлення...###SrBtn",
                                    "Creating restore point...###SrBtn")
                              : hdm::Tr(
                                    "Точка восстановления###SrBtn",
                                    "Точка відновлення###SrBtn",
                                    "Restore Point###SrBtn");
    const char* srMeasure = systemRestore_.IsBusy()
                                ? hdm::Tr(
                                      "Создание точки восстановления...",
                                      "Створення точки відновлення...",
                                      "Creating restore point...")
                                : hdm::Tr(
                                      "Точка восстановления",
                                      "Точка відновлення",
                                      "Restore Point");
    const char* srShortLabel = systemRestore_.IsBusy()
                                   ? hdm::Tr("Точка...###SrBtn", "Точка...###SrBtn", "Restore...###SrBtn")
                                   : hdm::Tr("Точка восст.###SrBtn", "Точка відн.###SrBtn", "Restore###SrBtn");
    const char* srShortMeasure = systemRestore_.IsBusy()
                                     ? hdm::Tr("Точка...", "Точка...", "Restore...")
                                     : hdm::Tr("Точка восст.", "Точка відн.", "Restore");
    const float restoreFullW = CalcButtonWidth(srMeasure) + 6.0f * uiScale;
    const float restoreShortW = CalcButtonWidth(srShortMeasure) + 4.0f * uiScale;

    struct LangOption {
        hdm::Lang lang;
        const char* label;
        const char* shortLabel;
    };
    static constexpr LangOption kLangOptions[] = {
        {hdm::Lang::Ru, "RU — Русский", "RU"},
        {hdm::Lang::Uk, "UA — Українська", "UA"},
        {hdm::Lang::En, "EN — English", "EN"},
    };
    const hdm::Lang curLang = hdm::GetLang();
    const char* curLangLabel = kLangOptions[0].label;
    const char* curLangShortLabel = kLangOptions[0].shortLabel;
    float maxLangLabelW = 0.0f;
    float maxLangShortW = 0.0f;
    for (const auto& opt : kLangOptions) {
        maxLangLabelW = std::max(maxLangLabelW, ImGui::CalcTextSize(opt.label).x);
        maxLangShortW = std::max(maxLangShortW, ImGui::CalcTextSize(opt.shortLabel).x);
        if (opt.lang == curLang) {
            curLangLabel = opt.label;
            curLangShortLabel = opt.shortLabel;
        }
    }
    const float fullLangComboW =
        maxLangLabelW + style.FramePadding.x * 2.0f + baseFrameH + 6.0f * uiScale;
    const float shortLangComboW =
        maxLangShortW + style.FramePadding.x * 2.0f + baseFrameH + 4.0f * uiScale;

    const float fullModeBtnW = std::max(
        CalcButtonWidth(hdm::Tr("Режим: Обычный", "Режим: Звичайний", "Mode: Standard")),
        CalcButtonWidth(hdm::Tr("Режим: Расширенный", "Режим: Розширений", "Mode: Advanced")));
    const float shortModeBtnW = std::max(
        CalcButtonWidth(hdm::Tr("Обычный", "Звичайний", "Standard")),
        CalcButtonWidth(hdm::Tr("Расширенный", "Розширений", "Advanced")));
    const float fullThemeBtnW = std::max(
        CalcButtonWidth(hdm::Tr("Тема: Светлая", "Тема: Світла", "Theme: Light")),
        CalcButtonWidth(hdm::Tr("Тема: Тёмная", "Тема: Темна", "Theme: Dark")));
    const float shortThemeBtnW = std::max(
        CalcButtonWidth(hdm::Tr("Светлая", "Світла", "Light")),
        CalcButtonWidth(hdm::Tr("Тёмная", "Темна", "Dark")));

    // Measure left group width before creating ##HeaderBar so we know if 2 rows are needed.
    PushFontSafe(g_fontPrimaryHeader);
    const ImVec2 titleSize = ImGui::CalcTextSize("DriverLoom");
    PopFontSafe(g_fontPrimaryHeader);
    const float divGap = std::round(6.0f * uiScale);
    const char* privText =
        isElevated_ ? hdm::Tr("Администратор", "Адміністратор", "Administrator")
                    : hdm::Tr("Без прав админа", "Без прав адміна", "Standard user");
    const float textH = ImGui::GetTextLineHeight();
    const float privBadgeW =
        std::round(8.0f * uiScale) + std::max(3.2f, textH * 0.21f) * 2.0f +
        std::round(6.0f * uiScale) + ImGui::CalcTextSize(privText).x + std::round(9.0f * uiScale);
    const float leftStaticW =
        titleSize.x + (2.0f + divGap * 2.0f) + (2.0f * uiScale) + ImGui::CalcTextSize("v1.0.0").x +
        (12.0f * uiScale) + privBadgeW;
    const float fullElevBtnW =
        isElevated_ ? 0.0f
                    : CalcButtonWidth(hdm::Tr(
                          "Перезапустить как администратор",
                          "Перезапустити як адміністратор",
                          "Restart as Administrator"));
    const float shortElevBtnW =
        isElevated_ ? 0.0f : CalcButtonWidth(hdm::Tr("Админ-режим", "Права адміна", "Run as Admin"));
    const float leftFullW = leftStaticW + (isElevated_ ? 0.0f : (10.0f * uiScale + fullElevBtnW));
    const float leftShortW = leftStaticW + (isElevated_ ? 0.0f : (8.0f * uiScale + shortElevBtnW));

    const float rightFullW =
        fullModeBtnW + hdrGap + fullThemeBtnW + hdrGap + fullLangComboW + hdrGap + restoreFullW + hdrGap + aboutBtnW;
    const float rightShortThemeW =
        fullModeBtnW + hdrGap + shortThemeBtnW + hdrGap + fullLangComboW + hdrGap + restoreFullW + hdrGap + aboutBtnW;
    const float rightShortModeThemeW =
        shortModeBtnW + hdrGap + shortThemeBtnW + hdrGap + fullLangComboW + hdrGap + restoreFullW + hdrGap + aboutBtnW;
    const float rightShortLangW =
        shortModeBtnW + hdrGap + shortThemeBtnW + hdrGap + shortLangComboW + hdrGap + restoreFullW + hdrGap + aboutBtnW;
    const float rightMiniW =
        shortModeBtnW + hdrGap + shortThemeBtnW + hdrGap + shortLangComboW + hdrGap + restoreShortW + hdrGap + aboutBtnW;

    bool twoRows = false;
    bool useShortElev = false;
    bool useShortMode = false;
    bool useShortTheme = false;
    bool useShortLang = false;
    bool useShortRestore = false;

    if (leftFullW + minSectionGap + rightFullW <= innerAvailW) {
        // Fits comfortably on one row with full labels
    } else if (leftFullW + minSectionGap + rightShortThemeW <= innerAvailW) {
        useShortTheme = true;
    } else if (leftFullW + minSectionGap + rightShortModeThemeW <= innerAvailW) {
        useShortTheme = true;
        useShortMode = true;
    } else if (leftShortW + minSectionGap + rightShortLangW <= innerAvailW) {
        useShortElev = !isElevated_;
        useShortTheme = true;
        useShortMode = true;
        useShortLang = true;
    } else if (isElevated_ && leftShortW + minSectionGap + rightMiniW <= innerAvailW) {
        useShortTheme = true;
        useShortMode = true;
        useShortLang = true;
        useShortRestore = true;
    } else {
        twoRows = true;
        useShortElev = (!isElevated_ && leftFullW > innerAvailW);
        if (rightFullW <= innerAvailW) {
            // Row 2 fits with full labels
        } else if (rightShortThemeW <= innerAvailW) {
            useShortTheme = true;
        } else if (rightShortModeThemeW <= innerAvailW) {
            useShortTheme = true;
            useShortMode = true;
        } else if (rightShortLangW <= innerAvailW) {
            useShortTheme = true;
            useShortMode = true;
            useShortLang = true;
        } else {
            useShortTheme = true;
            useShortMode = true;
            useShortLang = true;
            useShortRestore = true;
        }
    }

    const float headerHeight =
        twoRows ? (baseFrameH * 2.0f + style.ItemSpacing.y + padY * 2.0f)
                : (baseFrameH + padY * 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, padY));
    ImGui::PushStyleColor(
        ImGuiCol_Border,
        g_isLightTheme ? ImVec4(0.70f, 0.77f, 0.87f, 0.95f) : ImVec4(0.24f, 0.36f, 0.54f, 0.95f));
    ImGui::BeginChild(
        "##HeaderBar",
        ImVec2(0.0f, headerHeight),
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();

    {
        PushFontSafe(g_fontPrimaryHeader);
        const ImVec2 startScreen = ImGui::GetCursorScreenPos();
        const float yOff = std::max(0.0f, (baseFrameH - titleSize.y) * 0.5f);
        const ImVec4 titleCol = g_isLightTheme
                                    ? ImVec4(0.08f, 0.34f, 0.74f, 1.00f)
                                    : ImVec4(0.50f, 0.78f, 1.00f, 1.00f);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(startScreen.x, startScreen.y + yOff),
            ImGui::ColorConvertFloat4ToU32(titleCol),
            "DriverLoom");
        ImGui::Dummy(ImVec2(titleSize.x, baseFrameH));
        PopFontSafe(g_fontPrimaryHeader);
    }
    RenderVerticalDivider(6.0f);
    ImGui::SameLine(0.0f, 2.0f * uiScale);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("v1.0.0");

    ImGui::SameLine(0.0f, 12.0f * uiScale);
    if (isElevated_) {
        RenderStatusBadge(ImVec4(0.20f, 0.83f, 0.60f, 1.00f), privText, true);
    } else {
        RenderStatusBadge(ImVec4(0.98f, 0.75f, 0.14f, 1.00f), privText, true);
        ImGui::SameLine(0.0f, useShortElev ? (8.0f * uiScale) : (10.0f * uiScale));
        const char* elevLbl =
            useShortElev
                ? hdm::Tr("Админ-режим###ElevBtn", "Права адміна###ElevBtn", "Run as Admin###ElevBtn")
                : hdm::Tr(
                      "Перезапустить как администратор###ElevBtn",
                      "Перезапустити як адміністратор###ElevBtn",
                      "Restart as Administrator###ElevBtn");
        if (RenderSecondaryButton(elevLbl)) {
            const hdm::ElevatedRunResult run = hdm::ExecuteElevatedSelf(L"", false);
            if (run.launched) {
                exitRequested_ = true;
            } else if (run.launchError != ERROR_CANCELLED) {
                logBuffer_.Push(
                    hdm::LogLevel::Error,
                    std::string(hdm::Tr(
                        "Не удалось повысить права: ",
                        "Не вдалося підвищити права: ",
                        "Failed to elevate privileges: ")) +
                        hdm::FormatWin32Error(run.launchError));
            }
        }
    }

    const float modeBtnW = useShortMode ? shortModeBtnW : fullModeBtnW;
    const char* modeBtnLbl =
        useShortMode
            ? (g_isAdvancedMode
                   ? hdm::Tr("Расширенный###ModeBtn", "Розширений###ModeBtn", "Advanced###ModeBtn")
                   : hdm::Tr("Обычный###ModeBtn", "Звичайний###ModeBtn", "Standard###ModeBtn"))
            : (g_isAdvancedMode
                   ? hdm::Tr("Режим: Расширенный###ModeBtn", "Режим: Розширений###ModeBtn", "Mode: Advanced###ModeBtn")
                   : hdm::Tr("Режим: Обычный###ModeBtn", "Режим: Звичайний###ModeBtn", "Mode: Standard###ModeBtn"));

    const float themeBtnW = useShortTheme ? shortThemeBtnW : fullThemeBtnW;
    const char* themeBtnLbl =
        useShortTheme
            ? (g_isLightTheme
                   ? hdm::Tr("Светлая###ThemeBtn", "Світла###ThemeBtn", "Light###ThemeBtn")
                   : hdm::Tr("Тёмная###ThemeBtn", "Темна###ThemeBtn", "Dark###ThemeBtn"))
            : (g_isLightTheme
                   ? hdm::Tr("Тема: Светлая###ThemeBtn", "Тема: Світла###ThemeBtn", "Theme: Light###ThemeBtn")
                   : hdm::Tr("Тема: Тёмная###ThemeBtn", "Тема: Темна###ThemeBtn", "Theme: Dark###ThemeBtn"));

    const float langComboW = useShortLang ? shortLangComboW : fullLangComboW;
    const char* shownLangPreview = useShortLang ? curLangShortLabel : curLangLabel;
    const float restoreBtnW = useShortRestore ? restoreShortW : restoreFullW;
    const char* shownSrLbl = useShortRestore ? srShortLabel : srLabel;

    const float rightGroupW =
        modeBtnW + hdrGap + themeBtnW + hdrGap + langComboW + hdrGap + restoreBtnW + hdrGap + aboutBtnW;
    const float winContentMaxX = ImGui::GetWindowContentRegionMax().x;
    if (!twoRows) {
        const float prevItemEndX = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        const float rightX = winContentMaxX - rightGroupW;
        if (rightX > prevItemEndX + minSectionGap) {
            ImGui::SameLine(rightX);
        } else {
            ImGui::SameLine(0.0f, hdrGap);
        }
    } else {
        const float rightX = winContentMaxX - rightGroupW;
        if (rightX > style.WindowPadding.x) {
            ImGui::SetCursorPosX(rightX);
        }
    }

    if (RenderPillButton(modeBtnLbl, g_isAdvancedMode, ImVec2(modeBtnW, 0.0f))) {
        g_isAdvancedMode = !g_isAdvancedMode;
        if (!g_isAdvancedMode) {
            if (activeTabIndex_ == 3) {
                pendingInitialTab_ = 0;
            }
            wuShowHidden_ = false;
            wuRestoreBefore_ = true;
            compRestoreBefore_ = true;
        }
        SaveUserPreferences();
        logBuffer_.Push(
            hdm::LogLevel::Info,
            g_isAdvancedMode
                ? hdm::Tr(
                      "Режим интерфейса переключён: Расширенный (полный доступ к PnP, DriverStore и скрытию обновлений).",
                      "Режим інтерфейсу перемкнуто: Розширений (повний доступ до PnP, DriverStore і приховування оновлень).",
                      "Interface mode switched: Advanced (full access to PnP, DriverStore, and update hiding).")
                : hdm::Tr(
                      "Режим интерфейса переключён: Обычный (безопасный режим).",
                      "Режим інтерфейсу перемкнуто: Звичайний (безпечний режим).",
                      "Interface mode switched: Standard (safe mode)."));
    }
    if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip(
            "%s",
            hdm::Tr(
                "Обычный режим: безопасная работа, скрыты опасные действия PnP и ручное управление DriverStore/.inf.\n"
                "Расширенный режим: полный доступ ко всем вкладкам, Hardware ID, откату/удалению устройств и скрытию обновлений.",
                "Звичайний режим: безпечна робота, приховано небезпечні дії PnP та ручне керування DriverStore/.inf.\n"
                "Розширений режим: повний доступ до всіх вкладок, Hardware ID, відкоту/видалення пристроїв і приховування оновлень.",
                "Standard mode: safe daily use with destructive PnP actions and DriverStore/.inf hidden.\n"
                "Advanced mode: full access to all tabs, Hardware IDs, device rollback/uninstall, and update hiding."));
    }

    ImGui::SameLine(0.0f, hdrGap);
    if (RenderSecondaryButton(themeBtnLbl, ImVec2(themeBtnW, 0.0f))) {
        ApplyAppTheme(hwnd_, g_dpiScale, !g_isLightTheme);
        SaveUserPreferences();
    }
    if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip(
            "%s",
            hdm::Tr(
                "Переключить оформление: тёмная / светлая тема",
                "Перемкнути оформлення: темна / світла тема",
                "Switch appearance: Dark / Light theme"));
    }

    ImGui::SameLine(0.0f, hdrGap);
    ImGui::SetNextItemWidth(langComboW);
    if (g_isLightTheme) {
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.68f, 0.75f, 0.86f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.94f, 0.96f, 0.99f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.88f, 0.91f, 0.97f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.83f, 0.88f, 0.95f, 1.00f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.36f, 0.52f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.106f, 0.133f, 0.184f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.145f, 0.184f, 0.255f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.169f, 0.216f, 0.298f, 1.00f));
    }
    if (useShortLang) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(fullLangComboW, 0.0f), ImVec2(10000.0f, 10000.0f));
    }
    if (ImGui::BeginCombo("##LangCombo", shownLangPreview, ImGuiComboFlags_HeightSmall)) {
        for (const auto& opt : kLangOptions) {
            const bool isSelected = (opt.lang == curLang);
            if (ImGui::Selectable(opt.label, isSelected)) {
                hdm::SetLang(opt.lang);
                SaveUserPreferences();
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopStyleColor(4);

    ImGui::SameLine(0.0f, hdrGap);
    ImGui::BeginDisabled(systemRestore_.IsBusy());
    if (RenderSecondaryButton(shownSrLbl, ImVec2(restoreBtnW, 0.0f))) {
        systemRestore_.CreateRestorePointAsync(
            hdm::Tr(
                "DriverLoom (ручная точка восстановления)",
                "DriverLoom (ручна точка відновлення)",
                "DriverLoom (manual restore point)"),
            MakeLogger(),
            [this](bool ok, const std::string&) {
                if (ok && !diagnostics_.IsBusy()) {
                    diagnostics_.StartScanAsync(devicesCache_, nullptr);
                }
            });
    }
    ImGui::EndDisabled();

    ImGui::SameLine(0.0f, hdrGap);
    const bool clickedAbout = hasUpdate
                                  ? RenderActionButton(aboutLabel, ImVec2(aboutBtnW, 0.0f), true)
                                  : RenderSecondaryButton(aboutLabel, ImVec2(aboutBtnW, 0.0f));
    if (!hasUpdate && ImGui::IsItemVisible()) {
        const ImVec2 rMin = ImGui::GetItemRectMin();
        const ImVec2 rMax = ImGui::GetItemRectMax();
        const bool hovered = isAppForeground && ImGui::IsItemHovered();
        const float cx = (rMin.x + rMax.x) * 0.5f;
        const float cy = (rMin.y + rMax.y) * 0.5f;
        const float r = std::max(6.5f, std::floor((rMax.y - rMin.y) * 0.30f));
        const ImVec4 colVec =
            hovered
                ? AccentTextColor()
                : (g_isLightTheme ? ImVec4(0.18f, 0.32f, 0.54f, 1.00f)
                                  : ImVec4(0.74f, 0.85f, 0.98f, 1.00f));
        const ImU32 iconCol = ImGui::ColorConvertFloat4ToU32(colVec);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircle(ImVec2(cx, cy), r, iconCol, 24, std::max(1.3f, 1.4f * uiScale));
        dl->AddCircleFilled(
            ImVec2(cx, cy - r * 0.38f),
            std::max(1.1f, r * 0.145f),
            iconCol,
            12);
        dl->AddLine(
            ImVec2(cx, cy - r * 0.08f),
            ImVec2(cx, cy + r * 0.44f),
            iconCol,
            std::max(1.5f, 1.8f * uiScale));
    }
    if (clickedAbout) {
        openAboutPopupNextFrame_ = true;
        aboutModalOpen_ = true;
    }
    if (isAppForeground && ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip(
            "%s",
            hdm::Tr(
                "О приложении DriverLoom • Автор (Arvexer), GitHub, обратная связь и проверка обновлений (F1)",
                "Про програму DriverLoom • Автор (Arvexer), GitHub, зворотний зв'язок і перевірка оновлень (F1)",
                "About DriverLoom • Author (Arvexer), GitHub, contact & update checker (F1)"));
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void DriverLoomApp::RenderRebootBanner() {
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float padY = std::round(7.0f * uiScale);
    const float bannerHeight = ImGui::GetFrameHeight() + padY * 2.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, padY));
    ImGui::PushStyleColor(
        ImGuiCol_ChildBg,
        g_isLightTheme ? ImVec4(1.00f, 0.96f, 0.84f, 0.98f) : ImVec4(0.24f, 0.16f, 0.05f, 0.95f));
    ImGui::PushStyleColor(
        ImGuiCol_Border,
        g_isLightTheme ? ImVec4(0.86f, 0.58f, 0.10f, 0.95f) : ImVec4(0.96f, 0.68f, 0.16f, 0.90f));
    ImGui::BeginChild(
        "##RebootBanner",
        ImVec2(0.0f, bannerHeight),
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor(2);
    RenderStatusBadge(
        ImVec4(0.98f, 0.75f, 0.14f, 1.00f),
        hdm::Tr(
            "Требуется перезагрузка Windows для завершения установки драйверов.",
            "Потрібне перезавантаження Windows для завершення встановлення драйверів.",
            "Windows restart is required to finish driver installation."),
        true);
    const char* hideLabel = hdm::Tr("Скрыть###RebHide", "Приховати###RebHide", "Dismiss###RebHide");
    const float hideW = CalcButtonWidth(hdm::Tr("Скрыть", "Приховати", "Dismiss"));
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0f,
                             ImGui::GetWindowContentRegionMax().x - hideW));
    if (RenderSecondaryButton(hideLabel, ImVec2(hideW, 0.0f))) {
        rebootRequired_.store(false);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void DriverLoomApp::StartAppUpdateCheckAsync(bool manual) {
    if (checkingAppUpdate_.exchange(true)) {
        return;
    }
    if (appUpdateThread_.joinable()) {
        appUpdateThread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(appUpdateMutex_);
        appUpdateState_ = AppUpdateState::Checking;
    }

    appUpdateThread_ = std::thread([this, manual]() {
        struct WinHttpHandleGuard {
            HINTERNET h{nullptr};
            ~WinHttpHandleGuard() {
                if (h != nullptr) {
                    WinHttpCloseHandle(h);
                }
            }
        };

        constexpr const char* kCurrentVersion = "1.0.0";
        AppUpdateState resultState = AppUpdateState::Error;
        std::string foundVer = kCurrentVersion;
        std::string foundUrl = "https://github.com/Arvexer/DriverLoom/releases";

        WinHttpHandleGuard session{WinHttpOpen(
            L"DriverLoom/1.0",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0)};
        if (session.h != nullptr) {
            WinHttpSetTimeouts(session.h, 3000, 3000, 3500, 5000);
            WinHttpHandleGuard connect{
                WinHttpConnect(session.h, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0)};
            if (connect.h != nullptr) {
                WinHttpHandleGuard request{WinHttpOpenRequest(
                    connect.h,
                    L"HEAD",
                    L"/Arvexer/DriverLoom/releases/latest",
                    nullptr,
                    WINHTTP_NO_REFERER,
                    WINHTTP_DEFAULT_ACCEPT_TYPES,
                    WINHTTP_FLAG_SECURE)};
                if (request.h != nullptr) {
                    DWORD redirPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
                    WinHttpSetOption(
                        request.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirPolicy, sizeof(redirPolicy));
                    if (WinHttpSendRequest(
                            request.h,
                            WINHTTP_NO_ADDITIONAL_HEADERS,
                            0,
                            WINHTTP_NO_REQUEST_DATA,
                            0,
                            0,
                            0) &&
                        WinHttpReceiveResponse(request.h, nullptr)) {
                        DWORD statusCode = 0;
                        DWORD statusBytes = sizeof(statusCode);
                        WinHttpQueryHeaders(
                            request.h,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &statusCode,
                            &statusBytes,
                            WINHTTP_NO_HEADER_INDEX);

                        std::string locationUrl;
                        if (statusCode >= 300 && statusCode < 400) {
                            wchar_t locBuf[1024]{};
                            DWORD locBytes = sizeof(locBuf) - sizeof(wchar_t);
                            if (WinHttpQueryHeaders(
                                    request.h,
                                    WINHTTP_QUERY_LOCATION,
                                    WINHTTP_HEADER_NAME_BY_INDEX,
                                    locBuf,
                                    &locBytes,
                                    WINHTTP_NO_HEADER_INDEX)) {
                                locationUrl = hdm::WideToUtf8(locBuf);
                            }
                        }

                        std::string tagVer;
                        const size_t tagPos = locationUrl.find("/releases/tag/");
                        if (tagPos != std::string::npos) {
                            tagVer = locationUrl.substr(tagPos + 14);
                            const size_t endPos = tagVer.find_first_of("/?#\r\n ");
                            if (endPos != std::string::npos) {
                                tagVer.resize(endPos);
                            }
                            if (!tagVer.empty() && (tagVer[0] == 'v' || tagVer[0] == 'V')) {
                                tagVer.erase(0, 1);
                            }
                        }

                        if (!tagVer.empty() &&
                            CompareVersionOrString(tagVer, kCurrentVersion) > 0) {
                            resultState = AppUpdateState::UpdateAvailable;
                            foundVer = tagVer;
                            if (!locationUrl.empty()) {
                                if (locationUrl.rfind("http", 0) == 0) {
                                    foundUrl = locationUrl;
                                } else if (locationUrl.front() == '/') {
                                    foundUrl = "https://github.com" + locationUrl;
                                } else {
                                    foundUrl = "https://github.com/" + locationUrl;
                                }
                            }
                        } else if (statusCode == 404 || statusCode == 200 ||
                                   (statusCode >= 300 && statusCode < 400)) {
                            resultState = AppUpdateState::UpToDate;
                        }
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(appUpdateMutex_);
            appUpdateState_ = resultState;
            appLatestVersion_ = foundVer;
            appReleaseUrl_ = foundUrl;
        }
        checkingAppUpdate_.store(false);

        if (resultState == AppUpdateState::UpdateAvailable) {
            logBuffer_.Push(
                hdm::LogLevel::Success,
                std::string(hdm::Tr(
                    "Доступна новая версия DriverLoom v",
                    "Доступна нова версія DriverLoom v",
                    "New DriverLoom version available: v")) +
                    foundVer +
                    hdm::Tr(" на GitHub Releases!", " на GitHub Releases!", " on GitHub Releases!"));
        } else if (manual && resultState == AppUpdateState::UpToDate) {
            logBuffer_.Push(
                hdm::LogLevel::Info,
                hdm::Tr(
                    "Установлена актуальная версия DriverLoom (v1.0.0).",
                    "Встановлено актуальну версію DriverLoom (v1.0.0).",
                    "DriverLoom is up to date (v1.0.0)."));
        } else if (manual && resultState == AppUpdateState::Error) {
            logBuffer_.Push(
                hdm::LogLevel::Warning,
                hdm::Tr(
                    "Не удалось проверить обновления DriverLoom на GitHub.",
                    "Не вдалося перевірити оновлення DriverLoom на GitHub.",
                    "Failed to check DriverLoom updates on GitHub."));
        }
    });
}

void DriverLoomApp::RenderAboutModal() {
    const char* modalTitle = hdm::Tr(
        "О приложении DriverLoom###AboutModal",
        "Про програму DriverLoom###AboutModal",
        "About DriverLoom###AboutModal");

    if (openAboutPopupNextFrame_) {
        aboutModalOpen_ = true;
        ImGui::OpenPopup(modalTitle);
        openAboutPopupNextFrame_ = false;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(
        ImVec2(std::min(620.0f * uiScale, vp->WorkSize.x * 0.92f), 0.0f),
        ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal(
            modalTitle,
            &aboutModalOpen_,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    {
        PushFontSafe(g_fontPrimaryHeader);
        ImGui::TextColored(AccentTextColor(), "DriverLoom v1.0.0 (x64)");
        PopFontSafe(g_fontPrimaryHeader);
    }
    ImGui::TextWrapped(
        "%s",
        hdm::Tr(
            "Нативный гибридный менеджер драйверов, устройств, системных компонентов и диагностики для Windows 10/11.",
            "Нативний гібридний менеджер драйверів, пристроїв, системних компонентів і діагностики для Windows 10/11.",
            "Native hybrid driver, device, runtime component, and diagnostics manager for Windows 10/11."));

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Border, PanelBorderColor());
    ImGui::BeginChild(
        "##AboutAuthorCard",
        ImVec2(0.0f, 0.0f),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    {
        RenderSecondaryCardHeader(hdm::Tr(
            "Автор и контакты",
            "Автор і контакти",
            "Author & Contact"));
        ImGui::TextDisabled("%s", hdm::Tr("Автор:", "Автор:", "Author:"));
        ImGui::SameLine();
        ImGui::TextColored(AccentTextColor(), "Arvexer");

        ImGui::TextDisabled("GitHub:");
        ImGui::SameLine();
        ImGui::TextUnformatted("https://github.com/Arvexer/DriverLoom");
        if (RenderActionButton(
                hdm::Tr("Открыть GitHub###AboutGh", "Відкрити GitHub###AboutGh", "Open GitHub###AboutGh"),
                ImVec2(0.0f, 0.0f),
                true)) {
            OpenUrlInBrowser("https://github.com/Arvexer/DriverLoom");
        }
        ImGui::SameLine();
        if (RenderSecondaryButton(hdm::Tr(
                "Релизы###AboutRel",
                "Релізи###AboutRel",
                "Releases###AboutRel"))) {
            OpenUrlInBrowser("https://github.com/Arvexer/DriverLoom/releases");
        }
        ImGui::SameLine();
        if (RenderSecondaryButton(hdm::Tr(
                "Сообщить о проблеме###AboutIss",
                "Повідомити про проблему###AboutIss",
                "Report Issue###AboutIss"))) {
            OpenUrlInBrowser("https://github.com/Arvexer/DriverLoom/issues");
        }

        ImGui::Spacing();
        ImGui::TextDisabled("E-mail:");
        ImGui::SameLine();
        ImGui::TextUnformatted("arvexer.dev@proton.me");
        if (RenderSecondaryButton(hdm::Tr(
                "Написать письмо###AboutMail",
                "Написати листа###AboutMail",
                "Send E-mail###AboutMail"))) {
            OpenUrlInBrowser("mailto:arvexer.dev@proton.me?subject=DriverLoom%20Feedback");
        }
        ImGui::SameLine();
        if (RenderSecondaryButton(hdm::Tr(
                "Копировать e-mail###AboutCopyMail",
                "Копіювати e-mail###AboutCopyMail",
                "Copy E-mail###AboutCopyMail"))) {
            ImGui::SetClipboardText("arvexer.dev@proton.me");
            logBuffer_.Push(
                hdm::LogLevel::Info,
                hdm::Tr(
                    "Скопирован e-mail разработчика: arvexer.dev@proton.me",
                    "Скопійовано e-mail розробника: arvexer.dev@proton.me",
                    "Developer e-mail copied: arvexer.dev@proton.me"));
        }
        ImGui::SameLine();
        if (RenderSecondaryButton(hdm::Tr(
                "Поддержать (Ko-fi)###AboutKofi",
                "Підтримати (Ko-fi)###AboutKofi",
                "Support (Ko-fi)###AboutKofi"))) {
            OpenUrlInBrowser("https://ko-fi.com/arvexer");
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("https://ko-fi.com/arvexer");
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Border, PanelBorderColor());
    ImGui::BeginChild(
        "##AboutUpdatesCard",
        ImVec2(0.0f, 0.0f),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    {
        RenderSecondaryCardHeader(hdm::Tr(
            "Обновления приложения",
            "Оновлення програми",
            "Application Updates"));

        AppUpdateState updState = AppUpdateState::Idle;
        std::string latestVer;
        std::string releaseUrl;
        {
            std::lock_guard<std::mutex> lock(appUpdateMutex_);
            updState = appUpdateState_;
            latestVer = appLatestVersion_;
            releaseUrl = appReleaseUrl_;
        }

        if (updState == AppUpdateState::Checking) {
            RenderStatusBadge(
                ImVec4(0.38f, 0.68f, 0.98f, 1.00f),
                hdm::Tr(
                    "Проверка обновлений на GitHub...",
                    "Перевірка оновлень на GitHub...",
                    "Checking for updates on GitHub..."),
                true);
        } else if (updState == AppUpdateState::UpdateAvailable) {
            const std::string updMsg =
                std::string(hdm::Tr(
                    "Доступна новая версия: v",
                    "Доступна нова версія: v",
                    "New version available: v")) +
                latestVer;
            RenderStatusBadge(ImVec4(0.20f, 0.83f, 0.60f, 1.00f), updMsg.c_str(), true);
        } else if (updState == AppUpdateState::UpToDate) {
            RenderStatusBadge(
                ImVec4(0.20f, 0.83f, 0.60f, 1.00f),
                hdm::Tr(
                    "Установлена актуальная версия (v1.0.0)",
                    "Встановлено актуальну версію (v1.0.0)",
                    "You have the latest version (v1.0.0)"),
                true);
        } else if (updState == AppUpdateState::Error) {
            RenderStatusBadge(
                ImVec4(0.98f, 0.75f, 0.14f, 1.00f),
                hdm::Tr(
                    "Не удалось проверить обновления",
                    "Не вдалося перевірити оновлення",
                    "Failed to check for updates"),
                true);
        } else {
            RenderStatusBadge(
                ImVec4(0.58f, 0.68f, 0.86f, 1.00f),
                hdm::Tr(
                    "Автопроверка отключена (текущая: v1.0.0)",
                    "Автоперевірку вимкнено (поточна: v1.0.0)",
                    "Auto-check disabled (current: v1.0.0)"),
                true);
        }

        const bool busyCheck = checkingAppUpdate_.load();
        ImGui::BeginDisabled(busyCheck);
        if (RenderSecondaryButton(hdm::Tr(
                "Проверить обновления###AboutChkUpd",
                "Перевірити оновлення###AboutChkUpd",
                "Check for Updates###AboutChkUpd"))) {
            StartAppUpdateCheckAsync(true);
        }
        ImGui::EndDisabled();

        if (updState == AppUpdateState::UpdateAvailable) {
            ImGui::SameLine();
            if (RenderActionButton(
                    hdm::Tr(
                        "Скачать новую версию###AboutDlUpd",
                        "Завантажити нову версію###AboutDlUpd",
                        "Download Update###AboutDlUpd"),
                    ImVec2(0.0f, 0.0f),
                    true)) {
                OpenUrlInBrowser(releaseUrl);
            }
        }

        if (ImGui::Checkbox(
                hdm::Tr(
                    "Автоматически проверять обновления при запуске###AboutAutoUpd",
                    "Автоматично перевіряти оновлення при запуску###AboutAutoUpd",
                    "Automatically check for updates on startup###AboutAutoUpd"),
                &g_autoCheckAppUpdates)) {
            SaveUserPreferences();
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::TextDisabled("C++20 • Win32 SetupAPI / WUA COM • DirectX 11 • Dear ImGui • Static CRT");
    ImGui::TextDisabled("MIT License • Copyright (C) 2026 Arvexer");
    ImGui::Separator();

    const char* closeLbl = hdm::Tr("Закрыть###AboutCloseBtn", "Закрити###AboutCloseBtn", "Close###AboutCloseBtn");
    const float closeW = CalcButtonWidth(hdm::Tr("Закрыть", "Закрити", "Close")) + 12.0f * uiScale;
    const float closeX = ImGui::GetWindowContentRegionMax().x - closeW;
    if (closeX > style.WindowPadding.x) {
        ImGui::SetCursorPosX(closeX);
    }
    if (RenderActionButton(closeLbl, ImVec2(closeW, 0.0f), true) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
        aboutModalOpen_ = false;
    }

    ImGui::EndPopup();
}

void DriverLoomApp::SameLineWrapIfNeeded(float nextItemWidth) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float lastX = ImGui::GetItemRectMax().x;
    const float winRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (lastX + style.ItemSpacing.x + nextItemWidth <= winRight) {
        ImGui::SameLine();
    }
}

float DriverLoomApp::CalcButtonWidth(const char* label) {
    return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

const hdm::DeviceInfo* DriverLoomApp::FindSelectedDevice() const {
    if (selectedDeviceInstanceId_.empty()) {
        return nullptr;
    }
    for (const auto& d : devicesCache_) {
        if (d.instanceId == selectedDeviceInstanceId_) {
            return &d;
        }
    }
    return nullptr;
}

bool DriverLoomApp::IsOemInfName(const std::string& infName) {
    return hdm::ContainsCaseInsensitive(infName, "oem") &&
           hdm::ContainsCaseInsensitive(infName, ".inf");
}

void DriverLoomApp::JumpToDriverStorePackage(const std::string& infName) {
    if (infName.empty()) {
        return;
    }
    pendingInitialTab_ = 3;
    localSectionIndex_ = 1;
    storeFilterMode_ = DriverStoreFilterMode::All;
    std::snprintf(storeFilterBuf_, sizeof(storeFilterBuf_), "%s", infName.c_str());
    storeViewDirty_ = true;
}

void DriverLoomApp::HandleKeyboardShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();

    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        switch (activeTabIndex_) {
        case 0:
            if (!scanner_.IsBusy() && !componentsScanner_.IsInstalling()) {
                scanner_.StartScanAsync(MakeLogger());
            }
            break;
        case 1:
            if (!componentsScanner_.IsScanning() &&
                !componentsScanner_.IsCheckingOnline() &&
                !componentsScanner_.IsInstalling()) {
                componentsScanner_.StartLocalScanAsync(devicesCache_, MakeLogger());
            }
            break;
        case 2:
            if (!wuManager_.IsBusy()) {
                selectedWuIndex_ = -1;
                wuManager_.StartSearchAsync(MakeLogger(), !wuIncludeSystemUpdates_);
            }
            break;
        case 3:
            if (!g_isAdvancedMode) {
                break;
            }
            if (localSectionIndex_ == 1) {
                if (!localInstaller_.IsScanningStore() &&
                    !localInstaller_.IsExportingBackup() &&
                    !localInstaller_.IsRemovingStorePackages()) {
                    localInstaller_.ScanDriverStoreAsync(devicesCache_, MakeLogger());
                }
            } else if (std::strlen(localFolderBuf_) > 0 && !localInstaller_.IsScanningInf()) {
                localInstaller_.ScanInfFilesAsync(
                    hdm::Utf8ToWide(localFolderBuf_), MakeLogger());
            }
            break;
        case 4:
            if (!diagnostics_.IsBusy()) {
                diagnostics_.StartScanAsync(devicesCache_, MakeLogger());
            }
            break;
        default:
            break;
        }
    }

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        if (activeTabIndex_ != 0) {
            pendingInitialTab_ = 0;
        }
        focusDeviceSearchNextFrame_ = true;
    }

    if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        if (activeTabIndex_ == 0) {
            if (const hdm::DeviceInfo* sel = FindSelectedDevice()) {
                if (g_isAdvancedMode && !sel->hardwareId.empty()) {
                    ImGui::SetClipboardText(sel->hardwareId.c_str());
                    logBuffer_.Push(
                        hdm::LogLevel::Info,
                        std::string(hdm::Tr(
                            "Скопирован Hardware ID (Ctrl+C): ",
                            "Скопійовано Hardware ID (Ctrl+C): ",
                            "Copied Hardware ID (Ctrl+C): ")) +
                            sel->hardwareId);
                } else if (!sel->name.empty()) {
                    ImGui::SetClipboardText(sel->name.c_str());
                    logBuffer_.Push(
                        hdm::LogLevel::Info,
                        std::string(hdm::Tr(
                            "Скопировано название устройства (Ctrl+C): ",
                            "Скопійовано назву пристрою (Ctrl+C): ",
                            "Copied device name (Ctrl+C): ")) +
                            sel->name);
                }
            }
        } else if (activeTabIndex_ == 2) {
            if (selectedWuIndex_ >= 0 &&
                selectedWuIndex_ < static_cast<int>(wuCache_.size())) {
                const auto& wu = wuCache_[static_cast<size_t>(selectedWuIndex_)];
                if (!wu.title.empty()) {
                    ImGui::SetClipboardText(wu.title.c_str());
                    logBuffer_.Push(
                        hdm::LogLevel::Info,
                        std::string(hdm::Tr(
                            "Скопировано название обновления (Ctrl+C): ",
                            "Скопійовано назву оновлення (Ctrl+C): ",
                            "Copied update title (Ctrl+C): ")) +
                            wu.title);
                }
            }
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
        openAboutPopupNextFrame_ = true;
    }

    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (activeTabIndex_ == 0 && !selectedDeviceInstanceId_.empty()) {
            selectedDeviceInstanceId_.clear();
        } else if (activeTabIndex_ == 2 && selectedWuIndex_ >= 0) {
            selectedWuIndex_ = -1;
        }
    }
}

void DriverLoomApp::RenderLogPanel() {
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
    const ImGuiStyle& style = ImGui::GetStyle();
    bool wasDirty = false;
    const auto entries = logBuffer_.Snapshot(&wasDirty);

    ImGui::PushStyleColor(ImGuiCol_Border, PanelBorderColor());
    ImGui::BeginChild(
        "##LogPanel",
        ImVec2(0.0f, 0.0f),
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();

    const float frameH = ImGui::GetFrameHeight();
    {
        const float barW = std::max(2.5f, std::round(3.0f * uiScale));
        const float barPadY = std::round(4.0f * uiScale);
        const ImVec2 pMin = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(pMin.x, pMin.y + barPadY),
            ImVec2(pMin.x + barW, pMin.y + frameH - barPadY),
            ImGui::ColorConvertFloat4ToU32(AccentBarColor()),
            barW * 0.5f);
        ImGui::Dummy(ImVec2(barW + std::round(6.0f * uiScale), frameH));
    }
    ImGui::SameLine(0.0f, 0.0f);
    {
        PushFontSafe(g_fontSecondaryHeader);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(
            CardHeaderTextColor(),
            "%s",
            hdm::Tr("Журнал событий", "Журнал подій", "Event Log"));
        PopFontSafe(g_fontSecondaryHeader);
    }
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("(%zu)", entries.size());

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9.0f * uiScale, 3.0f * uiScale));
    const char* copyLogTxt = hdm::Tr("Копировать", "Копіювати", "Copy");
    const char* clearLogTxt = hdm::Tr("Очистить", "Очистити", "Clear");
    const char* autoLogTxt = hdm::Tr("Автопрокрутка", "Автопрокрутка", "Auto-scroll");
    const float copyW = ImGui::CalcTextSize(copyLogTxt).x + 18.0f * uiScale;
    const float clearW = ImGui::CalcTextSize(clearLogTxt).x + 18.0f * uiScale;
    const float autoW = ImGui::CalcTextSize(autoLogTxt).x + ImGui::GetFrameHeight() + 8.0f;
    const float totalRightW = autoW + copyW + clearW + style.ItemSpacing.x * 2.0f;
    const float winRight = ImGui::GetWindowContentRegionMax().x;
    if (winRight - totalRightW > ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 12.0f) {
        ImGui::SameLine(winRight - totalRightW);
    } else {
        ImGui::SameLine(0.0f, 16.0f);
    }

    ImGui::Checkbox(
        hdm::Tr("Автопрокрутка###LogAuto", "Автопрокрутка###LogAuto", "Auto-scroll###LogAuto"),
        &logAutoScroll_);
    ImGui::SameLine();
    if (RenderSecondaryButton(
            hdm::Tr("Копировать###LogCopy", "Копіювати###LogCopy", "Copy###LogCopy"),
            ImVec2(copyW, 0.0f))) {
        std::string all;
        for (const auto& e : entries) {
            all += "[" + e.timestamp + "] " + hdm::TrDyn(e.message) + "\n";
        }
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    if (RenderSecondaryButton(
            hdm::Tr("Очистить###LogClr", "Очистити###LogClr", "Clear###LogClr"),
            ImVec2(clearW, 0.0f))) {
        logBuffer_.Clear();
    }
    ImGui::PopStyleVar();
    ImGui::Separator();

    ImGui::BeginChild("##LogScroll", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(entries.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto& entry = entries[static_cast<size_t>(i)];
            ImVec4 col = g_isLightTheme
                             ? ImVec4(0.14f, 0.18f, 0.26f, 1.00f)
                             : ImVec4(0.84f, 0.88f, 0.94f, 1.00f);
            if (entry.level == hdm::LogLevel::Success) {
                col = g_isLightTheme
                          ? ImVec4(0.05f, 0.48f, 0.30f, 1.00f)
                          : ImVec4(0.20f, 0.83f, 0.60f, 1.00f);
            } else if (entry.level == hdm::LogLevel::Warning) {
                col = g_isLightTheme
                          ? ImVec4(0.68f, 0.42f, 0.02f, 1.00f)
                          : ImVec4(0.98f, 0.75f, 0.14f, 1.00f);
            } else if (entry.level == hdm::LogLevel::Error) {
                col = g_isLightTheme
                          ? ImVec4(0.75f, 0.14f, 0.16f, 1.00f)
                          : ImVec4(0.97f, 0.44f, 0.44f, 1.00f);
            }
            const ImVec4 tsCol = g_isLightTheme
                                     ? ImVec4(0.44f, 0.50f, 0.60f, 1.00f)
                                     : ImVec4(0.49f, 0.56f, 0.67f, 1.00f);
            ImGui::TextColored(tsCol, "[%s]", entry.timestamp.c_str());
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(hdm::TrDyn(entry.message).c_str());
            ImGui::PopStyleColor();
        }
    }
    if (logAutoScroll_ && wasDirty) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::EndChild();
}
