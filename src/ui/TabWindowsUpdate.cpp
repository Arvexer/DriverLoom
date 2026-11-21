#include "ui/DriverLoomApp.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

void DriverLoomApp::RenderWindowsUpdateTab() {
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
    const bool isAppForeground = (GetForegroundWindow() == hwnd_);
    if (!g_isAdvancedMode) {
        wuRestoreBefore_ = true;
        wuShowHidden_ = false;
    }
    const uint64_t wuGen = wuManager_.Generation();
    if (wuGen != lastWuGen_) {
        std::string selectedUid;
        if (selectedWuIndex_ >= 0 && selectedWuIndex_ < static_cast<int>(wuCache_.size())) {
            selectedUid = wuCache_[static_cast<size_t>(selectedWuIndex_)].updateId;
        }
        wuCache_ = wuManager_.GetUpdatesSnapshot();
        lastWuGen_ = wuGen;
        wuSortDirty_ = true;
        selectedWuIndex_ = -1;
        if (!selectedUid.empty()) {
            for (int i = 0; i < static_cast<int>(wuCache_.size()); ++i) {
                if (wuCache_[static_cast<size_t>(i)].updateId == selectedUid) {
                    selectedWuIndex_ = i;
                    break;
                }
            }
        }
    }

    const bool busy = wuManager_.IsBusy();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(hdm::Tr(
            "Найти обновления###WuFindBtn",
            "Знайти оновлення###WuFindBtn",
            "Check for Updates###WuFindBtn"))) {
        selectedWuIndex_ = -1;
        wuManager_.StartSearchAsync(MakeLogger(), !wuIncludeSystemUpdates_);
    }
    SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Выбрать все", "Вибрати всі", "Select All")));
    if (RenderSecondaryButton(hdm::Tr(
            "Выбрать все###WuSelAll",
            "Вибрати всі###WuSelAll",
            "Select All###WuSelAll"))) {
        wuManager_.SelectAll(true, wuShowHidden_);
        for (auto& u : wuCache_) {
            if (!u.installedOk && (wuShowHidden_ || !u.isHidden)) {
                u.selected = true;
            }
        }
    }
    SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Снять все", "Зняти всі", "Clear Selection")));
    if (RenderSecondaryButton(hdm::Tr(
            "Снять все###WuClearAll",
            "Зняти всі###WuClearAll",
            "Clear Selection###WuClearAll"))) {
        wuManager_.SelectAll(false, true);
        for (auto& u : wuCache_) u.selected = false;
    }
    ImGui::EndDisabled();

    size_t hiddenCount = 0;
    std::vector<std::string> selectedIds;
    std::vector<std::string> selectedToHide;
    std::vector<std::string> selectedToUnhide;
    for (const auto& u : wuCache_) {
        if (u.isHidden) {
            ++hiddenCount;
        }
        if (!u.installedOk && u.selected) {
            if (wuShowHidden_ || !u.isHidden) {
                selectedIds.push_back(u.updateId);
            }
            if (u.isHidden) {
                selectedToUnhide.push_back(u.updateId);
            } else {
                selectedToHide.push_back(u.updateId);
            }
        }
    }

    char installLbl[128];
    if (!selectedIds.empty()) {
        std::snprintf(
            installLbl,
            sizeof(installLbl),
            hdm::Tr(
                "Установить выбранные (%zu)###WuInstallBtn",
                "Встановити вибрані (%zu)###WuInstallBtn",
                "Install Selected (%zu)###WuInstallBtn"),
            selectedIds.size());
    } else {
        std::snprintf(
            installLbl,
            sizeof(installLbl),
            "%s",
            hdm::Tr(
                "Установить выбранные###WuInstallBtn",
                "Встановити вибрані###WuInstallBtn",
                "Install Selected###WuInstallBtn"));
    }
    const float installBtnW = CalcButtonWidth(
        selectedIds.empty()
            ? hdm::Tr("Установить выбранные", "Встановити вибрані", "Install Selected")
            : hdm::Tr("Установить выбранные (99)", "Встановити вибрані (99)", "Install Selected (99)"));
    SameLineWrapIfNeeded(installBtnW);
    ImGui::BeginDisabled(busy || selectedIds.empty());
    if (ImGui::Button(installLbl)) {
        wuManager_.StartDownloadAndInstallAsync(
            selectedIds,
            wuRestoreBefore_,
            MakeLogger(),
            [this](const hdm::WuInstallSummary& summary) {
                if (summary.rebootRequired) {
                    rebootRequired_.store(true);
                }
                if (summary.installedCount > 0) {
                    pendingDeviceRescan_.store(true);
                }
            },
            !wuIncludeSystemUpdates_);
    }
    ImGui::EndDisabled();

    if (g_isAdvancedMode) {
        char hideLbl[96];
        if (!selectedToHide.empty()) {
            std::snprintf(
                hideLbl,
                sizeof(hideLbl),
                hdm::Tr("Скрыть (%zu)###WuHideBtn", "Приховати (%zu)###WuHideBtn", "Hide (%zu)###WuHideBtn"),
                selectedToHide.size());
        } else {
            std::snprintf(
                hideLbl,
                sizeof(hideLbl),
                "%s",
                hdm::Tr("Скрыть###WuHideBtn", "Приховати###WuHideBtn", "Hide###WuHideBtn"));
        }
        const float hideBtnW = CalcButtonWidth(
            selectedToHide.empty()
                ? hdm::Tr("Скрыть", "Приховати", "Hide")
                : hdm::Tr("Скрыть (99)", "Приховати (99)", "Hide (99)"));
        SameLineWrapIfNeeded(hideBtnW);
        ImGui::BeginDisabled(busy || selectedToHide.empty());
        if (RenderSecondaryButton(hideLbl)) {
            wuManager_.SetUpdatesHiddenAsync(
                selectedToHide, true, MakeLogger(), !wuIncludeSystemUpdates_);
        }
        ImGui::EndDisabled();
        if (isAppForeground &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "%s",
                hdm::Tr(
                    "Запретить Windows Update предлагать и автоматически устанавливать выбранные обновления (IUpdate::put_IsHidden).",
                    "Заборонити Windows Update пропонувати й автоматично встановлювати вибрані оновлення (IUpdate::put_IsHidden).",
                    "Prevent Windows Update from offering and auto-installing selected updates (IUpdate::put_IsHidden)."));
        }

        if (wuShowHidden_ && !selectedToUnhide.empty()) {
            char unhideLbl[96];
            std::snprintf(
                unhideLbl,
                sizeof(unhideLbl),
                hdm::Tr(
                    "Вернуть (%zu)###WuUnhideBtn",
                    "Повернути (%zu)###WuUnhideBtn",
                    "Unhide (%zu)###WuUnhideBtn"),
                selectedToUnhide.size());
            SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Вернуть (99)", "Повернути (99)", "Unhide (99)")));
            ImGui::BeginDisabled(busy);
            if (RenderSecondaryButton(unhideLbl)) {
                wuManager_.SetUpdatesHiddenAsync(
                    selectedToUnhide, false, MakeLogger(), !wuIncludeSystemUpdates_);
            }
            ImGui::EndDisabled();
        }
    }

    if (busy) {
        SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Отмена", "Скасувати", "Cancel")));
        if (RenderSecondaryButton(hdm::Tr("Отмена###WuCancel", "Скасувати###WuCancel", "Cancel###WuCancel"))) {
            wuManager_.RequestCancel();
        }
    }

    const std::string phase = hdm::TrDyn(wuManager_.PhaseMessage());
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
            ImGui::SameLine(0.0f, 14.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", EllipsizeUtf8(phase, remain).c_str());
        }
    }

    ImGui::Checkbox(
        hdm::Tr(
            "Включать системные обновления и библиотеки (не только драйверы)###WuSysCb",
            "Включати системні оновлення та бібліотеки (не лише драйвери)###WuSysCb",
            "Include system updates & runtimes (not only drivers)###WuSysCb"),
        &wuIncludeSystemUpdates_);
    SameLineWrapIfNeeded(
        CalcButtonWidth(hdm::Tr(
            "Точка восстановления перед установкой",
            "Точка відновлення перед встановленням",
            "Create restore point before installing")) +
        32.0f * uiScale);
    ImGui::BeginDisabled(!g_isAdvancedMode);
    ImGui::Checkbox(
        hdm::Tr(
            "Точка восстановления перед установкой###WuRpCb",
            "Точка відновлення перед встановленням###WuRpCb",
            "Create restore point before installing###WuRpCb"),
        &wuRestoreBefore_);
    ImGui::EndDisabled();
    if (!g_isAdvancedMode && isAppForeground &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "%s",
            hdm::Tr(
                "В обычном режиме точка восстановления перед установкой всегда создаётся автоматически.",
                "У звичайному режимі точка відновлення перед встановленням завжди створюється автоматично.",
                "In Standard mode, a restore point is always created automatically before installation."));
    }
    if (g_isAdvancedMode) {
        char showHidLbl[96];
        std::snprintf(
            showHidLbl,
            sizeof(showHidLbl),
            hdm::Tr(
                "Показывать скрытые (%zu)###WuShowHiddenCb",
                "Показувати приховані (%zu)###WuShowHiddenCb",
                "Show hidden (%zu)###WuShowHiddenCb"),
            hiddenCount);
        SameLineWrapIfNeeded(
            CalcButtonWidth(hdm::Tr("Показывать скрытые (99)", "Показувати приховані (99)", "Show hidden (99)")) +
            32.0f * uiScale);
        ImGui::Checkbox(showHidLbl, &wuShowHidden_);
        if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip(
                "%s",
                hdm::Tr(
                    "Показать скрытые через IUpdate::IsHidden обновления, чтобы вернуть их в список.",
                    "Показати приховані через IUpdate::IsHidden оновлення, щоб повернути їх до списку.",
                    "Show updates hidden via IUpdate::IsHidden so you can restore them."));
        }
    }

    if (wuManager_.IsInstalling() || wuManager_.IsSearching()) {
        if (wuManager_.IsSearching()) {
            const float anim = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
            ImGui::ProgressBar(
                anim,
                ImVec2(-1.0f, 18.0f * uiScale),
                hdm::Tr(
                    "Поиск в Windows Update...",
                    "Пошук у Windows Update...",
                    "Searching Windows Update..."));
        } else {
            const float prog = wuManager_.Progress();
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "%.0f%%", prog * 100.0f);
            ImGui::ProgressBar(prog, ImVec2(-1.0f, 18.0f * uiScale), overlay);
        }
    }

    ImGui::Spacing();

    const bool hasVisibleDetails =
        (selectedWuIndex_ >= 0 && selectedWuIndex_ < static_cast<int>(wuCache_.size()) &&
         (wuShowHidden_ || !wuCache_[static_cast<size_t>(selectedWuIndex_)].isHidden));
    const float availY = ImGui::GetContentRegionAvail().y;
    const float wuSpacingY = ImGui::GetStyle().ItemSpacing.y;
    const float detailsH = hasVisibleDetails ? (90.0f * uiScale) : 0.0f;
    const float tableH = std::max(120.0f, availY - (detailsH > 0.0f ? detailsH + wuSpacingY : 0.0f));

    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY;

    if (ImGui::BeginTable("##WuTable", 5, kTableFlags, ImVec2(0.0f, tableH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(
            hdm::Tr("Выбор", "Вибір", "Select"),
            ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed,
            58.0f * uiScale,
            0);
        ImGui::TableSetupColumn(
            hdm::Tr("Обновление", "Оновлення", "Update"),
            ImGuiTableColumnFlags_WidthStretch,
            1.80f,
            1);
        ImGui::TableSetupColumn(
            hdm::Tr("Производитель / Модель", "Виробник / Модель", "Vendor / Model"),
            ImGuiTableColumnFlags_WidthStretch,
            1.05f,
            2);
        ImGui::TableSetupColumn(
            hdm::Tr("Размер", "Розмір", "Size"),
            ImGuiTableColumnFlags_WidthStretch,
            0.50f,
            3);
        ImGui::TableSetupColumn(
            hdm::Tr("Статус", "Статус", "Status"),
            ImGuiTableColumnFlags_WidthStretch,
            0.98f,
            4);
        RenderTableHeadersRow();

        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
            if ((sortSpecs->SpecsDirty || wuSortDirty_) && sortSpecs->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
                const bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
                std::string selectedUid;
                if (selectedWuIndex_ >= 0 && selectedWuIndex_ < static_cast<int>(wuCache_.size())) {
                    selectedUid = wuCache_[static_cast<size_t>(selectedWuIndex_)].updateId;
                }
                std::sort(
                    wuCache_.begin(),
                    wuCache_.end(),
                    [&](const hdm::DriverUpdateItem& a, const hdm::DriverUpdateItem& b) {
                        int cmp = 0;
                        switch (spec.ColumnUserID) {
                        case 1: cmp = a.title.compare(b.title); break;
                        case 2: {
                            const std::string ma = a.driverManufacturer + " " + a.driverModel;
                            const std::string mb = b.driverManufacturer + " " + b.driverModel;
                            cmp = ma.compare(mb);
                            break;
                        }
                        case 3:
                            cmp = (a.maxSizeBytes < b.maxSizeBytes)
                                      ? -1
                                      : (a.maxSizeBytes > b.maxSizeBytes ? 1 : 0);
                            break;
                        case 4: cmp = a.statusText.compare(b.statusText); break;
                        default: break;
                        }
                        return asc ? (cmp < 0) : (cmp > 0);
                    });
                if (!selectedUid.empty()) {
                    selectedWuIndex_ = -1;
                    for (int i = 0; i < static_cast<int>(wuCache_.size()); ++i) {
                        if (wuCache_[static_cast<size_t>(i)].updateId == selectedUid) {
                            selectedWuIndex_ = i;
                            break;
                        }
                    }
                }
                sortSpecs->SpecsDirty = false;
                wuSortDirty_ = false;
            }
        }

        const float frameH = ImGui::GetFrameHeight();
        for (int i = 0; i < static_cast<int>(wuCache_.size()); ++i) {
            auto& item = wuCache_[static_cast<size_t>(i)];
            if (item.isHidden && !wuShowHidden_) {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::PushID(i);

            ImGui::TableSetColumnIndex(0);
            const float cbOffset = std::max(0.0f, (ImGui::GetContentRegionAvail().x - frameH) * 0.5f);
            if (cbOffset > 0.0f) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cbOffset);
            }
            bool checked = item.selected;
            ImGui::BeginDisabled(busy || item.installedOk);
            if (ImGui::Checkbox("##sel", &checked)) {
                wuManager_.SetUpdateSelected(item.updateId, checked);
                item.selected = checked;
            }
            ImGui::EndDisabled();

            ImGui::TableSetColumnIndex(1);
            const std::string shownTitle = EllipsizeUtf8(item.title, ImGui::GetContentRegionAvail().x - 2.0f);
            const std::string selLabel = shownTitle + "##wuRow";
            ImGui::AlignTextToFramePadding();
            const bool isSelectedRow = (selectedWuIndex_ == i);
            if (ImGui::Selectable(
                    selLabel.c_str(),
                    isSelectedRow,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                    ImVec2(0.0f, frameH))) {
                selectedWuIndex_ = i;
            }
            if (ImGui::BeginPopupContextItem("##WuRowCtx")) {
                selectedWuIndex_ = i;
                ImGui::TextDisabled("%s", EllipsizeUtf8(item.title, 340.0f * uiScale).c_str());
                ImGui::Separator();
                if (g_isAdvancedMode) {
                    if (!item.isHidden) {
                        if (ImGui::MenuItem(
                                hdm::Tr(
                                    "Скрыть обновление (запретить автоустановку)",
                                    "Приховати оновлення (заборонити автовстановлення)",
                                    "Hide update (block automatic install)"),
                                nullptr,
                                false,
                                !busy && !item.installedOk)) {
                            wuManager_.SetUpdatesHiddenAsync(
                                {item.updateId}, true, MakeLogger(), !wuIncludeSystemUpdates_);
                        }
                    } else {
                        if (ImGui::MenuItem(
                                hdm::Tr(
                                    "Вернуть обновление (разрешить установку)",
                                    "Повернути оновлення (дозволити встановлення)",
                                    "Unhide update (allow installation)"),
                                nullptr,
                                false,
                                !busy)) {
                            wuManager_.SetUpdatesHiddenAsync(
                                {item.updateId}, false, MakeLogger(), !wuIncludeSystemUpdates_);
                        }
                    }
                    ImGui::Separator();
                }
                if (ImGui::MenuItem(
                        hdm::Tr(
                            "Копировать название обновления",
                            "Копіювати назву оновлення",
                            "Copy update title"),
                        nullptr,
                        false,
                        !item.title.empty())) {
                    ImGui::SetClipboardText(item.title.c_str());
                }
                if (g_isAdvancedMode &&
                    ImGui::MenuItem(
                        hdm::Tr("Копировать Update ID", "Копіювати Update ID", "Copy Update ID"),
                        nullptr,
                        false,
                        !item.updateId.empty())) {
                    ImGui::SetClipboardText(item.updateId.c_str());
                }
                ImGui::EndPopup();
            }
            if (shownTitle != item.title && isAppForeground &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", item.title.c_str());
            }

            ImGui::TableSetColumnIndex(2);
            std::string mfgModel = item.driverManufacturer;
            if (!item.driverModel.empty()) {
                if (!mfgModel.empty()) mfgModel += " — ";
                mfgModel += item.driverModel;
            }
            if (RenderCellText(mfgModel, mfgModel.empty(), true) && isAppForeground &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", mfgModel.c_str());
            }

            ImGui::TableSetColumnIndex(3);
            RenderCellText(hdm::TrDyn(item.sizeFormatted), true, true);

            ImGui::TableSetColumnIndex(4);
            ImVec4 col(0.60f, 0.72f, 0.90f, 1.00f);
            if (item.installedOk) {
                col = ImVec4(0.20f, 0.83f, 0.60f, 1.00f);
            } else if (item.isHidden) {
                col = ImVec4(0.58f, 0.68f, 0.86f, 1.00f);
            }
            const std::string wuStatusShown = hdm::TrDyn(item.statusText);
            RenderStatusBadge(col, wuStatusShown.c_str(), true);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (detailsH > 0.0f) {
        const auto& sel = wuCache_[static_cast<size_t>(selectedWuIndex_)];
        ImGui::BeginChild(
            "##WuDetails",
            ImVec2(0.0f, detailsH),
            true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::TextWrapped("%s", sel.description.empty() ? sel.title.c_str() : sel.description.c_str());
        if (g_isAdvancedMode) {
            ImGui::TextDisabled(
                hdm::Tr(
                    "Hardware ID: %s   |   Дата: %s   |   Update ID: %s",
                    "Hardware ID: %s   |   Дата: %s   |   Update ID: %s",
                    "Hardware ID: %s   |   Date: %s   |   Update ID: %s"),
                sel.driverHardwareId.empty() ? "—" : sel.driverHardwareId.c_str(),
                sel.driverVerDate.empty() ? "—" : sel.driverVerDate.c_str(),
                sel.updateId.c_str());
        } else {
            ImGui::TextDisabled(
                hdm::Tr(
                    "Производитель: %s   |   Модель: %s   |   Дата: %s",
                    "Виробник: %s   |   Модель: %s   |   Дата: %s",
                    "Vendor: %s   |   Model: %s   |   Date: %s"),
                sel.driverManufacturer.empty() ? "—" : sel.driverManufacturer.c_str(),
                sel.driverModel.empty() ? "—" : sel.driverModel.c_str(),
                sel.driverVerDate.empty() ? "—" : sel.driverVerDate.c_str());
        }
        ImGui::EndChild();
    }
}
