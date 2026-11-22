#include "ui/DriverLoomApp.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

void DriverLoomApp::RenderWindowsUpdateTab() {
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.0f);
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

    RenderPageHeader(
        hdm::Tr("Обновления Windows", "Оновлення Windows", "Windows Update"),
        hdm::Tr(
            "Официальные драйверы и обновления напрямую с серверов Microsoft (Windows Update).",
            "Офіційні драйвери та оновлення напряму з серверів Microsoft (Windows Update).",
            "Official hardware drivers and system updates directly from Microsoft Windows Update."));

    const bool busy = wuManager_.IsBusy();

    size_t visibleCount = 0;
    size_t hiddenCount = 0;
    std::vector<std::string> selectedIds;
    std::vector<std::string> selectedToHide;
    std::vector<std::string> selectedToUnhide;
    for (const auto& u : wuCache_) {
        if (u.isHidden) {
            ++hiddenCount;
        }
        if (wuShowHidden_ || !u.isHidden) {
            ++visibleCount;
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

    // Welcoming Empty State when no updates are listed
    if (visibleCount == 0) {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float cardW = std::min(avail.x, std::round(560.0f * uiScale));
        const float cardH = std::round(270.0f * uiScale);
        const float padX = std::max(0.0f, (avail.x - cardW) * 0.5f);
        const float padY = std::max(std::round(12.0f * uiScale), (avail.y - cardH) * 0.36f);

        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + padX, ImGui::GetCursorPosY() + padY));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(std::round(26.0f * uiScale), std::round(24.0f * uiScale)));
        ImGui::PushStyleColor(ImGuiCol_Border, PanelBorderColor());
        ImGui::BeginChild(
            "##WuEmptyCard",
            ImVec2(cardW, 0.0f),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
            ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleColor();

        const bool searching = wuManager_.IsSearching();
        const ImVec4 accent = searching ? InfoAccentColor() : (wuHasSearchedOnce_ ? HealthyColor() : InfoAccentColor());
        const AppIcon icon = searching ? AppIcon::Updates : (wuHasSearchedOnce_ ? AppIcon::CheckCircle : AppIcon::Updates);

        const float innerW = ImGui::GetContentRegionAvail().x;
        const float circleDia = std::round(52.0f * uiScale);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 center(p0.x + innerW * 0.5f, p0.y + circleDia * 0.5f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec4 fgCol =
            g_isLightTheme
                ? ImVec4(accent.x * 0.55f, accent.y * 0.55f, accent.z * 0.55f, 1.00f)
                : accent;
        dl->AddCircleFilled(center, circleDia * 0.5f, ImGui::ColorConvertFloat4ToU32(ImVec4(accent.x, accent.y, accent.z, 0.14f)), 32);
        DrawAppIcon(dl, center, circleDia * 0.52f, icon, ImGui::ColorConvertFloat4ToU32(fgCol));
        ImGui::Dummy(ImVec2(innerW, circleDia + std::round(10.0f * uiScale)));

        const char* heading =
            searching
                ? hdm::Tr("Ищем обновления в Windows Update...", "Шукаємо оновлення у Windows Update...", "Searching Windows Update...")
                : (wuHasSearchedOnce_
                       ? hdm::Tr("Новых обновлений не найдено", "Нових оновлень не знайдено", "You're up to date")
                       : hdm::Tr("Проверка обновлений драйверов Microsoft", "Перевірка оновлень драйверів Microsoft", "Check for Official Microsoft Updates"));
        PushFontSafe(g_fontSecondaryHeader);
        const float hw = ImGui::CalcTextSize(heading).x;
        if (hw < innerW) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (innerW - hw) * 0.5f);
        ImGui::TextUnformatted(heading);
        PopFontSafe(g_fontSecondaryHeader);

        ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));
        const char* body =
            searching
                ? hdm::Tr(
                      "Подключаемся к серверам Microsoft. Обычно это занимает от 5 до 20 секунд.",
                      "Підключаємося до серверів Microsoft. Зазвичай це займає від 5 до 20 секунд.",
                      "Connecting to Microsoft servers. This usually takes 5 to 20 seconds.")
                : (wuHasSearchedOnce_
                       ? hdm::Tr(
                             "Все доступные официальные драйверы из Центра обновления Windows уже установлены.",
                             "Усі доступні офіційні драйвери з Центру оновлення Windows вже встановлено.",
                             "All available official drivers from Windows Update are already installed.")
                       : hdm::Tr(
                             "Нажмите кнопку ниже, чтобы проверить доступные драйверы и обновления для вашего ПК. Перед установкой автоматически создастся точка отката.",
                             "Натисніть кнопку нижче, щоб перевірити доступні драйвери та оновлення для вашого ПК. Перед встановленням автоматично створиться точка відкату.",
                             "Click below to scan for official drivers and updates for your PC. A System Restore point is created automatically before installing."));
        ImGui::PushStyleColor(ImGuiCol_Text, MutedTextColor());
        ImGui::TextWrapped("%s", body);
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0.0f, std::round(12.0f * uiScale)));

        if (searching) {
            const float anim = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
            ImGui::ProgressBar(anim, ImVec2(innerW, std::round(18.0f * uiScale)), "");
            ImGui::Dummy(ImVec2(0.0f, std::round(8.0f * uiScale)));
            const char* cancelLbl = hdm::Tr("Отменить поиск###WuEmptyCancel", "Скасувати пошук###WuEmptyCancel", "Cancel Search###WuEmptyCancel");
            const float cW = CalcButtonWidth(hdm::Tr("Отменить поиск", "Скасувати пошук", "Cancel Search")) + 12.0f * uiScale;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (innerW - cW) * 0.5f));
            if (RenderSecondaryButton(cancelLbl, ImVec2(cW, 0.0f))) {
                wuManager_.RequestCancel();
            }
        } else {
            const char* btnLbl =
                wuHasSearchedOnce_
                    ? hdm::Tr("Проверить ещё раз###WuEmptyFind", "Перевірити ще раз###WuEmptyFind", "Check Again###WuEmptyFind")
                    : hdm::Tr("Найти обновления###WuEmptyFind", "Знайти оновлення###WuEmptyFind", "Check for Updates###WuEmptyFind");
            const float bW = std::max(std::round(210.0f * uiScale), CalcButtonWidth(btnLbl) + 24.0f * uiScale);
            const float bH = std::round(36.0f * uiScale);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (innerW - bW) * 0.5f));
            if (RenderPrimaryButton(btnLbl, ImVec2(bW, bH))) {
                selectedWuIndex_ = -1;
                wuHasSearchedOnce_ = true;
                wuManager_.StartSearchAsync(MakeLogger(), !wuIncludeSystemUpdates_);
            }

            ImGui::Dummy(ImVec2(0.0f, std::round(10.0f * uiScale)));
            PushFontSafe(g_fontSmall);
            ImGui::Checkbox(
                hdm::Tr(
                    "Искать также системные обновления Windows (не только драйверы)###WuEmptySysCb",
                    "Шукати також системні оновлення Windows (не лише драйвери)###WuEmptySysCb",
                    "Also include Windows system updates (not only drivers)###WuEmptySysCb"),
                &wuIncludeSystemUpdates_);
            if (g_isAdvancedMode && hiddenCount > 0) {
                char showHidLbl[96];
                std::snprintf(
                    showHidLbl,
                    sizeof(showHidLbl),
                    hdm::Tr("Показать скрытые обновления (%zu)###WuEmptyShowHid", "Показати приховані оновлення (%zu)###WuEmptyShowHid", "Show hidden updates (%zu)###WuEmptyShowHid"),
                    hiddenCount);
                ImGui::Checkbox(showHidLbl, &wuShowHidden_);
            }
            PopFontSafe(g_fontSmall);
        }

        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
    }

    // =========================================================================
    // TOOLBAR (When updates are listed)
    // =========================================================================
    char installLbl[128];
    if (!selectedIds.empty()) {
        std::snprintf(
            installLbl,
            sizeof(installLbl),
            hdm::Tr("Установить выбранные (%zu)###WuInstallBtn", "Встановити вибрані (%zu)###WuInstallBtn", "Install Selected (%zu)###WuInstallBtn"),
            selectedIds.size());
    } else {
        std::snprintf(
            installLbl,
            sizeof(installLbl),
            "%s",
            hdm::Tr("Установить выбранные###WuInstallBtn", "Встановити вибрані###WuInstallBtn", "Install Selected###WuInstallBtn"));
    }

    ImGui::BeginDisabled(busy || selectedIds.empty());
    if (RenderPrimaryButton(installLbl)) {
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

    SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Найти заново", "Знайти знову", "Check Again")));
    ImGui::BeginDisabled(busy);
    if (RenderSecondaryButton(hdm::Tr("Найти заново###WuFindBtn", "Знайти знову###WuFindBtn", "Check Again###WuFindBtn"))) {
        selectedWuIndex_ = -1;
        wuHasSearchedOnce_ = true;
        wuManager_.StartSearchAsync(MakeLogger(), !wuIncludeSystemUpdates_);
    }
    SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Выбрать все", "Вибрати всі", "Select All")));
    if (RenderSecondaryButton(hdm::Tr("Выбрать все###WuSelAll", "Вибрати всі###WuSelAll", "Select All###WuSelAll"))) {
        wuManager_.SelectAll(true, wuShowHidden_);
        for (auto& u : wuCache_) {
            if (!u.installedOk && (wuShowHidden_ || !u.isHidden)) {
                u.selected = true;
            }
        }
    }
    SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Снять все", "Зняти всі", "Clear Selection")));
    if (RenderSecondaryButton(hdm::Tr("Снять все###WuClearAll", "Зняти всі###WuClearAll", "Clear Selection###WuClearAll"))) {
        wuManager_.SelectAll(false, true);
        for (auto& u : wuCache_) u.selected = false;
    }
    ImGui::EndDisabled();

    if (g_isAdvancedMode) {
        char hideLbl[96];
        if (!selectedToHide.empty()) {
            std::snprintf(hideLbl, sizeof(hideLbl), hdm::Tr("Скрыть (%zu)###WuHideBtn", "Приховати (%zu)###WuHideBtn", "Hide (%zu)###WuHideBtn"), selectedToHide.size());
        } else {
            std::snprintf(hideLbl, sizeof(hideLbl), "%s", hdm::Tr("Скрыть###WuHideBtn", "Приховати###WuHideBtn", "Hide###WuHideBtn"));
        }
        SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Скрыть (99)", "Приховати (99)", "Hide (99)")));
        ImGui::BeginDisabled(busy || selectedToHide.empty());
        if (RenderSecondaryButton(hideLbl)) {
            wuManager_.SetUpdatesHiddenAsync(selectedToHide, true, MakeLogger());
        }
        ImGui::EndDisabled();

        if (wuShowHidden_ && !selectedToUnhide.empty()) {
            char unhideLbl[96];
            std::snprintf(unhideLbl, sizeof(unhideLbl), hdm::Tr("Вернуть (%zu)###WuUnhideBtn", "Повернути (%zu)###WuUnhideBtn", "Unhide (%zu)###WuUnhideBtn"), selectedToUnhide.size());
            SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Вернуть (99)", "Повернути (99)", "Unhide (99)")));
            ImGui::BeginDisabled(busy);
            if (RenderSecondaryButton(unhideLbl)) {
                wuManager_.SetUpdatesHiddenAsync(selectedToUnhide, false, MakeLogger());
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

    // Secondary Options Row
    PushFontSafe(g_fontSmall);
    ImGui::Checkbox(
        hdm::Tr("Включать системные обновления Windows###WuSysCb", "Включати системні оновлення Windows###WuSysCb", "Include Windows system updates###WuSysCb"),
        &wuIncludeSystemUpdates_);
    if (g_isAdvancedMode) {
        SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Точка восстановления", "Точка відновлення", "Restore point")) + 32.0f * uiScale);
        ImGui::Checkbox(
            hdm::Tr("Точка восстановления###WuRpCb", "Точка відновлення###WuRpCb", "Restore point###WuRpCb"),
            &wuRestoreBefore_);
        char showHidLbl[96];
        std::snprintf(showHidLbl, sizeof(showHidLbl), hdm::Tr("Скрытые (%zu)###WuShowHiddenCb", "Приховані (%zu)###WuShowHiddenCb", "Hidden (%zu)###WuShowHiddenCb"), hiddenCount);
        SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Скрытые (99)", "Приховані (99)", "Hidden (99)")) + 32.0f * uiScale);
        ImGui::Checkbox(showHidLbl, &wuShowHidden_);
    }
    PopFontSafe(g_fontSmall);

    if (wuManager_.IsInstalling() || wuManager_.IsSearching()) {
        if (wuManager_.IsSearching()) {
            const float anim = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
            ImGui::ProgressBar(anim, ImVec2(-1.0f, 18.0f * uiScale), hdm::Tr("Поиск в Windows Update...", "Пошук у Windows Update...", "Searching Windows Update..."));
        } else {
            const float prog = wuManager_.Progress();
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "%.0f%%", prog * 100.0f);
            ImGui::ProgressBar(prog, ImVec2(-1.0f, 18.0f * uiScale), overlay);
        }
    }

    ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));

    const bool hasVisibleDetails =
        (selectedWuIndex_ >= 0 && selectedWuIndex_ < static_cast<int>(wuCache_.size()) &&
         (wuShowHidden_ || !wuCache_[static_cast<size_t>(selectedWuIndex_)].isHidden));
    const float availY = ImGui::GetContentRegionAvail().y;
    const float wuSpacingY = ImGui::GetStyle().ItemSpacing.y;
    const float detailsH = hasVisibleDetails ? (86.0f * uiScale) : 0.0f;
    const float tableH = std::max(120.0f, availY - (detailsH > 0.0f ? detailsH + wuSpacingY : 0.0f));

    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY;

    if (ImGui::BeginTable("##WuTable", 5, kTableFlags, ImVec2(0.0f, tableH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(hdm::Tr("Выбор", "Вибір", "Select"), ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, 56.0f * uiScale, 0);
        ImGui::TableSetupColumn(hdm::Tr("Обновление", "Оновлення", "Update"), ImGuiTableColumnFlags_WidthStretch, 1.85f, 1);
        ImGui::TableSetupColumn(hdm::Tr("Производитель / Модель", "Виробник / Модель", "Vendor / Model"), ImGuiTableColumnFlags_WidthStretch, 1.05f, 2);
        ImGui::TableSetupColumn(hdm::Tr("Размер", "Розмір", "Size"), ImGuiTableColumnFlags_WidthStretch, 0.48f, 3);
        ImGui::TableSetupColumn(hdm::Tr("Статус", "Статус", "Status"), ImGuiTableColumnFlags_WidthStretch, 0.96f, 4);
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
                        case 3: cmp = (a.maxSizeBytes < b.maxSizeBytes) ? -1 : (a.maxSizeBytes > b.maxSizeBytes ? 1 : 0); break;
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
                                hdm::Tr("Скрыть обновление", "Приховати оновлення", "Hide update"),
                                nullptr,
                                false,
                                !busy && !item.installedOk)) {
                            wuManager_.SetUpdatesHiddenAsync({item.updateId}, true, MakeLogger());
                        }
                    } else {
                        if (ImGui::MenuItem(
                                hdm::Tr("Вернуть обновление", "Повернути оновлення", "Unhide update"),
                                nullptr,
                                false,
                                !busy)) {
                            wuManager_.SetUpdatesHiddenAsync({item.updateId}, false, MakeLogger());
                        }
                    }
                    ImGui::Separator();
                }
                if (ImGui::MenuItem(hdm::Tr("Копировать название", "Копіювати назву", "Copy title"), nullptr, false, !item.title.empty())) {
                    ImGui::SetClipboardText(item.title.c_str());
                }
                if (g_isAdvancedMode &&
                    ImGui::MenuItem(hdm::Tr("Копировать Update ID", "Копіювати Update ID", "Copy Update ID"), nullptr, false, !item.updateId.empty())) {
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
            ImVec4 col = InfoAccentColor();
            if (item.installedOk) col = HealthyColor();
            else if (item.isHidden) col = MutedTextColor();
            RenderStatusBadge(col, hdm::TrDyn(item.statusText).c_str(), true);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (detailsH > 0.0f) {
        const auto& sel = wuCache_[static_cast<size_t>(selectedWuIndex_)];
        ImGui::PushStyleColor(ImGuiCol_Border, PanelBorderColor());
        ImGui::BeginChild(
            "##WuDetails",
            ImVec2(0.0f, detailsH),
            true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        ImGui::TextWrapped("%s", sel.description.empty() ? sel.title.c_str() : sel.description.c_str());
        PushFontSafe(g_fontSmall);
        if (g_isAdvancedMode) {
            ImGui::TextDisabled(
                "Hardware ID: %s   |   %s: %s   |   Update ID: %s",
                sel.driverHardwareId.empty() ? "—" : sel.driverHardwareId.c_str(),
                hdm::Tr("Дата", "Дата", "Date"),
                sel.driverVerDate.empty() ? "—" : sel.driverVerDate.c_str(),
                sel.updateId.c_str());
        } else {
            ImGui::TextDisabled(
                hdm::Tr(
                    "Производитель: %s   •   Модель: %s   •   Дата: %s",
                    "Виробник: %s   •   Модель: %s   •   Дата: %s",
                    "Vendor: %s   •   Model: %s   •   Date: %s"),
                sel.driverManufacturer.empty() ? "—" : sel.driverManufacturer.c_str(),
                sel.driverModel.empty() ? "—" : sel.driverModel.c_str(),
                sel.driverVerDate.empty() ? "—" : sel.driverVerDate.c_str());
        }
        PopFontSafe(g_fontSmall);
        ImGui::EndChild();
    }
}
