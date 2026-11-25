#include "ui/DriverLoomApp.hpp"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <sstream>

void DriverLoomApp::RenderDiagnosticsTab() {
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.0f);
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool busy = diagnostics_.IsBusy();
    const bool isAppForeground = (GetForegroundWindow() == hwnd_);

    const bool showServicesTab = g_isAdvancedMode || (diagCache_.valid && diagCache_.disabledCriticalServices > 0);
    if (!showServicesTab && diagSectionIndex_ == 1) {
        diagSectionIndex_ = 0;
    }
    if (!g_isAdvancedMode && diagSectionIndex_ == 4) {
        diagSectionIndex_ = 0;
    }

    RenderPageHeader(
        hdm::Tr("Защита и система", "Захист і система", "System & Protection"),
        hdm::Tr(
            "Драйверы материнской платы, точки восстановления, резервное копирование и режим питания ПК.",
            "Драйвери материнської плати, точки відновлення, резервне копіювання та режим живлення ПК.",
            "Motherboard drivers, System Restore points, driver backups, and Windows power plan."));

    // Top Action Row
    ImGui::BeginDisabled(busy);
    if (RenderSecondaryButton(hdm::Tr("Обновить###DiagRef", "Оновити###DiagRef", "Refresh###DiagRef"))) {
        diagnostics_.StartScanAsync(devicesCache_, MakeLogger());
    }
    ImGui::EndDisabled();

    {
        const bool backupBusy =
            localInstaller_.IsPickingFolder() || localInstaller_.IsExportingBackup();
        SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Бэкап драйверов...", "Бекап драйверів...", "Backup Drivers...")));
        ImGui::BeginDisabled(backupBusy);
        if (RenderSecondaryButton(hdm::Tr("Бэкап драйверов...###DiagBackupExp", "Бекап драйверів...###DiagBackupExp", "Backup Drivers...###DiagBackupExp"))) {
            localInstaller_.PickFolderAsync(
                hwnd_,
                MakeLogger(),
                [this](bool picked, const std::wstring& path) {
                    if (picked && !path.empty()) {
                        localInstaller_.ExportDriverStoreBackupAsync(path, true, MakeLogger(), nullptr);
                    }
                });
        }
        ImGui::EndDisabled();
        if (isAppForeground &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "%s",
                hdm::Tr(
                    "Сохранить резервную копию всех установленных драйверов в выбранную папку (удобно перед переустановкой Windows).",
                    "Зберегти резервну копію всіх встановлених драйверів у вибрану папку (зручно перед перевстановленням Windows).",
                    "Export a backup copy of all installed third-party drivers to a folder (handy before reinstalling Windows)."));
        }
    }

    const bool canExport = diagCache_.valid && !diagnostics_.IsExportingReport();
    SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Сохранить отчёт...", "Зберегти звіт...", "Save Report...")));
    ImGui::BeginDisabled(!canExport);
    if (RenderSecondaryButton(hdm::Tr("Сохранить отчёт...###DiagSaveRep", "Зберегти звіт...###DiagSaveRep", "Save Report...###DiagSaveRep"))) {
        diagnostics_.ExportReportInteractiveAsync(
            hwnd_, diagCache_, devicesCache_, componentsCache_, MakeLogger());
    }
    ImGui::EndDisabled();

    if (g_isAdvancedMode) {
        SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Копировать отчёт", "Копіювати звіт", "Copy Report")));
        ImGui::BeginDisabled(!diagCache_.valid);
        if (RenderSecondaryButton(hdm::Tr("Копировать отчёт###DiagCopyRep", "Копіювати звіт###DiagCopyRep", "Copy Report###DiagCopyRep"))) {
            const std::string mdReport = hdm::SystemDiagnostics::BuildCleanSystemReport(
                hdm::ReportFormat::Markdown, diagCache_, devicesCache_, componentsCache_);
            ImGui::SetClipboardText(mdReport.c_str());
            logBuffer_.Push(
                hdm::LogLevel::Success,
                hdm::Tr("Отчёт о системе скопирован в буфер обмена.", "Звіт про систему скопійовано до буфера обміну.", "System report copied to clipboard."));
        }
        ImGui::EndDisabled();

        SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Очистить кэш WU", "Очистити кеш WU", "Reset WU Cache")));
        ImGui::BeginDisabled(busy);
        if (RenderSecondaryButton(hdm::Tr("Очистить кэш WU###DiagWuReset", "Очистити кеш WU###DiagWuReset", "Reset WU Cache###DiagWuReset"))) {
            diagnostics_.RunMaintenanceAsync(hdm::MaintenanceAction::ResetWuDownloadCache, "", devicesCache_, MakeLogger());
        }
        ImGui::EndDisabled();
    }

    const bool compInstalling = componentsScanner_.IsInstalling();
    if (diagnostics_.IsRunningAction()) {
        SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Отмена", "Скасувати", "Cancel")));
        if (RenderSecondaryButton(hdm::Tr("Отмена###DiagCancel", "Скасувати###DiagCancel", "Cancel###DiagCancel"))) {
            diagnostics_.RequestCancel();
        }
        const std::string phase = hdm::TrDyn(diagnostics_.ActionPhase());
        if (!phase.empty()) {
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(WarningTextColor(), "%s", phase.c_str());
        }
    } else if (compInstalling) {
        SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Отмена", "Скасувати", "Cancel")));
        if (RenderSecondaryButton(hdm::Tr("Отмена###DiagCompCancel", "Скасувати###DiagCompCancel", "Cancel###DiagCompCancel"))) {
            componentsScanner_.RequestCancelInstall();
        }
        const std::string phase = hdm::TrDyn(componentsScanner_.PhaseMessage());
        if (!phase.empty()) {
            ImGui::SameLine(0.0f, 12.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(WarningTextColor(), "%s", phase.c_str());
        }
    }

    if (localInstaller_.IsExportingBackup()) {
        const float prog = localInstaller_.Progress();
        const std::string statusMsg = hdm::TrDyn(localInstaller_.StatusMessage());
        char pctBuf[16];
        std::snprintf(pctBuf, sizeof(pctBuf), "%.0f%%", prog * 100.0f);
        const std::string overlay = std::string(pctBuf) + " — " + statusMsg;
        ImGui::ProgressBar(prog, ImVec2(-1.0f, 18.0f * uiScale), overlay.c_str());
    }

    if (!diagCache_.valid) {
        ImGui::Dummy(ImVec2(0.0f, std::round(12.0f * uiScale)));
        ImGui::TextDisabled(
            "%s",
            hdm::Tr(
                "Собираем сведения о материнской плате, защите системы и драйверах...",
                "Збираємо відомості про материнську плату, захист системи та драйвери...",
                "Collecting motherboard, protection, and driver diagnostics..."));
        return;
    }

    ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));

    // Compact 2-column summary strip (~104px high, leaves generous height for tables below!)
    const float cardPadX = std::round(13.0f * uiScale);
    const float cardPadY = std::round(9.0f * uiScale);
    const float cardSpacingY = std::round(4.0f * uiScale);
    const float cardH = std::round(108.0f * uiScale);
    const float cardGap = std::round(10.0f * uiScale);
    const float cardW = std::max(240.0f, (ImGui::GetContentRegionAvail().x - cardGap) * 0.5f);

    // Left Card: Motherboard & BIOS
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(cardPadX, cardPadY));
    ImGui::PushStyleColor(ImGuiCol_Border, PanelBorderColor());
    ImGui::BeginChild(
        "##MoboDiagCard",
        ImVec2(cardW, cardH),
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, cardSpacingY));
    {
        const auto& mb = diagCache_.motherboard;
        std::string boardTitle;
        if (!mb.manufacturer.empty() && mb.manufacturer != "—") boardTitle = mb.manufacturer;
        else if (!mb.systemManufacturer.empty() && mb.systemManufacturer != "—") boardTitle = mb.systemManufacturer;
        if (!mb.product.empty() && mb.product != "—") {
            if (!boardTitle.empty()) boardTitle += " ";
            boardTitle += mb.product;
        } else if (!mb.systemProduct.empty() && mb.systemProduct != "—") {
            if (!boardTitle.empty()) boardTitle += " ";
            boardTitle += mb.systemProduct;
        }
        if (boardTitle.empty()) {
            boardTitle = hdm::Tr("Модель платы не определена", "Модель плати не визначено", "Board model unknown");
        }

        const std::string biosInfo =
            "BIOS " + (mb.biosVersion.empty() ? "—" : mb.biosVersion) +
            (!mb.biosDate.empty() ? (" (" + mb.biosDate + ")") : "");

        RenderSecondaryCardHeader(
            hdm::Tr("Материнская плата", "Материнська плата", "Motherboard"),
            biosInfo.c_str());

        if (RenderCellText(boardTitle) && isAppForeground &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", boardTitle.c_str());
        }

        const float btnsY = cardH - cardPadY - ImGui::GetFrameHeight();
        if (ImGui::GetCursorPosY() < btnsY) {
            ImGui::SetCursorPosY(btnsY);
        }
        if (!mb.supportUrl.empty()) {
            if (RenderSecondaryButton(hdm::Tr("Сайт платы###MbSite", "Сайт плати###MbSite", "Board Website###MbSite"))) {
                OpenUrlInBrowser(mb.supportUrl);
            }
            ImGui::SameLine();
        }
        if (RenderSecondaryButton(hdm::Tr("Найти драйверы###MbSearch", "Знайти драйвери###MbSearch", "Find Drivers###MbSearch"))) {
            OpenUrlInBrowser(
                "https://www.google.com/search?q=" + UrlEncode(boardTitle + " chipset lan audio drivers Windows"));
        }
        if (g_isAdvancedMode) {
            ImGui::SameLine();
            if (RenderSecondaryButton(hdm::Tr("Копировать###MbCopy", "Копіювати###MbCopy", "Copy###MbCopy"))) {
                const std::string copyText = boardTitle + " | " + biosInfo;
                ImGui::SetClipboardText(copyText.c_str());
            }
        }
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::SameLine(0.0f, cardGap);

    // Right Card: Windows Protection & Readiness
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(cardPadX, cardPadY));
    ImGui::PushStyleColor(ImGuiCol_Border, PanelBorderColor());
    ImGui::BeginChild(
        "##ReadinessDiagCard",
        ImVec2(cardW, cardH),
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, cardSpacingY));
    {
        const auto& rd = diagCache_.readiness;
        RenderSecondaryCardHeader(
            hdm::Tr("Защита и готовность Windows", "Захист і готовність Windows", "Windows Readiness & Protection"));

        PushFontSafe(g_fontSmall);
        if (rd.systemRestorePolicyDisabled) {
            RenderStatusBadge(DangerColor(), hdm::Tr("Точки отката запрещены политикой", "Точки відкату заборонено політикою", "Restore Points blocked by policy"));
        } else if (rd.systemRestoreEnabled) {
            RenderStatusBadge(HealthyColor(), hdm::Tr("Защита системы включена (точки отката активны)", "Захист системи увімкнено (точки відкату активні)", "System Protection enabled (Restore Points active)"));
        } else {
            RenderStatusBadge(WarningColor(), hdm::Tr("Защита диска выключена — точки отката не создаются", "Захист диска вимкнено — точки відкату не створюються", "System Protection off — restore points may fail"));
        }

        if (diagCache_.disabledCriticalServices > 0) {
            char svcWarn[96];
            std::snprintf(svcWarn, sizeof(svcWarn), hdm::Tr("Отключено служб обновлений: %zu", "Вимкнено служб оновлень: %zu", "Disabled update services: %zu"), diagCache_.disabledCriticalServices);
            RenderStatusBadge(DangerColor(), svcWarn);
        } else if (rd.cbsRebootPending || rd.wuRebootRequired) {
            RenderStatusBadge(WarningColor(), hdm::Tr("Ожидается перезагрузка Windows", "Очікується перезавантаження Windows", "Windows restart pending"));
        } else {
            RenderStatusBadge(HealthyColor(), hdm::Tr("Службы обновлений работают, перезагрузка не требуется", "Служби оновлень працюють, перезавантаження не потрібне", "Update services healthy, no reboot pending"));
        }
        PopFontSafe(g_fontSmall);
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));

    // Sub-section Switcher Tabs
    char ctrlPill[96];
    std::snprintf(
        ctrlPill,
        sizeof(ctrlPill),
        hdm::Tr("Драйверы платы (%zu)###CtrlPill", "Драйвери плати (%zu)###CtrlPill", "Board Drivers (%zu)###CtrlPill"),
        diagCache_.controllers.size());
    if (RenderSubSectionHeaderTab(ctrlPill, diagSectionIndex_ == 0)) {
        diagSectionIndex_ = 0;
    }

    char rpPill[96];
    std::snprintf(
        rpPill,
        sizeof(rpPill),
        hdm::Tr("Точки отката (%zu)###RpPill", "Точки відкату (%zu)###RpPill", "Restore Points (%zu)###RpPill"),
        diagCache_.restorePoints.size());
    SameLineWrapIfNeeded(CalcSubSectionTabWidth(rpPill));
    if (RenderSubSectionHeaderTab(rpPill, diagSectionIndex_ == 2)) {
        diagSectionIndex_ = 2;
    }

    char storPill[96];
    std::snprintf(
        storPill,
        sizeof(storPill),
        hdm::Tr("Диски и питание (%zu)###StorPill", "Диски та живлення (%zu)###StorPill", "Storage & Power (%zu)###StorPill"),
        diagCache_.storageItems.size());
    SameLineWrapIfNeeded(CalcSubSectionTabWidth(storPill));
    if (RenderSubSectionHeaderTab(storPill, diagSectionIndex_ == 3)) {
        diagSectionIndex_ = 3;
    }

    if (showServicesTab) {
        char svcPill[128];
        if (diagCache_.disabledCriticalServices > 0) {
            std::snprintf(svcPill, sizeof(svcPill), hdm::Tr("Службы (%zu откл.!)###SvcPill", "Служби (%zu вимкн.!)###SvcPill", "Services (%zu off!)###SvcPill"), diagCache_.disabledCriticalServices);
        } else {
            std::snprintf(svcPill, sizeof(svcPill), hdm::Tr("Службы (%zu)###SvcPill", "Служби (%zu)###SvcPill", "Services (%zu)###SvcPill"), diagCache_.services.size());
        }
        SameLineWrapIfNeeded(CalcSubSectionTabWidth(svcPill));
        if (RenderSubSectionHeaderTab(svcPill, diagSectionIndex_ == 1)) {
            diagSectionIndex_ = 1;
        }
    }

    if (g_isAdvancedMode) {
        if (lastSnapshotLang_ != hdm::GetLang() && (!baselineEntries_.empty() || !devicesCache_.empty())) {
            RebuildSnapshotDiff();
        }
        char snapPill[96];
        std::snprintf(
            snapPill,
            sizeof(snapPill),
            hdm::Tr("До / После (%zu)###SnapPill", "До / Після (%zu)###SnapPill", "Before / After (%zu)###SnapPill"),
            snapshotChangedCount_);
        SameLineWrapIfNeeded(CalcSubSectionTabWidth(snapPill));
        if (RenderSubSectionHeaderTab(snapPill, diagSectionIndex_ == 4)) {
            diagSectionIndex_ = 4;
        }
    }

    ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));

    const bool showFooterBar =
        (diagSectionIndex_ == 2) || (diagSectionIndex_ == 3) ||
        (diagSectionIndex_ == 4 && g_isAdvancedMode) ||
        ((diagSectionIndex_ == 0 || diagSectionIndex_ == 1) && g_isAdvancedMode);

    const float footerPadY = std::round(8.0f * uiScale);
    const float footerH = showFooterBar ? (ImGui::GetFrameHeight() + footerPadY * 2.0f) : 0.0f;
    const float tableH = std::max(
        110.0f,
        ImGui::GetContentRegionAvail().y - (showFooterBar ? (footerH + style.ItemSpacing.y) : 0.0f));
    constexpr ImGuiTableFlags kDiagTableFlags =
        ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY;

    if (diagSectionIndex_ == 0) {
        const float instW = CalcButtonWidth(hdm::Tr("Установить", "Встановити", "Install")) + 10.0f * uiScale;
        const float catW = g_isAdvancedMode ? (CalcButtonWidth(hdm::Tr("Каталог", "Каталог", "Catalog")) + 10.0f * uiScale) : 0.0f;
        const float actionColW =
            instW + (g_isAdvancedMode ? (style.ItemSpacing.x + catW) : 0.0f) +
            style.CellPadding.x * 2.0f + style.ScrollbarSize + 10.0f * uiScale;

        if (ImGui::BeginTable(
                g_isAdvancedMode ? "##DiagControllersTableAdv" : "##DiagControllersTableStd",
                5,
                kDiagTableFlags,
                ImVec2(0.0f, tableH))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(hdm::Tr("Состояние", "Стан", "Status"), ImGuiTableColumnFlags_WidthStretch, 1.48f);
            ImGui::TableSetupColumn(hdm::Tr("Назначение", "Призначення", "Role"), ImGuiTableColumnFlags_WidthStretch, 0.72f);
            ImGui::TableSetupColumn(hdm::Tr("Устройство", "Пристрій", "Device"), ImGuiTableColumnFlags_WidthStretch, 1.24f);
            ImGui::TableSetupColumn(hdm::Tr("Драйвер / Дата", "Драйвер / Дата", "Driver / Date"), ImGuiTableColumnFlags_WidthStretch, 0.94f);
            ImGui::TableSetupColumn(hdm::Tr("Действие", "Дія", "Action"), ImGuiTableColumnFlags_WidthFixed, actionColW);
            RenderTableHeadersRow();

            for (size_t i = 0; i < diagCache_.controllers.size(); ++i) {
                const auto& item = diagCache_.controllers[i];
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(i));

                ImGui::TableSetColumnIndex(0);
                ImVec4 col = HealthyColor();
                if (item.health == hdm::ControllerHealth::Problem) col = DangerColor();
                else if (item.health == hdm::ControllerHealth::Attention) col = WarningColor();
                RenderStatusBadge(col, hdm::TrDyn(item.statusText).c_str(), true);

                ImGui::TableSetColumnIndex(1);
                RenderCellText(hdm::TrDyn(item.role), false, true);

                ImGui::TableSetColumnIndex(2);
                RenderCellText(item.deviceName, false, true);

                ImGui::TableSetColumnIndex(3);
                std::string verDate = item.driverVersion.empty() ? "—" : item.driverVersion;
                if (!item.driverDate.empty()) verDate += " (" + item.driverDate + ")";
                RenderCellText(verDate, false, true);

                ImGui::TableSetColumnIndex(4);
                const bool hasKey = !item.searchKey.empty() || !item.hardwareId.empty();
                const bool instPrimary = (item.health != hdm::ControllerHealth::Ok);
                ImGui::BeginDisabled(busy || compInstalling || !hasKey);
                if (RenderActionButton(
                        instPrimary
                            ? hdm::Tr("Обновить###CInst", "Оновити###CInst", "Update###CInst")
                            : hdm::Tr("Установить###CInst", "Встановити###CInst", "Install###CInst"),
                        ImVec2(instW, 0.0f),
                        instPrimary)) {
                    componentsScanner_.SearchAndInstallFromCatalogAsync(
                        item.deviceName,
                        item.searchKey,
                        item.hardwareId,
                        item.driverVersion,
                        devicesCache_,
                        MakeLogger(),
                        [this](bool ok, bool rebootReq) {
                            if (rebootReq) rebootRequired_.store(true);
                            if (ok) pendingDeviceRescan_.store(true);
                        });
                }
                ImGui::EndDisabled();
                if (g_isAdvancedMode) {
                    ImGui::SameLine();
                    ImGui::BeginDisabled(item.searchKey.empty());
                    if (RenderSecondaryButton(hdm::Tr("Каталог###CCat", "Каталог###CCat", "Catalog###CCat"), ImVec2(catW, 0.0f))) {
                        OpenUrlInBrowser("https://www.catalog.update.microsoft.com/Search.aspx?q=" + UrlEncode(item.searchKey));
                    }
                    ImGui::EndDisabled();
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    } else if (diagSectionIndex_ == 1) {
        const char* enSvcTxt = hdm::Tr("Включить", "Увімкнути", "Enable");
        const float svcBtnW = CalcButtonWidth(enSvcTxt) + 12.0f * uiScale;
        const float svcActionW = svcBtnW + style.CellPadding.x * 2.0f + style.ScrollbarSize + 8.0f * uiScale;
        if (ImGui::BeginTable("##DiagServicesTable", 5, kDiagTableFlags, ImVec2(0.0f, tableH))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(hdm::Tr("Статус", "Статус", "Status"), ImGuiTableColumnFlags_WidthStretch, 1.15f);
            ImGui::TableSetupColumn(hdm::Tr("Служба", "Служба", "Service"), ImGuiTableColumnFlags_WidthStretch, 1.12f);
            ImGui::TableSetupColumn(hdm::Tr("Назначение", "Призначення", "Purpose"), ImGuiTableColumnFlags_WidthStretch, 1.20f);
            ImGui::TableSetupColumn(hdm::Tr("Запуск", "Запуск", "Startup"), ImGuiTableColumnFlags_WidthStretch, 0.60f);
            ImGui::TableSetupColumn(hdm::Tr("Действие", "Дія", "Action"), ImGuiTableColumnFlags_WidthFixed, svcActionW);
            RenderTableHeadersRow();

            for (size_t i = 0; i < diagCache_.services.size(); ++i) {
                const auto& svc = diagCache_.services[i];
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(i));

                ImGui::TableSetColumnIndex(0);
                ImVec4 col = HealthyColor();
                if (svc.health == hdm::ServiceHealthState::Disabled) col = DangerColor();
                else if (svc.health == hdm::ServiceHealthState::MissingOrError) col = WarningColor();
                RenderStatusBadge(col, hdm::TrDyn(svc.stateText).c_str(), true);

                ImGui::TableSetColumnIndex(1);
                const std::string svcLabel = svc.serviceName + " — " + hdm::TrDyn(svc.displayName);
                RenderCellText(svcLabel, false, true);

                ImGui::TableSetColumnIndex(2);
                RenderCellText(hdm::TrDyn(svc.purpose), true, true);

                ImGui::TableSetColumnIndex(3);
                RenderCellText(hdm::TrDyn(svc.startTypeText), false, true);

                ImGui::TableSetColumnIndex(4);
                if (svc.health == hdm::ServiceHealthState::Disabled) {
                    ImGui::BeginDisabled(busy);
                    if (RenderPrimaryButton(hdm::Tr("Включить###SvcEn", "Увімкнути###SvcEn", "Enable###SvcEn"), ImVec2(svcBtnW, 0.0f))) {
                        diagnostics_.RunMaintenanceAsync(hdm::MaintenanceAction::EnableService, svc.serviceName, devicesCache_, MakeLogger());
                    }
                    ImGui::EndDisabled();
                } else {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", hdm::Tr("ОК", "ОК", "OK"));
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    } else if (diagSectionIndex_ == 2) {
        if (ImGui::BeginTable("##DiagRestorePointsTable", 4, kDiagTableFlags, ImVec2(0.0f, tableH))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(hdm::Tr("Тип", "Тип", "Type"), ImGuiTableColumnFlags_WidthStretch, 0.85f);
            ImGui::TableSetupColumn(hdm::Tr("Дата и время", "Дата і час", "Date & Time"), ImGuiTableColumnFlags_WidthStretch, 0.72f);
            ImGui::TableSetupColumn(hdm::Tr("Описание точки восстановления", "Опис точки відновлення", "Restore Point Description"), ImGuiTableColumnFlags_WidthStretch, 1.85f);
            ImGui::TableSetupColumn(hdm::Tr("Источник", "Джерело", "Source"), ImGuiTableColumnFlags_WidthStretch, 0.88f);
            RenderTableHeadersRow();

            for (size_t i = 0; i < diagCache_.restorePoints.size(); ++i) {
                const auto& rp = diagCache_.restorePoints[i];
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(i));

                ImGui::TableSetColumnIndex(0);
                ImVec4 col = MutedTextColor();
                if (rp.isDriverLoom) col = HealthyColor();
                else if (rp.categoryLabel == "Windows Update") col = InfoAccentColor();
                RenderStatusBadge(col, hdm::TrDyn(rp.categoryLabel).c_str(), true);

                ImGui::TableSetColumnIndex(1);
                RenderCellText(rp.timestamp, false, true);

                ImGui::TableSetColumnIndex(2);
                RenderCellText(rp.description, false, true);

                ImGui::TableSetColumnIndex(3);
                RenderCellText(rp.sourceProcess, true, true);

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    } else if (diagSectionIndex_ == 3) {
        const float instW = CalcButtonWidth(hdm::Tr("Установить", "Встановити", "Install")) + 10.0f * uiScale;
        const float actionColW = instW + style.CellPadding.x * 2.0f + style.ScrollbarSize + 8.0f * uiScale;

        if (ImGui::BeginTable("##DiagStorageTable", 5, kDiagTableFlags, ImVec2(0.0f, tableH))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(hdm::Tr("Состояние", "Стан", "Status"), ImGuiTableColumnFlags_WidthStretch, 1.24f);
            ImGui::TableSetupColumn(hdm::Tr("Тип", "Тип", "Type"), ImGuiTableColumnFlags_WidthStretch, 0.82f);
            ImGui::TableSetupColumn(hdm::Tr("Накопитель / Контроллер", "Накопичувач / Контролер", "Drive / Controller"), ImGuiTableColumnFlags_WidthStretch, 1.35f);
            ImGui::TableSetupColumn(hdm::Tr("Драйвер", "Драйвер", "Driver"), ImGuiTableColumnFlags_WidthStretch, 0.90f);
            ImGui::TableSetupColumn(hdm::Tr("Действие", "Дія", "Action"), ImGuiTableColumnFlags_WidthFixed, actionColW);
            RenderTableHeadersRow();

            for (size_t i = 0; i < diagCache_.storageItems.size(); ++i) {
                const auto& st = diagCache_.storageItems[i];
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(i));

                ImGui::TableSetColumnIndex(0);
                ImVec4 col = HealthyColor();
                if (st.health == hdm::ControllerHealth::Problem) col = DangerColor();
                else if (st.health == hdm::ControllerHealth::Attention) col = WarningColor();
                RenderStatusBadge(col, hdm::TrDyn(st.statusText).c_str(), true);

                ImGui::TableSetColumnIndex(1);
                RenderCellText(hdm::TrDyn(st.role), false, true);

                ImGui::TableSetColumnIndex(2);
                RenderCellText(st.deviceName, false, true);

                ImGui::TableSetColumnIndex(3);
                std::string verInf = st.driverVersion.empty() ? "—" : st.driverVersion;
                if (g_isAdvancedMode && !st.infName.empty()) verInf += " (" + st.infName + ")";
                RenderCellText(verInf, false, true);

                ImGui::TableSetColumnIndex(4);
                if (st.isController) {
                    const bool hasKey = !st.searchKey.empty() || !st.hardwareId.empty();
                    const bool instPrimary = (st.health != hdm::ControllerHealth::Ok);
                    ImGui::BeginDisabled(busy || compInstalling || !hasKey);
                    if (RenderActionButton(hdm::Tr("Обновить###StInst", "Оновити###StInst", "Update###StInst"), ImVec2(instW, 0.0f), instPrimary)) {
                        componentsScanner_.SearchAndInstallFromCatalogAsync(
                            st.deviceName, st.searchKey, st.hardwareId, st.driverVersion, devicesCache_, MakeLogger(),
                            [this](bool ok, bool rebootReq) {
                                if (rebootReq) rebootRequired_.store(true);
                                if (ok) pendingDeviceRescan_.store(true);
                            });
                    }
                    ImGui::EndDisabled();
                } else {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", hdm::Tr("Системный", "Системний", "Inbox"));
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    } else {
        if (ImGui::BeginTable("##DiagSnapshotTable", 5, kDiagTableFlags, ImVec2(0.0f, tableH))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(hdm::Tr("Изменение", "Зміна", "Change"), ImGuiTableColumnFlags_WidthStretch, 0.95f);
            ImGui::TableSetupColumn(hdm::Tr("Устройство", "Пристрій", "Device"), ImGuiTableColumnFlags_WidthStretch, 1.35f);
            ImGui::TableSetupColumn(hdm::Tr("Класс", "Клас", "Class"), ImGuiTableColumnFlags_WidthStretch, 0.65f);
            ImGui::TableSetupColumn(hdm::Tr("Было (До)", "Було (До)", "Before"), ImGuiTableColumnFlags_WidthStretch, 1.30f);
            ImGui::TableSetupColumn(hdm::Tr("Стало (Сейчас)", "Стало (Зараз)", "After"), ImGuiTableColumnFlags_WidthStretch, 1.30f);
            RenderTableHeadersRow();

            if (!snapShowAll_ && snapshotChangedCount_ == 0) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                RenderStatusBadge(HealthyColor(), hdm::Tr("Без изменений", "Без змін", "No changes"), true);
                ImGui::TableSetColumnIndex(1);
                RenderCellText(
                    hdm::Tr("Изменений драйверов с момента снимка не обнаружено.", "Змін драйверів з моменту знімка не виявлено.", "No driver changes detected since baseline."),
                    true,
                    true);
            } else {
                std::vector<size_t> shownIndices;
                for (size_t i = 0; i < snapshotDiffRows_.size(); ++i) {
                    if (snapShowAll_ || snapshotDiffRows_[i].kind != SnapshotDiffKind::Unchanged) {
                        shownIndices.push_back(i);
                    }
                }
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(shownIndices.size()));
                while (clipper.Step()) {
                    for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                        const auto& row = snapshotDiffRows_[shownIndices[static_cast<size_t>(r)]];
                        ImGui::TableNextRow();
                        ImGui::PushID(r);

                        ImGui::TableSetColumnIndex(0);
                        ImVec4 col = MutedTextColor();
                        const char* badgeTxt = hdm::Tr("Без изменений", "Без змін", "Unchanged");
                        switch (row.kind) {
                        case SnapshotDiffKind::DriverUpdated: col = InfoAccentColor(); badgeTxt = hdm::Tr("Обновлён", "Оновлено", "Updated"); break;
                        case SnapshotDiffKind::IssueFixed: col = HealthyColor(); badgeTxt = hdm::Tr("Исправлено", "Виправлено", "Fixed"); break;
                        case SnapshotDiffKind::NewIssue: col = DangerColor(); badgeTxt = hdm::Tr("Ошибка", "Помилка", "New Issue"); break;
                        case SnapshotDiffKind::DeviceAdded: col = InfoAccentColor(); badgeTxt = hdm::Tr("Добавлено", "Додано", "Added"); break;
                        case SnapshotDiffKind::DeviceRemoved: col = WarningColor(); badgeTxt = hdm::Tr("Удалено", "Видалено", "Removed"); break;
                        default: break;
                        }
                        RenderStatusBadge(col, badgeTxt, true);

                        ImGui::TableSetColumnIndex(1);
                        RenderCellText(hdm::TrDyn(row.name), false, true);
                        ImGui::TableSetColumnIndex(2);
                        RenderCellText(row.deviceClass, row.deviceClass.empty(), true);
                        ImGui::TableSetColumnIndex(3);
                        RenderCellText(row.beforeSummary, true, true);
                        ImGui::TableSetColumnIndex(4);
                        RenderCellText(row.afterSummary, row.kind == SnapshotDiffKind::DeviceRemoved, true);

                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndTable();
        }
    }

    if (!showFooterBar) {
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(cardPadX, footerPadY));
    ImGui::PushStyleColor(ImGuiCol_Border, PanelBorderColor());
    ImGui::BeginChild(
        "##DiagFooterBar",
        ImVec2(0.0f, footerH),
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();

    if (diagSectionIndex_ == 3) {
        const char* trimTxt =
            diagCache_.readiness.trimEnabled
                ? hdm::Tr("SSD TRIM: Включён", "SSD TRIM: Увімкнено", "SSD TRIM: Enabled")
                : hdm::Tr("SSD TRIM: Отключён", "SSD TRIM: Вимкнено", "SSD TRIM: Disabled");
        const float trimW = ImGui::CalcTextSize(trimTxt).x;
        const float winRight = ImGui::GetWindowContentRegionMax().x;

        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", hdm::Tr("Режим питания:", "Режим живлення:", "Power Plan:"));
        ImGui::BeginDisabled(busy);
        for (const auto& ps : diagCache_.powerSchemes) {
            const std::string psShown = hdm::TrDyn(ps.friendlyName);
            const float btnW = CalcButtonWidth(psShown.c_str());
            const float lastEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
            if (lastEnd + style.ItemSpacing.x + btnW > winRight - trimW - 16.0f) {
                break;
            }
            ImGui::SameLine();
            ImGui::PushID(ps.guidString.c_str());
            const std::string psBtnLbl = psShown + "###PsBtn";
            if (RenderPillButton(psBtnLbl.c_str(), ps.isActive) && !ps.isActive) {
                diagnostics_.SetActivePowerSchemeAsync(ps.guidString, MakeLogger());
            }
            ImGui::PopID();
        }
        ImGui::EndDisabled();

        const float lastEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        if (winRight - trimW > lastEnd + 16.0f) {
            ImGui::SameLine(winRight - trimW);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", trimTxt);
        }
    } else if (diagSectionIndex_ == 2) {
        auto launchSystem32Tool = [&](const wchar_t* exeName) {
            wchar_t sysDir[MAX_PATH]{};
            GetSystemDirectoryW(sysDir, MAX_PATH);
            const std::filesystem::path fullPath = std::filesystem::path(sysDir) / exeName;
            ShellExecuteW(hwnd_, L"open", fullPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        };

        ImGui::BeginDisabled(systemRestore_.IsBusy());
        if (RenderPrimaryButton(hdm::Tr("Создать точку отката###FtRpCreate", "Створити точку відкату###FtRpCreate", "Create Restore Point###FtRpCreate"))) {
            systemRestore_.CreateRestorePointAsync(
                hdm::Tr("DriverLoom (ручная точка восстановления)", "DriverLoom (ручна точка відновлення)", "DriverLoom (manual restore point)"),
                MakeLogger(),
                [this](bool ok, const std::string&) {
                    if (ok && !diagnostics_.IsBusy()) diagnostics_.StartScanAsync(devicesCache_, nullptr);
                });
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (RenderSecondaryButton(hdm::Tr("Откатить Windows...###FtRstrui", "Відкотити Windows...###FtRstrui", "Open System Restore###FtRstrui"))) {
            launchSystem32Tool(L"rstrui.exe");
        }
        ImGui::SameLine();
        if (RenderSecondaryButton(hdm::Tr("Настроить защиту диска###FtProt", "Налаштувати захист диска###FtProt", "Protection Settings###FtProt"))) {
            launchSystem32Tool(L"SystemPropertiesProtection.exe");
        }
    } else if (diagSectionIndex_ == 4) {
        ImGui::BeginDisabled(devicesCache_.empty());
        if (RenderPrimaryButton(hdm::Tr("Обновить снимок «До»###SnapSaveBtn", "Оновити знімок «До»###SnapSaveBtn", "Capture New Baseline###SnapSaveBtn"))) {
            CaptureCurrentAsBaseline(true);
        }
        ImGui::EndDisabled();

        char chgLbl[64], allLbl[64];
        std::snprintf(chgLbl, sizeof(chgLbl), hdm::Tr("Изменения (%zu)###SnapFiltChg", "Зміни (%zu)###SnapFiltChg", "Changes (%zu)###SnapFiltChg"), snapshotChangedCount_);
        std::snprintf(allLbl, sizeof(allLbl), hdm::Tr("Все (%zu)###SnapFiltAll", "Усі (%zu)###SnapFiltAll", "All (%zu)###SnapFiltAll"), snapshotDiffRows_.size());
        ImGui::SameLine();
        if (RenderPillButton(chgLbl, !snapShowAll_)) snapShowAll_ = false;
        ImGui::SameLine();
        if (RenderPillButton(allLbl, snapShowAll_)) snapShowAll_ = true;

        ImGui::SameLine();
        ImGui::BeginDisabled(snapshotChangedCount_ == 0);
        if (RenderSecondaryButton(hdm::Tr("Копировать###SnapCopyBtn", "Копіювати###SnapCopyBtn", "Copy###SnapCopyBtn"))) {
            std::ostringstream oss;
            oss << "## DriverLoom Before / After (" << baselineTimestamp_ << ")\n";
            for (const auto& r : snapshotDiffRows_) {
                if (r.kind == SnapshotDiffKind::Unchanged) continue;
                oss << "- **" << hdm::TrDyn(r.name) << "** (" << r.deviceClass << "): `"
                    << r.beforeSummary << "` -> `" << r.afterSummary << "`\n";
            }
            ImGui::SetClipboardText(oss.str().c_str());
        }
        ImGui::EndDisabled();
    } else {
        // Expert-only System File Check dock
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", hdm::Tr("Проверка системных файлов:", "Перевірка системних файлів:", "System File Check:"));
        ImGui::BeginDisabled(busy);
        ImGui::SameLine(0.0f, 10.0f);
        if (RenderSecondaryButton("SFC /scannow")) {
            diagnostics_.RunMaintenanceAsync(hdm::MaintenanceAction::SfcScan, "", devicesCache_, MakeLogger());
        }
        ImGui::SameLine();
        if (RenderSecondaryButton("DISM CheckHealth")) {
            diagnostics_.RunMaintenanceAsync(hdm::MaintenanceAction::DismCheckHealth, "", devicesCache_, MakeLogger());
        }
        ImGui::SameLine();
        if (RenderSecondaryButton("DISM RestoreHealth")) {
            diagnostics_.RunMaintenanceAsync(hdm::MaintenanceAction::DismRestoreHealth, "", devicesCache_, MakeLogger());
        }
        ImGui::EndDisabled();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
}
