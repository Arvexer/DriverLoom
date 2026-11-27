#include "ui/DriverLoomApp.hpp"

#include <cfgmgr32.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <utility>

DriverLoomApp::DriverLoomApp(HWND mainHwnd, int initialTab)
    : hwnd_(mainHwnd),
      isElevated_(hdm::IsProcessElevated()),
      activeTabIndex_(initialTab >= 0 && initialTab <= PageSettings ? initialTab : PageOverview),
      logPanelExpanded_(g_isAdvancedMode && g_pinLogPanel) {
    if (activeTabIndex_ == PageLocalInstaller && !g_isAdvancedMode) {
        activeTabIndex_ = PageOverview;
    }
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
    stopAppUpdate_.store(true);
    {
        HINTERNET activeReq = nullptr;
        {
            std::lock_guard<std::mutex> lock(appHttpMutex_);
            activeReq = static_cast<HINTERNET>(std::exchange(activeAppUpdateRequest_, nullptr));
        }
        if (activeReq != nullptr) {
            WinHttpCloseHandle(activeReq);
        }
    }
    componentsScanner_.RequestCancelInstall();
    diagnostics_.RequestCancel();
    wuManager_.RequestCancel();
    if (appUpdateThread_.joinable()) {
        appUpdateThread_.join();
    }
    if (baselineSaveThread_.joinable()) {
        baselineSaveThread_.join();
    }
    ShutdownTaskbarOverlay();
}

void DriverLoomApp::NavigateTo(int pageIndex) {
    if (pageIndex < PageOverview || pageIndex > PageSettings) {
        return;
    }
    if (pageIndex == PageLocalInstaller && !g_isAdvancedMode) {
        return;
    }
    activeTabIndex_ = pageIndex;
}

bool DriverLoomApp::IsOptionalVendorSoftware(const hdm::SystemComponentItem& item) {
    // Motherboard suites (Armoury Crate, etc.), peripheral suites (G HUB, Synapse, GG, iCUE),
    // Intel DSA assistant, and monitor companion apps are always optional third-party software.
    if (item.category == hdm::ComponentCategory::PeripheralSoftware) {
        return true;
    }
    if (item.id == "intel_dsa" || item.id.rfind("mon_", 0) == 0) {
        return true;
    }
    return false;
}

bool DriverLoomApp::IsOptionalLegacyRuntime(const hdm::SystemComponentItem& item) {
    return item.id == "rt_openal" || item.id == "rt_vc2010" ||
           item.id == "rt_vc2012" || item.id == "rt_vc2013";
}

bool DriverLoomApp::IsOptionalComponent(const hdm::SystemComponentItem& item) {
    return IsOptionalVendorSoftware(item) || IsOptionalLegacyRuntime(item);
}

bool DriverLoomApp::IsComponentNotificationMuted(const std::string& id) {
    return !id.empty() && g_mutedComponentIds.count(id) > 0;
}

bool DriverLoomApp::IsComponentRecommendedAction(const hdm::SystemComponentItem& item) {
    if (IsOptionalComponent(item) || IsComponentNotificationMuted(item.id)) {
        return false;
    }
    return (!item.installed || item.updateAvailable) && !item.directInstallerUrl.empty();
}

void DriverLoomApp::RecomputeComponentCounters() {
    actionableComponentsCount_ = 0;
    mutedComponentsCount_ = 0;
    gamingComponentsCount_ = 0;
    peripheralComponentsCount_ = 0;
    gpuMonitorComponentsCount_ = 0;
    for (const auto& c : componentsCache_) {
        if (IsComponentNotificationMuted(c.id)) {
            ++mutedComponentsCount_;
        }
        if (IsComponentRecommendedAction(c)) {
            ++actionableComponentsCount_;
        }
        if (c.category == hdm::ComponentCategory::GamingRuntime) {
            ++gamingComponentsCount_;
        } else if (IsOptionalVendorSoftware(c)) {
            ++peripheralComponentsCount_;
        } else {
            ++gpuMonitorComponentsCount_;
        }
    }
    compViewDirty_ = true;
}

bool DriverLoomApp::IsUserRelevantDeviceIssue(const hdm::DeviceInfo& d) {
    if (d.problemCode == CM_PROB_DISABLED) {
        return false;
    }
    if (d.health == hdm::DeviceHealth::Error) {
        return true;
    }
    if (d.isGenericMonitorDriver) {
        return true;
    }
    if (d.health == hdm::DeviceHealth::Warning) {
        // Virtual Bluetooth serial ports (COMx over BTHENUM) routinely lack a driver until paired
        const bool isBtSerial =
            hdm::ContainsCaseInsensitive(d.hardwareId, "BTHENUM\\") &&
            d.name.rfind("COM", 0) == 0;
        if (isBtSerial) {
            return false;
        }
        return true;
    }
    return false;
}

void DriverLoomApp::RenderFrame() {
    if (pendingThemeChange_ >= 0) {
        g_cliOverrideLightTheme = false;
        ApplyAppTheme(hwnd_, EffectiveUiScale(), pendingThemeChange_ != 0);
        SaveUserPreferences();
        pendingThemeChange_ = -1;
    }

    SyncScannerSnapshots();
    ProcessPendingSystemProtectionResult();

    bool appHasUpdate = false;
    {
        std::lock_guard<std::mutex> lock(appUpdateMutex_);
        appHasUpdate = (appUpdateState_ == AppUpdateState::UpdateAvailable);
    }
    TaskbarOverlayState desiredOverlay = TaskbarOverlayState::None;
    if ((diagCache_.valid && diagCache_.disabledCriticalServices > 0) ||
        criticalDeviceErrorCount_ > 0) {
        desiredOverlay = TaskbarOverlayState::Critical;
    } else if (actionableComponentsCount_ > 0 || appHasUpdate ||
               (diagCache_.valid && diagCache_.attentionControllers > 0) ||
               userRelevantIssueCount_ > 0) {
        desiredOverlay = TaskbarOverlayState::Attention;
    }
    const hdm::Lang curOverlayLang = hdm::GetLang();
    if (desiredOverlay != lastTaskbarOverlay_ ||
        (desiredOverlay != TaskbarOverlayState::None && curOverlayLang != lastTaskbarOverlayLang_)) {
        SetTaskbarOverlayBadge(hwnd_, desiredOverlay);
        lastTaskbarOverlay_ = desiredOverlay;
        lastTaskbarOverlayLang_ = curOverlayLang;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float uiScale = EffectiveUiScale();

    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    constexpr ImGuiWindowFlags kRootFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, CanvasBgColor());
    ImGui::Begin("##MainWorkspace", nullptr, kRootFlags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    const float sidebarW = std::round(244.0f * uiScale);
    const float totalH = ImGui::GetContentRegionAvail().y;

    // Left Navigation Sidebar
    ImGui::PushStyleColor(ImGuiCol_ChildBg, SidebarBgColor());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(std::round(14.0f * uiScale), std::round(16.0f * uiScale)));
    ImGui::BeginChild(
        "##Sidebar",
        ImVec2(sidebarW, totalH),
        ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    RenderSidebar();
    ImGui::EndChild();

    // Subtle vertical divider between sidebar and main content
    {
        const ImVec2 sp = ImGui::GetItemRectMax();
        const ImVec2 spMin = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(sp.x, spMin.y),
            ImVec2(sp.x, sp.y),
            ImGui::ColorConvertFloat4ToU32(PanelBorderColor()),
            1.0f);
    }

    ImGui::SameLine(0.0f, 0.0f);

    // Right Main Pane: Active Page + Bottom Status Bar / Collapsible Log Drawer
    const float rightW = std::max(320.0f, ImGui::GetContentRegionAvail().x);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, CanvasBgColor());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild(
        "##RightPane",
        ImVec2(rightW, totalH),
        ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    const float statusBarH = logPanelExpanded_
                                 ? std::clamp(totalH * 0.25f, 168.0f * uiScale, 236.0f * uiScale)
                                 : std::round(44.0f * uiScale);
    const float contentH = std::max(220.0f, totalH - statusBarH);

    // Main Content Area (open canvas, generous padding, no outer box border!)
    const float padX = std::round(22.0f * uiScale);
    const float padY = std::round(16.0f * uiScale);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, CanvasBgColor());
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padX, padY));
    ImGui::BeginChild(
        "##PageCanvas",
        ImVec2(rightW, contentH),
        ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    if (rebootRequired_.load()) {
        RenderRebootBanner();
        ImGui::Dummy(ImVec2(0.0f, std::round(6.0f * uiScale)));
    }

    if (activeTabIndex_ == PageLocalInstaller && !g_isAdvancedMode) {
        activeTabIndex_ = PageOverview;
    }

    switch (activeTabIndex_) {
    case PageOverview:
        RenderOverviewTab();
        break;
    case PageComponents:
        RenderComponentsTab();
        break;
    case PageWindowsUpdate:
        RenderWindowsUpdateTab();
        break;
    case PageDevices:
        RenderDevicesTab();
        break;
    case PageDiagnostics:
        RenderDiagnosticsTab();
        break;
    case PageLocalInstaller:
        RenderLocalInstallerTab();
        break;
    case PageSettings:
        RenderSettingsTab();
        break;
    default:
        RenderOverviewTab();
        break;
    }

    ImGui::EndChild(); // ##PageCanvas

    // Bottom Status Bar / Collapsible Log Drawer
    RenderStatusBarAndLogDrawer();

    ImGui::EndChild(); // ##RightPane

    HandleKeyboardShortcuts();
    RenderAboutModal();
    RenderSystemProtectionModal();

    ImGui::End();
}

void DriverLoomApp::RenderSidebar() {
    const float uiScale = EffectiveUiScale();
    const bool isAppForeground = (GetForegroundWindow() == hwnd_);

    AppUpdateState updState = AppUpdateState::Idle;
    std::string latestVer;
    {
        std::lock_guard<std::mutex> lock(appUpdateMutex_);
        updState = appUpdateState_;
        latestVer = appLatestVersion_;
    }
    const bool hasUpdate =
        (updState == AppUpdateState::UpdateAvailable && !latestVer.empty());

    // Brand Header
    {
        const float logoSz = std::round(30.0f * uiScale);
        const ImVec2 startPos = ImGui::GetCursorScreenPos();
        const ImVec2 logoMin(startPos.x, startPos.y + 1.0f);
        const ImVec2 logoMax(logoMin.x + logoSz, logoMin.y + logoSz);
        const ImVec2 logoCenter((logoMin.x + logoMax.x) * 0.5f, (logoMin.y + logoMax.y) * 0.5f);
        if (g_appLogoTexture != 0) {
            ImGui::GetWindowDrawList()->AddImage(g_appLogoTexture, logoMin, logoMax);
        } else {
            DrawAppIcon(
                ImGui::GetWindowDrawList(),
                logoCenter,
                logoSz * 0.92f,
                AppIcon::BrandLogo,
                ImGui::ColorConvertFloat4ToU32(AccentBarColor()));
        }

        PushFontSafe(g_fontMainTab);
        const float titleY = logoCenter.y - ImGui::GetTextLineHeight() * 0.5f - 1.0f;
        const ImVec4 titleCol = g_isLightTheme
                                    ? ImVec4(0.07f, 0.13f, 0.25f, 1.00f)
                                    : ImVec4(0.95f, 0.98f, 1.00f, 1.00f);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(startPos.x + logoSz + std::round(10.0f * uiScale), titleY),
            ImGui::ColorConvertFloat4ToU32(titleCol),
            "DriverLoom");
        PopFontSafe(g_fontMainTab);

        ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, logoSz + std::round(6.0f * uiScale)));

        // Compact subtle subtitle row: version + optional Admin badge or Update pill
        PushFontSafe(g_fontSmall);
        ImGui::TextColored(MutedTextColor(), "v%s", hdm::kAppVersion);
        if (isElevated_) {
            ImGui::SameLine(0.0f, 8.0f * uiScale);
            ImGui::TextColored(
                g_isLightTheme ? ImVec4(0.05f, 0.48f, 0.30f, 1.00f) : HealthyColor(),
                "• %s",
                hdm::Tr("Админ", "Адмін", "Admin"));
        }
        if (hasUpdate) {
            ImGui::SameLine(0.0f, 8.0f * uiScale);
            char updLbl[48];
            std::snprintf(updLbl, sizeof(updLbl), "v%s!###SbUpd", latestVer.c_str());
            if (RenderPillButton(updLbl, true)) {
                openAboutPopupNextFrame_ = true;
                aboutModalOpen_ = true;
            }
        }
        PopFontSafe(g_fontSmall);
    }

    ImGui::Dummy(ImVec2(0.0f, std::round(10.0f * uiScale)));

    // Compute live badge counts
    size_t wuCount = 0;
    for (const auto& u : wuCache_) {
        if (!u.installedOk && !u.isHidden) {
            ++wuCount;
        }
    }
    const size_t devBadgeCount = g_isAdvancedMode ? problemDevicesCount_ : userRelevantIssueCount_;
    const size_t diagBadgeCount =
        diagCache_.valid ? (diagCache_.disabledCriticalServices + diagCache_.attentionControllers) : 0;
    const ImVec4 diagBadgeCol =
        (diagCache_.valid && diagCache_.disabledCriticalServices > 0) ? DangerColor() : WarningColor();

    const ImGuiStyle& sbStyle = ImGui::GetStyle();
    const float footerItemH = std::round(38.0f * uiScale);
    const float quickRowH = std::round(32.0f * uiScale);
    const float bottomMargin = std::round(4.0f * uiScale);
    const float footerTotalH =
        1.0f + sbStyle.ItemSpacing.y +
        std::round(4.0f * uiScale) + sbStyle.ItemSpacing.y +
        footerItemH + sbStyle.ItemSpacing.y +
        std::round(4.0f * uiScale) + sbStyle.ItemSpacing.y +
        quickRowH;
    const float navChildH = std::max(
        std::round(80.0f * uiScale),
        ImGui::GetContentRegionAvail().y - bottomMargin - footerTotalH);

    // Primary Navigation Items (scrollable so tall font scales never collide with bottom utility footer)
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild("##SidebarNav", ImVec2(0.0f, navChildH), ImGuiChildFlags_None, ImGuiWindowFlags_None);
    ImGui::PopStyleColor();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, std::round(4.0f * uiScale)));

    if (RenderSidebarNavItem(
            "##NavOverview",
            AppIcon::Home,
            hdm::Tr("Обзор", "Огляд", "Overview"),
            activeTabIndex_ == PageOverview)) {
        NavigateTo(PageOverview);
    }

    if (RenderSidebarNavItem(
            "##NavComponents",
            AppIcon::Apps,
            hdm::Tr("Игры и ПО", "Ігри та ПЗ", "Games & Software"),
            activeTabIndex_ == PageComponents,
            actionableComponentsCount_,
            WarningColor())) {
        NavigateTo(PageComponents);
    }

    if (RenderSidebarNavItem(
            "##NavWu",
            AppIcon::Updates,
            hdm::Tr("Обновления Windows", "Оновлення Windows", "Windows Update"),
            activeTabIndex_ == PageWindowsUpdate,
            wuCount,
            InfoAccentColor())) {
        NavigateTo(PageWindowsUpdate);
    }

    if (RenderSidebarNavItem(
            "##NavDevices",
            AppIcon::Devices,
            hdm::Tr("Устройства", "Пристрої", "Devices"),
            activeTabIndex_ == PageDevices,
            devBadgeCount,
            (criticalDeviceErrorCount_ > 0) ? DangerColor() : WarningColor())) {
        NavigateTo(PageDevices);
    }

    if (RenderSidebarNavItem(
            "##NavDiag",
            AppIcon::Shield,
            hdm::Tr("Защита и система", "Захист і система", "System & Protection"),
            activeTabIndex_ == PageDiagnostics,
            diagBadgeCount,
            diagBadgeCol)) {
        NavigateTo(PageDiagnostics);
    }

    if (g_isAdvancedMode) {
        if (RenderSidebarNavItem(
                "##NavLocal",
                AppIcon::Folder,
                hdm::Tr("Локальные (.inf)", "Локальні (.inf)", "Local (.inf)"),
                activeTabIndex_ == PageLocalInstaller,
                driverStoreCache_.duplicateCount,
                WarningColor())) {
            NavigateTo(PageLocalInstaller);
        }
    }

    ImGui::PopStyleVar();
    ImGui::EndChild();

    // Bottom Utility Footer pinned accurately below the scrollable nav area
    const float targetY = ImGui::GetWindowHeight() - bottomMargin - footerTotalH;
    if (ImGui::GetCursorPosY() < targetY) {
        ImGui::SetCursorPosY(targetY);
    }

    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));

    if (RenderSidebarNavItem(
            "##NavSettings",
            AppIcon::Settings,
            hdm::Tr("Настройки", "Налаштування", "Settings"),
            activeTabIndex_ == PageSettings)) {
        NavigateTo(PageSettings);
    }

    ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));

    // Quick utility row: Mode switch pill + Theme icon + About icon
    const float availW = ImGui::GetContentRegionAvail().x;
    const float gap = std::round(6.0f * uiScale);
    const float iconBtnW = quickRowH + std::round(4.0f * uiScale);
    const float modeBtnW = std::max(60.0f, availW - iconBtnW * 2.0f - gap * 2.0f);

    const char* modeLbl =
        g_isAdvancedMode
            ? hdm::Tr("Эксперт###SbMode", "Експерт###SbMode", "Expert###SbMode")
            : hdm::Tr("Обычный###SbMode", "Звичайний###SbMode", "Simple###SbMode");
    if (RenderPillButton(modeLbl, g_isAdvancedMode, ImVec2(modeBtnW, quickRowH))) {
        g_isAdvancedMode = !g_isAdvancedMode;
        g_cliOverrideAdvancedMode = false;
        if (!g_isAdvancedMode) {
            if (activeTabIndex_ == PageLocalInstaller) {
                activeTabIndex_ = PageOverview;
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
                      "Режим интерфейса переключён: Эксперт (полный доступ к PnP, DriverStore, .inf и службам).",
                      "Режим інтерфейсу перемкнуто: Експерт (повний доступ до PnP, DriverStore, .inf і служб).",
                      "Interface mode switched: Expert (full access to PnP, DriverStore, .inf, and services).")
                : hdm::Tr(
                      "Режим интерфейса переключён: Обычный (простой и безопасный режим).",
                      "Режим інтерфейсу перемкнуто: Звичайний (простий і безпечний режим).",
                      "Interface mode switched: Simple (clear and safe mode)."));
    }
    if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip(
            "%s",
            hdm::Tr(
                "Переключить режим: Обычный (простой) / Эксперт (полный доступ ко всем инструментам).",
                "Перемкнути режим: Звичайний (простий) / Експерт (повний доступ до всіх інструментів).",
                "Toggle mode: Simple (everyday use) / Expert (full access to all power tools)."));
    }

    ImGui::SameLine(0.0f, gap);
    if (RenderIconButton(
            "###SbThemeBtn",
            g_isLightTheme ? AppIcon::Sun : AppIcon::Moon,
            ImVec2(iconBtnW, quickRowH),
            isAppForeground
                ? hdm::Tr("Переключить тему (Тёмная / Светлая)", "Перемкнути тему (Темна / Світла)", "Toggle Theme (Dark / Light)")
                : nullptr)) {
        pendingThemeChange_ = g_isLightTheme ? 0 : 1;
    }

    ImGui::SameLine(0.0f, gap);
    if (RenderIconButton(
            "###SbAboutBtn",
            AppIcon::Info,
            ImVec2(iconBtnW, quickRowH),
            isAppForeground
                ? hdm::Tr("О программе DriverLoom (F1)", "Про програму DriverLoom (F1)", "About DriverLoom (F1)")
                : nullptr,
            hasUpdate)) {
        openAboutPopupNextFrame_ = true;
        aboutModalOpen_ = true;
    }
}

void DriverLoomApp::RenderSettingsTab() {
    const float uiScale = EffectiveUiScale();
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool isAppForeground = (GetForegroundWindow() == hwnd_);

    RenderPageHeader(
        hdm::Tr("Настройки", "Налаштування", "Settings"),
        hdm::Tr(
            "Выберите удобный режим интерфейса, тему оформления, язык и параметры безопасности.",
            "Виберіть зручний режим інтерфейсу, тему оформлення, мову та параметри безпеки.",
            "Choose your preferred interface mode, appearance theme, language, and safety options."));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild(
        "##SettingsScroll",
        ImVec2(0.0f, 0.0f),
        ImGuiChildFlags_None,
        ImGuiWindowFlags_None);
    ImGui::PopStyleColor();

    const float scrollGap = (ImGui::GetScrollMaxY() > 0.0f) ? std::round(12.0f * uiScale) : 0.0f;
    const float fullW = std::max(320.0f, ImGui::GetContentRegionAvail().x - scrollGap);
    const float gap = std::round(12.0f * uiScale);
    const float cardPad = std::round(16.0f * uiScale);

    // 1. Interface Mode Selector (Two clear visual cards)
    {
        PushFontSafe(g_fontSecondaryHeader);
        ImGui::TextUnformatted(hdm::Tr("Режим интерфейса", "Режим інтерфейсу", "Interface Mode"));
        PopFontSafe(g_fontSecondaryHeader);
        ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));

        const char* simpleTitle = hdm::Tr("Обычный режим", "Звичайний режим", "Simple Mode");
        const char* simpleBadge = hdm::Tr("Рекомендуется", "Рекомендовано", "Recommended");
        const char* simpleDesc = hdm::Tr(
            "Понятный интерфейс простым языком без сложного жаргона. Перед установкой автоматически создаётся точка восстановления.",
            "Зрозумілий інтерфейс простою мовою без складного жаргону. Перед встановленням автоматично створюється точка відновлення.",
            "Clear, everyday interface without technical jargon. Automatically creates a System Restore point before updates.");
        const char* expertTitle = hdm::Tr("Режим эксперта", "Режим експерта", "Expert Mode");
        const char* expertBadge = g_isAdvancedMode ? hdm::Tr("Активен", "Активний", "Active") : "";
        const char* expertDesc = hdm::Tr(
            "Полный доступ: Hardware ID, установка .inf, очистка DriverStore, управление PnP, службы Windows, SFC/DISM и снимок До/После.",
            "Повний доступ: Hardware ID, встановлення .inf, очищення DriverStore, керування PnP, служби Windows, SFC/DISM і знімок До/Після.",
            "Full power tools: Hardware IDs, manual .inf install, DriverStore cleanup, PnP control, Services, SFC/DISM, and Before/After diff.");

        const float halfW = std::max(std::round(200.0f * uiScale), (fullW - gap) * 0.5f);
        const float wrapW = std::max(100.0f * uiScale, halfW - cardPad * 2.0f);
        PushFontSafe(g_fontSecondaryHeader);
        const float modeTitleH = ImGui::GetTextLineHeight();
        PopFontSafe(g_fontSecondaryHeader);
        PushFontSafe(g_fontSmall);
        const float descH = std::max(
            ImGui::CalcTextSize(simpleDesc, nullptr, false, wrapW).y,
            ImGui::CalcTextSize(expertDesc, nullptr, false, wrapW).y);
        PopFontSafe(g_fontSmall);
        const float modeCardH = std::max(
            std::round(112.0f * uiScale),
            cardPad * 2.0f + modeTitleH + std::round(10.0f * uiScale) + descH + std::round(4.0f * uiScale));

        auto renderModeCard = [&](const char* id,
                                  const char* title,
                                  const char* badge,
                                  const char* desc,
                                  bool selected) -> bool {
            const ImVec2 pMin = ImGui::GetCursorScreenPos();
            const ImVec2 pMax(pMin.x + halfW, pMin.y + modeCardH);
            const bool clicked = ImGui::InvisibleButton(id, ImVec2(halfW, modeCardH));
            const bool hovered = ImGui::IsItemHovered();
            if (hovered) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float rnd = std::round(10.0f * uiScale);
            const ImVec4 bg =
                selected
                    ? (g_isLightTheme ? ImVec4(0.92f, 0.96f, 1.00f, 1.00f)
                                      : ImVec4(0.11f, 0.17f, 0.27f, 1.00f))
                    : (hovered ? CardElevatedBgColor() : CardBgColor());
            const ImVec4 brd = selected ? AccentBarColor() : PanelBorderColor();
            dl->AddRectFilled(pMin, pMax, ImGui::ColorConvertFloat4ToU32(bg), rnd);
            dl->AddRect(pMin, pMax, ImGui::ColorConvertFloat4ToU32(brd), rnd, 0, selected ? 2.0f : 1.0f);

            const float inX = pMin.x + cardPad;
            float curY = pMin.y + std::round(14.0f * uiScale);

            PushFontSafe(g_fontSecondaryHeader);
            const float titleH = ImGui::GetTextLineHeight();
            PopFontSafe(g_fontSecondaryHeader);

            // Selection radio circle aligned to title midline
            const float rad = std::round(7.0f * uiScale);
            const ImVec2 radioC(inX + rad, curY + titleH * 0.5f);
            dl->AddCircle(
                radioC,
                rad,
                ImGui::ColorConvertFloat4ToU32(selected ? AccentBarColor() : MutedTextColor()),
                20,
                1.6f);
            if (selected) {
                dl->AddCircleFilled(
                    radioC,
                    rad * 0.52f,
                    ImGui::ColorConvertFloat4ToU32(AccentBarColor()),
                    16);
            }

            float badgeReserve = 0.0f;
            if (badge != nullptr && badge[0] != '\0') {
                PushFontSafe(g_fontSmall);
                const ImVec2 bs = ImGui::CalcTextSize(badge);
                const float bpX = std::round(8.0f * uiScale);
                const float bH = bs.y + std::round(4.0f * uiScale);
                const float bW = bs.x + bpX * 2.0f;
                const float bY = curY + (titleH - bH) * 0.5f;
                const ImVec2 bMin(pMax.x - cardPad - bW, bY);
                const ImVec2 bMax(pMax.x - cardPad, bY + bH);
                const ImVec4 ac = selected ? HealthyColor() : MutedTextColor();
                dl->AddRectFilled(bMin, bMax, ImGui::ColorConvertFloat4ToU32(ImVec4(ac.x, ac.y, ac.z, 0.16f)), bH * 0.5f);
                dl->AddText(
                    ImVec2(bMin.x + bpX, bMin.y + (bH - bs.y) * 0.5f),
                    ImGui::ColorConvertFloat4ToU32(g_isLightTheme ? ImVec4(ac.x * 0.5f, ac.y * 0.5f, ac.z * 0.5f, 1.0f) : ac),
                    badge);
                PopFontSafe(g_fontSmall);
                badgeReserve = bW + std::round(10.0f * uiScale);
            }

            PushFontSafe(g_fontSecondaryHeader);
            const float titleOffset = rad * 2.0f + std::round(10.0f * uiScale);
            const float maxTitleW = std::max(60.0f, halfW - cardPad * 2.0f - titleOffset - badgeReserve);
            const std::string shownTitle = EllipsizeUtf8(title != nullptr ? title : "", maxTitleW);
            dl->AddText(
                ImVec2(inX + titleOffset, curY),
                ImGui::ColorConvertFloat4ToU32(CardHeaderTextColor()),
                shownTitle.c_str());
            PopFontSafe(g_fontSecondaryHeader);

            curY += titleH + std::round(8.0f * uiScale);
            PushFontSafe(g_fontSmall);
            dl->AddText(
                g_fontSmall,
                g_fontSmall != nullptr ? g_fontSmall->LegacySize : ImGui::GetFontSize(),
                ImVec2(inX, curY),
                ImGui::ColorConvertFloat4ToU32(MutedTextColor()),
                desc,
                nullptr,
                wrapW);
            PopFontSafe(g_fontSmall);
            return clicked;
        };

        if (renderModeCard(
                "##ModeCardSimple",
                simpleTitle,
                simpleBadge,
                simpleDesc,
                !g_isAdvancedMode)) {
            if (g_isAdvancedMode) {
                g_isAdvancedMode = false;
                g_cliOverrideAdvancedMode = false;
                wuShowHidden_ = false;
                wuRestoreBefore_ = true;
                compRestoreBefore_ = true;
                SaveUserPreferences();
            }
        }
        ImGui::SameLine(0.0f, gap);
        if (renderModeCard(
                "##ModeCardExpert",
                expertTitle,
                expertBadge,
                expertDesc,
                g_isAdvancedMode)) {
            if (!g_isAdvancedMode) {
                g_isAdvancedMode = true;
                g_cliOverrideAdvancedMode = false;
                SaveUserPreferences();
            }
        }
    }

    ImGui::Dummy(ImVec2(0.0f, std::round(12.0f * uiScale)));

    // 2. Appearance & Language Card
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(cardPad, std::round(14.0f * uiScale)));
    ImGui::PushStyleColor(ImGuiCol_Border, PanelBorderColor());
    ImGui::BeginChild(
        "##SettingsAppearCard",
        ImVec2(fullW, 0.0f),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    {
        RenderSecondaryCardHeader(hdm::Tr("Оформление и язык", "Оформлення та мова", "Appearance & Language"));
        ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));

        const char* themeLbl = hdm::Tr("Тема оформления:", "Тема оформлення:", "Color Theme:");
        const char* langLbl = hdm::Tr("Язык интерфейса:", "Мова інтерфейсу:", "Language:");
        const char* scaleLbl = hdm::Tr("Масштаб интерфейса:", "Масштаб інтерфейсу:", "Interface Scale:");
        const float labelColW =
            std::max(
                ImGui::CalcTextSize(themeLbl).x,
                std::max(ImGui::CalcTextSize(langLbl).x, ImGui::CalcTextSize(scaleLbl).x)) +
            std::round(14.0f * uiScale);

        const char* darkBtnLbl = hdm::Tr("Тёмная###SetDark", "Темна###SetDark", "Dark###SetDark");
        const char* lightBtnLbl = hdm::Tr("Светлая###SetLight", "Світла###SetLight", "Light###SetLight");
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", themeLbl);
        if (cardPad + labelColW + CalcButtonWidth(darkBtnLbl) <= ImGui::GetWindowContentRegionMax().x) {
            ImGui::SameLine(cardPad + labelColW);
        }
        if (RenderPillButton(darkBtnLbl, !g_isLightTheme)) {
            if (g_isLightTheme) {
                pendingThemeChange_ = 0;
            }
        }
        SameLineWrapIfNeeded(CalcButtonWidth(lightBtnLbl));
        if (RenderPillButton(lightBtnLbl, g_isLightTheme)) {
            if (!g_isLightTheme) {
                pendingThemeChange_ = 1;
            }
        }

        ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", langLbl);
        if (cardPad + labelColW + CalcButtonWidth("Русский###SetRu") <= ImGui::GetWindowContentRegionMax().x) {
            ImGui::SameLine(cardPad + labelColW);
        }
        const hdm::Lang curLang = hdm::GetLang();
        if (RenderPillButton("Русский###SetRu", curLang == hdm::Lang::Ru)) {
            hdm::SetLang(hdm::Lang::Ru);
            g_cliOverrideLanguage = false;
            SaveUserPreferences();
        }
        SameLineWrapIfNeeded(CalcButtonWidth("Українська###SetUk"));
        if (RenderPillButton("Українська###SetUk", curLang == hdm::Lang::Uk)) {
            hdm::SetLang(hdm::Lang::Uk);
            g_cliOverrideLanguage = false;
            SaveUserPreferences();
        }
        SameLineWrapIfNeeded(CalcButtonWidth("English###SetEn"));
        if (RenderPillButton("English###SetEn", curLang == hdm::Lang::En)) {
            hdm::SetLang(hdm::Lang::En);
            g_cliOverrideLanguage = false;
            SaveUserPreferences();
        }

        ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", scaleLbl);
        if (cardPad + labelColW + CalcButtonWidth("90%###SetScale90") <= ImGui::GetWindowContentRegionMax().x) {
            ImGui::SameLine(cardPad + labelColW);
        }
        constexpr int kScalePresets[5] = {90, 100, 110, 125, 150};
        bool matchesPreset = false;
        for (int i = 0; i < 5; ++i) {
            const int preset = kScalePresets[i];
            if (g_uiScalePercent == preset) {
                matchesPreset = true;
            }
            char scaleBtnLbl[32];
            std::snprintf(scaleBtnLbl, sizeof(scaleBtnLbl), "%d%%###SetScale%d", preset, preset);
            if (i > 0) {
                SameLineWrapIfNeeded(CalcButtonWidth(scaleBtnLbl));
            }
            if (RenderPillButton(scaleBtnLbl, g_uiScalePercent == preset)) {
                if (g_uiScalePercent != preset) {
                    g_uiScalePercent = preset;
                    g_cliOverrideUiScale = false;
                    g_pendingScaleReload = true;
                    SaveUserPreferences();
                }
            }
        }
        if (!matchesPreset) {
            char customScaleLbl[64];
            std::snprintf(
                customScaleLbl,
                sizeof(customScaleLbl),
                "%d%% (%s)###SetScaleCustom",
                g_uiScalePercent,
                hdm::Tr("кастом", "свій", "custom"));
            SameLineWrapIfNeeded(CalcButtonWidth(customScaleLbl));
            RenderPillButton(customScaleLbl, true);
        }

        ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));
        if (ImGui::Checkbox(
                hdm::Tr(
                    "Всегда держать журнал событий раскрытым внизу###SetPinLog",
                    "Завжди тримати журнал подій розгорнутим унизу###SetPinLog",
                    "Keep Event Log drawer expanded at the bottom###SetPinLog"),
                &g_pinLogPanel)) {
            logPanelExpanded_ = g_pinLogPanel;
            SaveUserPreferences();
        }
        if (ImGui::Checkbox(
                hdm::Tr(
                    "Автоматически проверять обновления драйверов и компонентов при запуске (WinHTTP)###SetAutoOnline",
                    "Автоматично перевіряти оновлення драйверів і компонентів при запуску (WinHTTP)###SetAutoOnline",
                    "Automatically check for driver & component updates on startup (WinHTTP)###SetAutoOnline"),
                &g_autoCheckOnlineUpdates)) {
            SaveUserPreferences();
        }
        if (ImGui::Checkbox(
                hdm::Tr(
                    "Автоматически проверять обновления DriverLoom при запуске###SetAutoUpd",
                    "Автоматично перевіряти оновлення DriverLoom при запуску###SetAutoUpd",
                    "Automatically check for DriverLoom updates on startup###SetAutoUpd"),
                &g_autoCheckAppUpdates)) {
            SaveUserPreferences();
        }
        PushFontSafe(g_fontSmall);
        ImGui::PushStyleColor(ImGuiCol_Text, MutedTextColor());
        ImGui::TextWrapped(
            "%s",
            hdm::Tr(
                "Только прямые HTTPS-запросы через Windows WinHTTP при запуске или по кнопке «Проверить» — без фоновых запросов, служб и телеметрии.",
                "Лише прямі HTTPS-запити через Windows WinHTTP під час запуску або за кнопкою «Перевірити» — без фонових запитів, служб і телеметрії.",
                "Direct Windows WinHTTP HTTPS requests only on startup or when you click Check — no background polling, services, or telemetry."));
        ImGui::PopStyleColor();
        PopFontSafe(g_fontSmall);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::Dummy(ImVec2(0.0f, std::round(12.0f * uiScale)));

    // 3. Privileges & Protection Card
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(cardPad, std::round(14.0f * uiScale)));
    ImGui::PushStyleColor(ImGuiCol_Border, PanelBorderColor());
    ImGui::BeginChild(
        "##SettingsSecCard",
        ImVec2(fullW, 0.0f),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    {
        RenderSecondaryCardHeader(hdm::Tr(
            "Права доступа и точка восстановления",
            "Права доступу та точка відновлення",
            "Privileges & System Protection"));
        ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));

        if (isElevated_) {
            RenderStatusBadge(
                HealthyColor(),
                hdm::Tr(
                    "Запущено с правами администратора",
                    "Запущено з правами адміністратора",
                    "Running as Administrator"),
                false);
        } else {
            RenderStatusBadge(
                InfoAccentColor(),
                hdm::Tr(
                    "Обычный режим — права администратора запрашиваются только при установке",
                    "Звичайний режим — права адміністратора запитуються лише під час встановлення",
                    "Standard mode — Administrator rights are requested only when installing"),
                false);
        }

        const char* srBtnLbl =
            systemRestore_.IsBusy()
                ? hdm::Tr("Создание точки восстановления...###SetSrBtn", "Створення точки відновлення...###SetSrBtn", "Creating restore point...###SetSrBtn")
                : hdm::Tr("Создать точку восстановления сейчас###SetSrBtn", "Створити точку відновлення зараз###SetSrBtn", "Create Restore Point Now###SetSrBtn");
        ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));
        if (!isElevated_) {
            if (RenderSecondaryButton(hdm::Tr(
                    "Перезапустить как администратор###SetElevBtn",
                    "Перезапустити як адміністратор###SetElevBtn",
                    "Restart as Administrator###SetElevBtn"))) {
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
            SameLineWrapIfNeeded(CalcButtonWidth(srBtnLbl));
        }
        ImGui::BeginDisabled(systemRestore_.IsBusy());
        if (RenderSecondaryButton(srBtnLbl)) {
            systemRestore_.CreateRestorePointAsync(
                hdm::Tr(
                    "DriverLoom (ручная точка восстановления)",
                    "DriverLoom (ручна точка відновлення)",
                    "DriverLoom (manual restore point)"),
                MakeLogger(),
                [this](bool ok, const std::string&) {
                    if (ok) {
                        pendingDiagRescan_.store(true);
                    }
                });
        }
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::Dummy(ImVec2(0.0f, std::round(12.0f * uiScale)));

    // 4. About & Updates Card
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(cardPad, std::round(14.0f * uiScale)));
    ImGui::PushStyleColor(ImGuiCol_Border, PanelBorderColor());
    ImGui::BeginChild(
        "##SettingsAboutCard",
        ImVec2(fullW, 0.0f),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    {
        const std::string settingsVerMeta =
            std::string("v") + hdm::kAppVersion + " • MIT License";
        RenderSecondaryCardHeader(
            hdm::Tr("О программе DriverLoom", "Про програму DriverLoom", "About DriverLoom"),
            settingsVerMeta.c_str());
        ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));
        ImGui::TextWrapped(
            "%s",
            hdm::Tr(
                "Безопасный нативный менеджер драйверов, игровых библиотек и диагностики Windows 10/11. Автор: Arvexer.",
                "Безпечний нативний менеджер драйверів, ігрових бібліотек і діагностики Windows 10/11. Автор: Arvexer.",
                "Safe native driver, gaming runtime, and Windows 10/11 diagnostics manager. Author: Arvexer."));
        ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));

        const char* ghLbl = hdm::Tr("Открыть GitHub###SetGh", "Відкрити GitHub###SetGh", "Open GitHub###SetGh");
        const char* chkLbl = hdm::Tr(
            "Проверить обновления###SetChkUpd",
            "Перевірити оновлення###SetChkUpd",
            "Check for Updates###SetChkUpd");
        const char* issLbl = hdm::Tr(
            "Сообщить о проблеме###SetIss",
            "Повідомити про проблему###SetIss",
            "Report Issue###SetIss");
        const char* kofiLbl = hdm::Tr(
            "Поддержать (Ko-fi)###SetKofi",
            "Підтримати (Ko-fi)###SetKofi",
            "Support (Ko-fi)###SetKofi");

        if (RenderPrimaryButton(ghLbl)) {
            OpenUrlInBrowser("https://github.com/Arvexer/DriverLoom");
        }
        SameLineWrapIfNeeded(CalcButtonWidth(chkLbl));
        ImGui::BeginDisabled(checkingAppUpdate_.load());
        if (RenderSecondaryButton(chkLbl)) {
            StartAppUpdateCheckAsync(true);
        }
        ImGui::EndDisabled();
        SameLineWrapIfNeeded(CalcButtonWidth(issLbl));
        if (RenderSecondaryButton(issLbl)) {
            OpenUrlInBrowser("https://github.com/Arvexer/DriverLoom/issues");
        }
        SameLineWrapIfNeeded(CalcButtonWidth(kofiLbl));
        if (RenderSecondaryButton(kofiLbl)) {
            OpenUrlInBrowser("https://ko-fi.com/arvexer");
        }
        if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("https://ko-fi.com/arvexer");
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    (void)style;
    ImGui::EndChild(); // ##SettingsScroll
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
        ApplyAppTheme(hwnd_, EffectiveUiScale(), lightTheme);
    }
    activeTabIndex_ = tab;
    diagSectionIndex_ = diagSec;
    localSectionIndex_ = localSec;
    pendingSelectDeviceRow_ = selectDevRow;
    logPanelExpanded_ = false;
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
    (void)LoadBaselineSnapshotFromRegistry(baselineTimestamp_, baselineEntries_);
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
    if (baselineSaveThread_.joinable()) {
        baselineSaveThread_.join();
    }
    baselineSaveThread_ = std::thread(
        [ts = baselineTimestamp_, entries = baselineEntries_]() {
            SaveBaselineSnapshotToRegistry(ts, entries);
        });
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
                row.kind = (curHealth < base.health)
                               ? SnapshotDiffKind::IssueFixed
                               : SnapshotDiffKind::NewIssue;
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
        row.afterSummary = hdm::Tr(
            "Устройство удалено / отключено",
            "Пристрій видалено / відключено",
            "Device removed / disconnected");
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
        const bool wasSilentRescan = silentDeviceRescan_;
        silentDeviceRescan_ = false;
        devicesCache_ = scanner_.GetDevicesSnapshot();
        lastDeviceGen_ = devGen;
        totalDevicesCount_ = devicesCache_.size();
        problemDevicesCount_ = 0;
        userRelevantIssueCount_ = 0;
        criticalDeviceErrorCount_ = 0;
        monitorDevicesCount_ = 0;
        peripheralDevicesCount_ = 0;
        for (const auto& d : devicesCache_) {
            if (d.health != hdm::DeviceHealth::Ok || d.isGenericMonitorDriver) {
                ++problemDevicesCount_;
            }
            if (IsUserRelevantDeviceIssue(d)) {
                ++userRelevantIssueCount_;
            }
            if (d.health == hdm::DeviceHealth::Error && d.problemCode != CM_PROB_DISABLED) {
                ++criticalDeviceErrorCount_;
            }
            if (d.category == hdm::DeviceCategory::Monitor) ++monitorDevicesCount_;
            if (d.category == hdm::DeviceCategory::Peripheral) ++peripheralDevicesCount_;
        }
        deviceViewDirty_ = true;

        if (baselineEntries_.empty() && !devicesCache_.empty()) {
            CaptureCurrentAsBaseline(false);
        } else {
            RebuildSnapshotDiff();
        }

        if (!componentsScanner_.IsScanning() && !componentsScanner_.IsInstalling()) {
            if (wasSilentRescan && !componentsCache_.empty()) {
                componentsScanner_.RefreshLocalPreserveOnlineAsync(devicesCache_);
            } else {
                componentsScanner_.StartLocalScanAsync(
                    devicesCache_, MakeLogger(), g_autoCheckOnlineUpdates);
            }
        }
        if (!diagnostics_.IsBusy()) {
            diagnostics_.StartScanAsync(devicesCache_, wasSilentRescan ? nullptr : MakeLogger());
        }
        if (!localInstaller_.IsScanningStore() && !localInstaller_.IsRemovingStorePackages()) {
            localInstaller_.ScanDriverStoreAsync(devicesCache_, nullptr);
        }
    }

    const uint64_t compGen = componentsScanner_.Generation();
    if (compGen != lastComponentsGen_) {
        componentsCache_ = componentsScanner_.GetItemsSnapshot();
        lastComponentsGen_ = compGen;
        RecomputeComponentCounters();
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

    if (componentsScanner_.ConsumeDeviceRescanNeeded()) {
        silentDeviceRescan_ = true;
        pendingDeviceRescan_.store(true);
    }
    if (!scanner_.IsScanning() && pendingDeviceRescan_.exchange(false)) {
        scanner_.StartScanAsync(silentDeviceRescan_ ? nullptr : MakeLogger());
    }
    if (!diagnostics_.IsBusy() && pendingDiagRescan_.exchange(false)) {
        diagnostics_.StartScanAsync(devicesCache_, nullptr);
    }

    if (g_windowRegainedFocus.exchange(false)) {
        const double now = ImGui::GetTime();
        if (now - lastFocusRefreshTime_ > 2.5 &&
            !devicesCache_.empty() &&
            !scanner_.IsScanning() &&
            !pendingDeviceRescan_.load() &&
            !componentsScanner_.IsScanning() &&
            !componentsScanner_.IsInstalling() &&
            !componentsScanner_.IsCheckingOnline()) {
            lastFocusRefreshTime_ = now;
            silentDeviceRescan_ = true;
            scanner_.StartScanAsync(nullptr);
        }
    }
}

void DriverLoomApp::RenderRebootBanner() {
    const float uiScale = EffectiveUiScale();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float padY = std::round(8.0f * uiScale);
    const float bannerHeight = ImGui::GetFrameHeight() + padY * 2.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, padY));
    ImGui::PushStyleColor(
        ImGuiCol_ChildBg,
        g_isLightTheme ? ImVec4(1.00f, 0.96f, 0.86f, 0.98f) : ImVec4(0.22f, 0.16f, 0.06f, 0.96f));
    ImGui::PushStyleColor(
        ImGuiCol_Border,
        g_isLightTheme ? ImVec4(0.86f, 0.60f, 0.14f, 0.90f) : ImVec4(0.92f, 0.66f, 0.18f, 0.85f));
    ImGui::BeginChild(
        "##RebootBanner",
        ImVec2(0.0f, bannerHeight),
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor(2);
    const char* hideLabel = hdm::Tr("Позже###RebHide", "Пізніше###RebHide", "Later###RebHide");
    const float hideW = CalcButtonWidth(hdm::Tr("Позже", "Пізніше", "Later"));
    const float badgeMaxW =
        std::max(80.0f * uiScale, ImGui::GetContentRegionAvail().x - hideW - style.ItemSpacing.x);
    RenderStatusBadge(
        WarningColor(),
        hdm::Tr(
            "Перезагрузите компьютер, чтобы завершить установку драйверов.",
            "Перезавантажте комп'ютер, щоб завершити встановлення драйверів.",
            "Restart your PC to finish applying the installed drivers."),
        true,
        badgeMaxW);
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - hideW);
    if (RenderSecondaryButton(hideLabel, ImVec2(hideW, 0.0f))) {
        rebootRequired_.store(false);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void DriverLoomApp::StartAppUpdateCheckAsync(bool manual) {
    if (stopAppUpdate_.load() || checkingAppUpdate_.exchange(true)) {
        return;
    }
    if (appUpdateThread_.joinable()) {
        appUpdateThread_.join();
    }
    if (stopAppUpdate_.load()) {
        checkingAppUpdate_.store(false);
        return;
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
        struct TrackedAppRequestGuard {
            std::mutex& mtx;
            void*& slot;
            std::atomic<bool>& stopFlag;
            HINTERNET h{nullptr};
            TrackedAppRequestGuard(
                std::mutex& m, void*& s, std::atomic<bool>& st, HINTERNET req) noexcept
                : mtx(m), slot(s), stopFlag(st), h(req) {
                if (h != nullptr) {
                    bool cancelled = false;
                    {
                        std::lock_guard<std::mutex> lock(mtx);
                        if (stopFlag.load()) {
                            cancelled = true;
                        } else {
                            slot = h;
                        }
                    }
                    if (cancelled) {
                        WinHttpCloseHandle(h);
                        h = nullptr;
                    }
                }
            }
            ~TrackedAppRequestGuard() {
                if (h != nullptr) {
                    bool owned = false;
                    {
                        std::lock_guard<std::mutex> lock(mtx);
                        if (slot == h) {
                            slot = nullptr;
                            owned = true;
                        }
                    }
                    if (owned) {
                        WinHttpCloseHandle(h);
                    }
                    h = nullptr;
                }
            }
        };

        AppUpdateState resultState = AppUpdateState::Error;
        std::string foundVer = hdm::kAppVersion;
        std::string foundUrl = "https://github.com/Arvexer/DriverLoom/releases";

        const std::wstring userAgent = L"DriverLoom/" + hdm::Utf8ToWide(hdm::kAppVersion);
        WinHttpHandleGuard session{WinHttpOpen(
            userAgent.c_str(),
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0)};
        if (session.h != nullptr && !stopAppUpdate_.load()) {
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
            constexpr DWORD kTls13Flag = 0x00002000;
#else
            constexpr DWORD kTls13Flag = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
            DWORD tlsProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | kTls13Flag;
            if (!WinHttpSetOption(
                    session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &tlsProtocols, sizeof(tlsProtocols))) {
                tlsProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
                WinHttpSetOption(
                    session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &tlsProtocols, sizeof(tlsProtocols));
            }
            WinHttpSetTimeouts(session.h, 3000, 3000, 3500, 5000);
            WinHttpHandleGuard connect{
                WinHttpConnect(session.h, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0)};
            if (connect.h != nullptr && !stopAppUpdate_.load()) {
                TrackedAppRequestGuard request{
                    appHttpMutex_,
                    activeAppUpdateRequest_,
                    stopAppUpdate_,
                    WinHttpOpenRequest(
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

                        std::string rawTag;
                        std::string tagVer;
                        const size_t tagPos = locationUrl.find("/releases/tag/");
                        if (tagPos != std::string::npos) {
                            rawTag = locationUrl.substr(tagPos + 14);
                            const size_t endPos = rawTag.find_first_of("/?#\r\n ");
                            if (endPos != std::string::npos) {
                                rawTag.resize(endPos);
                            }
                            bool tagCharsOk = !rawTag.empty();
                            for (unsigned char ch : rawTag) {
                                const bool ok = (ch >= '0' && ch <= '9') ||
                                                (ch >= 'A' && ch <= 'Z') ||
                                                (ch >= 'a' && ch <= 'z') ||
                                                ch == '.' || ch == '_' || ch == '-';
                                if (!ok) {
                                    tagCharsOk = false;
                                    break;
                                }
                            }
                            if (tagCharsOk) {
                                tagVer = rawTag;
                                if (!tagVer.empty() && (tagVer[0] == 'v' || tagVer[0] == 'V')) {
                                    tagVer.erase(0, 1);
                                }
                                if (tagVer.empty() || !std::isdigit(static_cast<unsigned char>(tagVer[0]))) {
                                    tagVer.clear();
                                    rawTag.clear();
                                }
                            } else {
                                rawTag.clear();
                            }
                        }

                        if (!tagVer.empty() && !rawTag.empty() &&
                            CompareVersionOrString(tagVer, hdm::kAppVersion) > 0) {
                            resultState = AppUpdateState::UpdateAvailable;
                            foundVer = tagVer;
                            foundUrl = "https://github.com/Arvexer/DriverLoom/releases/tag/" + rawTag;
                        } else if (statusCode == 404 || statusCode == 200 ||
                                   (statusCode >= 300 && statusCode < 400)) {
                            resultState = AppUpdateState::UpToDate;
                        }
                    }
                }
            }
        }

        if (stopAppUpdate_.load()) {
            checkingAppUpdate_.store(false);
            return;
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
                std::string(hdm::Tr(
                    "Установлена актуальная версия DriverLoom (v",
                    "Встановлено актуальну версію DriverLoom (v",
                    "DriverLoom is up to date (v")) +
                    hdm::kAppVersion + ").");
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
    const float uiScale = EffectiveUiScale();
    const float modalW = std::min(620.0f * uiScale, vp->WorkSize.x * 0.92f);
    const float maxModalH = std::max(260.0f * uiScale, vp->WorkSize.y * 0.90f);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(modalW, 0.0f), ImVec2(modalW, maxModalH));

    if (!ImGui::BeginPopupModal(
            modalTitle,
            &aboutModalOpen_,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    {
        PushFontSafe(g_fontMainTab);
        const float logoSz = std::round(28.0f * uiScale);
        const float txtH = ImGui::GetTextLineHeight();
        const float rowH = std::max(logoSz, txtH);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(logoSz, rowH));
        DrawAppIcon(
            ImGui::GetWindowDrawList(),
            ImVec2(p0.x + logoSz * 0.5f, p0.y + rowH * 0.5f),
            logoSz,
            AppIcon::BrandLogo,
            ImGui::ColorConvertFloat4ToU32(AccentTextColor()));
        ImGui::SameLine(0.0f, std::round(10.0f * uiScale));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowH - txtH) * 0.5f);
        ImGui::TextColored(AccentTextColor(), "DriverLoom v%s (x64)", hdm::kAppVersion);
        PopFontSafe(g_fontMainTab);
    }
    ImGui::TextWrapped(
        "%s",
        hdm::Tr(
            "Безопасный нативный менеджер драйверов, игровых библиотек и диагностики для Windows 10/11.",
            "Безпечний нативний менеджер драйверів, ігрових бібліотек і діагностики для Windows 10/11.",
            "Safe native driver, gaming runtime, and diagnostics manager for Windows 10/11."));

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Border, PanelBorderColor());
    ImGui::BeginChild(
        "##AboutAuthorCard",
        ImVec2(0.0f, 0.0f),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    {
        RenderSecondaryCardHeader(hdm::Tr("Автор и контакты", "Автор і контакти", "Author & Contact"));
        ImGui::TextDisabled("%s", hdm::Tr("Автор:", "Автор:", "Author:"));
        ImGui::SameLine();
        ImGui::TextColored(AccentTextColor(), "Arvexer");

        ImGui::TextDisabled("GitHub:");
        ImGui::SameLine();
        ImGui::TextUnformatted("https://github.com/Arvexer/DriverLoom");
        const char* ghLbl = hdm::Tr("Открыть GitHub###AboutGh", "Відкрити GitHub###AboutGh", "Open GitHub###AboutGh");
        const char* relLbl = hdm::Tr("Релизы###AboutRel", "Релізи###AboutRel", "Releases###AboutRel");
        const char* issLbl = hdm::Tr("Сообщить о проблеме###AboutIss", "Повідомити про проблему###AboutIss", "Report Issue###AboutIss");
        if (RenderPrimaryButton(ghLbl)) {
            OpenUrlInBrowser("https://github.com/Arvexer/DriverLoom");
        }
        SameLineWrapIfNeeded(CalcButtonWidth(relLbl));
        if (RenderSecondaryButton(relLbl)) {
            OpenUrlInBrowser("https://github.com/Arvexer/DriverLoom/releases");
        }
        SameLineWrapIfNeeded(CalcButtonWidth(issLbl));
        if (RenderSecondaryButton(issLbl)) {
            OpenUrlInBrowser("https://github.com/Arvexer/DriverLoom/issues");
        }

        ImGui::Spacing();
        ImGui::TextDisabled("E-mail:");
        ImGui::SameLine();
        ImGui::TextUnformatted("arvexer.dev@proton.me");
        const char* mailLbl = hdm::Tr("Написать письмо###AboutMail", "Написати листа###AboutMail", "Send E-mail###AboutMail");
        const char* cpMailLbl = hdm::Tr("Копировать e-mail###AboutCopyMail", "Копіювати e-mail###AboutCopyMail", "Copy E-mail###AboutCopyMail");
        const char* kofiLbl = hdm::Tr("Поддержать (Ko-fi)###AboutKofi", "Підтримати (Ko-fi)###AboutKofi", "Support (Ko-fi)###AboutKofi");
        if (RenderSecondaryButton(mailLbl)) {
            OpenUrlInBrowser("mailto:arvexer.dev@proton.me?subject=DriverLoom%20Feedback");
        }
        SameLineWrapIfNeeded(CalcButtonWidth(cpMailLbl));
        if (RenderSecondaryButton(cpMailLbl)) {
            ImGui::SetClipboardText("arvexer.dev@proton.me");
            logBuffer_.Push(
                hdm::LogLevel::Info,
                hdm::Tr(
                    "Скопирован e-mail разработчика: arvexer.dev@proton.me",
                    "Скопійовано e-mail розробника: arvexer.dev@proton.me",
                    "Developer e-mail copied: arvexer.dev@proton.me"));
        }
        SameLineWrapIfNeeded(CalcButtonWidth(kofiLbl) + std::round(22.0f * uiScale));
        if (RenderSecondaryButtonWithIcon(
                kofiLbl,
                AppIcon::Heart,
                ImVec4(0.95f, 0.36f, 0.45f, 1.00f))) {
            OpenUrlInBrowser("https://ko-fi.com/arvexer");
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
        RenderSecondaryCardHeader(hdm::Tr("Обновления приложения", "Оновлення програми", "Application Updates"));

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
            RenderStatusBadge(InfoAccentColor(), hdm::Tr("Проверка обновлений...", "Перевірка оновлень...", "Checking for updates..."), true);
        } else if (updState == AppUpdateState::UpdateAvailable) {
            const std::string updMsg =
                std::string(hdm::Tr("Доступна новая версия: v", "Доступна нова версія: v", "New version available: v")) + latestVer;
            RenderStatusBadge(HealthyColor(), updMsg.c_str(), true);
        } else if (updState == AppUpdateState::UpToDate) {
            const std::string upToDateMsg =
                std::string(hdm::Tr("Установлена актуальная версия (v", "Встановлено актуальну версію (v", "You have the latest version (v")) +
                hdm::kAppVersion + ")";
            RenderStatusBadge(HealthyColor(), upToDateMsg.c_str(), true);
        } else if (updState == AppUpdateState::Error) {
            RenderStatusBadge(WarningColor(), hdm::Tr("Не удалось проверить обновления", "Не вдалося перевірити оновлення", "Failed to check for updates"), true);
        } else {
            const std::string curVerMsg =
                std::string(hdm::Tr("Текущая версия: v", "Поточна версія: v", "Current version: v")) +
                hdm::kAppVersion;
            RenderStatusBadge(MutedTextColor(), curVerMsg.c_str(), true);
        }

        const bool busyCheck = checkingAppUpdate_.load();
        ImGui::BeginDisabled(busyCheck);
        if (RenderSecondaryButtonWithIcon(
                hdm::Tr("Проверить обновления###AboutChkUpd", "Перевірити оновлення###AboutChkUpd", "Check for Updates###AboutChkUpd"),
                AppIcon::Refresh,
                AccentTextColor())) {
            StartAppUpdateCheckAsync(true);
        }
        ImGui::EndDisabled();

        if (updState == AppUpdateState::UpdateAvailable) {
            const char* dlLbl = hdm::Tr(
                "Скачать новую версию###AboutDlUpd",
                "Завантажити нову версію###AboutDlUpd",
                "Download Update###AboutDlUpd");
            SameLineWrapIfNeeded(CalcButtonWidth(dlLbl));
            if (RenderPrimaryButton(dlLbl)) {
                OpenUrlInBrowser(releaseUrl);
            }
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    PushFontSafe(g_fontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, MutedTextColor());
    ImGui::TextWrapped("C++20 • Win32 SetupAPI / WUA COM • DirectX 11 • Dear ImGui • MIT License © 2026 Arvexer");
    ImGui::PopStyleColor();
    PopFontSafe(g_fontSmall);
    ImGui::Separator();

    const char* closeLbl = hdm::Tr("Закрыть###AboutCloseBtn", "Закрити###AboutCloseBtn", "Close###AboutCloseBtn");
    const float closeW = CalcButtonWidth(hdm::Tr("Закрыть", "Закрити", "Close")) + 12.0f * uiScale;
    const float closeX = ImGui::GetWindowContentRegionMax().x - closeW;
    if (closeX > style.WindowPadding.x) {
        ImGui::SetCursorPosX(closeX);
    }
    if (RenderPrimaryButton(closeLbl, ImVec2(closeW, 0.0f)) ||
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
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
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
    if (!g_isAdvancedMode) {
        g_isAdvancedMode = true;
    }
    activeTabIndex_ = PageLocalInstaller;
    localSectionIndex_ = 1;
    storeFilterMode_ = DriverStoreFilterMode::All;
    std::snprintf(storeFilterBuf_, sizeof(storeFilterBuf_), "%s", infName.c_str());
    storeViewDirty_ = true;
}

void DriverLoomApp::HandleKeyboardShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();

    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        switch (activeTabIndex_) {
        case PageOverview:
        case PageDevices:
            if (!scanner_.IsBusy() && !componentsScanner_.IsInstalling()) {
                scanner_.StartScanAsync(MakeLogger());
            }
            break;
        case PageComponents:
            if (!componentsScanner_.IsScanning() &&
                !componentsScanner_.IsCheckingOnline() &&
                !componentsScanner_.IsInstalling()) {
                componentsScanner_.StartLocalScanAsync(devicesCache_, MakeLogger());
            }
            break;
        case PageWindowsUpdate:
            if (!wuManager_.IsBusy()) {
                selectedWuIndex_ = -1;
                wuHasSearchedOnce_ = true;
                wuManager_.StartSearchAsync(MakeLogger(), !wuIncludeSystemUpdates_);
            }
            break;
        case PageLocalInstaller:
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
        case PageDiagnostics:
            if (!diagnostics_.IsBusy()) {
                diagnostics_.StartScanAsync(devicesCache_, MakeLogger());
            }
            break;
        default:
            break;
        }
    }

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        if (activeTabIndex_ != PageDevices) {
            activeTabIndex_ = PageDevices;
        }
        focusDeviceSearchNextFrame_ = true;
    }

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_L, false)) {
        logPanelExpanded_ = !logPanelExpanded_;
    }

    if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        if (activeTabIndex_ == PageDevices) {
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
        } else if (activeTabIndex_ == PageWindowsUpdate) {
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

    if (!io.WantTextInput && !aboutModalOpen_ &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (activeTabIndex_ == PageDevices && !selectedDeviceInstanceId_.empty()) {
            selectedDeviceInstanceId_.clear();
        } else if (activeTabIndex_ == PageWindowsUpdate && selectedWuIndex_ >= 0) {
            selectedWuIndex_ = -1;
        }
    }
}

void DriverLoomApp::RenderStatusBarAndLogDrawer() {
    const float uiScale = EffectiveUiScale();
    bool wasDirty = false;
    const auto entries = logBuffer_.Snapshot(&wasDirty);

    // Top separator line above status bar / drawer
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(
            p,
            ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y),
            ImGui::ColorConvertFloat4ToU32(PanelBorderColor()),
            1.0f);
    }

    const ImVec4 barBg = g_isLightTheme
                             ? ImVec4(0.937f, 0.949f, 0.973f, 1.00f)
                             : ImVec4(0.051f, 0.066f, 0.098f, 1.00f);

    if (!logPanelExpanded_) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, barBg);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(std::round(18.0f * uiScale), std::round(5.0f * uiScale)));
        ImGui::BeginChild(
            "##StatusBar",
            ImVec2(0.0f, 0.0f),
            ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        const float barH = ImGui::GetContentRegionAvail().y;
        const bool anyBusy = scanner_.IsBusy() || componentsScanner_.IsScanning() ||
                             componentsScanner_.IsCheckingOnline() || componentsScanner_.IsInstalling() ||
                             wuManager_.IsBusy() || diagnostics_.IsBusy() ||
                             localInstaller_.IsInstalling() || localInstaller_.IsExportingBackup() ||
                             localInstaller_.IsRemovingStorePackages() || systemRestore_.IsBusy();

        ImVec4 dotCol = HealthyColor();
        std::string statusText;
        if (componentsScanner_.IsInstalling()) {
            dotCol = InfoAccentColor();
            statusText = hdm::TrDyn(componentsScanner_.PhaseMessage());
        } else if (wuManager_.IsBusy()) {
            dotCol = InfoAccentColor();
            statusText = hdm::TrDyn(wuManager_.PhaseMessage());
        } else if (diagnostics_.IsRunningAction()) {
            dotCol = WarningColor();
            statusText = hdm::TrDyn(diagnostics_.ActionPhase());
        } else if (localInstaller_.IsInstalling() || localInstaller_.IsExportingBackup() ||
                   localInstaller_.IsRemovingStorePackages()) {
            dotCol = InfoAccentColor();
            statusText = hdm::TrDyn(localInstaller_.StatusMessage());
        } else if (systemRestore_.IsBusy()) {
            dotCol = InfoAccentColor();
            statusText = hdm::Tr(
                "Создание точки восстановления...",
                "Створення точки відновлення...",
                "Creating restore point...");
        } else if (anyBusy) {
            dotCol = InfoAccentColor();
            statusText = hdm::Tr("Выполняется проверка...", "Виконується перевірка...", "Checking system...");
        } else if (!entries.empty()) {
            const auto& last = entries.back();
            if (last.level == hdm::LogLevel::Error) dotCol = DangerColor();
            else if (last.level == hdm::LogLevel::Warning) dotCol = WarningColor();
            else if (last.level == hdm::LogLevel::Success) dotCol = HealthyColor();
            else dotCol = InfoAccentColor();
            statusText = hdm::TrDyn(last.message);
        } else {
            statusText = hdm::Tr("Готово", "Готово", "Ready");
        }

        // Right-side Journal toggle button (comfortable regular font size + chevron)
        char logBtnLbl[64];
        std::snprintf(
            logBtnLbl,
            sizeof(logBtnLbl),
            hdm::Tr("Журнал (%zu)    ###ToggleLogBtn", "Журнал (%zu)    ###ToggleLogBtn", "Log (%zu)    ###ToggleLogBtn"),
            entries.size());
        const float toggleBtnW =
            ImGui::CalcTextSize(logBtnLbl, nullptr, true).x + std::round(30.0f * uiScale);
        const float availW = ImGui::GetContentRegionAvail().x;
        const float maxTextW = std::max(80.0f, availW - toggleBtnW - std::round(26.0f * uiScale));

        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float cy = p0.y + barH * 0.5f;
        const float dotR = std::round(4.2f * uiScale);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(ImVec2(p0.x + dotR, cy), dotR, ImGui::ColorConvertFloat4ToU32(dotCol));

        const std::string shownStatus = EllipsizeUtf8(statusText, maxTextW);
        const ImVec2 ts = ImGui::CalcTextSize(shownStatus.c_str());
        const ImVec4 statusTextCol = g_isLightTheme
                                         ? ImVec4(0.18f, 0.24f, 0.34f, 1.00f)
                                         : ImVec4(0.80f, 0.86f, 0.94f, 1.00f);
        dl->AddText(
            ImVec2(p0.x + dotR * 2.0f + std::round(10.0f * uiScale), cy - ts.y * 0.5f),
            ImGui::ColorConvertFloat4ToU32(statusTextCol),
            shownStatus.c_str());

        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - toggleBtnW);
        const float btnH = std::min(barH, std::round(30.0f * uiScale));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (barH - btnH) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(std::round(10.0f * uiScale), 0.0f));
        if (RenderSecondaryButton(logBtnLbl, ImVec2(toggleBtnW, btnH))) {
            logPanelExpanded_ = true;
        }
        ImGui::PopStyleVar();
        if (ImGui::IsItemVisible()) {
            const ImVec2 rMax = ImGui::GetItemRectMax();
            const ImVec2 rMin = ImGui::GetItemRectMin();
            DrawAppIcon(
                dl,
                ImVec2(rMax.x - std::round(12.0f * uiScale), (rMin.y + rMax.y) * 0.5f),
                std::round(10.0f * uiScale),
                AppIcon::ChevronUp,
                ImGui::ColorConvertFloat4ToU32(statusTextCol));
        }
        ImGui::EndChild();
        return;
    }

    // Expanded Event Log Drawer
    ImGui::PushStyleColor(ImGuiCol_ChildBg, barBg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(std::round(18.0f * uiScale), std::round(8.0f * uiScale)));
    ImGui::BeginChild(
        "##LogDrawer",
        ImVec2(0.0f, 0.0f),
        ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    const ImGuiStyle& style = ImGui::GetStyle();
    {
        PushFontSafe(g_fontSecondaryHeader);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(CardHeaderTextColor(), "%s", hdm::Tr("Журнал событий", "Журнал подій", "Event Log"));
        PopFontSafe(g_fontSecondaryHeader);
    }
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("(%zu)", entries.size());

    const float headerEndX = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * uiScale, 4.0f * uiScale));
    const char* copyTxt = hdm::Tr("Копировать", "Копіювати", "Copy");
    const char* clearTxt = hdm::Tr("Очистить", "Очистити", "Clear");
    const char* hideTxt = hdm::Tr("Свернуть", "Згорнути", "Collapse");
    const char* autoTxt = hdm::Tr("Автопрокрутка", "Автопрокрутка", "Auto-scroll");
    const float copyW = ImGui::CalcTextSize(copyTxt).x + 20.0f * uiScale;
    const float clearW = ImGui::CalcTextSize(clearTxt).x + 20.0f * uiScale;
    const float hideW = ImGui::CalcTextSize(hideTxt).x + 22.0f * uiScale;
    const float autoW = ImGui::CalcTextSize(autoTxt).x + ImGui::GetFrameHeight() + 8.0f;
    const float totalRightW = autoW + copyW + clearW + hideW + style.ItemSpacing.x * 3.0f;
    const float winRight = ImGui::GetWindowContentRegionMax().x;
    if (winRight - totalRightW > headerEndX + 12.0f * uiScale) {
        ImGui::SameLine(winRight - totalRightW);
    }

    ImGui::Checkbox(hdm::Tr("Автопрокрутка###LogAuto", "Автопрокрутка###LogAuto", "Auto-scroll###LogAuto"), &logAutoScroll_);
    SameLineWrapIfNeeded(copyW);
    if (RenderSecondaryButton(hdm::Tr("Копировать###LogCopy", "Копіювати###LogCopy", "Copy###LogCopy"), ImVec2(copyW, 0.0f))) {
        std::string all;
        for (const auto& e : entries) {
            all += "[" + e.timestamp + "] " + hdm::TrDyn(e.message) + "\n";
        }
        ImGui::SetClipboardText(all.c_str());
    }
    SameLineWrapIfNeeded(clearW);
    if (RenderSecondaryButton(hdm::Tr("Очистить###LogClr", "Очистити###LogClr", "Clear###LogClr"), ImVec2(clearW, 0.0f))) {
        logBuffer_.Clear();
    }
    SameLineWrapIfNeeded(hideW);
    if (RenderSecondaryButton(hdm::Tr("Свернуть###LogHide", "Згорнути###LogHide", "Collapse###LogHide"), ImVec2(hideW, 0.0f))) {
        logPanelExpanded_ = false;
    }
    ImGui::PopStyleVar();
    ImGui::Separator();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild("##LogScroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_None);
    ImGui::PopStyleColor();

    const float wrapRightX =
        std::max(140.0f, ImGui::GetWindowContentRegionMax().x - std::round(12.0f * uiScale));
    ImGui::PushTextWrapPos(wrapRightX);
    for (const auto& entry : entries) {
        ImVec4 col = g_isLightTheme
                         ? ImVec4(0.16f, 0.21f, 0.30f, 1.00f)
                         : ImVec4(0.84f, 0.89f, 0.96f, 1.00f);
        if (entry.level == hdm::LogLevel::Success) {
            col = g_isLightTheme ? ImVec4(0.05f, 0.48f, 0.30f, 1.00f) : HealthyColor();
        } else if (entry.level == hdm::LogLevel::Warning) {
            col = WarningTextColor();
        } else if (entry.level == hdm::LogLevel::Error) {
            col = g_isLightTheme ? ImVec4(0.76f, 0.14f, 0.14f, 1.00f) : DangerColor();
        }
        const std::string msgShown = hdm::TrDyn(entry.message);
        ImGui::TextDisabled("[%s]", entry.timestamp.c_str());
        ImGui::SameLine(0.0f, std::round(8.0f * uiScale));
        ImGui::BeginGroup();
        ImGui::TextColored(col, "%s", msgShown.c_str());
        ImGui::EndGroup();
    }
    ImGui::PopTextWrapPos();
    if (logAutoScroll_ && wasDirty) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

void DriverLoomApp::OpenSystemProtectionPreviewModal() noexcept {
    srPromptState_ = hdm::SystemProtectionState::Disabled;
    srPromptActionTitle_ = "NVIDIA GeForce / Display Driver";
    srPromptInstallCallback_ = nullptr;
    openSrPromptNextFrame_ = true;
}

void DriverLoomApp::RequestProtectedInstall(
    std::string actionTitle,
    bool wantRestorePoint,
    std::function<void(bool createRestorePoint)> startInstall) {
    if (!startInstall) {
        return;
    }
    if (!wantRestorePoint) {
        startInstall(false);
        return;
    }
    const hdm::SystemProtectionState st = hdm::SystemRestore::QuerySystemProtectionState();
    if (st == hdm::SystemProtectionState::Enabled) {
        startInstall(true);
        return;
    }
    srPromptState_ = st;
    lastSrPromptPollTime_ = ImGui::GetTime();
    srPromptActionTitle_ = std::move(actionTitle);
    srPromptInstallCallback_ = std::move(startInstall);
    openSrPromptNextFrame_ = true;
}

void DriverLoomApp::ProcessPendingSystemProtectionResult() {
    if (!srEnablePendingResult_.exchange(false)) {
        return;
    }
    pendingDiagRescan_.store(true);
    std::function<void(bool)> pendingInstall = std::move(srPendingAfterEnableCallback_);
    srPendingAfterEnableCallback_ = nullptr;

    std::string resultMsg;
    {
        std::lock_guard<std::mutex> lock(srEnableMsgMutex_);
        resultMsg = std::move(srEnableResultMsg_);
        srEnableResultMsg_.clear();
    }

    if (srEnableSucceeded_.load()) {
        diagCache_.readiness.systemRestoreEnabled = true;
        diagCache_.readiness.systemRestorePolicyDisabled = false;
        if (pendingInstall) {
            pendingInstall(false);
        }
    } else if (!resultMsg.empty()) {
        logBuffer_.Push(hdm::LogLevel::Warning, resultMsg);
    }
}

void DriverLoomApp::RenderSystemProtectionModal() {
    const char* modalTitle = hdm::Tr(
        "Защита системы отключена###SrPromptModal",
        "Захист системи вимкнено###SrPromptModal",
        "System Protection Is Off###SrPromptModal");

    if (openSrPromptNextFrame_) {
        srPromptModalOpen_ = true;
        lastSrPromptPollTime_ = ImGui::GetTime();
        ImGui::OpenPopup(modalTitle);
        openSrPromptNextFrame_ = false;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float uiScale = EffectiveUiScale();
    const float modalW = std::min(560.0f * uiScale, vp->WorkSize.x * 0.92f);
    const float maxModalH = std::max(240.0f * uiScale, vp->WorkSize.y * 0.90f);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(modalW, 0.0f), ImVec2(modalW, maxModalH));

    if (!ImGui::BeginPopupModal(
            modalTitle,
            &srPromptModalOpen_,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        if (!srPromptModalOpen_ && srPromptInstallCallback_) {
            srPromptInstallCallback_ = nullptr;
            srPromptActionTitle_.clear();
        }
        return;
    }

    const double now = ImGui::GetTime();
    if (ImGui::IsWindowAppearing() || now - lastSrPromptPollTime_ >= 0.5) {
        if (srPromptInstallCallback_) {
            srPromptState_ = hdm::SystemRestore::QuerySystemProtectionState();
        }
        lastSrPromptPollTime_ = now;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    {
        PushFontSafe(g_fontSecondaryHeader);
        const float iconSz = std::round(24.0f * uiScale);
        const float txtH = ImGui::GetTextLineHeight();
        const float rowH = std::max(iconSz, txtH);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec4 hdrCol =
            (srPromptState_ == hdm::SystemProtectionState::Enabled)
                ? HealthyColor()
                : (srPromptState_ == hdm::SystemProtectionState::DisabledByPolicy
                       ? DangerColor()
                       : WarningColor());
        ImGui::Dummy(ImVec2(iconSz, rowH));
        DrawAppIcon(
            ImGui::GetWindowDrawList(),
            ImVec2(p0.x + iconSz * 0.5f, p0.y + rowH * 0.5f),
            iconSz,
            AppIcon::Shield,
            ImGui::ColorConvertFloat4ToU32(hdrCol));
        ImGui::SameLine(0.0f, std::round(10.0f * uiScale));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowH - txtH) * 0.5f);
        const char* headerText =
            (srPromptState_ == hdm::SystemProtectionState::Enabled)
                ? hdm::Tr(
                      "Защита системы включена",
                      "Захист системи увімкнено",
                      "System Protection Enabled")
                : (srPromptState_ == hdm::SystemProtectionState::DisabledByPolicy
                       ? hdm::Tr(
                             "Точки восстановления запрещены политикой",
                             "Точки відновлення заборонено політикою",
                             "Restore Points Blocked by Policy")
                       : hdm::Tr(
                             "Защита системы для точек отката выключена",
                             "Захист системи для точок відкату вимкнено",
                             "System Protection for Restore Points Is Off"));
        ImGui::TextColored(CardHeaderTextColor(), "%s", headerText);
        PopFontSafe(g_fontSecondaryHeader);
    }

    if (!srPromptActionTitle_.empty()) {
        PushFontSafe(g_fontSmall);
        ImGui::TextColored(
            MutedTextColor(),
            "%s %s",
            hdm::Tr("Действие:", "Дія:", "Action:"),
            hdm::TrDyn(srPromptActionTitle_).c_str());
        PopFontSafe(g_fontSmall);
    }

    ImGui::Spacing();
    if (srPromptState_ == hdm::SystemProtectionState::Enabled) {
        ImGui::TextWrapped(
            "%s",
            hdm::Tr(
                "Защита системного диска теперь активна. Можно создать точку восстановления и безопасно продолжить установку.",
                "Захист системного диска тепер активний. Можна створити точку відновлення та безпечно продовжити встановлення.",
                "System drive protection is now active. You can create a restore point and safely proceed with installation."));
    } else if (srPromptState_ == hdm::SystemProtectionState::DisabledByPolicy) {
        ImGui::TextWrapped(
            "%s",
            hdm::Tr(
                "Создание точек восстановления заблокировано групповой политикой Windows (DisableSR). Автоматический откат перед установкой невозможен.",
                "Створення точок відновлення заблоковано груповою політикою Windows (DisableSR). Автоматичний відкат перед встановленням неможливий.",
                "Restore point creation is blocked by Windows Group Policy (DisableSR). Automatic rollback before installation is unavailable."));
    } else {
        ImGui::TextWrapped(
            "%s",
            hdm::Tr(
                "Перед установкой драйвера или компонента DriverLoom создаёт точку восстановления Windows, но защита системного диска сейчас отключена. Рекомендуется включить её перед продолжением.",
                "Перед встановленням драйвера або компонента DriverLoom створює точку відновлення Windows, але захист системного диска зараз вимкнено. Рекомендується увімкнути його перед продовженням.",
                "Before installing a driver or component, DriverLoom creates a Windows Restore Point, but System Protection on your system drive is currently off. Enabling it before continuing is recommended."));
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Border, PanelBorderColor());
    ImGui::BeginChild(
        "##SrPromptStatusCard",
        ImVec2(0.0f, 0.0f),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    {
        const std::string driveUtf8 = hdm::WideToUtf8(hdm::SystemRestore::GetSystemDriveRootW());
        PushFontSafe(g_fontSmall);
        if (srPromptState_ == hdm::SystemProtectionState::Enabled) {
            const std::string okTxt =
                std::string(hdm::Tr("Системный диск (", "Системний диск (", "System drive (")) +
                driveUtf8 +
                hdm::Tr("): защита включена", "): захист увімкнено", "): protection enabled");
            RenderStatusBadge(HealthyColor(), okTxt.c_str(), true);
        } else if (srPromptState_ == hdm::SystemProtectionState::DisabledByPolicy) {
            RenderStatusBadge(
                DangerColor(),
                hdm::Tr(
                    "Ограничение политики Windows (DisableSR = 1)",
                    "Обмеження політики Windows (DisableSR = 1)",
                    "Windows Policy restriction (DisableSR = 1)"),
                true);
        } else {
            const std::string warnTxt =
                std::string(hdm::Tr("Системный диск (", "Системний диск (", "System drive (")) +
                driveUtf8 +
                hdm::Tr("): защита отключена", "): захист вимкнено", "): protection disabled");
            RenderStatusBadge(WarningColor(), warnTxt.c_str(), true);
        }
        PopFontSafe(g_fontSmall);
    }
    ImGui::EndChild();

    ImGui::Spacing();
    const bool srBusy = systemRestore_.IsBusy();

    if (srPromptState_ == hdm::SystemProtectionState::Enabled) {
        const char* contLbl = hdm::Tr(
            "Продолжить установку###SrContBtn",
            "Продовжити встановлення###SrContBtn",
            "Continue Installation###SrContBtn");
        ImGui::BeginDisabled(srBusy);
        if (RenderPrimaryButton(contLbl)) {
            std::function<void(bool)> cb = std::move(srPromptInstallCallback_);
            srPromptInstallCallback_ = nullptr;
            srPromptActionTitle_.clear();
            diagCache_.readiness.systemRestoreEnabled = true;
            diagCache_.readiness.systemRestorePolicyDisabled = false;
            pendingDiagRescan_.store(true);
            srPromptModalOpen_ = false;
            ImGui::CloseCurrentPopup();
            if (cb) {
                cb(true);
            }
        }
        ImGui::EndDisabled();
    } else if (srPromptState_ == hdm::SystemProtectionState::Disabled) {
        const char* enableLbl = hdm::Tr(
            "Включить защиту и продолжить###SrEnableContBtn",
            "Увімкнути захист і продовжити###SrEnableContBtn",
            "Enable Protection & Continue###SrEnableContBtn");
        const char* winSetLbl = hdm::Tr(
            "Настройки Windows...###SrWinSetBtn",
            "Налаштування Windows...###SrWinSetBtn",
            "Windows Settings...###SrWinSetBtn");
        const char* skipLbl = hdm::Tr(
            "Установить без точки отката###SrSkipBtn",
            "Встановити без точки відкату###SrSkipBtn",
            "Install Without Restore Point###SrSkipBtn");

        ImGui::BeginDisabled(srBusy);
        if (RenderPrimaryButton(enableLbl)) {
            std::function<void(bool)> cb = std::move(srPromptInstallCallback_);
            srPromptInstallCallback_ = nullptr;
            srPromptActionTitle_.clear();
            srPendingAfterEnableCallback_ = std::move(cb);
            srPromptModalOpen_ = false;
            ImGui::CloseCurrentPopup();
            if (cb || srPendingAfterEnableCallback_) {
                systemRestore_.EnableSystemProtectionAsync(
                    true,
                    "DriverLoom (перед установкой драйвера/компонента)",
                    MakeLogger(),
                    [this](bool ok, const std::string& msg) {
                        {
                            std::lock_guard<std::mutex> lock(srEnableMsgMutex_);
                            srEnableResultMsg_ = msg;
                        }
                        srEnableSucceeded_.store(ok);
                        srEnablePendingResult_.store(true);
                    });
            }
        }
        ImGui::EndDisabled();

        SameLineWrapIfNeeded(CalcButtonWidth(winSetLbl));
        if (RenderSecondaryButton(winSetLbl)) {
            wchar_t sysDir[MAX_PATH]{};
            const UINT sysLen = GetSystemDirectoryW(sysDir, MAX_PATH);
            if (sysLen > 0 && sysLen < MAX_PATH) {
                const std::filesystem::path fullPath =
                    std::filesystem::path(sysDir) / L"SystemPropertiesProtection.exe";
                ShellExecuteW(hwnd_, L"open", fullPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        }

        SameLineWrapIfNeeded(CalcButtonWidth(skipLbl));
        if (RenderSecondaryButton(skipLbl)) {
            std::function<void(bool)> cb = std::move(srPromptInstallCallback_);
            srPromptInstallCallback_ = nullptr;
            srPromptActionTitle_.clear();
            srPromptModalOpen_ = false;
            ImGui::CloseCurrentPopup();
            if (cb) {
                cb(false);
            }
        }
    } else {
        const char* skipLbl = hdm::Tr(
            "Установить без точки отката###SrSkipBtn",
            "Встановити без точки відкату###SrSkipBtn",
            "Install Without Restore Point###SrSkipBtn");
        if (RenderSecondaryButton(skipLbl)) {
            std::function<void(bool)> cb = std::move(srPromptInstallCallback_);
            srPromptInstallCallback_ = nullptr;
            srPromptActionTitle_.clear();
            srPromptModalOpen_ = false;
            ImGui::CloseCurrentPopup();
            if (cb) {
                cb(false);
            }
        }
    }

    const char* cancelLbl = hdm::Tr(
        "Отмена###SrCancelBtn",
        "Скасувати###SrCancelBtn",
        "Cancel###SrCancelBtn");
    SameLineWrapIfNeeded(CalcButtonWidth(cancelLbl) + style.ItemSpacing.x);
    if (RenderSecondaryButton(cancelLbl)) {
        srPromptInstallCallback_ = nullptr;
        srPromptActionTitle_.clear();
        srPromptModalOpen_ = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}
