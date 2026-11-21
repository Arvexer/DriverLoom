#include "ui/DriverLoomApp.hpp"

#include <shellapi.h>

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
    const float headerHeight = baseFrameH + padY * 2.0f;
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
        const ImVec2 titleSize = ImGui::CalcTextSize("DriverLoom");
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
        RenderStatusBadge(
            ImVec4(0.20f, 0.83f, 0.60f, 1.00f),
            hdm::Tr("Администратор", "Адміністратор", "Administrator"),
            true);
    } else {
        RenderStatusBadge(
            ImVec4(0.98f, 0.75f, 0.14f, 1.00f),
            hdm::Tr("Без прав админа", "Без прав адміна", "Standard user"),
            true);
        ImGui::SameLine(0.0f, 10.0f * uiScale);
        if (RenderSecondaryButton(hdm::Tr(
                "Перезапустить как администратор###ElevBtn",
                "Перезапустити як адміністратор###ElevBtn",
                "Restart as Administrator###ElevBtn"))) {
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
    const float btnWidth = CalcButtonWidth(srMeasure) + 6.0f * uiScale;

    struct LangOption {
        hdm::Lang lang;
        const char* label;
    };
    static constexpr LangOption kLangOptions[] = {
        {hdm::Lang::Ru, "RU — Русский"},
        {hdm::Lang::Uk, "UA — Українська"},
        {hdm::Lang::En, "EN — English"},
    };
    const hdm::Lang curLang = hdm::GetLang();
    const char* curLangLabel = kLangOptions[0].label;
    float maxLangLabelW = 0.0f;
    for (const auto& opt : kLangOptions) {
        maxLangLabelW = std::max(maxLangLabelW, ImGui::CalcTextSize(opt.label).x);
        if (opt.lang == curLang) {
            curLangLabel = opt.label;
        }
    }
    const float langComboW =
        maxLangLabelW + style.FramePadding.x * 2.0f + baseFrameH + 6.0f * uiScale;

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
    const float hdrGap = std::round(8.0f * uiScale);
    const float prevItemEndX = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    const float winContentMaxX = ImGui::GetWindowContentRegionMax().x;
    const float availRightW = winContentMaxX - prevItemEndX - 12.0f * uiScale;
    const float allFullW =
        fullModeBtnW + hdrGap + fullThemeBtnW + hdrGap + langComboW + hdrGap + btnWidth;
    const float modeFullThemeShortW =
        fullModeBtnW + hdrGap + shortThemeBtnW + hdrGap + langComboW + hdrGap + btnWidth;
    const bool useShortThemeLbl = availRightW < allFullW;
    const bool useShortModeLbl = availRightW < modeFullThemeShortW;

    const float modeBtnW = useShortModeLbl ? shortModeBtnW : fullModeBtnW;
    const char* modeBtnLbl =
        useShortModeLbl
            ? (g_isAdvancedMode
                   ? hdm::Tr("Расширенный###ModeBtn", "Розширений###ModeBtn", "Advanced###ModeBtn")
                   : hdm::Tr("Обычный###ModeBtn", "Звичайний###ModeBtn", "Standard###ModeBtn"))
            : (g_isAdvancedMode
                   ? hdm::Tr("Режим: Расширенный###ModeBtn", "Режим: Розширений###ModeBtn", "Mode: Advanced###ModeBtn")
                   : hdm::Tr("Режим: Обычный###ModeBtn", "Режим: Звичайний###ModeBtn", "Mode: Standard###ModeBtn"));

    const float themeBtnW = useShortThemeLbl ? shortThemeBtnW : fullThemeBtnW;
    const char* themeBtnLbl =
        useShortThemeLbl
            ? (g_isLightTheme
                   ? hdm::Tr("Светлая###ThemeBtn", "Світла###ThemeBtn", "Light###ThemeBtn")
                   : hdm::Tr("Тёмная###ThemeBtn", "Темна###ThemeBtn", "Dark###ThemeBtn"))
            : (g_isLightTheme
                   ? hdm::Tr("Тема: Светлая###ThemeBtn", "Тема: Світла###ThemeBtn", "Theme: Light###ThemeBtn")
                   : hdm::Tr("Тема: Тёмная###ThemeBtn", "Тема: Темна###ThemeBtn", "Theme: Dark###ThemeBtn"));

    const float rightGroupW =
        modeBtnW + hdrGap + themeBtnW + hdrGap + langComboW + hdrGap + btnWidth;
    const float rightX = winContentMaxX - rightGroupW;
    if (rightX > prevItemEndX + 12.0f * uiScale) {
        ImGui::SameLine(rightX);
    } else {
        ImGui::SameLine();
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
    if (ImGui::BeginCombo("##LangCombo", curLangLabel, ImGuiComboFlags_HeightSmall)) {
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
    if (RenderSecondaryButton(srLabel, ImVec2(btnWidth, 0.0f))) {
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
