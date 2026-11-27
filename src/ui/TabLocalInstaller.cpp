#include "ui/DriverLoomApp.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

void DriverLoomApp::RebuildDriverStoreViewCache(ImGuiTableSortSpecs* sortSpecs) {
    visibleStoreIndices_.clear();
    visibleStoreIndices_.reserve(driverStoreCache_.packages.size());

    for (size_t i = 0; i < driverStoreCache_.packages.size(); ++i) {
        const auto& pkg = driverStoreCache_.packages[i];
        if (storeFilterMode_ == DriverStoreFilterMode::OldDuplicates && !pkg.isOldDuplicate) {
            continue;
        }
        if (storeFilterMode_ == DriverStoreFilterMode::ActiveOnly && !pkg.inUse) {
            continue;
        }
        if (storeFilterMode_ == DriverStoreFilterMode::UnusedOnly && pkg.inUse) {
            continue;
        }
        if (storeFilterBuf_[0] != '\0') {
            const bool matched =
                hdm::ContainsCaseInsensitive(pkg.oemInfName, storeFilterBuf_) ||
                hdm::ContainsCaseInsensitive(pkg.originalInfName, storeFilterBuf_) ||
                hdm::ContainsCaseInsensitive(pkg.activeDeviceName, storeFilterBuf_) ||
                hdm::ContainsCaseInsensitive(pkg.provider, storeFilterBuf_) ||
                hdm::ContainsCaseInsensitive(pkg.className, storeFilterBuf_) ||
                hdm::ContainsCaseInsensitive(pkg.driverVersion, storeFilterBuf_);
            if (!matched) {
                continue;
            }
        }
        visibleStoreIndices_.push_back(i);
    }

    if (sortSpecs != nullptr && sortSpecs->SpecsCount > 0) {
        const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
        const bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
        std::sort(
            visibleStoreIndices_.begin(),
            visibleStoreIndices_.end(),
            [&](size_t ia, size_t ib) {
                const auto& a = driverStoreCache_.packages[ia];
                const auto& b = driverStoreCache_.packages[ib];
                int cmp = 0;
                switch (spec.ColumnUserID) {
                case 0: {
                    auto rank = [](const hdm::DriverStorePackageEntry& x) {
                        return x.isOldDuplicate ? 0 : (x.inUse ? 1 : 2);
                    };
                    cmp = rank(a) - rank(b);
                    break;
                }
                case 1: cmp = a.oemInfName.compare(b.oemInfName); break;
                case 2: cmp = a.originalInfName.compare(b.originalInfName); break;
                case 3: cmp = a.provider.compare(b.provider); break;
                case 4: cmp = a.className.compare(b.className); break;
                case 5: cmp = CompareVersionOrString(a.driverVersion, b.driverVersion); break;
                case 6:
                    cmp = (a.sizeBytes < b.sizeBytes) ? -1 : (a.sizeBytes > b.sizeBytes ? 1 : 0);
                    break;
                default: break;
                }
                if (cmp == 0) cmp = a.oemInfName.compare(b.oemInfName);
                return asc ? (cmp < 0) : (cmp > 0);
            });
    }
    storeViewDirty_ = false;
}

void DriverLoomApp::RenderDriverStoreSection() {
    const float uiScale = EffectiveUiScale();
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool isAppForeground = (GetForegroundWindow() == hwnd_);
    const bool busy = localInstaller_.IsPickingFolder() ||
                      localInstaller_.IsScanningStore() ||
                      localInstaller_.IsExportingBackup() ||
                      localInstaller_.IsRemovingStorePackages() ||
                      localInstaller_.IsInstalling();

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(std::round(8.5f * uiScale), style.FramePadding.y));
    ImGui::BeginDisabled(busy);
    if (RenderPrimaryButton(hdm::Tr(
            "Экспорт бэкапа в папку...###DsExp",
            "Експорт бекапу в папку...###DsExp",
            "Export Backup Folder...###DsExp"))) {
        const bool skipDuplicatesSnapshot = backupSkipDuplicates_;
        localInstaller_.PickFolderAsync(
            hwnd_,
            MakeLogger(),
            [this, skipDuplicatesSnapshot](bool picked, const std::wstring& path) {
                if (picked && !path.empty()) {
                    const std::string utf8 = hdm::WideToUtf8(path);
                    {
                        std::lock_guard<std::mutex> lock(pendingPathMutex_);
                        pendingFolderUtf8_ = utf8;
                        hasPendingFolder_ = true;
                    }
                    localInstaller_.ExportDriverStoreBackupAsync(
                        path,
                        skipDuplicatesSnapshot,
                        MakeLogger(),
                        [this, path](bool ok, size_t exportedCount) {
                            if (ok && exportedCount > 0) {
                                localInstaller_.ScanInfFilesAsync(path, nullptr);
                            }
                        });
                }
            },
            hdm::Utf8ToWide(hdm::Tr(
                "Выберите папку для сохранения бэкапа драйверов",
                "Виберіть папку для збереження бекапу драйверів",
                "Select folder to save driver backup")));
    }
    if (isAppForeground &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "%s",
            hdm::Tr(
                "Копирует установленные сторонние пакеты драйверов (FileRepository) в выбранную папку.\n"
                "После переустановки Windows выберите эту папку во вкладке «Установка из папки (.inf)».",
                "Копіює встановлені сторонні пакети драйверів (FileRepository) до вибраної папки.\n"
                "Після перевстановлення Windows виберіть цю папку у вкладці «Встановлення з папки (.inf)».",
                "Copies installed third-party driver packages (FileRepository) to the chosen folder.\n"
                "After reinstalling Windows, select this folder in the 'Install from Folder (.inf)' tab."));
    }
    ImGui::EndDisabled();

    size_t selectedCount = 0;
    uint64_t selectedBytes = 0;
    for (const auto& pkg : driverStoreCache_.packages) {
        if (!pkg.inUse && pkg.selected) {
            ++selectedCount;
            selectedBytes += pkg.sizeBytes;
        }
    }

    char rmSelLbl[128];
    if (selectedCount > 0) {
        std::snprintf(
            rmSelLbl,
            sizeof(rmSelLbl),
            hdm::Tr(
                "Удалить выбранные (%zu / %s)###RmDupBtn",
                "Видалити вибрані (%zu / %s)###RmDupBtn",
                "Remove Selected (%zu / %s)###RmDupBtn"),
            selectedCount,
            hdm::FormatBytes(selectedBytes).c_str());
    } else {
        std::snprintf(
            rmSelLbl,
            sizeof(rmSelLbl),
            "%s",
            hdm::Tr(
                "Удалить выбранные (0)###RmDupBtn",
                "Видалити вибрані (0)###RmDupBtn",
                "Remove Selected (0)###RmDupBtn"));
    }
    SameLineWrapIfNeeded(
        CalcButtonWidth(hdm::Tr(
            "Удалить выбранные (99 / 999.9 МБ)",
            "Видалити вибрані (99 / 999.9 МБ)",
            "Remove Selected (99 / 999.9 MB)")) +
        6.0f);
    ImGui::BeginDisabled(busy || selectedCount == 0);
    if (RenderSecondaryButton(rmSelLbl)) {
        std::vector<std::string> targets;
        targets.reserve(selectedCount);
        for (const auto& pkg : driverStoreCache_.packages) {
            if (!pkg.inUse && pkg.selected) {
                targets.push_back(pkg.oemInfName);
            }
        }
        if (!targets.empty()) {
            localInstaller_.RemoveDriverStorePackagesAsync(
                std::move(targets),
                storeRestoreBefore_,
                devicesCache_,
                MakeLogger(),
                [this](size_t removed, size_t failed, bool rebootReq) {
                    (void)failed;
                    if (rebootReq) rebootRequired_.store(true);
                    if (removed > 0) pendingDeviceRescan_.store(true);
                });
        }
    }
    ImGui::EndDisabled();

    const char* selDupTxt = hdm::Tr("Выбрать дубликаты", "Вибрати дублікати", "Select Duplicates");
    SameLineWrapIfNeeded(CalcButtonWidth(selDupTxt));
    ImGui::BeginDisabled(busy || driverStoreCache_.duplicateCount == 0);
    if (RenderSecondaryButton(hdm::Tr(
            "Выбрать дубликаты###DsSelDup",
            "Вибрати дублікати###DsSelDup",
            "Select Duplicates###DsSelDup"))) {
        for (auto& pkg : driverStoreCache_.packages) {
            pkg.selected = (!pkg.inUse && pkg.isOldDuplicate);
        }
    }
    ImGui::EndDisabled();

    SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Снять все", "Зняти всі", "Clear Selection")));
    ImGui::BeginDisabled(busy || selectedCount == 0);
    if (RenderSecondaryButton(hdm::Tr(
            "Снять все###DsClear",
            "Зняти всі###DsClear",
            "Clear Selection###DsClear"))) {
        for (auto& pkg : driverStoreCache_.packages) {
            pkg.selected = false;
        }
    }
    ImGui::EndDisabled();

    SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Обновить", "Оновити", "Refresh")));
    ImGui::BeginDisabled(busy);
    if (RenderSecondaryButton(hdm::Tr("Обновить###DsRef", "Оновити###DsRef", "Refresh###DsRef"))) {
        localInstaller_.ScanDriverStoreAsync(devicesCache_, MakeLogger());
    }
    ImGui::EndDisabled();

    // Row 2: Filter pills + right-aligned SummaryInfoBadge
    char allLbl[64], dupFiltLbl[64], actLbl[64];
    std::snprintf(
        allLbl,
        sizeof(allLbl),
        hdm::Tr("Все (%zu)###DsAll", "Усі (%zu)###DsAll", "All (%zu)###DsAll"),
        driverStoreCache_.packages.size());
    std::snprintf(
        dupFiltLbl,
        sizeof(dupFiltLbl),
        hdm::Tr("Дубликаты (%zu)###DsDup", "Дублікати (%zu)###DsDup", "Duplicates (%zu)###DsDup"),
        driverStoreCache_.duplicateCount);
    std::snprintf(
        actLbl,
        sizeof(actLbl),
        hdm::Tr("Активные (%zu)###DsAct", "Активні (%zu)###DsAct", "Active (%zu)###DsAct"),
        driverStoreCache_.activeCount);

    RenderSecondaryGroupLabel(hdm::Tr("Фильтр:", "Фільтр:", "Filter:"));
    ImGui::SameLine(0.0f, 8.0f * uiScale);
    if (RenderPillButton(allLbl, storeFilterMode_ == DriverStoreFilterMode::All)) {
        storeFilterMode_ = DriverStoreFilterMode::All;
        storeViewDirty_ = true;
    }
    SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Дубликаты (999)", "Дублікати (999)", "Duplicates (999)")));
    if (RenderPillButton(dupFiltLbl, storeFilterMode_ == DriverStoreFilterMode::OldDuplicates)) {
        storeFilterMode_ = DriverStoreFilterMode::OldDuplicates;
        storeViewDirty_ = true;
    }
    SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Активные (999)", "Активні (999)", "Active (999)")));
    if (RenderPillButton(actLbl, storeFilterMode_ == DriverStoreFilterMode::ActiveOnly)) {
        storeFilterMode_ = DriverStoreFilterMode::ActiveOnly;
        storeViewDirty_ = true;
    }
    SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Неиспользуемые", "Невикористані", "Unused")));
    if (RenderPillButton(
            hdm::Tr("Неиспользуемые###DsUn", "Невикористані###DsUn", "Unused###DsUn"),
            storeFilterMode_ == DriverStoreFilterMode::UnusedOnly)) {
        storeFilterMode_ = DriverStoreFilterMode::UnusedOnly;
        storeViewDirty_ = true;
    }

    if (driverStoreCache_.valid) {
        const std::string totalSizeStr = hdm::FormatBytes(driverStoreCache_.totalStoreBytes);
        char actStr[32], dupStr[32];
        std::snprintf(actStr, sizeof(actStr), "%zu", driverStoreCache_.activeCount);
        std::snprintf(dupStr, sizeof(dupStr), "%zu", driverStoreCache_.duplicateCount);

        const ImVec4 dupAccent =
            (driverStoreCache_.duplicateCount > 0)
                ? ImVec4(0.98f, 0.72f, 0.16f, 1.00f)
                : ImVec4(0.20f, 0.83f, 0.60f, 1.00f);
        const SummaryBadgeSegment segments[3] = {
            {hdm::Tr("Всего:", "Усього:", "Total:"), totalSizeStr.c_str(), ImVec4(0.46f, 0.74f, 0.98f, 1.00f), false},
            {hdm::Tr("Активных:", "Активних:", "Active:"), actStr, ImVec4(0.20f, 0.83f, 0.60f, 1.00f), false},
            {hdm::Tr("Дубликатов:", "Дублікатів:", "Duplicates:"), dupStr, dupAccent, driverStoreCache_.duplicateCount > 0},
        };

        const float sumW = CalcSummaryInfoBadgeWidth(segments, 3);
        const float winRight = ImGui::GetWindowContentRegionMax().x;
        const float lastEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        if (winRight - sumW > lastEnd + 10.0f * uiScale) {
            ImGui::SameLine(winRight - sumW);
        }
        RenderSummaryInfoBadge(segments, 3);
    }
    ImGui::PopStyleVar();

    // Row 3: Checkboxes kept together cleanly
    ImGui::Checkbox(hdm::Tr("Без дубликатов###DsSkipDup", "Без дублікатів###DsSkipDup", "Skip duplicates###DsSkipDup"), &backupSkipDuplicates_);
    if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip(
            "%s",
            hdm::Tr(
                "Не включать старые дубликаты пакетов при экспорте бэкапа в папку.",
                "Не включати старі дублікати пакетів під час експорту бекапу в папку.",
                "Exclude older duplicate driver packages when exporting backup."));
    }
    SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Точка восстановления", "Точка відновлення", "Restore point")) + 32.0f * uiScale);
    ImGui::Checkbox(hdm::Tr("Точка восстановления###DsRp", "Точка відновлення###DsRp", "Restore point###DsRp"), &storeRestoreBefore_);

    {
        const bool hasStoreFilter = (storeFilterBuf_[0] != '\0');
        const char* clrTxt = hdm::Tr("Сброс", "Скинути", "Clear");
        const float clrW = hasStoreFilter ? (CalcButtonWidth(clrTxt) + 8.0f * uiScale) : 0.0f;
        const float searchW = std::max(
            180.0f,
            ImGui::GetContentRegionAvail().x - (hasStoreFilter ? (clrW + style.ItemSpacing.x) : 0.0f));
        ImGui::SetNextItemWidth(searchW);
        if (ImGui::InputTextWithHint(
                "##DriverStoreSearch",
                hdm::Tr(
                    "Поиск по oem*.inf, оригинальному .inf, устройству, поставщику, классу или версии...",
                    "Пошук за oem*.inf, оригінальним .inf, пристроєм, виробником, класом або версією...",
                    "Search by oem*.inf, original .inf, device, vendor, class, or version..."),
                storeFilterBuf_,
                sizeof(storeFilterBuf_))) {
            storeViewDirty_ = true;
        }
        if (hasStoreFilter) {
            ImGui::SameLine();
            if (RenderSecondaryButton(
                    hdm::Tr("Сброс###DsClrSearch", "Скинути###DsClrSearch", "Clear###DsClrSearch"),
                    ImVec2(clrW, 0.0f))) {
                storeFilterBuf_[0] = '\0';
                storeViewDirty_ = true;
            }
        }
    }

    if (localInstaller_.IsExportingBackup() || localInstaller_.IsRemovingStorePackages()) {
        const float prog = localInstaller_.Progress();
        const std::string statusMsg = hdm::TrDyn(localInstaller_.StatusMessage());
        char pctBuf[16];
        std::snprintf(pctBuf, sizeof(pctBuf), "%.0f%%", prog * 100.0f);
        const std::string overlay = std::string(pctBuf) + " — " + statusMsg;
        ImGui::ProgressBar(prog, ImVec2(-1.0f, 18.0f * uiScale), overlay.c_str());
    }

    ImGui::Spacing();

    if (!driverStoreCache_.valid) {
        ImGui::TextDisabled(
            "%s",
            hdm::Tr(
                "Выполняется анализ хранилища драйверов DriverStore (oem*.inf)...",
                "Виконується аналіз сховища драйверів DriverStore (oem*.inf)...",
                "Analyzing Windows DriverStore (oem*.inf)..."));
        return;
    }

    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY;

    const char* rmTxt = hdm::Tr("Удалить", "Видалити", "Remove");
    const float rmBtnW = CalcButtonWidth(rmTxt) + 8.0f * uiScale;
    const float actionColW = rmBtnW + style.CellPadding.x * 2.0f + std::round(6.0f * uiScale);

    const char* selHdr = hdm::Tr("Выбор", "Вибір", "Select");
    PushFontSafe(g_fontSecondaryHeader);
    const float selHdrW = ImGui::CalcTextSize(selHdr).x;
    PopFontSafe(g_fontSecondaryHeader);
    const float selColW = std::max(
        74.0f * uiScale,
        selHdrW + style.CellPadding.x * 2.0f + std::round(8.0f * uiScale));

    if (ImGui::BeginTable("##DriverStoreTable", 9, kTableFlags, ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(selHdr, ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, selColW, 8);
        ImGui::TableSetupColumn(hdm::Tr("Статус", "Статус", "Status"), ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 1.88f, 0);
        ImGui::TableSetupColumn(hdm::Tr("Пакет", "Пакет", "Package"), ImGuiTableColumnFlags_WidthStretch, 0.98f, 1);
        ImGui::TableSetupColumn(hdm::Tr("INF / Активное устройство", "INF / Активний пристрій", "INF / Active Device"), ImGuiTableColumnFlags_WidthStretch, 1.58f, 2);
        ImGui::TableSetupColumn(hdm::Tr("Поставщик", "Виробник", "Provider"), ImGuiTableColumnFlags_WidthStretch, 0.92f, 3);
        ImGui::TableSetupColumn(hdm::Tr("Класс", "Клас", "Class"), ImGuiTableColumnFlags_WidthStretch, 0.62f, 4);
        ImGui::TableSetupColumn(hdm::Tr("Версия / Дата", "Версія / Дата", "Version / Date"), ImGuiTableColumnFlags_WidthStretch, 1.20f, 5);
        ImGui::TableSetupColumn(hdm::Tr("Размер", "Розмір", "Size"), ImGuiTableColumnFlags_WidthStretch, 0.78f, 6);
        ImGui::TableSetupColumn(hdm::Tr("Действие", "Дія", "Action"), ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, actionColW, 7);
        RenderTableHeadersRow();

        ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
        if (sortSpecs != nullptr && sortSpecs->SpecsDirty) {
            storeViewDirty_ = true;
            sortSpecs->SpecsDirty = false;
        }
        if (storeViewDirty_) {
            RebuildDriverStoreViewCache(sortSpecs);
        }

        const float frameH = ImGui::GetFrameHeight();
        for (size_t vIdx = 0; vIdx < visibleStoreIndices_.size(); ++vIdx) {
            const size_t origIdx = visibleStoreIndices_[vIdx];
            auto& pkg = driverStoreCache_.packages[origIdx];

            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(origIdx));

            ImGui::TableSetColumnIndex(0);
            const float cbOffset = std::max(0.0f, (ImGui::GetContentRegionAvail().x - frameH) * 0.5f);
            if (cbOffset > 0.0f) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cbOffset);
            }
            ImGui::BeginDisabled(busy || pkg.inUse);
            ImGui::Checkbox("##dsSel", &pkg.selected);
            ImGui::EndDisabled();

            ImGui::TableSetColumnIndex(1);
            if (pkg.isOldDuplicate) {
                RenderStatusBadge(
                    ImVec4(0.98f, 0.75f, 0.14f, 1.00f),
                    hdm::Tr("Старый дубликат", "Старий дублікат", "Old duplicate"),
                    true);
            } else if (pkg.inUse) {
                RenderStatusBadge(
                    ImVec4(0.20f, 0.83f, 0.60f, 1.00f),
                    hdm::Tr("Активен", "Активний", "In Use"),
                    true);
            } else {
                RenderStatusBadge(
                    ImVec4(0.58f, 0.68f, 0.86f, 1.00f),
                    hdm::Tr("В хранилище", "У сховищі", "Stored"),
                    true);
            }

            ImGui::TableSetColumnIndex(2);
            RenderCellText(pkg.oemInfName, false, true);

            ImGui::TableSetColumnIndex(3);
            const std::string activeDevShown = hdm::TrDyn(pkg.activeDeviceName);
            const std::string title =
                activeDevShown.empty()
                    ? pkg.originalInfName
                    : (pkg.originalInfName + " — " + activeDevShown);
            RenderCellText(title, false, true);
            if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(460.0f * uiScale);
                ImGui::TextColored(AccentTextColor(), "%s (%s)", pkg.originalInfName.c_str(), pkg.oemInfName.c_str());
                if (!activeDevShown.empty()) {
                    ImGui::Text(
                        hdm::Tr("Устройство: %s", "Пристрій: %s", "Device: %s"),
                        activeDevShown.c_str());
                }
                ImGui::TextDisabled(
                    hdm::Tr(
                        "Поставщик: %s  |  Класс: %s",
                        "Виробник: %s  |  Клас: %s",
                        "Provider: %s  |  Class: %s"),
                    pkg.provider.empty() ? "—" : pkg.provider.c_str(),
                    pkg.className.empty() ? "—" : pkg.className.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }

            ImGui::TableSetColumnIndex(4);
            RenderCellText(pkg.provider, pkg.provider.empty(), true);

            ImGui::TableSetColumnIndex(5);
            RenderCellText(pkg.className, pkg.className.empty(), true);

            ImGui::TableSetColumnIndex(6);
            std::string verDate = pkg.driverVersion.empty() ? "—" : pkg.driverVersion;
            if (!pkg.driverDate.empty()) {
                verDate += " (" + pkg.driverDate + ")";
            }
            RenderCellText(verDate, pkg.driverVersion.empty(), true);

            ImGui::TableSetColumnIndex(7);
            RenderCellText(hdm::FormatBytes(pkg.sizeBytes), true, true);

            ImGui::TableSetColumnIndex(8);
            if (!pkg.inUse) {
                ImGui::BeginDisabled(busy);
                if (RenderSecondaryButton(
                        hdm::Tr("Удалить###DsRm", "Видалити###DsRm", "Remove###DsRm"),
                        ImVec2(rmBtnW, 0.0f))) {
                    localInstaller_.RemoveDriverStorePackagesAsync(
                        {pkg.oemInfName},
                        storeRestoreBefore_,
                        devicesCache_,
                        MakeLogger(),
                        [this](size_t removed, size_t failed, bool rebootReq) {
                            (void)failed;
                            if (rebootReq) rebootRequired_.store(true);
                            if (removed > 0) pendingDeviceRescan_.store(true);
                        });
                }
                ImGui::EndDisabled();
            } else {
                const char* busyTxt = hdm::Tr("Занят", "Зайнятий", "In use");
                const float usedW = ImGui::CalcTextSize(busyTxt).x;
                const float availCell = ImGui::GetContentRegionAvail().x;
                if (availCell > usedW) {
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availCell - usedW) * 0.5f);
                }
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", busyTxt);
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void DriverLoomApp::RenderLocalInstallerTab() {
    const float uiScale = EffectiveUiScale();
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool isAppForeground = (GetForegroundWindow() == hwnd_);

    RenderPageHeader(
        hdm::Tr("Локальные драйверы (.inf) и DriverStore", "Локальні драйвери (.inf) та DriverStore", "Local Drivers (.inf) & DriverStore"),
        hdm::Tr(
            "Режим эксперта: ручная установка .inf-пакетов, резервное копирование и очистка старых дубликатов DriverStore.",
            "Режим експерта: ручне встановлення .inf-пакетів, резервне копіювання та очищення старих дублікатів DriverStore.",
            "Expert mode: manual .inf driver installation, DriverStore backup export, and old duplicate cleanup."));

    if (RenderSubSectionHeaderTab(
            hdm::Tr(
                "Установка из папки (.inf)###LocSec0",
                "Встановлення з папки (.inf)###LocSec0",
                "Install from Folder (.inf)###LocSec0"),
            localSectionIndex_ == 0)) {
        localSectionIndex_ = 0;
    }
    char storePill[96];
    if (driverStoreCache_.duplicateCount > 0) {
        std::snprintf(
            storePill,
            sizeof(storePill),
            hdm::Tr(
                "Бэкап и хранилище DriverStore (%zu | %zu дубл.)###StorePill",
                "Бекап і сховище DriverStore (%zu | %zu дубл.)###StorePill",
                "DriverStore Backup & Cleanup (%zu | %zu dup.)###StorePill"),
            driverStoreCache_.packages.size(),
            driverStoreCache_.duplicateCount);
    } else {
        std::snprintf(
            storePill,
            sizeof(storePill),
            hdm::Tr(
                "Бэкап и хранилище DriverStore (%zu)###StorePill",
                "Бекап і сховище DriverStore (%zu)###StorePill",
                "DriverStore Backup & Cleanup (%zu)###StorePill"),
            driverStoreCache_.packages.size());
    }
    SameLineWrapIfNeeded(CalcSubSectionTabWidth(hdm::Tr(
        "Бэкап и хранилище DriverStore (999 | 99 дубл.)",
        "Бекап і сховище DriverStore (999 | 99 дубл.)",
        "DriverStore Backup & Cleanup (999 | 99 dup.)")));
    if (RenderSubSectionHeaderTab(storePill, localSectionIndex_ == 1)) {
        localSectionIndex_ = 1;
    }

    ImGui::Separator();

    if (localSectionIndex_ == 1) {
        RenderDriverStoreSection();
        return;
    }

    const bool busy =
        localInstaller_.IsPickingFolder() ||
        localInstaller_.IsScanningInf() ||
        localInstaller_.IsInstalling() ||
        localInstaller_.IsExportingBackup() ||
        localInstaller_.IsRemovingStorePackages();

    const char* folderLbl = hdm::Tr("Папка с драйверами:", "Папка з драйверами:", "Drivers Folder:");
    const char* pickLbl = hdm::Tr("Обзор папки...###LocPick", "Огляд папки...###LocPick", "Browse Folder...###LocPick");
    const char* scanLbl = hdm::Tr("Найти .inf###LocScan", "Знайти .inf###LocScan", "Scan .inf###LocScan");
    const float pickW = CalcButtonWidth(hdm::Tr("Обзор папки...", "Огляд папки...", "Browse Folder..."));
    const float scanW = CalcButtonWidth(hdm::Tr("Найти .inf", "Знайти .inf", "Scan .inf"));
    const float buttonsW = pickW + scanW + style.ItemSpacing.x * 2.0f + std::round(4.0f * uiScale);
    const float labelW = ImGui::CalcTextSize(folderLbl).x + style.ItemSpacing.x;
    const float availRowW = ImGui::GetContentRegionAvail().x;
    const bool wrapButtonsToNextLine = (availRowW < labelW + std::round(220.0f * uiScale) + buttonsW);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(folderLbl);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(
        wrapButtonsToNextLine
            ? std::max(140.0f * uiScale, ImGui::GetContentRegionAvail().x)
            : std::max(180.0f * uiScale, ImGui::GetContentRegionAvail().x - buttonsW));
    ImGui::InputTextWithHint(
        "##FolderInput",
        hdm::Tr(
            "Выберите папку с распакованными .inf драйверами...",
            "Виберіть папку з розпакованими .inf драйверами...",
            "Select folder containing extracted .inf drivers..."),
        localFolderBuf_,
        sizeof(localFolderBuf_));

    if (!wrapButtonsToNextLine) {
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(busy);
    if (RenderSecondaryButton(pickLbl)) {
        localInstaller_.PickFolderAsync(
            hwnd_,
            MakeLogger(),
            [this](bool picked, const std::wstring& path) {
                if (picked && !path.empty()) {
                    const std::string utf8 = hdm::WideToUtf8(path);
                    {
                        std::lock_guard<std::mutex> lock(pendingPathMutex_);
                        pendingFolderUtf8_ = utf8;
                        hasPendingFolder_ = true;
                    }
                    localInstaller_.ScanInfFilesAsync(path, MakeLogger());
                }
            });
    }
    SameLineWrapIfNeeded(scanW);
    if (RenderSecondaryButton(scanLbl)) {
        localInstaller_.ScanInfFilesAsync(
            hdm::Utf8ToWide(localFolderBuf_), MakeLogger());
    }
    ImGui::EndDisabled();

    {
        std::lock_guard<std::mutex> lock(pendingPathMutex_);
        if (hasPendingFolder_) {
            std::snprintf(
                localFolderBuf_, sizeof(localFolderBuf_), "%s", pendingFolderUtf8_.c_str());
            hasPendingFolder_ = false;
        }
    }

    ImGui::Checkbox(
        hdm::Tr(
            "Принудительная установка (DIIRFLAG_FORCE_INF)###LocForce",
            "Примусове встановлення (DIIRFLAG_FORCE_INF)###LocForce",
            "Force install (DIIRFLAG_FORCE_INF)###LocForce"),
        &localForceInstall_);
    SameLineWrapIfNeeded(
        CalcButtonWidth(hdm::Tr(
            "Точка восстановления перед установкой",
            "Точка відновлення перед встановленням",
            "Create restore point before installing")) +
        32.0f * uiScale);
    ImGui::Checkbox(
        hdm::Tr(
            "Точка восстановления перед установкой###LocRp",
            "Точка відновлення перед встановленням###LocRp",
            "Create restore point before installing###LocRp"),
        &localRestoreBefore_);

    auto infFiles = localInstaller_.GetInfFilesSnapshot();
    SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr(
        "Установить все .inf (DiInstallDriverW)",
        "Встановити всі .inf (DiInstallDriverW)",
        "Install All .inf (DiInstallDriverW)")));
    ImGui::BeginDisabled(busy || std::strlen(localFolderBuf_) == 0);
    if (RenderPrimaryButton(hdm::Tr(
            "Установить все .inf (DiInstallDriverW)###LocInstAll",
            "Встановити всі .inf (DiInstallDriverW)###LocInstAll",
            "Install All .inf (DiInstallDriverW)###LocInstAll"))) {
        RequestProtectedInstall(
            hdm::Tr("Установка локальных драйверов (.inf)", "Встановлення локальних драйверів (.inf)", "Install Local .inf Drivers"),
            localRestoreBefore_,
            [this, folderW = hdm::Utf8ToWide(localFolderBuf_), force = localForceInstall_](bool rp) {
                localInstaller_.InstallFromFolderAsync(
                    folderW,
                    force,
                    rp,
                    MakeLogger(),
                    [this](const hdm::LocalInstallSummary& summary) {
                        if (summary.rebootRequired) {
                            rebootRequired_.store(true);
                        }
                        if (summary.installedCount > 0 || summary.stagedOnlyCount > 0) {
                            pendingDeviceRescan_.store(true);
                        }
                    });
            });
    }
    ImGui::EndDisabled();

    {
        char infValStr[32];
        std::snprintf(infValStr, sizeof(infValStr), "%zu", infFiles.size());
        const ImVec4 infAccent =
            infFiles.empty()
                ? ImVec4(0.58f, 0.68f, 0.86f, 1.00f)
                : ImVec4(0.46f, 0.74f, 0.98f, 1.00f);
        const SummaryBadgeSegment segment = {
            hdm::Tr("Найдено .inf:", "Знайдено .inf:", "Found .inf:"),
            infValStr,
            infAccent,
            false,
        };
        const float countW = CalcSummaryInfoBadgeWidth(&segment, 1);
        const float winRight = ImGui::GetWindowContentRegionMax().x;
        const float lastEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        if (winRight - countW > lastEnd + 10.0f * uiScale) {
            ImGui::SameLine(winRight - countW);
        } else {
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), winRight - countW));
        }
        RenderSummaryInfoBadge(&segment, 1);
    }

    if (localInstaller_.IsInstalling()) {
        const float prog = localInstaller_.Progress();
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "%.0f%%", prog * 100.0f);
        ImGui::ProgressBar(prog, ImVec2(-1.0f, 18.0f * uiScale), overlay);
    }

    ImGui::Spacing();

    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY;

    if (ImGui::BeginTable("##InfTable", 3, kTableFlags, ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(hdm::Tr("Имя файла", "Ім'я файлу", "File Name"), ImGuiTableColumnFlags_WidthStretch, 1.00f, 0);
        ImGui::TableSetupColumn(hdm::Tr("Относительный путь", "Відносний шлях", "Relative Path"), ImGuiTableColumnFlags_WidthStretch, 2.00f, 1);
        ImGui::TableSetupColumn(hdm::Tr("Размер", "Розмір", "Size"), ImGuiTableColumnFlags_WidthStretch, 0.50f, 2);
        RenderTableHeadersRow();

        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
            if (sortSpecs->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
                const bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
                std::sort(
                    infFiles.begin(),
                    infFiles.end(),
                    [&](const hdm::InfFileEntry& a, const hdm::InfFileEntry& b) {
                        int cmp = 0;
                        switch (spec.ColumnUserID) {
                        case 0: cmp = a.fileName.compare(b.fileName); break;
                        case 1: cmp = a.relativePath.compare(b.relativePath); break;
                        case 2:
                            cmp = (a.sizeBytes < b.sizeBytes)
                                      ? -1
                                      : (a.sizeBytes > b.sizeBytes ? 1 : 0);
                            break;
                        default: break;
                        }
                        return asc ? (cmp < 0) : (cmp > 0);
                    });
            }
        }

        for (const auto& entry : infFiles) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            RenderCellText(entry.fileName);
            ImGui::TableSetColumnIndex(1);
            if (RenderCellText(entry.relativePath) && isAppForeground &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", entry.relativePath.c_str());
            }
            ImGui::TableSetColumnIndex(2);
            RenderCellText(hdm::FormatBytes(entry.sizeBytes), true);
        }
        ImGui::EndTable();
    }
}
