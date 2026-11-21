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
            if (d.health == hdm::DeviceHealth::Ok && !d.isGenericMonitorDriver) continue;
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
                    auto rank = [](hdm::DeviceHealth h) {
                        return h == hdm::DeviceHealth::Error ? 0
                               : (h == hdm::DeviceHealth::Warning ? 1 : 2);
                    };
                    cmp = rank(a.health) - rank(b.health);
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
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
    const bool isAppForeground = (GetForegroundWindow() == hwnd_);

    // Row 1: Scan button + PnP rescan + Category filter pills (auto-wrap on narrow windows)
    const bool scanning = scanner_.IsScanning();
    const bool catalogBusy = componentsScanner_.IsInstalling();
    const bool devBusy = scanner_.IsBusy() || catalogBusy;
    ImGui::BeginDisabled(devBusy);
    const char* devScanLbl =
        scanning
            ? hdm::Tr("Сканирование...###DevScan", "Сканування...###DevScan", "Scanning...###DevScan")
            : (devBusy
                   ? hdm::Tr("Выполнение...###DevScan", "Виконання...###DevScan", "Working...###DevScan")
                   : hdm::Tr("Сканировать###DevScan", "Сканувати###DevScan", "Scan###DevScan"));
    if (ImGui::Button(devScanLbl)) {
        scanner_.StartScanAsync(MakeLogger());
    }
    ImGui::EndDisabled();

    if (g_isAdvancedMode) {
        const char* pnpTxt = hdm::Tr("Обновить PnP", "Оновити PnP", "Rescan PnP");
        SameLineWrapIfNeeded(CalcButtonWidth(pnpTxt));
        ImGui::BeginDisabled(devBusy);
        if (RenderSecondaryButton(hdm::Tr(
                "Обновить PnP###DevPnp",
                "Оновити PnP###DevPnp",
                "Rescan PnP###DevPnp"))) {
            scanner_.RunDeviceActionAsync(
                hdm::DeviceAction::Reenumerate,
                {},
                hdm::Tr("Шина PnP", "Шина PnP", "PnP Bus"),
                MakeLogger(),
                [this](bool, bool rebootReq) {
                    if (rebootReq) {
                        rebootRequired_.store(true);
                    }
                });
        }
        if (isAppForeground &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "%s",
                hdm::Tr(
                    "Повторный опрос шины PnP (CM_Reenumerate_DevNode) для поиска подключённых или удалённых устройств.",
                    "Повторне опитування шини PnP (CM_Reenumerate_DevNode) для пошуку підключених або видалених пристроїв.",
                    "Rescan the PnP bus (CM_Reenumerate_DevNode) to detect connected or removed hardware."));
        }
        ImGui::EndDisabled();
    }

    const char* fAllTxt = hdm::Tr("Все", "Усі", "All");
    const char* fProbTxt = hdm::Tr("Проблемные", "Проблемні", "Issues");
    const char* fMonTxt = hdm::Tr("Мониторы", "Монітори", "Monitors");
    const char* fPerTxt = hdm::Tr("Периферия", "Периферія", "Peripherals");
    const char* fGpuTxt = hdm::Tr("Видео и чипсет", "Відео та чипсет", "GPU & Chipset");

    const char* filterLbl = hdm::Tr("Фильтр:", "Фільтр:", "Filter:");
    float filterLblW = 0.0f;
    {
        PushFontSafe(g_fontSecondaryHeader);
        filterLblW = ImGui::CalcTextSize(filterLbl).x;
        PopFontSafe(g_fontSecondaryHeader);
    }
    const float divCheckW = 14.0f * uiScale + filterLblW + 6.0f * uiScale + CalcButtonWidth(fAllTxt);
    const float lastX = ImGui::GetItemRectMax().x;
    const float winRightLim = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (lastX + ImGui::GetStyle().ItemSpacing.x + divCheckW <= winRightLim) {
        RenderVerticalDivider(3.0f);
        ImGui::SameLine(0.0f, 3.0f * uiScale);
    }
    RenderSecondaryGroupLabel(filterLbl);
    ImGui::SameLine(0.0f, 6.0f * uiScale);
    if (RenderPillButton(hdm::Tr("Все###DfAll", "Усі###DfAll", "All###DfAll"),
                         deviceFilterMode_ == DeviceFilterMode::All)) {
        deviceFilterMode_ = DeviceFilterMode::All;
        deviceViewDirty_ = true;
    }
    SameLineWrapIfNeeded(CalcButtonWidth(fProbTxt));
    if (RenderPillButton(hdm::Tr("Проблемные###DfProb", "Проблемні###DfProb", "Issues###DfProb"),
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

    {
        char totStr[32], monStr[32], probStr[32];
        std::snprintf(totStr, sizeof(totStr), "%zu", totalDevicesCount_);
        std::snprintf(monStr, sizeof(monStr), "%zu", monitorDevicesCount_);
        std::snprintf(probStr, sizeof(probStr), "%zu", problemDevicesCount_);

        const ImVec4 probAccent =
            (problemDevicesCount_ > 0)
                ? ImVec4(0.98f, 0.72f, 0.16f, 1.00f)
                : ImVec4(0.20f, 0.83f, 0.60f, 1.00f);
        const SummaryBadgeSegment segments[3] = {
            {hdm::Tr("Всего:", "Усього:", "Total:"), totStr, ImVec4(0.46f, 0.74f, 0.98f, 1.00f), false},
            {hdm::Tr("Мониторы:", "Монітори:", "Monitors:"), monStr, ImVec4(0.26f, 0.82f, 0.92f, 1.00f), false},
            {hdm::Tr("Проблемные:", "Проблемні:", "Issues:"), probStr, probAccent, problemDevicesCount_ > 0},
        };

        const float summaryW = CalcSummaryInfoBadgeWidth(segments, 3);
        const float lastItemEndLocalX = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        const float winRightX = ImGui::GetWindowContentRegionMax().x;
        if (winRightX - summaryW > lastItemEndLocalX + 10.0f * uiScale) {
            ImGui::SameLine(winRightX - summaryW);
        } else {
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), winRightX - summaryW));
        }
        RenderSummaryInfoBadge(segments, 3);
    }

    // Row 2: Responsive full-width search bar
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
                      "Поиск по названию устройства, модели монитора, бренду или версии... (Ctrl+F)",
                      "Пошук за назвою пристрою, моделлю монітора, брендом або версією... (Ctrl+F)",
                      "Search by device name, monitor model, brand, or version... (Ctrl+F)"),
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
                ? hdm::Tr(
                      "Установка драйвера из Microsoft Update Catalog...",
                      "Встановлення драйвера з Microsoft Update Catalog...",
                      "Installing driver from Microsoft Update Catalog...")
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

    ImGui::Spacing();

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
    const float cardPadY = std::round(8.0f * uiScale);
    const float detailsH =
        (selectedDevBefore != nullptr)
            ? (ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetFrameHeight() +
               cardPadY * 2.0f + 8.0f * uiScale)
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
            g_isAdvancedMode ? 7 : 6,
            kTableFlags,
            ImVec2(0.0f, tableH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(
            hdm::Tr("Статус", "Статус", "Status"),
            ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch,
            1.28f,
            0);
        ImGui::TableSetupColumn(
            hdm::Tr("Устройство / Модель", "Пристрій / Модель", "Device / Model"),
            ImGuiTableColumnFlags_WidthStretch,
            1.45f,
            1);
        ImGui::TableSetupColumn(
            hdm::Tr("Производитель", "Виробник", "Manufacturer"),
            ImGuiTableColumnFlags_WidthStretch,
            0.90f,
            2);
        ImGui::TableSetupColumn(
            hdm::Tr("Класс", "Клас", "Class"),
            ImGuiTableColumnFlags_WidthStretch,
            0.60f,
            3);
        ImGui::TableSetupColumn(
            hdm::Tr("Версия", "Версія", "Version"),
            ImGuiTableColumnFlags_WidthStretch,
            0.76f,
            4);
        ImGui::TableSetupColumn(
            hdm::Tr("Дата", "Дата", "Date"),
            ImGuiTableColumnFlags_WidthStretch,
            0.60f,
            5);
        if (g_isAdvancedMode) {
            ImGui::TableSetupColumn("Hardware ID", ImGuiTableColumnFlags_WidthStretch, 1.28f, 6);
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

                // Col 0: Full-row selectable behind rounded status badge (zero X-shift)
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
                            hdm::Tr(
                                "Скачать и установить драйвер (Catalog WHQL)",
                                "Завантажити та встановити драйвер (Catalog WHQL)",
                                "Download & Install Driver (Catalog WHQL)"),
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
                                if (rebootReq) {
                                    rebootRequired_.store(true);
                                }
                                if (ok) {
                                    pendingDeviceRescan_.store(true);
                                }
                            });
                    }
                    ImGui::Separator();

                    const bool canControl = !devBusy && !dev.instanceId.empty();
                    [[maybe_unused]] auto triggerAction = [&](hdm::DeviceAction act) {
                        scanner_.RunDeviceActionAsync(
                            act,
                            dev.instanceId,
                            dev.name,
                            MakeLogger(),
                            [this](bool, bool rebootReq) {
                                if (rebootReq) {
                                    rebootRequired_.store(true);
                                }
                            });
                    };

                    if (g_isAdvancedMode) {
                        if (ImGui::MenuItem(
                                hdm::Tr("Перезапустить устройство", "Перезапустити пристрій", "Restart device"),
                                nullptr,
                                false,
                                canControl)) {
                            triggerAction(hdm::DeviceAction::Restart);
                        }
                        if (dev.problemCode == CM_PROB_DISABLED) {
                            if (ImGui::MenuItem(
                                    hdm::Tr("Включить устройство", "Увімкнути пристрій", "Enable device"),
                                    nullptr,
                                    false,
                                    canControl)) {
                                triggerAction(hdm::DeviceAction::Enable);
                            }
                        } else {
                            if (ImGui::MenuItem(
                                    hdm::Tr("Отключить устройство", "Вимкнути пристрій", "Disable device"),
                                    nullptr,
                                    false,
                                    canControl)) {
                                triggerAction(hdm::DeviceAction::Disable);
                            }
                        }
                        if (ImGui::MenuItem(
                                hdm::Tr(
                                    "Откатить драйвер (предыдущая версия)",
                                    "Відкотити драйвер (попередня версія)",
                                    "Roll back driver (previous version)"),
                                nullptr,
                                false,
                                canControl && dev.hasDriver)) {
                            triggerAction(hdm::DeviceAction::RollbackDriver);
                        }
                        if (ImGui::MenuItem(
                                hdm::Tr(
                                    "Удалить устройство (переустановка PnP)",
                                    "Видалити пристрій (перевстановлення PnP)",
                                    "Uninstall device (PnP reinstall)"),
                                nullptr,
                                false,
                                canControl)) {
                            triggerAction(hdm::DeviceAction::UninstallDevice);
                        }
                        if (ImGui::MenuItem(
                                hdm::Tr(
                                    "Открыть Диспетчер устройств Windows",
                                    "Відкрити Диспетчер пристроїв Windows",
                                    "Open Windows Device Manager"))) {
                            ShellExecuteW(nullptr, L"open", L"devmgmt.msc", nullptr, nullptr, SW_SHOWNORMAL);
                        }
                        if (ImGui::MenuItem(
                                hdm::Tr(
                                    "Показать пакет в DriverStore (oem*.inf)",
                                    "Показати пакет у DriverStore (oem*.inf)",
                                    "Show package in DriverStore (oem*.inf)"),
                                nullptr,
                                false,
                                IsOemInfName(dev.infName))) {
                            JumpToDriverStorePackage(dev.infName);
                        }
                        ImGui::Separator();

                        if (ImGui::MenuItem(
                                hdm::Tr("Копировать Hardware ID", "Копіювати Hardware ID", "Copy Hardware ID"),
                                nullptr,
                                false,
                                !dev.hardwareId.empty())) {
                            ImGui::SetClipboardText(dev.hardwareId.c_str());
                            logBuffer_.Push(
                                hdm::LogLevel::Info,
                                std::string(hdm::Tr("Скопирован Hardware ID: ", "Скопійовано Hardware ID: ", "Copied Hardware ID: ")) +
                                    dev.hardwareId);
                        }
                        if (ImGui::MenuItem(
                                hdm::Tr("Копировать короткий ID (VEN/DEV)", "Копіювати короткий ID (VEN/DEV)", "Copy Short ID (VEN/DEV)"),
                                nullptr,
                                false,
                                !dev.searchHardwareKey.empty())) {
                            ImGui::SetClipboardText(dev.searchHardwareKey.c_str());
                        }
                    } else if (dev.problemCode == CM_PROB_DISABLED) {
                        if (ImGui::MenuItem(
                                hdm::Tr("Включить устройство", "Увімкнути пристрій", "Enable device"),
                                nullptr,
                                false,
                                canControl)) {
                            triggerAction(hdm::DeviceAction::Enable);
                        }
                        ImGui::Separator();
                    }
                    if (ImGui::MenuItem(
                            hdm::Tr("Копировать название устройства", "Копіювати назву пристрою", "Copy Device Name"))) {
                        ImGui::SetClipboardText(dev.name.c_str());
                    }
                    ImGui::Separator();
                    const std::string queryId =
                        !dev.edidModel.empty()
                            ? (dev.manufacturer + " " + dev.edidModel + " monitor driver")
                            : (!dev.searchHardwareKey.empty()
                                   ? (dev.searchHardwareKey + " Windows driver")
                                   : (dev.name + " Windows driver"));
                    if (ImGui::MenuItem(
                            hdm::Tr("Найти драйвер в браузере (Google)", "Знайти драйвер у браузері (Google)", "Search driver in browser (Google)"))) {
                        OpenUrlInBrowser("https://www.google.com/search?q=" + UrlEncode(queryId));
                    }
                    if (g_isAdvancedMode &&
                        ImGui::MenuItem(
                            hdm::Tr("Найти в каталоге Microsoft Update", "Знайти в каталозі Microsoft Update", "Search in Microsoft Update Catalog"),
                            nullptr,
                            false,
                            !dev.searchHardwareKey.empty())) {
                        OpenUrlInBrowser(
                            "https://www.catalog.update.microsoft.com/Search.aspx?q=" +
                            UrlEncode(dev.searchHardwareKey));
                    }
                    ImGui::EndPopup();
                }

                ImGui::SetCursorPos(cellStart);
                ImVec4 statusColor(0.20f, 0.83f, 0.60f, 1.00f); // Emerald
                if (dev.health == hdm::DeviceHealth::Error) {
                    statusColor = ImVec4(0.97f, 0.44f, 0.44f, 1.00f); // Coral Red
                } else if (dev.health == hdm::DeviceHealth::Warning || dev.isGenericMonitorDriver) {
                    statusColor = ImVec4(0.98f, 0.75f, 0.14f, 1.00f); // Amber
                }
                const std::string devStatusShown = hdm::TrDyn(dev.statusText);
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
                    ImGui::PushTextWrapPos(460.0f * uiScale);
                    ImGui::TextColored(AccentTextColor(), "%s", devNameShown.c_str());
                    ImGui::TextDisabled(
                        hdm::Tr(
                            "Производитель: %s  |  Класс: %s",
                            "Виробник: %s  |  Клас: %s",
                            "Manufacturer: %s  |  Class: %s"),
                        dev.manufacturer.empty() ? "—" : dev.manufacturer.c_str(),
                        dev.deviceClass.empty() ? "—" : dev.deviceClass.c_str());
                    if (g_isAdvancedMode) {
                        ImGui::TextDisabled(
                            hdm::Tr(
                                "INF: %s  |  Статус: %s",
                                "INF: %s  |  Статус: %s",
                                "INF: %s  |  Status: %s"),
                            dev.infName.empty() ? "—" : dev.infName.c_str(),
                            devStatusShown.c_str());
                        if (!dev.hardwareId.empty()) {
                            ImGui::TextWrapped("Hardware ID: %s", dev.hardwareId.c_str());
                        }
                    } else {
                        ImGui::TextDisabled(
                            hdm::Tr("Статус: %s", "Статус: %s", "Status: %s"),
                            devStatusShown.c_str());
                    }
                    if (!dev.edidModel.empty()) {
                        ImGui::Text(
                            hdm::Tr("Модель (EDID): %s", "Модель (EDID): %s", "Model (EDID): %s"),
                            dev.edidModel.c_str());
                    }
                    if (!dev.displayModeText.empty()) {
                        ImGui::Text(
                            hdm::Tr("Режим: %s", "Режим: %s", "Mode: %s"),
                            dev.displayModeText.c_str());
                        if (!dev.connectedGpuName.empty()) {
                            ImGui::TextDisabled(
                                hdm::Tr("Видеокарта: %s", "Відеокарта: %s", "GPU: %s"),
                                dev.connectedGpuName.c_str());
                        }
                    }
                    if (dev.gpuVramBytes >= (64ull * 1024ull * 1024ull)) {
                        ImGui::Text(
                            hdm::Tr("Видеопамять (VRAM): %s", "Відеопам'ять (VRAM): %s", "Video Memory (VRAM): %s"),
                            hdm::FormatBytes(dev.gpuVramBytes).c_str());
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

                // Col 3: Class
                ImGui::TableSetColumnIndex(3);
                RenderCellText(dev.deviceClass);

                // Col 4: Version
                ImGui::TableSetColumnIndex(4);
                RenderCellText(dev.driverVersion, dev.driverVersion.empty());

                // Col 5: Date
                ImGui::TableSetColumnIndex(5);
                RenderCellText(dev.driverDate, true);

                if (g_isAdvancedMode) {
                    // Col 6: Hardware ID
                    ImGui::TableSetColumnIndex(6);
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

    if (const hdm::DeviceInfo* selectedDev = FindSelectedDevice()) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(devStyle.WindowPadding.x, cardPadY));
        ImGui::PushStyleColor(ImGuiCol_Border, PanelBorderColor());
        ImGui::BeginChild(
            "##DeviceDetailsCard",
            ImVec2(0.0f, detailsH),
            true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();

        {
            PushFontSafe(g_fontSecondaryHeader);
            const float lineH = ImGui::GetTextLineHeight();
            const float barW = std::max(2.5f, std::round(3.0f * uiScale));
            const ImVec2 barMin = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(barMin.x, barMin.y + 1.0f),
                ImVec2(barMin.x + barW, barMin.y + lineH - 1.0f),
                ImGui::ColorConvertFloat4ToU32(AccentBarColor()),
                barW * 0.5f);
            ImGui::Dummy(ImVec2(barW + std::round(5.0f * uiScale), lineH));
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextColored(
                CardHeaderTextColor(),
                "%s",
                EllipsizeUtf8(selectedDev->name, 420.0f * uiScale).c_str());
            PopFontSafe(g_fontSecondaryHeader);
        }
        ImGui::SameLine(0.0f, 12.0f);
        std::string metaLine =
            g_isAdvancedMode
                ? (std::string(hdm::Tr("Статус: ", "Статус: ", "Status: ")) +
                   hdm::TrDyn(selectedDev->statusText) +
                   hdm::Tr("  |  Класс: ", "  |  Клас: ", "  |  Class: ") +
                   (selectedDev->deviceClass.empty() ? "—" : selectedDev->deviceClass) +
                   "  |  INF: " + (selectedDev->infName.empty() ? "—" : selectedDev->infName))
                : (std::string(hdm::Tr("Статус: ", "Статус: ", "Status: ")) +
                   hdm::TrDyn(selectedDev->statusText) +
                   hdm::Tr("  |  Класс: ", "  |  Клас: ", "  |  Class: ") +
                   (selectedDev->deviceClass.empty() ? "—" : selectedDev->deviceClass) +
                   hdm::Tr("  |  Производитель: ", "  |  Виробник: ", "  |  Vendor: ") +
                   (selectedDev->manufacturer.empty() ? "—" : selectedDev->manufacturer));
        if (!selectedDev->displayModeText.empty()) {
            metaLine += std::string(hdm::Tr("  |  Режим: ", "  |  Режим: ", "  |  Mode: ")) +
                        selectedDev->displayModeText;
            if (!selectedDev->connectedGpuName.empty()) {
                metaLine += "  |  GPU: " + selectedDev->connectedGpuName;
            }
        } else if (selectedDev->gpuVramBytes >= (64ull * 1024ull * 1024ull)) {
            metaLine += "  |  VRAM: " + hdm::FormatBytes(selectedDev->gpuVramBytes);
        }
        RenderCellText(metaLine, true);

        if (g_isAdvancedMode) {
            const std::string idLine =
                "Hardware ID: " + (selectedDev->hardwareId.empty() ? "—" : selectedDev->hardwareId) +
                "   |   Instance ID: " +
                (selectedDev->instanceId.empty() ? "—" : selectedDev->instanceId);
            if (RenderCellText(idLine, true) && isAppForeground &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", idLine.c_str());
            }
        } else {
            const bool needsAttention =
                (selectedDev->health != hdm::DeviceHealth::Ok || selectedDev->isGenericMonitorDriver);
            std::string friendlyLine;
            if (needsAttention) {
                friendlyLine = hdm::Tr(
                    "Рекомендуется установить официальный WHQL-драйвер кнопкой ниже.",
                    "Рекомендується встановити офіційний WHQL-драйвер кнопкою нижче.",
                    "Recommended: install an official WHQL driver using the button below.");
            } else {
                friendlyLine = hdm::Tr(
                    "Устройство работает исправно. Драйвер не требует вмешательства.",
                    "Пристрій працює справно. Драйвер не потребує втручання.",
                    "Device is working properly. No driver action is required.");
                if (!selectedDev->driverVersion.empty()) {
                    friendlyLine += std::string(hdm::Tr("  (версия: ", "  (версія: ", "  (version: ")) +
                                    selectedDev->driverVersion + ")";
                }
            }
            RenderCellText(friendlyLine, true);
        }

        const bool hasHwKey =
            !selectedDev->searchHardwareKey.empty() || !selectedDev->hardwareId.empty();
        const bool highlightCatalog =
            (selectedDev->health != hdm::DeviceHealth::Ok || selectedDev->isGenericMonitorDriver);

        ImGui::BeginDisabled(devBusy || !hasHwKey);
        if (RenderActionButton(
                hdm::Tr(
                    "Установить из Catalog (WHQL)###DevCatBtn",
                    "Встановити з Catalog (WHQL)###DevCatBtn",
                    "Install from Catalog (WHQL)###DevCatBtn"),
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
            if (RenderSecondaryButton(
                    hdm::Tr("Перезапустить###DevRst", "Перезапустити###DevRst", "Restart###DevRst"))) {
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
            if (RenderSecondaryButton(
                    hdm::Tr("Откатить драйвер###DevRb", "Відкотити драйвер###DevRb", "Roll Back###DevRb"))) {
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
            if (RenderSecondaryButton(
                    hdm::Tr("Копировать HWID (Ctrl+C)###DevCpHw", "Копіювати HWID (Ctrl+C)###DevCpHw", "Copy HWID (Ctrl+C)###DevCpHw"))) {
                ImGui::SetClipboardText(selectedDev->hardwareId.c_str());
                logBuffer_.Push(
                    hdm::LogLevel::Info,
                    std::string(hdm::Tr("Скопирован Hardware ID: ", "Скопійовано Hardware ID: ", "Copied Hardware ID: ")) +
                        selectedDev->hardwareId);
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(selectedDev->instanceId.empty());
            if (RenderSecondaryButton(
                    hdm::Tr("Копировать Instance ID###DevCpInst", "Копіювати Instance ID###DevCpInst", "Copy Instance ID###DevCpInst"))) {
                ImGui::SetClipboardText(selectedDev->instanceId.c_str());
                logBuffer_.Push(
                    hdm::LogLevel::Info,
                    std::string(hdm::Tr("Скопирован Instance ID: ", "Скопійовано Instance ID: ", "Copied Instance ID: ")) +
                        selectedDev->instanceId);
            }
            ImGui::EndDisabled();

            if (IsOemInfName(selectedDev->infName)) {
                ImGui::SameLine();
                if (RenderSecondaryButton(hdm::Tr(
                        "В DriverStore###DevGoStore",
                        "У DriverStore###DevGoStore",
                        "In DriverStore###DevGoStore"))) {
                    JumpToDriverStorePackage(selectedDev->infName);
                }
                if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip(
                        hdm::Tr(
                            "Перейти к пакету %s во вкладке DriverStore для бэкапа или удаления",
                            "Перейти до пакета %s у вкладці DriverStore для бекапу або видалення",
                            "Jump to package %s in DriverStore tab for backup or removal"),
                        selectedDev->infName.c_str());
                }
            }
        } else {
            ImGui::SameLine();
            if (RenderSecondaryButton(hdm::Tr(
                    "Найти драйвер в Google###DevGglBtn",
                    "Знайти драйвер у Google###DevGglBtn",
                    "Search Driver in Google###DevGglBtn"))) {
                const std::string queryId =
                    !selectedDev->edidModel.empty()
                        ? (selectedDev->manufacturer + " " + selectedDev->edidModel + " monitor driver")
                        : (!selectedDev->searchHardwareKey.empty()
                               ? (selectedDev->searchHardwareKey + " Windows driver")
                               : (selectedDev->name + " Windows driver"));
                OpenUrlInBrowser("https://www.google.com/search?q=" + UrlEncode(queryId));
            }
        }

        const char* closeTxt = hdm::Tr("Закрыть (Esc)", "Закрити (Esc)", "Close (Esc)");
        const float closeW = CalcButtonWidth(closeTxt);
        const float winRight = ImGui::GetWindowContentRegionMax().x;
        const float lastEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        if (winRight - closeW > lastEnd + devStyle.ItemSpacing.x) {
            ImGui::SameLine(winRight - closeW);
        } else {
            ImGui::SameLine();
        }
        if (RenderSecondaryButton(
                hdm::Tr("Закрыть (Esc)###DevClose", "Закрити (Esc)###DevClose", "Close (Esc)###DevClose"),
                ImVec2(closeW, 0.0f))) {
            selectedDeviceInstanceId_.clear();
        }

        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
}
