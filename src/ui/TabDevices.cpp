#include "ui/DriverLoomApp.hpp"

#include <cfgmgr32.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

void DriverLoomApp::RebuildDeviceViewCache(ImGuiTableSortSpecs* sortSpecs) {
    visibleDeviceIndices_.clear();
    visibleDeviceIndices_.reserve(devicesCache_.size());

    std::wstring needleLower = hdm::Utf8ToWide(deviceFilterBuf_);
    if (!needleLower.empty()) {
        CharLowerBuffW(needleLower.data(), static_cast<DWORD>(needleLower.size()));
    }

    for (size_t i = 0; i < devicesCache_.size(); ++i) {
        const auto& d = devicesCache_[i];
        switch (deviceFilterMode_) {
        case DeviceFilterMode::ProblemOnly:
            if (g_isAdvancedMode) {
                if (d.health == hdm::DeviceHealth::Ok && !d.isGenericMonitorDriver) continue;
            } else {
                if (!IsUserRelevantDeviceIssue(d)) continue;
            }
            break;
        case DeviceFilterMode::Monitors:
            if (d.category != hdm::DeviceCategory::Monitor) continue;
            break;
        case DeviceFilterMode::Peripherals:
            if (d.category != hdm::DeviceCategory::Peripheral) continue;
            break;
        case DeviceFilterMode::DisplayAndChipset:
            if (d.category != hdm::DeviceCategory::DisplayAndChipset) continue;
            break;
        case DeviceFilterMode::All:
        default:
            break;
        }

        if (!needleLower.empty() &&
            d.searchIndexLower.find(needleLower) == std::wstring::npos) {
            continue;
        }
        visibleDeviceIndices_.push_back(i);
    }

    if (sortSpecs != nullptr && sortSpecs->SpecsCount > 0) {
        const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
        const bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
        std::sort(
            visibleDeviceIndices_.begin(),
            visibleDeviceIndices_.end(),
            [&](size_t ia, size_t ib) {
                const auto& a = devicesCache_[ia];
                const auto& b = devicesCache_[ib];
                int cmp = 0;
                switch (spec.ColumnUserID) {
                case 0: {
                    auto rank = [&](const hdm::DeviceInfo& d) {
                        if (d.health == hdm::DeviceHealth::Error) return 0;
                        if (IsUserRelevantDeviceIssue(d)) return 1;
                        if (d.health == hdm::DeviceHealth::Warning || d.isGenericMonitorDriver) return 2;
                        return 3;
                    };
                    cmp = rank(a) - rank(b);
                    if (cmp == 0) cmp = a.statusText.compare(b.statusText);
                    break;
                }
                case 1: cmp = a.name.compare(b.name); break;
                case 2: cmp = a.manufacturer.compare(b.manufacturer); break;
                case 3: cmp = a.deviceClass.compare(b.deviceClass); break;
                case 4: cmp = CompareVersionOrString(a.driverVersion, b.driverVersion); break;
                case 5: cmp = a.driverDate.compare(b.driverDate); break;
                case 6: cmp = a.hardwareId.compare(b.hardwareId); break;
                default: break;
                }
                return asc ? (cmp < 0) : (cmp > 0);
            });
    }

    deviceViewDirty_ = false;
}

void DriverLoomApp::RenderDevicesTab() {
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.0f);
    const bool isAppForeground = (GetForegroundWindow() == hwnd_);

    RenderPageHeader(
        hdm::Tr("Устройства и мониторы", "Пристрої та монітори", "Devices & Monitors"),
        hdm::Tr(
            "Нажмите на любое устройство в списке, чтобы посмотреть сведения или установить официальный драйвер.",
            "Натисніть на будь-який пристрій у списку, щоб переглянути відомості або встановити офіційний драйвер.",
            "Click any device below to view details or install an official driver."));

    const bool scanning = scanner_.IsScanning();
    const bool catalogBusy = componentsScanner_.IsInstalling();
    const bool devBusy = scanner_.IsBusy() || catalogBusy;

    // Row 1: Filter Pills + Scan + Summary Badge
    const char* fProbTxt = hdm::Tr("Внимание", "Увага", "Attention");
    const char* fMonTxt = hdm::Tr("Мониторы", "Монітори", "Monitors");
    const char* fPerTxt = hdm::Tr("Периферия", "Периферія", "Peripherals");
    const char* fGpuTxt = hdm::Tr("Видео и чипсет", "Відео та чипсет", "GPU & Chipset");

    if (RenderPillButton(hdm::Tr("Все###DfAll", "Усі###DfAll", "All###DfAll"),
                         deviceFilterMode_ == DeviceFilterMode::All)) {
        deviceFilterMode_ = DeviceFilterMode::All;
        deviceViewDirty_ = true;
    }
    SameLineWrapIfNeeded(CalcButtonWidth(fProbTxt));
    if (RenderPillButton(hdm::Tr("Внимание###DfProb", "Увага###DfProb", "Attention###DfProb"),
                         deviceFilterMode_ == DeviceFilterMode::ProblemOnly)) {
        deviceFilterMode_ = DeviceFilterMode::ProblemOnly;
        deviceViewDirty_ = true;
    }
    SameLineWrapIfNeeded(CalcButtonWidth(fMonTxt));
    if (RenderPillButton(hdm::Tr("Мониторы###DfMon", "Монітори###DfMon", "Monitors###DfMon"),
                         deviceFilterMode_ == DeviceFilterMode::Monitors)) {
        deviceFilterMode_ = DeviceFilterMode::Monitors;
        deviceViewDirty_ = true;
    }
    SameLineWrapIfNeeded(CalcButtonWidth(fPerTxt));
    if (RenderPillButton(hdm::Tr("Периферия###DfPer", "Периферія###DfPer", "Peripherals###DfPer"),
                         deviceFilterMode_ == DeviceFilterMode::Peripherals)) {
        deviceFilterMode_ = DeviceFilterMode::Peripherals;
        deviceViewDirty_ = true;
    }
    SameLineWrapIfNeeded(CalcButtonWidth(fGpuTxt));
    if (RenderPillButton(hdm::Tr("Видео и чипсет###DfGpu", "Відео та чипсет###DfGpu", "GPU & Chipset###DfGpu"),
                         deviceFilterMode_ == DeviceFilterMode::DisplayAndChipset)) {
        deviceFilterMode_ = DeviceFilterMode::DisplayAndChipset;
        deviceViewDirty_ = true;
    }

    // Right-aligned Refresh + optional PnP Rescan + Summary
    const char* devScanLbl =
        scanning
            ? hdm::Tr("Поиск...###DevScan", "Пошук...###DevScan", "Scanning...###DevScan")
            : hdm::Tr("Обновить###DevScan", "Оновити###DevScan", "Refresh###DevScan");
    const float scanW = CalcButtonWidth(scanning ? hdm::Tr("Поиск...", "Пошук...", "Scanning...")
                                                 : hdm::Tr("Обновить", "Оновити", "Refresh")) +
                        4.0f * uiScale;
    const float pnpW =
        g_isAdvancedMode ? (CalcButtonWidth(hdm::Tr("Обновить PnP", "Оновити PnP", "Rescan PnP")) + ImGui::GetStyle().ItemSpacing.x)
                         : 0.0f;

    char totStr[32], monStr[32], probStr[32];
    const size_t shownIssues = g_isAdvancedMode ? problemDevicesCount_ : userRelevantIssueCount_;
    std::snprintf(totStr, sizeof(totStr), "%zu", totalDevicesCount_);
    std::snprintf(monStr, sizeof(monStr), "%zu", monitorDevicesCount_);
    std::snprintf(probStr, sizeof(probStr), "%zu", shownIssues);

    const ImVec4 probAccent = (shownIssues > 0) ? WarningColor() : HealthyColor();
    const SummaryBadgeSegment segments[3] = {
        {hdm::Tr("Всего:", "Усього:", "Total:"), totStr, InfoAccentColor(), false},
        {hdm::Tr("Мониторы:", "Монітори:", "Monitors:"), monStr, ImVec4(0.24f, 0.78f, 0.90f, 1.00f), false},
        {hdm::Tr("Внимание:", "Увага:", "Attention:"), probStr, probAccent, shownIssues > 0},
    };
    const float summaryW = CalcSummaryInfoBadgeWidth(segments, 3);
    const float rightTotalW = summaryW + ImGui::GetStyle().ItemSpacing.x + pnpW + scanW;
    const float lastItemEndLocalX = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    const float winRightX = ImGui::GetWindowContentRegionMax().x;
    if (winRightX - rightTotalW > lastItemEndLocalX + 10.0f * uiScale) {
        ImGui::SameLine(winRightX - rightTotalW);
    } else {
        ImGui::Dummy(ImVec2(0.0f, std::round(2.0f * uiScale)));
    }

    RenderSummaryInfoBadge(segments, 3);
    ImGui::SameLine();
    if (g_isAdvancedMode) {
        ImGui::BeginDisabled(devBusy);
        if (RenderSecondaryButton(hdm::Tr("Обновить PnP###DevPnp", "Оновити PnP###DevPnp", "Rescan PnP###DevPnp"))) {
            scanner_.RunDeviceActionAsync(
                hdm::DeviceAction::Reenumerate,
                {},
                hdm::Tr("Шина PnP", "Шина PnP", "PnP Bus"),
                MakeLogger(),
                [this](bool, bool rebootReq) {
                    if (rebootReq) rebootRequired_.store(true);
                });
        }
        if (isAppForeground &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "%s",
                hdm::Tr(
                    "Повторный опрос шины PnP (CM_Reenumerate_DevNode) для поиска подключённых устройств.",
                    "Повторне опитування шини PnP (CM_Reenumerate_DevNode) для пошуку підключених пристроїв.",
                    "Rescan the PnP bus (CM_Reenumerate_DevNode) to detect connected hardware."));
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(devBusy);
    if (RenderSecondaryButton(devScanLbl, ImVec2(scanW, 0.0f))) {
        scanner_.StartScanAsync(MakeLogger());
    }
    ImGui::EndDisabled();

    // Row 2: Search Bar
    ImGui::SetNextItemWidth(std::max(180.0f, ImGui::GetContentRegionAvail().x));
    if (focusDeviceSearchNextFrame_) {
        ImGui::SetKeyboardFocusHere();
        focusDeviceSearchNextFrame_ = false;
    }
    if (ImGui::InputTextWithHint(
            "##DeviceSearch",
            g_isAdvancedMode
                ? hdm::Tr(
                      "Поиск по названию, модели монитора (EDID), бренду, версии или Hardware ID... (Ctrl+F)",
                      "Пошук за назвою, моделлю монітора (EDID), брендом, версією або Hardware ID... (Ctrl+F)",
                      "Search by name, monitor model (EDID), brand, version, or Hardware ID... (Ctrl+F)")
                : hdm::Tr(
                      "Поиск устройства, монитора, мыши, клавиатуры или производителя... (Ctrl+F)",
                      "Пошук пристрою, монітора, миші, клавіатури або виробника... (Ctrl+F)",
                      "Search for a device, monitor, mouse, keyboard, or brand... (Ctrl+F)"),
            deviceFilterBuf_,
            sizeof(deviceFilterBuf_))) {
        deviceViewDirty_ = true;
    }

    if (catalogBusy) {
        const float prog = componentsScanner_.Progress();
        const std::string phase = hdm::TrDyn(componentsScanner_.PhaseMessage());
        char overlay[256];
        std::snprintf(
            overlay,
            sizeof(overlay),
            "%s (%.0f%%)",
            phase.empty()
                ? hdm::Tr("Установка официального драйвера...", "Встановлення офіційного драйвера...", "Installing official driver...")
                : phase.c_str(),
            prog * 100.0f);
        const char* cancelTxt = hdm::Tr("Отмена", "Скасувати", "Cancel");
        const float cancelW = CalcButtonWidth(cancelTxt) + 8.0f * uiScale;
        ImGui::ProgressBar(
            prog,
            ImVec2(std::max(120.0f, ImGui::GetContentRegionAvail().x - cancelW - ImGui::GetStyle().ItemSpacing.x),
                   ImGui::GetFrameHeight()),
            overlay);
        ImGui::SameLine();
        if (RenderSecondaryButton(hdm::Tr("Отмена###CatCancel", "Скасувати###CatCancel", "Cancel###CatCancel"),
                                  ImVec2(cancelW, 0.0f))) {
            componentsScanner_.RequestCancelInstall();
        }
    }

    ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));

    if (pendingSelectDeviceRow_ >= 0) {
        if (deviceViewDirty_ && !devicesCache_.empty()) {
            RebuildDeviceViewCache(nullptr);
        }
        if (!visibleDeviceIndices_.empty()) {
            const size_t pick = std::min(
                static_cast<size_t>(pendingSelectDeviceRow_),
                visibleDeviceIndices_.size() - 1);
            selectedDeviceInstanceId_ = devicesCache_[visibleDeviceIndices_[pick]].instanceId;
            pendingSelectDeviceRow_ = -1;
        }
    }

    const hdm::DeviceInfo* selectedDevBefore = FindSelectedDevice();
    const ImGuiStyle& devStyle = ImGui::GetStyle();
    const float cardPadY = std::round(10.0f * uiScale);
    const float detailsH =
        (selectedDevBefore != nullptr)
            ? (ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetFrameHeight() +
               cardPadY * 2.0f + 10.0f * uiScale)
            : 0.0f;
    const float tableH = std::max(
        110.0f,
        ImGui::GetContentRegionAvail().y -
            (detailsH > 0.0f ? detailsH + devStyle.ItemSpacing.y : 0.0f));

    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
        ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY;

    if (ImGui::BeginTable(
            g_isAdvancedMode ? "##DevicesTable" : "##DevicesTableStd",
            g_isAdvancedMode ? 7 : 5,
            kTableFlags,
            ImVec2(0.0f, tableH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(hdm::Tr("Статус", "Статус", "Status"), ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 1.15f, 0);
        ImGui::TableSetupColumn(hdm::Tr("Устройство / Модель", "Пристрій / Модель", "Device / Model"), ImGuiTableColumnFlags_WidthStretch, 1.65f, 1);
        ImGui::TableSetupColumn(hdm::Tr("Производитель", "Виробник", "Manufacturer"), ImGuiTableColumnFlags_WidthStretch, 0.90f, 2);
        if (g_isAdvancedMode) {
            ImGui::TableSetupColumn(hdm::Tr("Класс", "Клас", "Class"), ImGuiTableColumnFlags_WidthStretch, 0.58f, 3);
        }
        ImGui::TableSetupColumn(hdm::Tr("Версия", "Версія", "Version"), ImGuiTableColumnFlags_WidthStretch, 0.72f, 4);
        ImGui::TableSetupColumn(hdm::Tr("Дата", "Дата", "Date"), ImGuiTableColumnFlags_WidthStretch, 0.56f, 5);
        if (g_isAdvancedMode) {
            ImGui::TableSetupColumn("Hardware ID", ImGuiTableColumnFlags_WidthStretch, 1.25f, 6);
        }
        RenderTableHeadersRow();

        ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
        if (sortSpecs != nullptr && sortSpecs->SpecsDirty) {
            deviceViewDirty_ = true;
            sortSpecs->SpecsDirty = false;
        }
        if (deviceViewDirty_) {
            RebuildDeviceViewCache(sortSpecs);
        }

        const float rowBadgeH = ImGui::GetTextLineHeight() + std::round(6.0f * uiScale);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(visibleDeviceIndices_.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const hdm::DeviceInfo& dev =
                    devicesCache_[visibleDeviceIndices_[static_cast<size_t>(row)]];
                ImGui::TableNextRow();
                ImGui::PushID(row);

                // Col 0: Full-row selectable + Status badge
                ImGui::TableSetColumnIndex(0);
                const ImVec2 cellStart = ImGui::GetCursorPos();
                const bool isRowSelected =
                    (!selectedDeviceInstanceId_.empty() &&
                     dev.instanceId == selectedDeviceInstanceId_);
                if (ImGui::Selectable(
                        "##rowSel",
                        isRowSelected,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                        ImVec2(0.0f, rowBadgeH))) {
                    if (isRowSelected) {
                        selectedDeviceInstanceId_.clear();
                    } else if (!dev.instanceId.empty()) {
                        selectedDeviceInstanceId_ = dev.instanceId;
                    }
                }
                if (ImGui::BeginPopupContextItem("##DevCtxMenu")) {
                    if (!dev.instanceId.empty()) {
                        selectedDeviceInstanceId_ = dev.instanceId;
                    }
                    ImGui::TextDisabled("%s", dev.name.c_str());
                    ImGui::Separator();

                    const bool hasHwKey = !dev.searchHardwareKey.empty() || !dev.hardwareId.empty();
                    if (ImGui::MenuItem(
                            g_isAdvancedMode
                                ? hdm::Tr("Скачать и установить драйвер (Catalog WHQL)", "Завантажити та встановити драйвер (Catalog WHQL)", "Download & Install Driver (Catalog WHQL)")
                                : hdm::Tr("Установить официальный драйвер", "Встановити офіційний драйвер", "Install Official Driver"),
                            nullptr,
                            false,
                            !devBusy && hasHwKey)) {
                        componentsScanner_.SearchAndInstallFromCatalogAsync(
                            dev.name,
                            dev.searchHardwareKey,
                            dev.hardwareId,
                            dev.driverVersion,
                            devicesCache_,
                            MakeLogger(),
                            [this](bool ok, bool rebootReq) {
                                if (rebootReq) rebootRequired_.store(true);
                                if (ok) pendingDeviceRescan_.store(true);
                            });
                    }
                    ImGui::Separator();

                    const bool canControl = !devBusy && !dev.instanceId.empty();
                    auto triggerAction = [&](hdm::DeviceAction act) {
                        scanner_.RunDeviceActionAsync(
                            act,
                            dev.instanceId,
                            dev.name,
                            MakeLogger(),
                            [this](bool, bool rebootReq) {
                                if (rebootReq) rebootRequired_.store(true);
                            });
                    };

                    if (g_isAdvancedMode) {
                        if (ImGui::MenuItem(hdm::Tr("Перезапустить устройство", "Перезапустити пристрій", "Restart device"), nullptr, false, canControl)) {
                            triggerAction(hdm::DeviceAction::Restart);
                        }
                        if (dev.problemCode == CM_PROB_DISABLED) {
                            if (ImGui::MenuItem(hdm::Tr("Включить устройство", "Увімкнути пристрій", "Enable device"), nullptr, false, canControl)) {
                                triggerAction(hdm::DeviceAction::Enable);
                            }
                        } else {
                            if (ImGui::MenuItem(hdm::Tr("Отключить устройство", "Вимкнути пристрій", "Disable device"), nullptr, false, canControl)) {
                                triggerAction(hdm::DeviceAction::Disable);
                            }
                        }
                        if (ImGui::MenuItem(hdm::Tr("Откатить драйвер", "Відкотити драйвер", "Roll back driver"), nullptr, false, canControl && dev.hasDriver)) {
                            triggerAction(hdm::DeviceAction::RollbackDriver);
                        }
                        if (ImGui::MenuItem(hdm::Tr("Удалить устройство (PnP)", "Видалити пристрій (PnP)", "Uninstall device (PnP)"), nullptr, false, canControl)) {
                            triggerAction(hdm::DeviceAction::UninstallDevice);
                        }
                        if (ImGui::MenuItem(
                                hdm::Tr("Свойства устройства (Windows)", "Властивості пристрою (Windows)", "Device Properties (Windows)"),
                                nullptr,
                                false,
                                !dev.instanceId.empty())) {
                            scanner_.OpenDevicePropertiesAsync(dev.instanceId, MakeLogger());
                        }
                        if (ImGui::MenuItem(hdm::Tr("Диспетчер устройств Windows", "Диспетчер пристроїв Windows", "Windows Device Manager"))) {
                            ShellExecuteW(nullptr, L"open", L"devmgmt.msc", nullptr, nullptr, SW_SHOWNORMAL);
                        }
                        if (ImGui::MenuItem(hdm::Tr("Показать в DriverStore", "Показати у DriverStore", "Show in DriverStore"), nullptr, false, IsOemInfName(dev.infName))) {
                            JumpToDriverStorePackage(dev.infName);
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem(hdm::Tr("Копировать Hardware ID", "Копіювати Hardware ID", "Copy Hardware ID"), nullptr, false, !dev.hardwareId.empty())) {
                            ImGui::SetClipboardText(dev.hardwareId.c_str());
                        }
                        if (ImGui::MenuItem(hdm::Tr("Копировать короткий ID", "Копіювати короткий ID", "Copy Short ID"), nullptr, false, !dev.searchHardwareKey.empty())) {
                            ImGui::SetClipboardText(dev.searchHardwareKey.c_str());
                        }
                    } else if (dev.problemCode == CM_PROB_DISABLED) {
                        if (ImGui::MenuItem(hdm::Tr("Включить устройство", "Увімкнути пристрій", "Enable device"), nullptr, false, canControl)) {
                            triggerAction(hdm::DeviceAction::Enable);
                        }
                        ImGui::Separator();
                    }
                    if (ImGui::MenuItem(hdm::Tr("Копировать название", "Копіювати назву", "Copy Name"))) {
                        ImGui::SetClipboardText(dev.name.c_str());
                    }
                    const std::string queryId =
                        !dev.edidModel.empty()
                            ? (dev.manufacturer + " " + dev.edidModel + " monitor driver")
                            : (!dev.searchHardwareKey.empty()
                                   ? (dev.searchHardwareKey + " Windows driver")
                                   : (dev.name + " Windows driver"));
                    if (ImGui::MenuItem(hdm::Tr("Найти драйвер в интернете", "Знайти драйвер в інтернеті", "Search driver online"))) {
                        OpenUrlInBrowser("https://www.google.com/search?q=" + UrlEncode(queryId));
                    }
                    if (g_isAdvancedMode &&
                        ImGui::MenuItem(hdm::Tr("Открыть в Microsoft Update Catalog", "Відкрити у Microsoft Update Catalog", "Search Microsoft Update Catalog"), nullptr, false, !dev.searchHardwareKey.empty())) {
                        OpenUrlInBrowser("https://www.catalog.update.microsoft.com/Search.aspx?q=" + UrlEncode(dev.searchHardwareKey));
                    }
                    ImGui::EndPopup();
                }

                ImGui::SetCursorPos(cellStart);
                ImVec4 statusColor = HealthyColor();
                std::string devStatusShown = hdm::TrDyn(dev.statusText);
                if (dev.problemCode == CM_PROB_DISABLED) {
                    statusColor = MutedTextColor();
                    if (!g_isAdvancedMode) {
                        devStatusShown = hdm::Tr("Отключено", "Вимкнено", "Disabled");
                    }
                } else if (dev.health == hdm::DeviceHealth::Error) {
                    statusColor = DangerColor();
                } else if (dev.health == hdm::DeviceHealth::Warning || dev.isGenericMonitorDriver) {
                    statusColor = IsUserRelevantDeviceIssue(dev) ? WarningColor() : MutedTextColor();
                }
                RenderStatusBadge(statusColor, devStatusShown.c_str(), false);

                // Col 1: Device Name
                ImGui::TableSetColumnIndex(1);
                const std::string devNameShown = hdm::TrDyn(dev.name);
                std::string devTitleCell = devNameShown;
                if (!dev.displayModeText.empty()) {
                    devTitleCell += "  [" + dev.displayModeText + "]";
                } else if (dev.gpuVramBytes >= (64ull * 1024ull * 1024ull)) {
                    devTitleCell += "  [VRAM: " + hdm::FormatBytes(dev.gpuVramBytes) + "]";
                }
                RenderCellText(devTitleCell);
                if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(440.0f * uiScale);
                    ImGui::TextColored(AccentTextColor(), "%s", devNameShown.c_str());
                    ImGui::TextDisabled(
                        hdm::Tr("Производитель: %s  |  Статус: %s", "Виробник: %s  |  Статус: %s", "Manufacturer: %s  |  Status: %s"),
                        dev.manufacturer.empty() ? "—" : dev.manufacturer.c_str(),
                        devStatusShown.c_str());
                    if (g_isAdvancedMode && !dev.hardwareId.empty()) {
                        ImGui::TextWrapped("INF: %s  |  HWID: %s", dev.infName.empty() ? "—" : dev.infName.c_str(), dev.hardwareId.c_str());
                    }
                    if (!dev.displayModeText.empty()) {
                        ImGui::Text(hdm::Tr("Экран: %s", "Екран: %s", "Display: %s"), dev.displayModeText.c_str());
                    }
                    if (dev.gpuVramBytes >= (64ull * 1024ull * 1024ull)) {
                        ImGui::Text(hdm::Tr("Видеопамять: %s", "Відеопам'ять: %s", "VRAM: %s"), hdm::FormatBytes(dev.gpuVramBytes).c_str());
                    }
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }

                // Col 2: Manufacturer
                ImGui::TableSetColumnIndex(2);
                if (RenderCellText(dev.manufacturer) && isAppForeground &&
                    ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("%s", dev.manufacturer.c_str());
                }

                int colIdx = 3;
                if (g_isAdvancedMode) {
                    ImGui::TableSetColumnIndex(colIdx++);
                    RenderCellText(dev.deviceClass);
                }

                ImGui::TableSetColumnIndex(colIdx++);
                RenderCellText(dev.driverVersion, dev.driverVersion.empty());

                ImGui::TableSetColumnIndex(colIdx++);
                RenderCellText(dev.driverDate, true);

                if (g_isAdvancedMode) {
                    ImGui::TableSetColumnIndex(colIdx++);
                    if (RenderCellText(dev.hardwareId, dev.hardwareId.empty()) &&
                        isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("%s", dev.hardwareId.c_str());
                    }
                }

                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    // Selected Device Details Card
    if (const hdm::DeviceInfo* selectedDev = FindSelectedDevice()) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(std::round(14.0f * uiScale), cardPadY));
        ImGui::PushStyleColor(ImGuiCol_Border, PanelBorderColor());
        ImGui::BeginChild(
            "##DeviceDetailsCard",
            ImVec2(0.0f, detailsH),
            true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();

        {
            PushFontSafe(g_fontSecondaryHeader);
            ImGui::TextColored(CardHeaderTextColor(), "%s", EllipsizeUtf8(selectedDev->name, 420.0f * uiScale).c_str());
            PopFontSafe(g_fontSecondaryHeader);
        }
        ImGui::SameLine(0.0f, 12.0f);
        std::string metaLine =
            g_isAdvancedMode
                ? (std::string(hdm::Tr("Статус: ", "Статус: ", "Status: ")) + hdm::TrDyn(selectedDev->statusText) +
                   "  •  " + (selectedDev->deviceClass.empty() ? "—" : selectedDev->deviceClass) +
                   "  •  INF: " + (selectedDev->infName.empty() ? "—" : selectedDev->infName))
                : (std::string(hdm::Tr("Производитель: ", "Виробник: ", "Vendor: ")) +
                   (selectedDev->manufacturer.empty() ? "—" : selectedDev->manufacturer) +
                   (!selectedDev->driverVersion.empty()
                        ? (std::string(hdm::Tr("  •  Версия: ", "  •  Версія: ", "  •  Version: ")) + selectedDev->driverVersion)
                        : ""));
        if (!selectedDev->displayModeText.empty()) {
            metaLine += "  •  " + selectedDev->displayModeText;
        }
        RenderCellText(metaLine, true);

        if (g_isAdvancedMode) {
            const std::string idLine =
                "HWID: " + (selectedDev->hardwareId.empty() ? "—" : selectedDev->hardwareId) +
                "   |   Instance: " + (selectedDev->instanceId.empty() ? "—" : selectedDev->instanceId);
            if (RenderCellText(idLine, true) && isAppForeground &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", idLine.c_str());
            }
        } else {
            std::string friendlyLine;
            if (selectedDev->problemCode == CM_PROB_DISABLED) {
                friendlyLine = hdm::Tr(
                    "Это устройство отключено вручную в Windows. Если оно вам нужно, вы можете включить его.",
                    "Цей пристрій вимкнено вручну у Windows. Якщо він вам потрібен, ви можете увімкнути його.",
                    "This device is manually disabled in Windows. You can re-enable it if needed.");
            } else if (IsUserRelevantDeviceIssue(*selectedDev)) {
                friendlyLine = hdm::Tr(
                    "Рекомендуется установить официальный драйвер Microsoft кнопкой ниже.",
                    "Рекомендується встановити офіційний драйвер Microsoft кнопкою нижче.",
                    "Recommended: install an official Microsoft driver using the button below.");
            } else if (selectedDev->health != hdm::DeviceHealth::Ok) {
                friendlyLine = hdm::Tr(
                    "Служебный / виртуальный порт — отдельный драйвер обычно не требуется.",
                    "Службовий / віртуальний порт — окремий драйвер зазвичай не потрібен.",
                    "Auxiliary / virtual port — a separate driver is normally not required.");
            } else {
                friendlyLine = hdm::Tr(
                    "Устройство работает исправно — дополнительных действий не требуется.",
                    "Пристрій працює справно — додаткових дій не потрібно.",
                    "This device is working properly — no action is required.");
            }
            PushFontSafe(g_fontSmall);
            RenderCellText(friendlyLine, true);
            PopFontSafe(g_fontSmall);
        }

        const bool hasHwKey =
            !selectedDev->searchHardwareKey.empty() || !selectedDev->hardwareId.empty();
        const bool highlightCatalog = IsUserRelevantDeviceIssue(*selectedDev);

        if (!g_isAdvancedMode && selectedDev->problemCode == CM_PROB_DISABLED) {
            ImGui::BeginDisabled(devBusy || selectedDev->instanceId.empty());
            if (RenderPrimaryButton(hdm::Tr("Включить устройство###DevEnBtn", "Увімкнути пристрій###DevEnBtn", "Enable Device###DevEnBtn"))) {
                scanner_.RunDeviceActionAsync(
                    hdm::DeviceAction::Enable,
                    selectedDev->instanceId,
                    selectedDev->name,
                    MakeLogger(),
                    [this](bool, bool rebootReq) {
                        if (rebootReq) rebootRequired_.store(true);
                    });
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
        }

        ImGui::BeginDisabled(devBusy || !hasHwKey);
        if (RenderActionButton(
                g_isAdvancedMode
                    ? hdm::Tr("Установить из Catalog (WHQL)###DevCatBtn", "Встановити з Catalog (WHQL)###DevCatBtn", "Install from Catalog (WHQL)###DevCatBtn")
                    : hdm::Tr("Установить официальный драйвер###DevCatBtn", "Встановити офіційний драйвер###DevCatBtn", "Install Official Driver###DevCatBtn"),
                ImVec2(0.0f, 0.0f),
                highlightCatalog)) {
            componentsScanner_.SearchAndInstallFromCatalogAsync(
                selectedDev->name,
                selectedDev->searchHardwareKey,
                selectedDev->hardwareId,
                selectedDev->driverVersion,
                devicesCache_,
                MakeLogger(),
                [this](bool ok, bool rebootReq) {
                    if (rebootReq) rebootRequired_.store(true);
                    if (ok) pendingDeviceRescan_.store(true);
                });
        }
        ImGui::EndDisabled();

        if (g_isAdvancedMode) {
            const bool canControl = !devBusy && !selectedDev->instanceId.empty();
            ImGui::SameLine();
            ImGui::BeginDisabled(!canControl);
            if (RenderSecondaryButton(hdm::Tr("Перезапустить###DevRst", "Перезапустити###DevRst", "Restart###DevRst"))) {
                scanner_.RunDeviceActionAsync(
                    hdm::DeviceAction::Restart,
                    selectedDev->instanceId,
                    selectedDev->name,
                    MakeLogger(),
                    [this](bool, bool rebootReq) {
                        if (rebootReq) rebootRequired_.store(true);
                    });
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!selectedDev->hasDriver);
            if (RenderSecondaryButton(hdm::Tr("Откатить###DevRb", "Відкотити###DevRb", "Roll Back###DevRb"))) {
                scanner_.RunDeviceActionAsync(
                    hdm::DeviceAction::RollbackDriver,
                    selectedDev->instanceId,
                    selectedDev->name,
                    MakeLogger(),
                    [this](bool, bool rebootReq) {
                        if (rebootReq) rebootRequired_.store(true);
                    });
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(selectedDev->hardwareId.empty());
            if (RenderSecondaryButton(hdm::Tr("HWID###DevCpHw", "HWID###DevCpHw", "Copy HWID###DevCpHw"))) {
                ImGui::SetClipboardText(selectedDev->hardwareId.c_str());
                logBuffer_.Push(hdm::LogLevel::Info, std::string("Copied Hardware ID: ") + selectedDev->hardwareId);
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(selectedDev->instanceId.empty());
            if (RenderSecondaryButton(hdm::Tr("Свойства###DevProp", "Властивості###DevProp", "Properties###DevProp"))) {
                scanner_.OpenDevicePropertiesAsync(selectedDev->instanceId, MakeLogger());
            }
            ImGui::EndDisabled();

            if (IsOemInfName(selectedDev->infName)) {
                ImGui::SameLine();
                if (RenderSecondaryButton(hdm::Tr("В DriverStore###DevGoStore", "У DriverStore###DevGoStore", "DriverStore###DevGoStore"))) {
                    JumpToDriverStorePackage(selectedDev->infName);
                }
            }
        } else {
            ImGui::SameLine();
            if (RenderSecondaryButton(hdm::Tr("Найти в интернете###DevGglBtn", "Знайти в інтернеті###DevGglBtn", "Search Online###DevGglBtn"))) {
                const std::string queryId =
                    !selectedDev->edidModel.empty()
                        ? (selectedDev->manufacturer + " " + selectedDev->edidModel + " monitor driver")
                        : (!selectedDev->searchHardwareKey.empty()
                               ? (selectedDev->searchHardwareKey + " Windows driver")
                               : (selectedDev->name + " Windows driver"));
                OpenUrlInBrowser("https://www.google.com/search?q=" + UrlEncode(queryId));
            }
        }

        const char* closeTxt = hdm::Tr("Закрыть", "Закрити", "Close");
        const float closeW = CalcButtonWidth(closeTxt) + 4.0f * uiScale;
        const float winRight = ImGui::GetWindowContentRegionMax().x;
        const float lastEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        if (winRight - closeW > lastEnd + devStyle.ItemSpacing.x) {
            ImGui::SameLine(winRight - closeW);
        } else {
            ImGui::SameLine();
        }
        if (RenderSecondaryButton(hdm::Tr("Закрыть###DevClose", "Закрити###DevClose", "Close###DevClose"), ImVec2(closeW, 0.0f))) {
            selectedDeviceInstanceId_.clear();
        }

        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
}
