#include "ui/DriverLoomApp.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

void DriverLoomApp::RebuildComponentsViewCache(ImGuiTableSortSpecs* sortSpecs) {
    (void)sortSpecs;
    visibleComponentIndices_.clear();
    visibleComponentIndices_.reserve(componentsCache_.size());

    for (size_t i = 0; i < componentsCache_.size(); ++i) {
        const auto& item = componentsCache_[i];
        if (compFilterMode_ == ComponentFilterMode::GamingRuntimes &&
            item.category != hdm::ComponentCategory::GamingRuntime) {
            continue;
        }
        if (compFilterMode_ == ComponentFilterMode::Peripherals &&
            !IsOptionalVendorSoftware(item)) {
            continue;
        }
        if (compFilterMode_ == ComponentFilterMode::MonitorsAndGpu &&
            (item.category == hdm::ComponentCategory::GamingRuntime || IsOptionalVendorSoftware(item))) {
            continue;
        }
        visibleComponentIndices_.push_back(i);
    }

    // Sort: recommended updates/missing first, then optional vendor updates/installs, then up-to-date.
    std::sort(
        visibleComponentIndices_.begin(),
        visibleComponentIndices_.end(),
        [&](size_t ia, size_t ib) {
            const auto& a = componentsCache_[ia];
            const auto& b = componentsCache_[ib];
            auto rank = [](const hdm::SystemComponentItem& x) {
                const bool isOpt = IsOptionalComponent(x);
                if (!isOpt) {
                    if (!x.installed && !x.directInstallerUrl.empty()) return 0;
                    if (x.updateAvailable && !x.directInstallerUrl.empty()) return 1;
                    if (!x.installed) return 2;
                    if (x.updateAvailable) return 3;
                    return 6;
                }
                if (x.installed && x.updateAvailable) return 4;
                if (!x.installed) return 5;
                return 6;
            };
            const int ra = rank(a);
            const int rb = rank(b);
            if (ra != rb) {
                return ra < rb;
            }
            return a.name < b.name;
        });

    compViewDirty_ = false;
}

void DriverLoomApp::RenderComponentsTab() {
    const float uiScale = EffectiveUiScale();
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool isAppForeground = (GetForegroundWindow() == hwnd_);

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
        if (IsComponentRecommendedAction(comp)) {
            batchTargets.push_back(comp);
        }
        if (!comp.directInstallerUrl.empty() && selectedComponentIds_.count(comp.id) > 0) {
            selectedTargets.push_back(comp);
        }
    }

    RenderPageHeader(
        hdm::Tr("Игры и программы", "Ігри та програми", "Games & Software"),
        hdm::Tr(
            "Библиотеки для запуска игр (DirectX, Visual C++, .NET), драйверы видеокарты и фирменное ПО для вашей периферии.",
            "Бібліотеки для запуску ігор (DirectX, Visual C++, .NET), драйвери відеокарти та фірмове ПЗ для вашої периферії.",
            "Essential gaming runtimes (DirectX, Visual C++, .NET), graphics drivers, and official software for your peripherals."));

    // Row 1: Category Filter Pills (left) + Main Action Buttons (right)
    char allLbl[48], gameLbl[64], gpuLbl[72], optLbl[72];
    std::snprintf(allLbl, sizeof(allLbl), hdm::Tr("Все (%zu)###CfAll", "Усі (%zu)###CfAll", "All (%zu)###CfAll"), componentsCache_.size());
    std::snprintf(gameLbl, sizeof(gameLbl), hdm::Tr("Для игр (%zu)###CfGame", "Для ігор (%zu)###CfGame", "Gaming (%zu)###CfGame"), gamingComponentsCount_);
    std::snprintf(gpuLbl, sizeof(gpuLbl), hdm::Tr("Драйверы и GPU (%zu)###CfMon", "Драйвери та GPU (%zu)###CfMon", "Drivers & GPU (%zu)###CfMon"), gpuMonitorComponentsCount_);
    std::snprintf(optLbl, sizeof(optLbl), hdm::Tr("Доп. ПО (%zu)###CfPer", "Дод. ПЗ (%zu)###CfPer", "Optional Apps (%zu)###CfPer"), peripheralComponentsCount_);

    if (RenderPillButton(allLbl, compFilterMode_ == ComponentFilterMode::All)) {
        compFilterMode_ = ComponentFilterMode::All;
        compViewDirty_ = true;
    }
    SameLineWrapIfNeeded(CalcButtonWidth(gameLbl));
    if (RenderPillButton(gameLbl, compFilterMode_ == ComponentFilterMode::GamingRuntimes)) {
        compFilterMode_ = ComponentFilterMode::GamingRuntimes;
        compViewDirty_ = true;
    }
    SameLineWrapIfNeeded(CalcButtonWidth(gpuLbl));
    if (RenderPillButton(gpuLbl, compFilterMode_ == ComponentFilterMode::MonitorsAndGpu)) {
        compFilterMode_ = ComponentFilterMode::MonitorsAndGpu;
        compViewDirty_ = true;
    }
    SameLineWrapIfNeeded(CalcButtonWidth(optLbl));
    if (RenderPillButton(optLbl, compFilterMode_ == ComponentFilterMode::Peripherals)) {
        compFilterMode_ = ComponentFilterMode::Peripherals;
        compViewDirty_ = true;
    }

    // Right-aligned primary CTA + Check button
    const bool useSelectedBatch = g_isAdvancedMode && !selectedTargets.empty();
    char mainActLbl[128];
    if (useSelectedBatch) {
        std::snprintf(
            mainActLbl,
            sizeof(mainActLbl),
            hdm::Tr("Установить выбранные (%zu)###BatchActBtn", "Встановити вибрані (%zu)###BatchActBtn", "Install Selected (%zu)###BatchActBtn"),
            selectedTargets.size());
    } else if (!batchTargets.empty()) {
        std::snprintf(
            mainActLbl,
            sizeof(mainActLbl),
            hdm::Tr("Установить рекомендуемые (%zu)###BatchActBtn", "Встановити рекомендовані (%zu)###BatchActBtn", "Install Recommended (%zu)###BatchActBtn"),
            batchTargets.size());
    } else {
        std::snprintf(
            mainActLbl,
            sizeof(mainActLbl),
            "%s",
            hdm::Tr("Всё актуально###BatchActBtn", "Усе актуально###BatchActBtn", "All Up to Date###BatchActBtn"));
    }

    const char* checkLbl = hdm::Tr("Проверить###CompChk", "Перевірити###CompChk", "Check###CompChk");
    const float checkW = CalcButtonWidth(hdm::Tr("Проверить", "Перевірити", "Check")) + 4.0f * uiScale;
    const float mainW = ImGui::CalcTextSize(mainActLbl, nullptr, true).x + style.FramePadding.x * 2.0f + 4.0f * uiScale;
    const float rightClusterW = checkW + style.ItemSpacing.x + mainW;
    const float winRight = ImGui::GetWindowContentRegionMax().x;
    const float lastEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;

    if (winRight - rightClusterW > lastEnd + 12.0f * uiScale) {
        ImGui::SameLine(winRight - rightClusterW);
    }

    ImGui::BeginDisabled(busy);
    if (RenderSecondaryButton(checkLbl, ImVec2(checkW, 0.0f))) {
        componentsScanner_.StartLocalScanAsync(devicesCache_, MakeLogger(), true);
    }
    ImGui::EndDisabled();

    SameLineWrapIfNeeded(mainW);
    ImGui::BeginDisabled(busy || (!useSelectedBatch && batchTargets.empty()));
    if (RenderActionButton(mainActLbl, ImVec2(mainW, 0.0f), !batchTargets.empty() || useSelectedBatch)) {
        if (useSelectedBatch) {
            componentsScanner_.DownloadAndInstallBatchAsync(
                std::move(selectedTargets), devicesCache_, MakeLogger(), compRestoreBefore_);
        } else {
            componentsScanner_.DownloadAndInstallBatchAsync(
                batchTargets, devicesCache_, MakeLogger(), compRestoreBefore_);
        }
    }
    ImGui::EndDisabled();

    // Expert-only batch selection controls & restore checkbox
    if (g_isAdvancedMode) {
        ImGui::Dummy(ImVec2(0.0f, std::round(2.0f * uiScale)));
        PushFontSafe(g_fontSmall);
        const char* selAvailLbl = hdm::Tr("Выбрать доступные###CompSelAvail", "Вибрати доступні###CompSelAvail", "Select Available###CompSelAvail");
        const char* clrSelLbl = hdm::Tr("Снять выбор###CompClrSel", "Зняти вибір###CompClrSel", "Clear Selection###CompClrSel");
        const char* rpCbLbl = hdm::Tr("Точка восстановления перед установкой###CompRpCb", "Точка відновлення перед встановленням###CompRpCb", "Create restore point before installing###CompRpCb");
        const float clrSelW = CalcButtonWidth(clrSelLbl);
        const char* rpCbVisible = hdm::Tr("Точка восстановления перед установкой", "Точка відновлення перед встановленням", "Create restore point before installing");
        const float rpCbW =
            ImGui::CalcTextSize(rpCbVisible).x + ImGui::GetFrameHeight() +
            style.ItemInnerSpacing.x + std::round(8.0f * uiScale);

        ImGui::BeginDisabled(busy || batchTargets.empty());
        if (RenderSecondaryButton(selAvailLbl)) {
            for (const auto& comp : batchTargets) {
                selectedComponentIds_.insert(comp.id);
            }
        }
        ImGui::EndDisabled();
        SameLineWrapIfNeeded(clrSelW);
        ImGui::BeginDisabled(busy || selectedComponentIds_.empty());
        if (RenderSecondaryButton(clrSelLbl)) {
            selectedComponentIds_.clear();
        }
        ImGui::EndDisabled();
        SameLineWrapIfNeeded(rpCbW);
        ImGui::Checkbox(rpCbLbl, &compRestoreBefore_);
        PopFontSafe(g_fontSmall);
    }

    // Active download/install progress bar
    if (componentsScanner_.IsInstalling()) {
        ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));
        const float prog = componentsScanner_.Progress();
        const std::string phase = hdm::TrDyn(componentsScanner_.PhaseMessage());
        char pctBuf[16];
        std::snprintf(pctBuf, sizeof(pctBuf), "%.0f%%", prog * 100.0f);
        const std::string overlay = phase + " (" + pctBuf + ")";
        const char* cancelTxt = hdm::Tr("Отмена", "Скасувати", "Cancel");
        const float cancelW = CalcButtonWidth(cancelTxt) + 8.0f * uiScale;
        ImGui::ProgressBar(
            prog,
            ImVec2(std::max(120.0f, ImGui::GetContentRegionAvail().x - cancelW - style.ItemSpacing.x),
                   ImGui::GetFrameHeight()),
            overlay.c_str());
        ImGui::SameLine();
        if (RenderSecondaryButton(hdm::Tr("Отмена###CompCancel", "Скасувати###CompCancel", "Cancel###CompCancel"), ImVec2(cancelW, 0.0f))) {
            componentsScanner_.RequestCancelInstall();
        }
    }

    ImGui::Dummy(ImVec2(0.0f, std::round(6.0f * uiScale)));

    if (compViewDirty_) {
        RebuildComponentsViewCache(nullptr);
    }

    // =========================================================================
    // SPACIOUS CARD-ROW LIST
    // =========================================================================
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild(
        "##ComponentsCardList",
        ImGui::GetContentRegionAvail(),
        ImGuiChildFlags_None,
        ImGuiWindowFlags_None);
    ImGui::PopStyleColor();

    if (visibleComponentIndices_.empty()) {
        ImGui::Dummy(ImVec2(0.0f, std::round(24.0f * uiScale)));
        ImGui::TextColored(
            MutedTextColor(),
            "%s",
            busy ? hdm::Tr("Идёт сканирование компонентов...", "Триває сканування компонентів...", "Scanning components...")
                 : hdm::Tr("В этой категории компонентов не найдено.", "У цій категорії компонентів не знайдено.", "No components found in this category."));
        ImGui::EndChild();
        return;
    }

    const float rowPadX = std::round(14.0f * uiScale);
    const float rowPadY = std::round(11.0f * uiScale);
    const float primaryW =
        std::max({
            CalcButtonWidth(hdm::Tr("Установить", "Встановити", "Install")),
            CalcButtonWidth(hdm::Tr("Обновить", "Оновити", "Update")),
            CalcButtonWidth(hdm::Tr("Переуст.", "Перевст.", "Reinstall")),
        }) +
        6.0f * uiScale;
    const float siteW = CalcButtonWidth(hdm::Tr("Сайт", "Сайт", "Site")) + 2.0f * uiScale;
    const float openSiteW = primaryW + style.ItemSpacing.x + siteW;
    const float checkBoxReserve = g_isAdvancedMode ? (ImGui::GetFrameHeight() + style.ItemSpacing.x) : 0.0f;
    const float actionsBlockW = openSiteW + checkBoxReserve;
    const float line1EstH =
        (g_fontSecondaryHeader != nullptr ? g_fontSecondaryHeader->LegacySize : ImGui::GetTextLineHeight()) +
        std::round(8.0f * uiScale);
    const float line2EstH =
        (g_fontSmall != nullptr ? g_fontSmall->LegacySize : ImGui::GetTextLineHeight()) +
        std::round(4.0f * uiScale);
    const float cardH = std::max(
        std::round(76.0f * uiScale),
        rowPadY * 2.0f + line1EstH + line2EstH + std::round(6.0f * uiScale));

    for (size_t viewIdx = 0; viewIdx < visibleComponentIndices_.size(); ++viewIdx) {
        const size_t origIdx = visibleComponentIndices_[viewIdx];
        const auto& item = componentsCache_[origIdx];
        const bool canInstallDirectly = !item.directInstallerUrl.empty();
        const bool isOptional = IsOptionalComponent(item);
        const bool needAction = !isOptional && (!item.installed || item.updateAvailable);
        const bool optionalUpdate = isOptional && item.installed && item.updateAvailable;
        const bool optionalInstall = isOptional && !item.installed;
        bool isSelected = canInstallDirectly && selectedComponentIds_.count(item.id) > 0;

        ImGui::PushID(static_cast<int>(origIdx));

        const float scrollGap = (ImGui::GetScrollMaxY() > 0.0f) ? std::round(12.0f * uiScale) : 0.0f;
        const float availCardW = std::max(320.0f, ImGui::GetContentRegionAvail().x - scrollGap);
        const ImVec4 cardBg =
            isSelected
                ? (g_isLightTheme ? ImVec4(0.92f, 0.96f, 1.00f, 1.00f) : ImVec4(0.11f, 0.16f, 0.25f, 1.00f))
                : CardBgColor();
        const ImVec4 cardBorder =
            isSelected ? AccentBarColor() : PanelBorderColor();

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(rowPadX, rowPadY));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg);
        ImGui::PushStyleColor(ImGuiCol_Border, cardBorder);
        ImGui::BeginChild(
            "##CompCard",
            ImVec2(availCardW, cardH),
            true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor(2);

        // Subtle left accent strip only for recommended actions or optional updates to already-installed software
        if (needAction || optionalUpdate) {
            const ImVec2 cMin = ImGui::GetWindowPos();
            const ImVec2 cMax(cMin.x + std::round(3.5f * uiScale), cMin.y + cardH);
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(cMin.x + 1.0f, cMin.y + std::round(8.0f * uiScale)),
                ImVec2(cMax.x + 1.0f, cMax.y - std::round(8.0f * uiScale)),
                ImGui::ColorConvertFloat4ToU32(needAction ? WarningColor() : InfoAccentColor()),
                2.0f);
        }

        const float innerW = ImGui::GetContentRegionAvail().x;
        const float versionBlockW = std::min(
            std::round(146.0f * uiScale),
            std::max(std::round(88.0f * uiScale),
                     (innerW - actionsBlockW - std::round(16.0f * uiScale)) * 0.28f));
        const float leftTextMaxW = std::max(
            100.0f * uiScale,
            innerW - actionsBlockW - versionBlockW - std::round(16.0f * uiScale));

        // Left: Title + Status Pill (Line 1) and Plain Description (Line 2), vertically centered
        const std::string nameShown = hdm::TrDyn(item.name);
        const char* shortBadge = hdm::Tr("Установлен", "Встановлено", "Installed");
        ImVec4 badgeCol = HealthyColor();
        if (optionalInstall) {
            shortBadge = hdm::Tr("По желанию", "За бажанням", "Optional");
            badgeCol = MutedTextColor();
        } else if (optionalUpdate) {
            shortBadge = hdm::Tr("Есть обновление", "Є оновлення", "Update available");
            badgeCol = InfoAccentColor();
        } else if (!item.installed) {
            shortBadge = hdm::Tr("Не установлен", "Не встановлено", "Not installed");
            badgeCol = WarningColor();
        } else if (item.updateAvailable) {
            shortBadge = hdm::Tr("Есть обновление", "Є оновлення", "Update available");
            badgeCol = WarningColor();
        }

        PushFontSafe(g_fontSmall);
        const float smallTextH = ImGui::GetTextLineHeight();
        const float badgeH = smallTextH + std::round(6.0f * uiScale);
        const float badgePadW = std::round(30.0f * uiScale);
        const float badgeEstW = ImGui::CalcTextSize(shortBadge).x + badgePadW;
        const float badgeMaxW = std::min(badgeEstW, std::max(72.0f * uiScale, leftTextMaxW * 0.44f));
        PopFontSafe(g_fontSmall);

        PushFontSafe(g_fontSecondaryHeader);
        const float titleH = ImGui::GetTextLineHeight();
        PopFontSafe(g_fontSecondaryHeader);

        const float line1H = std::max(titleH, badgeH);
        const float lineGapY = std::round(5.0f * uiScale);
        const float leftBlockH = line1H + lineGapY + smallTextH;
        const float line1TopY = std::max(rowPadY, std::round((cardH - leftBlockH) * 0.5f));
        const float titleGapX = std::round(10.0f * uiScale);

        ImGui::SetCursorPosY(line1TopY);
        ImGui::BeginGroup();
        {
            PushFontSafe(g_fontSecondaryHeader);
            ImGui::SetCursorPosY(line1TopY + std::max(0.0f, (line1H - titleH) * 0.5f));
            const float maxTitleCap = std::max(40.0f * uiScale, leftTextMaxW - badgeMaxW - titleGapX);
            const std::string shownTitle = EllipsizeUtf8(nameShown, maxTitleCap);
            const float shownTitleW = ImGui::CalcTextSize(shownTitle.c_str()).x;
            ImGui::TextColored(CardHeaderTextColor(), "%s", shownTitle.c_str());
            PopFontSafe(g_fontSecondaryHeader);

            const float remainingBadgeW = std::max(
                24.0f * uiScale,
                leftTextMaxW - shownTitleW - titleGapX);
            ImGui::SameLine(0.0f, titleGapX);
            PushFontSafe(g_fontSmall);
            ImGui::SetCursorPosY(line1TopY + std::max(0.0f, (line1H - badgeH) * 0.5f));
            RenderStatusBadge(badgeCol, shortBadge, false, remainingBadgeW);
            PopFontSafe(g_fontSmall);

            ImGui::SetCursorPosY(line1TopY + line1H + lineGapY);

            // Second line: human-readable purpose or hardware target
            std::string descLine;
            if (!item.purposeDescription.empty()) {
                descLine = hdm::TrDyn(item.purposeDescription);
            } else if (!item.detectedHardware.empty()) {
                descLine = hdm::TrDyn(item.detectedHardware);
            } else {
                descLine = hdm::Tr("Системный компонент Windows", "Системний компонент Windows", "Windows system component");
            }
            PushFontSafe(g_fontSmall);
            ImGui::TextColored(MutedTextColor(), "%s", EllipsizeUtf8(descLine, leftTextMaxW).c_str());
            PopFontSafe(g_fontSmall);
        }
        ImGui::EndGroup();

        if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(440.0f * uiScale);
            ImGui::TextColored(AccentTextColor(), "%s", hdm::TrDyn(item.name).c_str());
            ImGui::TextDisabled(
                hdm::Tr("Издатель: %s", "Видавець: %s", "Publisher: %s"),
                item.vendor.c_str());
            if (!item.purposeDescription.empty()) {
                ImGui::Separator();
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

        // Middle-Right: Clean Version Info (vertically centered inside card)
        const float rightStartX = ImGui::GetWindowContentRegionMax().x - actionsBlockW;
        const float verStartX = rightStartX - versionBlockW - std::round(12.0f * uiScale);
        ImGui::SameLine(verStartX);
        PushFontSafe(g_fontSmall);
        const float verBlockH = ImGui::GetTextLineHeight() * 2.0f + style.ItemSpacing.y;
        ImGui::SetCursorPosY(std::max(rowPadY, std::round((cardH - verBlockH) * 0.5f)));
        ImGui::BeginGroup();
        const std::string instVer = hdm::TrDyn(item.installedVersion);
        const std::string latVer = hdm::TrDyn(item.latestVersion);
        if (item.installed && item.updateAvailable && !latVer.empty()) {
            ImGui::TextColored(MutedTextColor(), "%s", EllipsizeUtf8(instVer.empty() ? "—" : instVer, versionBlockW).c_str());
            const float arrowW = ImGui::CalcTextSize("-> ").x;
            ImGui::TextColored(
                WarningTextColor(),
                "-> %s",
                EllipsizeUtf8(latVer, std::max(24.0f * uiScale, versionBlockW - arrowW)).c_str());
        } else if (!item.installed && !latVer.empty()) {
            ImGui::TextColored(MutedTextColor(), "%s", hdm::Tr("Версия:", "Версія:", "Version:"));
            ImGui::TextUnformatted(EllipsizeUtf8(latVer, versionBlockW).c_str());
        } else {
            ImGui::TextColored(MutedTextColor(), "%s", hdm::Tr("Установлено:", "Встановлено:", "Installed:"));
            ImGui::TextUnformatted(EllipsizeUtf8(instVer.empty() ? "—" : instVer, versionBlockW).c_str());
        }
        ImGui::EndGroup();
        PopFontSafe(g_fontSmall);

        // Far-Right: Clean Action Buttons (vertically centered together inside a group)
        const float btnOffsetY = std::round((cardH - ImGui::GetFrameHeight()) * 0.5f);
        ImGui::SetCursorPos(ImVec2(rightStartX, btnOffsetY));
        ImGui::BeginGroup();

        if (canInstallDirectly) {
            const char* btnLbl =
                !item.installed
                    ? hdm::Tr("Установить###CAct", "Встановити###CAct", "Install###CAct")
                    : (item.updateAvailable
                           ? hdm::Tr("Обновить###CAct", "Оновити###CAct", "Update###CAct")
                           : hdm::Tr("Переуст.###CAct", "Перевст.###CAct", "Reinstall###CAct"));
            ImGui::BeginDisabled(busy);
            if (RenderActionButton(btnLbl, ImVec2(primaryW, 0.0f), needAction)) {
                componentsScanner_.DownloadAndInstallAsync(item, devicesCache_, MakeLogger());
                selectedComponentIds_.erase(item.id);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (RenderSecondaryButton(hdm::Tr("Сайт###CSite", "Сайт###CSite", "Site###CSite"), ImVec2(siteW, 0.0f))) {
                OpenUrlInBrowser(item.officialPageUrl);
            }
            if (g_isAdvancedMode) {
                ImGui::SameLine();
                ImGui::BeginDisabled(busy);
                if (ImGui::Checkbox("##compSel", &isSelected)) {
                    if (isSelected) selectedComponentIds_.insert(item.id);
                    else selectedComponentIds_.erase(item.id);
                }
                ImGui::EndDisabled();
            }
        } else if (!item.officialPageUrl.empty()) {
            if (RenderSecondaryButton(
                    hdm::Tr("Открыть сайт###COpenSite", "Відкрити сайт###COpenSite", "Open Website###COpenSite"),
                    ImVec2(openSiteW, 0.0f))) {
                OpenUrlInBrowser(item.officialPageUrl);
            }
        }
        ImGui::EndGroup();

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopID();

        if (viewIdx + 1 < visibleComponentIndices_.size()) {
            ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));
        }
    }

    ImGui::EndChild();
}
