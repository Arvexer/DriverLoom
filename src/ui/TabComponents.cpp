#include "ui/DriverLoomApp.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

void DriverLoomApp::RebuildComponentsViewCache(ImGuiTableSortSpecs* sortSpecs) {
    visibleComponentIndices_.clear();
    visibleComponentIndices_.reserve(componentsCache_.size());

    for (size_t i = 0; i < componentsCache_.size(); ++i) {
        const auto& item = componentsCache_[i];
        if (compFilterMode_ == ComponentFilterMode::GamingRuntimes &&
            item.category != hdm::ComponentCategory::GamingRuntime) {
            continue;
        }
        if (compFilterMode_ == ComponentFilterMode::Peripherals &&
            item.category != hdm::ComponentCategory::PeripheralSoftware) {
            continue;
        }
        if (compFilterMode_ == ComponentFilterMode::MonitorsAndGpu &&
            item.category != hdm::ComponentCategory::MonitorAndGpu) {
            continue;
        }
        visibleComponentIndices_.push_back(i);
    }

    if (sortSpecs != nullptr && sortSpecs->SpecsCount > 0) {
        const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
        const bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
        std::sort(
            visibleComponentIndices_.begin(),
            visibleComponentIndices_.end(),
            [&](size_t ia, size_t ib) {
                const auto& a = componentsCache_[ia];
                const auto& b = componentsCache_[ib];
                int cmp = 0;
                switch (spec.ColumnUserID) {
                case 0: {
                    auto statusRank = [](const hdm::SystemComponentItem& x) {
                        if (!x.installed) return 0;
                        if (x.updateAvailable) return 1;
                        return 2;
                    };
                    cmp = statusRank(a) - statusRank(b);
                    if (cmp == 0) cmp = a.statusText.compare(b.statusText);
                    break;
                }
                case 1:
                    cmp = a.name.compare(b.name);
                    break;
                case 2: {
                    const std::string& ha = a.detectedHardware.empty()
                                                ? std::string("Игры и системные библиотеки Windows")
                                                : a.detectedHardware;
                    const std::string& hb = b.detectedHardware.empty()
                                                ? std::string("Игры и системные библиотеки Windows")
                                                : b.detectedHardware;
                    cmp = ha.compare(hb);
                    break;
                }
                case 3:
                    cmp = CompareVersionOrString(a.installedVersion, b.installedVersion);
                    break;
                case 4:
                    cmp = CompareVersionOrString(a.latestVersion, b.latestVersion);
                    break;
                default:
                    break;
                }
                if (cmp == 0) {
                    cmp = a.name.compare(b.name);
                }
                return asc ? (cmp < 0) : (cmp > 0);
            });
    }

    compViewDirty_ = false;
}

void DriverLoomApp::RenderComponentsTab() {
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
    const ImGuiStyle& compStyle = ImGui::GetStyle();
    if (!g_isAdvancedMode) {
        compRestoreBefore_ = true;
    }
    const bool busy =
        componentsScanner_.IsScanning() ||
        componentsScanner_.IsCheckingOnline() ||
        componentsScanner_.IsInstalling();

    std::vector<hdm::SystemComponentItem> batchTargets;
    std::vector<hdm::SystemComponentItem> selectedTargets;
    for (const auto& comp : componentsCache_) {
        if ((!comp.installed || comp.updateAvailable) && !comp.directInstallerUrl.empty()) {
            batchTargets.push_back(comp);
        }
        if (!comp.directInstallerUrl.empty() && selectedComponentIds_.count(comp.id) > 0) {
            selectedTargets.push_back(comp);
        }
    }

    ImGui::BeginDisabled(busy);
    if (RenderSecondaryButton(hdm::Tr(
            "Обновить проверку###CompScan",
            "Оновити перевірку###CompScan",
            "Rescan###CompScan"))) {
        componentsScanner_.StartLocalScanAsync(devicesCache_, MakeLogger());
    }
    const char* chkOnlineTxt = hdm::Tr("Проверить онлайн", "Перевірити онлайн", "Check Online");
    SameLineWrapIfNeeded(CalcButtonWidth(chkOnlineTxt));
    if (RenderSecondaryButton(hdm::Tr(
            "Проверить онлайн###CompOnline",
            "Перевірити онлайн###CompOnline",
            "Check Online###CompOnline"))) {
        componentsScanner_.StartOnlineVersionCheckAsync(MakeLogger());
    }
    ImGui::EndDisabled();

    char batchLbl[128];
    if (!batchTargets.empty()) {
        std::snprintf(
            batchLbl,
            sizeof(batchLbl),
            hdm::Tr(
                "Обновить все доступные (%zu)###BatchCompBtn",
                "Оновити всі доступні (%zu)###BatchCompBtn",
                "Update All Available (%zu)###BatchCompBtn"),
            batchTargets.size());
    } else {
        std::snprintf(
            batchLbl,
            sizeof(batchLbl),
            "%s",
            hdm::Tr(
                "Обновить все доступные (0)###BatchCompBtn",
                "Оновити всі доступні (0)###BatchCompBtn",
                "Update All Available (0)###BatchCompBtn"));
    }
    SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr(
        "Обновить все доступные (99)",
        "Оновити всі доступні (99)",
        "Update All Available (99)")));
    ImGui::BeginDisabled(busy || batchTargets.empty());
    if (RenderActionButton(batchLbl, ImVec2(0.0f, 0.0f), selectedTargets.empty())) {
        componentsScanner_.DownloadAndInstallBatchAsync(
            std::move(batchTargets), devicesCache_, MakeLogger(), compRestoreBefore_);
    }
    ImGui::EndDisabled();

    char selBatchLbl[128];
    if (!selectedTargets.empty()) {
        std::snprintf(
            selBatchLbl,
            sizeof(selBatchLbl),
            hdm::Tr(
                "Обновить выбранные (%zu)###BatchSelCompBtn",
                "Оновити вибрані (%zu)###BatchSelCompBtn",
                "Update Selected (%zu)###BatchSelCompBtn"),
            selectedTargets.size());
    } else {
        std::snprintf(
            selBatchLbl,
            sizeof(selBatchLbl),
            "%s",
            hdm::Tr(
                "Обновить выбранные (0)###BatchSelCompBtn",
                "Оновити вибрані (0)###BatchSelCompBtn",
                "Update Selected (0)###BatchSelCompBtn"));
    }
    SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr(
        "Обновить выбранные (99)",
        "Оновити вибрані (99)",
        "Update Selected (99)")));
    ImGui::BeginDisabled(busy || selectedTargets.empty());
    if (RenderActionButton(selBatchLbl, ImVec2(0.0f, 0.0f), !selectedTargets.empty())) {
        componentsScanner_.DownloadAndInstallBatchAsync(
            std::move(selectedTargets), devicesCache_, MakeLogger(), compRestoreBefore_);
    }
    ImGui::EndDisabled();

    const char* selAvailTxt =
        hdm::Tr("Выбрать доступные", "Вибрати доступні", "Select Available");
    SameLineWrapIfNeeded(CalcButtonWidth(selAvailTxt));
    ImGui::BeginDisabled(busy || batchTargets.empty());
    if (RenderSecondaryButton(hdm::Tr(
            "Выбрать доступные###CompSelAvail",
            "Вибрати доступні###CompSelAvail",
            "Select Available###CompSelAvail"))) {
        for (const auto& comp : batchTargets) {
            selectedComponentIds_.insert(comp.id);
        }
    }
    ImGui::EndDisabled();

    const char* clrSelTxt = hdm::Tr("Снять все", "Зняти всі", "Clear Selection");
    SameLineWrapIfNeeded(CalcButtonWidth(clrSelTxt));
    ImGui::BeginDisabled(busy || selectedComponentIds_.empty());
    if (RenderSecondaryButton(hdm::Tr(
            "Снять все###CompClrSel",
            "Зняти всі###CompClrSel",
            "Clear Selection###CompClrSel"))) {
        selectedComponentIds_.clear();
    }
    ImGui::EndDisabled();

    if (componentsScanner_.IsInstalling()) {
        SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Отмена", "Скасувати", "Cancel")));
        if (RenderSecondaryButton(hdm::Tr("Отмена###CompCancel", "Скасувати###CompCancel", "Cancel###CompCancel"))) {
            componentsScanner_.RequestCancelInstall();
        }
    }

    const std::string phase = hdm::TrDyn(componentsScanner_.PhaseMessage());
    if (busy || !phase.empty()) {
        const float winRight = ImGui::GetWindowContentRegionMax().x;
        const float lastEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        const float remain = winRight - lastEnd - 16.0f;
        const float phaseW = ImGui::CalcTextSize(phase.c_str()).x;
        if (remain >= phaseW) {
            ImGui::SameLine(winRight - phaseW);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", phase.c_str());
        } else if (remain >= 150.0f * uiScale) {
            ImGui::SameLine(0.0f, 16.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", EllipsizeUtf8(phase, remain).c_str());
        } else {
            ImGui::TextDisabled("%s", phase.c_str());
        }
    }

    // Row 2: Filter pills
    RenderSecondaryGroupLabel(hdm::Tr("Фильтр:", "Фільтр:", "Filter:"));
    ImGui::SameLine(0.0f, 8.0f * uiScale);
    if (RenderPillButton(hdm::Tr("Все###CfAll", "Усі###CfAll", "All###CfAll"),
                         compFilterMode_ == ComponentFilterMode::All)) {
        compFilterMode_ = ComponentFilterMode::All;
        compViewDirty_ = true;
    }
    const char* cfGameTxt = hdm::Tr("Игры и библиотеки", "Ігри та бібліотеки", "Games & Runtimes");
    SameLineWrapIfNeeded(CalcButtonWidth(cfGameTxt));
    if (RenderPillButton(hdm::Tr("Игры и библиотеки###CfGame", "Ігри та бібліотеки###CfGame", "Games & Runtimes###CfGame"),
                         compFilterMode_ == ComponentFilterMode::GamingRuntimes)) {
        compFilterMode_ = ComponentFilterMode::GamingRuntimes;
        compViewDirty_ = true;
    }
    const char* cfPerTxt = hdm::Tr("Клавиатуры / Мыши / Аудио", "Клавіатури / Миші / Аудіо", "Keyboards / Mice / Audio");
    SameLineWrapIfNeeded(CalcButtonWidth(cfPerTxt));
    if (RenderPillButton(hdm::Tr("Клавиатуры / Мыши / Аудио###CfPer", "Клавіатури / Миші / Аудіо###CfPer", "Keyboards / Mice / Audio###CfPer"),
                         compFilterMode_ == ComponentFilterMode::Peripherals)) {
        compFilterMode_ = ComponentFilterMode::Peripherals;
        compViewDirty_ = true;
    }
    const char* cfMonTxt = hdm::Tr("Мониторы, GPU и плата", "Монітори, GPU та плата", "Monitors, GPU & Board");
    SameLineWrapIfNeeded(CalcButtonWidth(cfMonTxt));
    if (RenderPillButton(hdm::Tr("Мониторы, GPU и плата###CfMon", "Монітори, GPU та плата###CfMon", "Monitors, GPU & Board###CfMon"),
                         compFilterMode_ == ComponentFilterMode::MonitorsAndGpu)) {
        compFilterMode_ = ComponentFilterMode::MonitorsAndGpu;
        compViewDirty_ = true;
    }
    SameLineWrapIfNeeded(
        CalcButtonWidth(hdm::Tr(
            "Точка восстановления перед обновлением",
            "Точка відновлення перед оновленням",
            "Restore point before update")) +
        32.0f * uiScale);
    ImGui::BeginDisabled(!g_isAdvancedMode);
    ImGui::Checkbox(
        hdm::Tr(
            "Точка восстановления перед обновлением###CompRpCb",
            "Точка відновлення перед оновленням###CompRpCb",
            "Restore point before update###CompRpCb"),
        &compRestoreBefore_);
    ImGui::EndDisabled();
    if (GetForegroundWindow() == hwnd_ &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "%s",
            g_isAdvancedMode
                ? hdm::Tr(
                      "Автоматически создать точку восстановления Windows перед пакетным обновлением компонентов.",
                      "Автоматично створити точку відновлення Windows перед пакетним оновленням компонентів.",
                      "Automatically create a Windows restore point before batch updating components.")
                : hdm::Tr(
                      "В обычном режиме точка восстановления перед пакетным обновлением всегда создаётся автоматически.",
                      "У звичайному режимі точка відновлення перед пакетним оновленням завжди створюється автоматично.",
                      "In Standard mode, a restore point is always created automatically before batch updates."));
    }

    if (componentsScanner_.IsInstalling()) {
        const float prog = componentsScanner_.Progress();
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "%.0f%%", prog * 100.0f);
        ImGui::ProgressBar(prog, ImVec2(-1.0f, 18.0f * uiScale), overlay);
    }

    ImGui::Spacing();

    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
        ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY;

    const float primaryW = CalcButtonWidth(hdm::Tr("Установить", "Встановити", "Download")) + 8.0f * uiScale;
    const float siteW = CalcButtonWidth(hdm::Tr("Сайт", "Сайт", "Site")) + 6.0f * uiScale;
    const float fullActionW = primaryW + compStyle.ItemSpacing.x + siteW;
    const float checkW = ImGui::GetFrameHeight();
    const float actionColW =
        fullActionW + compStyle.ItemSpacing.x + checkW + compStyle.CellPadding.x * 2.0f + 4.0f;

    if (ImGui::BeginTable("##ComponentsTable", 6, kTableFlags, ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(
            hdm::Tr("Статус", "Статус", "Status"),
            ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch,
            1.46f,
            0);
        ImGui::TableSetupColumn(
            hdm::Tr("Компонент / Официальное ПО", "Компонент / Офіційне ПЗ", "Component / Official Software"),
            ImGuiTableColumnFlags_WidthStretch,
            1.36f,
            1);
        ImGui::TableSetupColumn(
            hdm::Tr("Устройство / Назначение", "Пристрій / Призначення", "Hardware / Purpose"),
            ImGuiTableColumnFlags_WidthStretch,
            1.10f,
            2);
        ImGui::TableSetupColumn(
            hdm::Tr("Установлено", "Встановлено", "Installed"),
            ImGuiTableColumnFlags_WidthStretch,
            0.70f,
            3);
        ImGui::TableSetupColumn(
            hdm::Tr("Актуальная", "Актуальна", "Latest"),
            ImGuiTableColumnFlags_WidthStretch,
            0.76f,
            4);
        ImGui::TableSetupColumn(
            hdm::Tr("Действие", "Дія", "Action"),
            ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed,
            actionColW,
            5);
        RenderTableHeadersRow();

        ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
        if (sortSpecs != nullptr && sortSpecs->SpecsDirty) {
            compViewDirty_ = true;
            sortSpecs->SpecsDirty = false;
        }
        if (compViewDirty_) {
            RebuildComponentsViewCache(sortSpecs);
        }

        const bool isAppForeground = (GetForegroundWindow() == hwnd_);

        for (size_t viewIdx = 0; viewIdx < visibleComponentIndices_.size(); ++viewIdx) {
            const size_t origIdx = visibleComponentIndices_[viewIdx];
            const auto& item = componentsCache_[origIdx];
            const bool canSelectItem = !item.directInstallerUrl.empty();
            bool isSelected = canSelectItem && selectedComponentIds_.count(item.id) > 0;

            ImGui::TableNextRow();
            if (isSelected) {
                const ImU32 selRowBg = ImGui::ColorConvertFloat4ToU32(
                    g_isLightTheme
                        ? ImVec4(0.16f, 0.48f, 0.90f, 0.12f)
                        : ImVec4(0.22f, 0.50f, 0.90f, 0.18f));
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, selRowBg);
            }
            ImGui::PushID(static_cast<int>(origIdx));

            ImGui::TableSetColumnIndex(0);
            ImVec4 statusCol(0.20f, 0.83f, 0.60f, 1.00f);
            if (!item.installed || item.updateAvailable) {
                statusCol = ImVec4(0.98f, 0.75f, 0.14f, 1.00f);
            }
            const std::string compStatusShown = hdm::TrDyn(item.statusText);
            RenderStatusBadge(statusCol, compStatusShown.c_str(), true);
            if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", compStatusShown.c_str());
            }

            ImGui::TableSetColumnIndex(1);
            RenderCellText(hdm::TrDyn(item.name), false, true);
            if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(440.0f * uiScale);
                ImGui::TextColored(AccentTextColor(), "%s", hdm::TrDyn(item.name).c_str());
                ImGui::TextDisabled(
                    hdm::Tr("Издатель: %s", "Видавець: %s", "Publisher: %s"),
                    item.vendor.c_str());
                if (!item.purposeDescription.empty()) {
                    ImGui::Separator();
                    ImGui::TextUnformatted(hdm::Tr("За что отвечает:", "За що відповідає:", "Purpose:"));
                    ImGui::TextWrapped("%s", hdm::TrDyn(item.purposeDescription).c_str());
                }
                if (!item.detectedHardware.empty()) {
                    ImGui::Separator();
                    ImGui::TextWrapped(
                        hdm::Tr("Оборудование: %s", "Обладнання: %s", "Hardware: %s"),
                        hdm::TrDyn(item.detectedHardware).c_str());
                }
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }

            ImGui::TableSetColumnIndex(2);
            const std::string targetHw = hdm::TrDyn(
                item.detectedHardware.empty() ? "Игры и системные библиотеки Windows"
                                              : item.detectedHardware);
            if (RenderCellText(targetHw, false, true) && isAppForeground &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", targetHw.c_str());
            }

            ImGui::TableSetColumnIndex(3);
            const std::string instShown = hdm::TrDyn(item.installedVersion);
            if (RenderCellText(instShown, item.installedVersion.empty(), true) &&
                isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", instShown.c_str());
            }

            ImGui::TableSetColumnIndex(4);
            const std::string latestShown = hdm::TrDyn(item.latestVersion);
            if (RenderCellText(latestShown, item.latestVersion.empty(), true) &&
                isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", latestShown.c_str());
            }

            ImGui::TableSetColumnIndex(5);
            if (!item.directInstallerUrl.empty()) {
                const bool needAction = !item.installed || item.updateAvailable;
                const char* btnLbl =
                    !item.installed
                        ? hdm::Tr("Установить###CAct", "Встановити###CAct", "Install###CAct")
                        : (item.updateAvailable
                               ? hdm::Tr("Обновить###CAct", "Оновити###CAct", "Update###CAct")
                               : hdm::Tr("Скачать###CAct", "Завантажити###CAct", "Download###CAct"));
                ImGui::BeginDisabled(busy);
                if (RenderActionButton(btnLbl, ImVec2(primaryW, 0.0f), needAction)) {
                    componentsScanner_.DownloadAndInstallAsync(
                        item, devicesCache_, MakeLogger());
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (RenderSecondaryButton(
                        hdm::Tr("Сайт###CSite", "Сайт###CSite", "Site###CSite"),
                        ImVec2(siteW, 0.0f))) {
                    OpenUrlInBrowser(item.officialPageUrl);
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(busy);
                if (ImGui::Checkbox("##compSel", &isSelected)) {
                    if (isSelected) {
                        selectedComponentIds_.insert(item.id);
                    } else {
                        selectedComponentIds_.erase(item.id);
                    }
                }
                ImGui::EndDisabled();
                if (isAppForeground &&
                    ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip(
                        "%s",
                        hdm::Tr(
                            "Выбрать компонент для пакетной установки / обновления",
                            "Вибрати компонент для пакетного встановлення / оновлення",
                            "Select component for batch install / update"));
                }
            } else if (!item.officialPageUrl.empty()) {
                if (RenderSecondaryButton(
                        hdm::Tr("Открыть сайт###COpenSite", "Відкрити сайт###COpenSite", "Open Website###COpenSite"),
                        ImVec2(fullActionW, 0.0f))) {
                    OpenUrlInBrowser(item.officialPageUrl);
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(true);
                bool noDirectSel = false;
                ImGui::Checkbox("##compSel", &noDirectSel);
                ImGui::EndDisabled();
            } else {
                ImGui::Dummy(ImVec2(fullActionW, 0.0f));
                ImGui::SameLine();
                ImGui::BeginDisabled(true);
                bool noDirectSel = false;
                ImGui::Checkbox("##compSel", &noDirectSel);
                ImGui::EndDisabled();
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}
