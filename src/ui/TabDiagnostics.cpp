#include "ui/DriverLoomApp.hpp"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <sstream>

void DriverLoomApp::RenderDiagnosticsTab() {
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool busy = diagnostics_.IsBusy();
    const bool isAppForeground = (GetForegroundWindow() == hwnd_);

    ImGui::BeginDisabled(busy);
    if (ImGui::Button(hdm::Tr(
            "Обновить диагностику###DiagRef",
            "Оновити діагностику###DiagRef",
            "Refresh Diagnostics###DiagRef"))) {
        diagnostics_.StartScanAsync(devicesCache_, MakeLogger());
    }
    ImGui::EndDisabled();

    if (g_isAdvancedMode) {
        SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Очистить кэш WU", "Очистити кеш WU", "Reset WU Cache")));
        ImGui::BeginDisabled(busy);
        if (RenderSecondaryButton(hdm::Tr(
                "Очистить кэш WU###DiagWuReset",
                "Очистити кеш WU###DiagWuReset",
                "Reset WU Cache###DiagWuReset"))) {
            diagnostics_.RunMaintenanceAsync(
                hdm::MaintenanceAction::ResetWuDownloadCache,
                "",
                devicesCache_,
                MakeLogger());
        }
        ImGui::EndDisabled();
        if (isAppForeground &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "%s",
                hdm::Tr(
                    "Останавливает wuauserv и BITS, очищает папку загрузок Windows Update "
                    "(SoftwareDistribution\\Download) и запускает службы заново.\n"
                    "Помогает, если скачивание обновлений зависло или повреждено.",
                    "Зупиняє wuauserv та BITS, очищує папку завантажень Windows Update "
                    "(SoftwareDistribution\\Download) і запускає служби знову.\n"
                    "Допомагає, якщо завантаження оновлень зависло або пошкоджено.",
                    "Stops wuauserv and BITS, clears the Windows Update download cache "
                    "(SoftwareDistribution\\Download), and restarts both services.\n"
                    "Fixes stuck or corrupted Windows Update downloads."));
        }
    } else {
        const bool backupBusy =
            localInstaller_.IsPickingFolder() || localInstaller_.IsExportingBackup();
        SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr(
            "Экспорт бэкапа драйверов...",
            "Експорт бекапу драйверів...",
            "Export Driver Backup...")));
        ImGui::BeginDisabled(backupBusy);
        if (RenderSecondaryButton(hdm::Tr(
                "Экспорт бэкапа драйверов...###DiagBackupExp",
                "Експорт бекапу драйверів...###DiagBackupExp",
                "Export Driver Backup...###DiagBackupExp"))) {
            localInstaller_.PickFolderAsync(
                hwnd_,
                MakeLogger(),
                [this](bool picked, const std::wstring& path) {
                    if (picked && !path.empty()) {
                        localInstaller_.ExportDriverStoreBackupAsync(
                            path, true, MakeLogger(), nullptr);
                    }
                });
        }
        ImGui::EndDisabled();
        if (isAppForeground &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "%s",
                hdm::Tr(
                    "Сохраняет резервную копию установленных сторонних драйверов в выбранную папку (без старых дубликатов).",
                    "Зберігає резервну копію встановлених сторонніх драйверів у вибрану папку (без старих дублікатів).",
                    "Exports a backup copy of installed third-party drivers to the chosen folder (excluding old duplicates)."));
        }
    }

    const bool canExport = diagCache_.valid && !diagnostics_.IsExportingReport();
    SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Сохранить отчёт...", "Зберегти звіт...", "Save Report...")));
    ImGui::BeginDisabled(!canExport);
    if (RenderSecondaryButton(hdm::Tr(
            "Сохранить отчёт...###DiagSaveRep",
            "Зберегти звіт...###DiagSaveRep",
            "Save Report...###DiagSaveRep"))) {
        diagnostics_.ExportReportInteractiveAsync(
            hwnd_,
            diagCache_,
            devicesCache_,
            componentsCache_,
            MakeLogger());
    }
    ImGui::EndDisabled();
    if (isAppForeground &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            "%s",
            hdm::Tr(
                "Сохранить сводный отчёт о плате, BIOS, драйверах, проблемных устройствах, библиотеках и службах\n"
                "в формате Markdown (.md), TXT или JSON (без имени пользователя и личных данных).",
                "Зберегти зведений звіт про плату, BIOS, драйвери, проблемні пристрої, бібліотеки та служби\n"
                "у форматі Markdown (.md), TXT або JSON (без імені користувача та особистих даних).",
                "Save a summary report on motherboard, BIOS, drivers, issue devices, runtimes, and services\n"
                "in Markdown (.md), TXT, or JSON (privacy-clean, without usernames or personal paths)."));
    }

    SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Копировать отчёт", "Копіювати звіт", "Copy Report")));
    ImGui::BeginDisabled(!diagCache_.valid);
    if (RenderSecondaryButton(hdm::Tr(
            "Копировать отчёт###DiagCopyRep",
            "Копіювати звіт###DiagCopyRep",
            "Copy Report###DiagCopyRep"))) {
        const std::string mdReport = hdm::SystemDiagnostics::BuildCleanSystemReport(
            hdm::ReportFormat::Markdown,
            diagCache_,
            devicesCache_,
            componentsCache_);
        ImGui::SetClipboardText(mdReport.c_str());
        logBuffer_.Push(
            hdm::LogLevel::Success,
            hdm::Tr(
                "Отчёт о системе (Markdown) скопирован в буфер обмена.",
                "Звіт про систему (Markdown) скопійовано до буфера обміну.",
                "System report (Markdown) copied to clipboard."));
    }
    ImGui::EndDisabled();

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
    } else if (diagCache_.valid) {
        ImGui::SameLine(0.0f, 12.0f);
        if (diagCache_.disabledCriticalServices > 0) {
            char msg[96];
            std::snprintf(
                msg,
                sizeof(msg),
                hdm::Tr(
                    "Отключено критичных служб: %zu",
                    "Вимкнено критичних служб: %zu",
                    "Disabled critical services: %zu"),
                diagCache_.disabledCriticalServices);
            RenderStatusBadge(ImVec4(0.97f, 0.44f, 0.44f, 1.00f), msg, true);
        } else if (diagCache_.attentionControllers > 0) {
            char msg[96];
            std::snprintf(
                msg,
                sizeof(msg),
                hdm::Tr(
                    "Контроллеры требуют внимания: %zu",
                    "Контролери потребують уваги: %zu",
                    "Controllers need attention: %zu"),
                diagCache_.attentionControllers);
            RenderStatusBadge(ImVec4(0.98f, 0.75f, 0.14f, 1.00f), msg, true);
        } else {
            RenderStatusBadge(
                ImVec4(0.20f, 0.83f, 0.60f, 1.00f),
                hdm::Tr(
                    "Системное окружение готово к обновлениям",
                    "Системне оточення готове до оновлень",
                    "System environment is ready for updates"),
                true);
        }
    }

    if (!g_isAdvancedMode && localInstaller_.IsExportingBackup()) {
        const float prog = localInstaller_.Progress();
        const std::string statusMsg = hdm::TrDyn(localInstaller_.StatusMessage());
        char overlay[128];
        std::snprintf(overlay, sizeof(overlay), "%.0f%% — %s", prog * 100.0f, statusMsg.c_str());
        ImGui::ProgressBar(prog, ImVec2(-1.0f, 18.0f * uiScale), overlay);
    }

    if (!diagCache_.valid) {
        ImGui::TextDisabled(
            "%s",
            hdm::Tr(
                "Выполняется сбор сведений о материнской плате, службах и драйверах...",
                "Виконується збір відомостей про материнську плату, служби та драйвери...",
                "Collecting motherboard, BIOS, service, and driver diagnostics..."));
        return;
    }

    // Top cards: Motherboard/BIOS & Windows Readiness (compact padding so table & footer fit cleanly)
    const float cardPadY = std::round(9.0f * uiScale);
    const float cardSpacingY = std::round(5.0f * uiScale);
    const float badgeH = ImGui::GetTextLineHeight() + std::round(6.0f * uiScale);
    const float headerFontH =
        (g_fontSecondaryHeader != nullptr) ? g_fontSecondaryHeader->LegacySize : ImGui::GetTextLineHeight();
    const float headerBannerH = headerFontH + std::round(8.0f * uiScale);
    const float cardH = std::max(
        134.0f * uiScale,
        headerBannerH + cardSpacingY * 2.0f +
            (badgeH + cardSpacingY) * 3.0f + cardPadY * 2.0f);
    const float cardGap = style.ItemSpacing.x;
    const float cardW = std::max(260.0f, (ImGui::GetContentRegionAvail().x - cardGap) * 0.5f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, cardPadY));
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
        RenderSecondaryCardHeader(
            hdm::Tr("Материнская плата и BIOS", "Материнська плата та BIOS", "Motherboard & BIOS"));

        std::string boardTitle;
        if (!mb.manufacturer.empty() && mb.manufacturer != "—") {
            boardTitle = mb.manufacturer;
        } else if (!mb.systemManufacturer.empty() && mb.systemManufacturer != "—") {
            boardTitle = mb.systemManufacturer;
        }
        if (!mb.product.empty() && mb.product != "—") {
            if (!boardTitle.empty()) boardTitle += " ";
            boardTitle += mb.product;
        } else if (!mb.systemProduct.empty() && mb.systemProduct != "—") {
            if (!boardTitle.empty()) boardTitle += " ";
            boardTitle += mb.systemProduct;
        }
        if (boardTitle.empty()) {
            boardTitle = hdm::Tr(
                "Модель не определена в SMBIOS",
                "Модель не визначено в SMBIOS",
                "Model not defined in SMBIOS");
        }

        ImGui::TextDisabled("%s", hdm::Tr("Плата:", "Плата:", "Board:"));
        ImGui::SameLine();
        if (RenderCellText(boardTitle) && isAppForeground &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("%s", boardTitle.c_str());
        }

        const std::string biosInfo =
            (mb.biosVersion.empty() ? "—" : mb.biosVersion) + "  (" +
            (mb.biosDate.empty() ? "—" : mb.biosDate) + ")";
        ImGui::TextDisabled("BIOS:");
        ImGui::SameLine();
        RenderCellText(biosInfo, true);

        const float btnsY = cardH - cardPadY - ImGui::GetFrameHeight();
        if (ImGui::GetCursorPosY() < btnsY) {
            ImGui::SetCursorPosY(btnsY);
        }
        if (!mb.supportUrl.empty()) {
            if (RenderSecondaryButton(hdm::Tr(
                    "Сайт платы###MbSite",
                    "Сайт плати###MbSite",
                    "Board Website###MbSite"))) {
                OpenUrlInBrowser(mb.supportUrl);
            }
            if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", mb.supportUrl.c_str());
            }
            ImGui::SameLine();
        }
        if (RenderSecondaryButton(hdm::Tr(
                "Поиск драйверов###MbSearch",
                "Пошук драйверів###MbSearch",
                "Find Drivers###MbSearch"))) {
            OpenUrlInBrowser(
                "https://www.google.com/search?q=" +
                UrlEncode(boardTitle + " chipset lan audio drivers Windows"));
        }
        ImGui::SameLine();
        if (RenderSecondaryButton(hdm::Tr(
                "Копировать###MbCopy",
                "Копіювати###MbCopy",
                "Copy###MbCopy"))) {
            const std::string copyText = boardTitle + " | BIOS: " + biosInfo;
            ImGui::SetClipboardText(copyText.c_str());
            logBuffer_.Push(
                hdm::LogLevel::Info,
                std::string(hdm::Tr("Скопировано в буфер: ", "Скопійовано до буфера: ", "Copied to clipboard: ")) +
                    copyText);
        }
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::SameLine(0.0f, cardGap);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, cardPadY));
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
            hdm::Tr(
                "Готовность Windows и защита",
                "Готовність Windows та захист",
                "Windows Readiness & Protection"));

        if (rd.cbsRebootPending || rd.wuRebootRequired) {
            RenderStatusBadge(
                ImVec4(0.98f, 0.75f, 0.14f, 1.00f),
                hdm::Tr(
                    "Ожидается перезагрузка Windows (WU / CBS)",
                    "Очікується перезавантаження Windows (WU / CBS)",
                    "Windows reboot pending (WU / CBS)"));
        } else if (rd.pendingFileRenames) {
            RenderStatusBadge(
                ImVec4(0.98f, 0.75f, 0.14f, 1.00f),
                hdm::Tr(
                    "Есть отложенные операции замены системных файлов",
                    "Є відкладені операції заміни системних файлів",
                    "Pending system file rename operations"));
        } else {
            RenderStatusBadge(
                ImVec4(0.20f, 0.83f, 0.60f, 1.00f),
                hdm::Tr(
                    "Перезагрузка не требуется",
                    "Перезавантаження не потрібне",
                    "No reboot required"));
        }

        if (rd.systemRestorePolicyDisabled) {
            RenderStatusBadge(
                ImVec4(0.97f, 0.44f, 0.44f, 1.00f),
                hdm::Tr(
                    "Точки восстановления запрещены политикой (DisableSR)",
                    "Точки відновлення заборонено політикою (DisableSR)",
                    "System Restore disabled by policy (DisableSR)"));
        } else if (rd.systemRestoreEnabled) {
            RenderStatusBadge(
                ImVec4(0.20f, 0.83f, 0.60f, 1.00f),
                hdm::Tr(
                    "Защита системы (точки восстановления) активна",
                    "Захист системи (точки відновлення) активний",
                    "System Protection (Restore Points) enabled"));
        } else {
            RenderStatusBadge(
                ImVec4(0.98f, 0.75f, 0.14f, 1.00f),
                hdm::Tr(
                    "Защита системы отключена — точки могут не создаться",
                    "Захист системи вимкнено — точки можуть не створюватися",
                    "System Protection disabled — restore points may fail"));
        }

        if (diagCache_.disabledCriticalServices > 0) {
            char svcWarn[128];
            std::snprintf(
                svcWarn,
                sizeof(svcWarn),
                hdm::Tr(
                    "Отключены критичные службы: %zu шт. (см. вкладку служб)",
                    "Вимкнено критичних служб: %zu шт. (див. вкладку служб)",
                    "Disabled critical services: %zu (check Services tab)"),
                diagCache_.disabledCriticalServices);
            RenderStatusBadge(ImVec4(0.97f, 0.44f, 0.44f, 1.00f), svcWarn);
        } else {
            RenderStatusBadge(
                ImVec4(0.20f, 0.83f, 0.60f, 1.00f),
                hdm::Tr(
                    "Критичные службы обновлений и защиты не отключены",
                    "Критичні служби оновлень і захисту не вимкнено",
                    "Critical update & protection services are enabled"));
        }
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // Section switcher headers: Controllers vs Windows Services vs Restore Points vs Storage & Power
    char ctrlPill[96];
    std::snprintf(
        ctrlPill,
        sizeof(ctrlPill),
        hdm::Tr(
            "Контроллеры платы (%zu)###CtrlPill",
            "Контролери плати (%zu)###CtrlPill",
            "Board Controllers (%zu)###CtrlPill"),
        diagCache_.controllers.size());
    if (RenderSubSectionHeaderTab(ctrlPill, diagSectionIndex_ == 0)) {
        diagSectionIndex_ = 0;
    }

    char svcPill[128];
    if (diagCache_.disabledCriticalServices > 0) {
        std::snprintf(
            svcPill,
            sizeof(svcPill),
            hdm::Tr(
                "Службы и обновления (%zu | %zu откл.!)###SvcPill",
                "Служби та оновлення (%zu | %zu вимкн.!)###SvcPill",
                "Services & Updates (%zu | %zu off!)###SvcPill"),
            diagCache_.services.size(),
            diagCache_.disabledCriticalServices);
    } else {
        std::snprintf(
            svcPill,
            sizeof(svcPill),
            hdm::Tr(
                "Службы и обновления (%zu)###SvcPill",
                "Служби та оновлення (%zu)###SvcPill",
                "Services & Updates (%zu)###SvcPill"),
            diagCache_.services.size());
    }
    SameLineWrapIfNeeded(CalcSubSectionTabWidth(svcPill));
    if (RenderSubSectionHeaderTab(svcPill, diagSectionIndex_ == 1)) {
        diagSectionIndex_ = 1;
    }

    char rpPill[96];
    std::snprintf(
        rpPill,
        sizeof(rpPill),
        hdm::Tr(
            "Точки восстановления (%zu)###RpPill",
            "Точки відновлення (%zu)###RpPill",
            "Restore Points (%zu)###RpPill"),
        diagCache_.restorePoints.size());
    SameLineWrapIfNeeded(CalcSubSectionTabWidth(rpPill));
    if (RenderSubSectionHeaderTab(rpPill, diagSectionIndex_ == 2)) {
        diagSectionIndex_ = 2;
    }

    char storPill[96];
    std::snprintf(
        storPill,
        sizeof(storPill),
        hdm::Tr(
            "Накопители и питание (%zu)###StorPill",
            "Накопичувачі та живлення (%zu)###StorPill",
            "Storage & Power (%zu)###StorPill"),
        diagCache_.storageItems.size());
    SameLineWrapIfNeeded(CalcSubSectionTabWidth(storPill));
    if (RenderSubSectionHeaderTab(storPill, diagSectionIndex_ == 3)) {
        diagSectionIndex_ = 3;
    }

    if (lastSnapshotLang_ != hdm::GetLang() &&
        (!baselineEntries_.empty() || !devicesCache_.empty())) {
        RebuildSnapshotDiff();
    }
    char snapPill[96];
    if (snapshotChangedCount_ > 0) {
        std::snprintf(
            snapPill,
            sizeof(snapPill),
            hdm::Tr(
                "Снимок До/После (%zu изм.)###SnapPill",
                "Знімок До/Після (%zu зм.)###SnapPill",
                "Before / After (%zu chg.)###SnapPill"),
            snapshotChangedCount_);
    } else {
        std::snprintf(
            snapPill,
            sizeof(snapPill),
            "%s",
            hdm::Tr(
                "Снимок До/После (0)###SnapPill",
                "Знімок До/Після (0)###SnapPill",
                "Before / After (0)###SnapPill"));
    }
    SameLineWrapIfNeeded(CalcSubSectionTabWidth(snapPill));
    if (RenderSubSectionHeaderTab(snapPill, diagSectionIndex_ == 4)) {
        diagSectionIndex_ = 4;
    }

    ImGui::Separator();

    const float footerPadY = std::round(7.0f * uiScale);
    const float footerH = ImGui::GetFrameHeight() + footerPadY * 2.0f;
    const float tableH = std::max(84.0f, ImGui::GetContentRegionAvail().y - footerH - style.ItemSpacing.y);
    constexpr ImGuiTableFlags kDiagTableFlags =
        ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY;

    if (diagSectionIndex_ == 0) {
        const float instW = CalcButtonWidth(hdm::Tr("Установить", "Встановити", "Install")) + 8.0f * uiScale;
        const float catW = CalcButtonWidth(hdm::Tr("Каталог", "Каталог", "Catalog")) + 8.0f * uiScale;
        const float actionColW = instW + style.ItemSpacing.x + catW + style.CellPadding.x * 2.0f + 4.0f;

        if (ImGui::BeginTable("##DiagControllersTable", 5, kDiagTableFlags, ImVec2(0.0f, tableH))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(hdm::Tr("Состояние", "Стан", "Status"), ImGuiTableColumnFlags_WidthStretch, 1.34f);
            ImGui::TableSetupColumn(hdm::Tr("Роль", "Роль", "Role"), ImGuiTableColumnFlags_WidthStretch, 0.62f);
            ImGui::TableSetupColumn(hdm::Tr("Устройство", "Пристрій", "Device"), ImGuiTableColumnFlags_WidthStretch, 1.20f);
            ImGui::TableSetupColumn(hdm::Tr("Драйвер / Дата", "Драйвер / Дата", "Driver / Date"), ImGuiTableColumnFlags_WidthStretch, 0.94f);
            ImGui::TableSetupColumn(hdm::Tr("Действие", "Дія", "Action"), ImGuiTableColumnFlags_WidthFixed, actionColW);
            RenderTableHeadersRow();

            for (size_t i = 0; i < diagCache_.controllers.size(); ++i) {
                const auto& item = diagCache_.controllers[i];
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(i));

                ImGui::TableSetColumnIndex(0);
                ImVec4 col(0.20f, 0.83f, 0.60f, 1.00f);
                if (item.health == hdm::ControllerHealth::Problem) {
                    col = ImVec4(0.97f, 0.44f, 0.44f, 1.00f);
                } else if (item.health == hdm::ControllerHealth::Attention) {
                    col = ImVec4(0.98f, 0.75f, 0.14f, 1.00f);
                }
                const std::string ctrlSt = hdm::TrDyn(item.statusText);
                RenderStatusBadge(col, ctrlSt.c_str(), true);

                ImGui::TableSetColumnIndex(1);
                RenderCellText(hdm::TrDyn(item.role), false, true);

                ImGui::TableSetColumnIndex(2);
                RenderCellText(item.deviceName, false, true);
                if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(440.0f * uiScale);
                    ImGui::TextColored(AccentTextColor(), "%s", item.deviceName.c_str());
                    if (g_isAdvancedMode) {
                        ImGui::TextDisabled(
                            hdm::Tr(
                                "Поставщик: %s | INF: %s",
                                "Виробник: %s | INF: %s",
                                "Vendor: %s | INF: %s"),
                            item.manufacturer.empty() ? "—" : item.manufacturer.c_str(),
                            item.infName.empty() ? "—" : item.infName.c_str());
                        if (!item.hardwareId.empty()) {
                            ImGui::TextWrapped("Hardware ID: %s", item.hardwareId.c_str());
                        }
                    } else {
                        ImGui::TextDisabled(
                            hdm::Tr("Поставщик: %s", "Виробник: %s", "Vendor: %s"),
                            item.manufacturer.empty() ? "—" : item.manufacturer.c_str());
                    }
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }

                ImGui::TableSetColumnIndex(3);
                std::string verDate = item.driverVersion.empty() ? "—" : item.driverVersion;
                if (!item.driverDate.empty()) {
                    verDate += " (" + item.driverDate + ")";
                }
                RenderCellText(verDate, false, true);

                ImGui::TableSetColumnIndex(4);
                const bool hasKey = !item.searchKey.empty() || !item.hardwareId.empty();
                const bool instPrimary = (item.health != hdm::ControllerHealth::Ok);
                ImGui::BeginDisabled(busy || compInstalling || !hasKey);
                if (RenderActionButton(
                        hdm::Tr("Установить###CInst", "Встановити###CInst", "Install###CInst"),
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
                            if (rebootReq) {
                                rebootRequired_.store(true);
                            }
                            if (ok) {
                                pendingDeviceRescan_.store(true);
                            }
                        });
                }
                ImGui::EndDisabled();
                if (isAppForeground &&
                    ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip(
                        "%s",
                        hdm::Tr(
                            "Автоматически найти, скачать (.cab), проверить подпись и установить свежий WHQL-драйвер из Microsoft Update Catalog.",
                            "Автоматично знайти, завантажити (.cab), перевірити підпис і встановити свіжий WHQL-драйвер із Microsoft Update Catalog.",
                            "Automatically search, download (.cab), verify signature, and install the latest WHQL driver from Microsoft Update Catalog."));
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(item.searchKey.empty());
                if (RenderSecondaryButton(
                        hdm::Tr("Каталог###CCat", "Каталог###CCat", "Catalog###CCat"),
                        ImVec2(catW, 0.0f))) {
                    OpenUrlInBrowser(
                        "https://www.catalog.update.microsoft.com/Search.aspx?q=" +
                        UrlEncode(item.searchKey));
                }
                ImGui::EndDisabled();

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    } else if (diagSectionIndex_ == 1) {
        const char* enSvcTxt = hdm::Tr("Включить", "Увімкнути", "Enable");
        const float svcBtnW = CalcButtonWidth(enSvcTxt) + 10.0f * uiScale;
        const float svcActionW = svcBtnW + style.CellPadding.x * 2.0f + 4.0f;
        if (ImGui::BeginTable("##DiagServicesTable", 5, kDiagTableFlags, ImVec2(0.0f, tableH))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(hdm::Tr("Статус", "Статус", "Status"), ImGuiTableColumnFlags_WidthStretch, 1.18f);
            ImGui::TableSetupColumn(hdm::Tr("Служба", "Служба", "Service"), ImGuiTableColumnFlags_WidthStretch, 1.12f);
            ImGui::TableSetupColumn(hdm::Tr("Назначение", "Призначення", "Purpose"), ImGuiTableColumnFlags_WidthStretch, 1.15f);
            ImGui::TableSetupColumn(hdm::Tr("Тип запуска", "Тип запуску", "Startup"), ImGuiTableColumnFlags_WidthStretch, 0.62f);
            ImGui::TableSetupColumn(hdm::Tr("Действие", "Дія", "Action"), ImGuiTableColumnFlags_WidthFixed, svcActionW);
            RenderTableHeadersRow();

            for (size_t i = 0; i < diagCache_.services.size(); ++i) {
                const auto& svc = diagCache_.services[i];
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(i));

                ImGui::TableSetColumnIndex(0);
                ImVec4 col(0.20f, 0.83f, 0.60f, 1.00f);
                if (svc.health == hdm::ServiceHealthState::Disabled) {
                    col = ImVec4(0.97f, 0.44f, 0.44f, 1.00f);
                } else if (svc.health == hdm::ServiceHealthState::MissingOrError) {
                    col = ImVec4(0.98f, 0.75f, 0.14f, 1.00f);
                }
                const std::string svcStateShown = hdm::TrDyn(svc.stateText);
                RenderStatusBadge(col, svcStateShown.c_str(), true);

                ImGui::TableSetColumnIndex(1);
                const std::string svcLabel = svc.serviceName + " — " + hdm::TrDyn(svc.displayName);
                if (RenderCellText(svcLabel, false, true) && isAppForeground &&
                    ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("%s", svcLabel.c_str());
                }

                ImGui::TableSetColumnIndex(2);
                const std::string svcPurposeShown = hdm::TrDyn(svc.purpose);
                if (RenderCellText(svcPurposeShown, true, true) && isAppForeground &&
                    ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("%s", svcPurposeShown.c_str());
                }

                ImGui::TableSetColumnIndex(3);
                RenderCellText(hdm::TrDyn(svc.startTypeText), false, true);

                ImGui::TableSetColumnIndex(4);
                if (svc.health == hdm::ServiceHealthState::Disabled) {
                    ImGui::BeginDisabled(busy);
                    if (ImGui::Button(
                            hdm::Tr("Включить###SvcEn", "Увімкнути###SvcEn", "Enable###SvcEn"),
                            ImVec2(svcBtnW, 0.0f))) {
                        diagnostics_.RunMaintenanceAsync(
                            hdm::MaintenanceAction::EnableService,
                            svc.serviceName,
                            devicesCache_,
                            MakeLogger());
                    }
                    ImGui::EndDisabled();
                } else {
                    const char* okTxt = hdm::Tr("ОК", "ОК", "OK");
                    const float okW = ImGui::CalcTextSize(okTxt).x;
                    const float availCell = ImGui::GetContentRegionAvail().x;
                    if (availCell > okW) {
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availCell - okW) * 0.5f);
                    }
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", okTxt);
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    } else if (diagSectionIndex_ == 2) {
        if (ImGui::BeginTable("##DiagRestorePointsTable", 4, kDiagTableFlags, ImVec2(0.0f, tableH))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(hdm::Tr("Тип / Статус", "Тип / Статус", "Type / Status"), ImGuiTableColumnFlags_WidthStretch, 0.85f);
            ImGui::TableSetupColumn(hdm::Tr("Дата и время", "Дата і час", "Date & Time"), ImGuiTableColumnFlags_WidthStretch, 0.72f);
            ImGui::TableSetupColumn(hdm::Tr("Описание точки восстановления", "Опис точки відновлення", "Restore Point Description"), ImGuiTableColumnFlags_WidthStretch, 1.85f);
            ImGui::TableSetupColumn(hdm::Tr("Источник", "Джерело", "Source"), ImGuiTableColumnFlags_WidthStretch, 0.88f);
            RenderTableHeadersRow();

            for (size_t i = 0; i < diagCache_.restorePoints.size(); ++i) {
                const auto& rp = diagCache_.restorePoints[i];
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(i));

                ImGui::TableSetColumnIndex(0);
                ImVec4 col(0.58f, 0.68f, 0.86f, 1.00f);
                if (rp.isDriverLoom) {
                    col = ImVec4(0.20f, 0.83f, 0.60f, 1.00f);
                } else if (rp.categoryLabel == "Windows Update") {
                    col = ImVec4(0.38f, 0.68f, 0.98f, 1.00f);
                }
                const std::string rpCatShown = hdm::TrDyn(rp.categoryLabel);
                RenderStatusBadge(col, rpCatShown.c_str(), true);

                ImGui::TableSetColumnIndex(1);
                RenderCellText(rp.timestamp, false, true);

                ImGui::TableSetColumnIndex(2);
                if (RenderCellText(rp.description, false, true) && isAppForeground &&
                    ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("%s", rp.description.c_str());
                }

                ImGui::TableSetColumnIndex(3);
                RenderCellText(rp.sourceProcess, true, true);

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    } else if (diagSectionIndex_ == 3) {
        const float instW = CalcButtonWidth(hdm::Tr("Установить", "Встановити", "Install")) + 8.0f * uiScale;
        const float catW = CalcButtonWidth(hdm::Tr("Каталог", "Каталог", "Catalog")) + 8.0f * uiScale;
        const float actionColW = instW + style.ItemSpacing.x + catW + style.CellPadding.x * 2.0f + 4.0f;

        if (ImGui::BeginTable("##DiagStorageTable", 5, kDiagTableFlags, ImVec2(0.0f, tableH))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(hdm::Tr("Состояние", "Стан", "Status"), ImGuiTableColumnFlags_WidthStretch, 1.28f);
            ImGui::TableSetupColumn(hdm::Tr("Тип", "Тип", "Type"), ImGuiTableColumnFlags_WidthStretch, 0.80f);
            ImGui::TableSetupColumn(hdm::Tr("Устройство / Накопитель", "Пристрій / Накопичувач", "Device / Storage Drive"), ImGuiTableColumnFlags_WidthStretch, 1.30f);
            ImGui::TableSetupColumn(hdm::Tr("Драйвер / INF", "Драйвер / INF", "Driver / INF"), ImGuiTableColumnFlags_WidthStretch, 0.94f);
            ImGui::TableSetupColumn(hdm::Tr("Действие", "Дія", "Action"), ImGuiTableColumnFlags_WidthFixed, actionColW);
            RenderTableHeadersRow();

            for (size_t i = 0; i < diagCache_.storageItems.size(); ++i) {
                const auto& st = diagCache_.storageItems[i];
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(i));

                ImGui::TableSetColumnIndex(0);
                ImVec4 col(0.20f, 0.83f, 0.60f, 1.00f);
                if (st.health == hdm::ControllerHealth::Problem) {
                    col = ImVec4(0.97f, 0.44f, 0.44f, 1.00f);
                } else if (st.health == hdm::ControllerHealth::Attention) {
                    col = ImVec4(0.98f, 0.75f, 0.14f, 1.00f);
                }
                const std::string stStatusShown = hdm::TrDyn(st.statusText);
                RenderStatusBadge(col, stStatusShown.c_str(), true);

                ImGui::TableSetColumnIndex(1);
                RenderCellText(hdm::TrDyn(st.role), false, true);

                ImGui::TableSetColumnIndex(2);
                RenderCellText(st.deviceName, false, true);
                if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(440.0f * uiScale);
                    ImGui::TextColored(AccentTextColor(), "%s", st.deviceName.c_str());
                    if (g_isAdvancedMode) {
                        ImGui::TextDisabled(
                            hdm::Tr(
                                "Поставщик: %s | INF: %s",
                                "Виробник: %s | INF: %s",
                                "Vendor: %s | INF: %s"),
                            st.manufacturer.empty() ? "—" : st.manufacturer.c_str(),
                            st.infName.empty() ? "—" : st.infName.c_str());
                        if (!st.hardwareId.empty()) {
                            ImGui::TextWrapped("Hardware ID: %s", st.hardwareId.c_str());
                        }
                    } else {
                        ImGui::TextDisabled(
                            hdm::Tr("Поставщик: %s", "Виробник: %s", "Vendor: %s"),
                            st.manufacturer.empty() ? "—" : st.manufacturer.c_str());
                    }
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }

                ImGui::TableSetColumnIndex(3);
                std::string verInf = st.driverVersion.empty() ? "—" : st.driverVersion;
                if (!st.infName.empty()) {
                    verInf += " (" + st.infName + ")";
                }
                RenderCellText(verInf, false, true);

                ImGui::TableSetColumnIndex(4);
                if (st.isController) {
                    const bool hasKey = !st.searchKey.empty() || !st.hardwareId.empty();
                    const bool instPrimary = (st.health != hdm::ControllerHealth::Ok);
                    ImGui::BeginDisabled(busy || compInstalling || !hasKey);
                    if (RenderActionButton(
                            hdm::Tr("Установить###StInst", "Встановити###StInst", "Install###StInst"),
                            ImVec2(instW, 0.0f),
                            instPrimary)) {
                        componentsScanner_.SearchAndInstallFromCatalogAsync(
                            st.deviceName,
                            st.searchKey,
                            st.hardwareId,
                            st.driverVersion,
                            devicesCache_,
                            MakeLogger(),
                            [this](bool ok, bool rebootReq) {
                                if (rebootReq) rebootRequired_.store(true);
                                if (ok) pendingDeviceRescan_.store(true);
                            });
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(st.searchKey.empty());
                    if (RenderSecondaryButton(
                            hdm::Tr("Каталог###StCat", "Каталог###StCat", "Catalog###StCat"),
                            ImVec2(catW, 0.0f))) {
                        OpenUrlInBrowser(
                            "https://www.catalog.update.microsoft.com/Search.aspx?q=" +
                            UrlEncode(st.searchKey));
                    }
                    ImGui::EndDisabled();
                } else {
                    const char* sysLbl = hdm::Tr(
                        "Системный (disk.inf)",
                        "Системний (disk.inf)",
                        "Inbox (disk.inf)");
                    const float sysW = ImGui::CalcTextSize(sysLbl).x;
                    const float availCell = ImGui::GetContentRegionAvail().x;
                    if (availCell > sysW) {
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availCell - sysW) * 0.5f);
                    }
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", sysLbl);
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    } else {
        if (ImGui::BeginTable("##DiagSnapshotTable", 5, kDiagTableFlags, ImVec2(0.0f, tableH))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(
                hdm::Tr("Изменение", "Зміна", "Change"),
                ImGuiTableColumnFlags_WidthStretch,
                0.95f);
            ImGui::TableSetupColumn(
                hdm::Tr("Устройство", "Пристрій", "Device"),
                ImGuiTableColumnFlags_WidthStretch,
                1.35f);
            ImGui::TableSetupColumn(
                hdm::Tr("Класс", "Клас", "Class"),
                ImGuiTableColumnFlags_WidthStretch,
                0.65f);
            ImGui::TableSetupColumn(
                hdm::Tr("Было (До снимка)", "Було (До знімка)", "Before (Baseline)"),
                ImGuiTableColumnFlags_WidthStretch,
                1.30f);
            ImGui::TableSetupColumn(
                hdm::Tr("Стало (Сейчас)", "Стало (Зараз)", "After (Current)"),
                ImGuiTableColumnFlags_WidthStretch,
                1.30f);
            RenderTableHeadersRow();

            if (!snapShowAll_ && snapshotChangedCount_ == 0) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                RenderStatusBadge(
                    ImVec4(0.20f, 0.83f, 0.60f, 1.00f),
                    hdm::Tr("Без изменений", "Без змін", "No changes"),
                    true);
                ImGui::TableSetColumnIndex(1);
                const std::string noDiffMsg =
                    baselineTimestamp_.empty()
                        ? std::string(hdm::Tr(
                              "Базовый снимок будет создан после завершения сканирования.",
                              "Базовий знімок буде створено після завершення сканування.",
                              "Baseline snapshot will be created when scan completes."))
                        : (std::string(hdm::Tr(
                               "Версии драйверов и статусы устройств не изменились со снимка ",
                               "Версії драйверів і статуси пристроїв не змінилися зі знімка ",
                               "Driver versions and device statuses have not changed since snapshot ")) +
                           baselineTimestamp_ + ".");
                RenderCellText(noDiffMsg, true, true);
            } else {
                std::vector<size_t> shownIndices;
                shownIndices.reserve(snapshotDiffRows_.size());
                for (size_t i = 0; i < snapshotDiffRows_.size(); ++i) {
                    if (snapShowAll_ ||
                        snapshotDiffRows_[i].kind != SnapshotDiffKind::Unchanged) {
                        shownIndices.push_back(i);
                    }
                }

                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(shownIndices.size()));
                while (clipper.Step()) {
                    for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                        const size_t idx = shownIndices[static_cast<size_t>(r)];
                        const auto& row = snapshotDiffRows_[idx];
                        ImGui::TableNextRow();
                        ImGui::PushID(static_cast<int>(idx));

                        ImGui::TableSetColumnIndex(0);
                        ImVec4 col(0.58f, 0.68f, 0.86f, 1.00f);
                        const char* badgeTxt = hdm::Tr("Без изменений", "Без змін", "Unchanged");
                        switch (row.kind) {
                        case SnapshotDiffKind::DriverUpdated:
                            col = ImVec4(0.38f, 0.68f, 0.98f, 1.00f);
                            badgeTxt = hdm::Tr("Обновлён драйвер", "Оновлено драйвер", "Driver Updated");
                            break;
                        case SnapshotDiffKind::IssueFixed:
                            col = ImVec4(0.20f, 0.83f, 0.60f, 1.00f);
                            badgeTxt = hdm::Tr("Исправлено", "Виправлено", "Issue Fixed");
                            break;
                        case SnapshotDiffKind::NewIssue:
                            col = ImVec4(0.97f, 0.44f, 0.44f, 1.00f);
                            badgeTxt = hdm::Tr("Новая ошибка", "Нова помилка", "New Issue");
                            break;
                        case SnapshotDiffKind::DeviceAdded:
                            col = ImVec4(0.26f, 0.82f, 0.92f, 1.00f);
                            badgeTxt = hdm::Tr("Добавлено", "Додано", "Added");
                            break;
                        case SnapshotDiffKind::DeviceRemoved:
                            col = ImVec4(0.98f, 0.75f, 0.14f, 1.00f);
                            badgeTxt = hdm::Tr("Удалено", "Видалено", "Removed");
                            break;
                        case SnapshotDiffKind::Unchanged:
                        default:
                            break;
                        }
                        RenderStatusBadge(col, badgeTxt, true);

                        ImGui::TableSetColumnIndex(1);
                        if (RenderCellText(hdm::TrDyn(row.name), false, true) &&
                            isAppForeground &&
                            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip("%s", hdm::TrDyn(row.name).c_str());
                        }

                        ImGui::TableSetColumnIndex(2);
                        RenderCellText(row.deviceClass, row.deviceClass.empty(), true);

                        ImGui::TableSetColumnIndex(3);
                        if (RenderCellText(row.beforeSummary, true, true) &&
                            isAppForeground &&
                            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip("%s", row.beforeSummary.c_str());
                        }

                        ImGui::TableSetColumnIndex(4);
                        if (RenderCellText(
                                row.afterSummary,
                                row.kind == SnapshotDiffKind::DeviceRemoved,
                                true) &&
                            isAppForeground &&
                            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                            ImGui::SetTooltip("%s", row.afterSummary.c_str());
                        }

                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndTable();
        }
    }

    // Bottom Maintenance Dock (bordered card footer aligned symmetrically)
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, footerPadY));
    ImGui::BeginChild(
        "##DiagFooterBar",
        ImVec2(0.0f, footerH),
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (diagSectionIndex_ == 3) {
        const char* trimTxt =
            diagCache_.readiness.trimEnabled
                ? hdm::Tr("NTFS TRIM: Активен", "NTFS TRIM: Активний", "NTFS TRIM: Enabled")
                : hdm::Tr("NTFS TRIM: Отключён", "NTFS TRIM: Вимкнено", "NTFS TRIM: Disabled");
        const float trimW = ImGui::CalcTextSize(trimTxt).x;
        const float winRight = ImGui::GetWindowContentRegionMax().x;

        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(
            "%s",
            hdm::Tr("Схема электропитания:", "Схема електроживлення:", "Windows Power Plan:"));
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
                diagnostics_.SetActivePowerSchemeAsync(
                    ps.guidString, devicesCache_, MakeLogger());
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
        if (ImGui::Button(hdm::Tr(
                "Создать точку восстановления###FtRpCreate",
                "Створити точку відновлення###FtRpCreate",
                "Create Restore Point###FtRpCreate"))) {
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

        ImGui::SameLine();
        if (RenderSecondaryButton(hdm::Tr(
                "Запустить откат системы (rstrui.exe)###FtRstrui",
                "Запустити відкат системи (rstrui.exe)###FtRstrui",
                "Open System Restore (rstrui.exe)###FtRstrui"))) {
            launchSystem32Tool(L"rstrui.exe");
        }

        ImGui::SameLine();
        if (RenderSecondaryButton(hdm::Tr(
                "Настроить защиту диска###FtProt",
                "Налаштувати захист диска###FtProt",
                "Configure Protection###FtProt"))) {
            launchSystem32Tool(L"SystemPropertiesProtection.exe");
        }

        char srMeta[96];
        std::snprintf(
            srMeta,
            sizeof(srMeta),
            hdm::Tr(
                "Резерв диска: %u%%  |  Индекс: #%u",
                "Резерв диска: %u%%  |  Індекс: #%u",
                "Disk Quota: %u%%  |  Index: #%u"),
            diagCache_.readiness.restoreDiskPercent,
            diagCache_.readiness.restoreLastIndex);
        const float metaW = ImGui::CalcTextSize(srMeta).x;
        const float winRight = ImGui::GetWindowContentRegionMax().x;
        const float lastEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        if (winRight - metaW > lastEnd + 16.0f) {
            ImGui::SameLine(winRight - metaW);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", srMeta);
        }
    } else if (diagSectionIndex_ == 4) {
        ImGui::BeginDisabled(devicesCache_.empty());
        if (ImGui::Button(hdm::Tr(
                "Обновить снимок «До»###SnapSaveBtn",
                "Оновити знімок «До»###SnapSaveBtn",
                "Capture New 'Before' Baseline###SnapSaveBtn"))) {
            CaptureCurrentAsBaseline(true);
        }
        ImGui::EndDisabled();

        char chgLbl[64], allLbl[64];
        std::snprintf(
            chgLbl,
            sizeof(chgLbl),
            hdm::Tr("Изменения (%zu)###SnapFiltChg", "Зміни (%zu)###SnapFiltChg", "Changes (%zu)###SnapFiltChg"),
            snapshotChangedCount_);
        std::snprintf(
            allLbl,
            sizeof(allLbl),
            hdm::Tr("Все (%zu)###SnapFiltAll", "Усі (%zu)###SnapFiltAll", "All (%zu)###SnapFiltAll"),
            snapshotDiffRows_.size());
        ImGui::SameLine();
        if (RenderPillButton(chgLbl, !snapShowAll_)) {
            snapShowAll_ = false;
        }
        ImGui::SameLine();
        if (RenderPillButton(allLbl, snapShowAll_)) {
            snapShowAll_ = true;
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(snapshotChangedCount_ == 0);
        if (RenderSecondaryButton(hdm::Tr(
                "Копировать изменения###SnapCopyBtn",
                "Копіювати зміни###SnapCopyBtn",
                "Copy Changes###SnapCopyBtn"))) {
            std::ostringstream oss;
            oss << "## DriverLoom Before / After (" << baselineTimestamp_ << ")\n";
            for (const auto& r : snapshotDiffRows_) {
                if (r.kind == SnapshotDiffKind::Unchanged) {
                    continue;
                }
                oss << "- **" << r.name << "** (" << r.deviceClass << "): `"
                    << r.beforeSummary << "` -> `" << r.afterSummary << "`\n";
            }
            ImGui::SetClipboardText(oss.str().c_str());
            logBuffer_.Push(
                hdm::LogLevel::Success,
                hdm::Tr(
                    "Список изменений «До / После» скопирован в буфер обмена.",
                    "Список змін «До / Після» скопійовано до буфера обміну.",
                    "Before / After changelog copied to clipboard."));
        }
        ImGui::EndDisabled();

        char baseMeta[128];
        std::snprintf(
            baseMeta,
            sizeof(baseMeta),
            hdm::Tr(
                "Снимок «До»: %s (%zu устр.)",
                "Знімок «До»: %s (%zu пристр.)",
                "Baseline: %s (%zu dev.)"),
            baselineTimestamp_.empty() ? "—" : baselineTimestamp_.c_str(),
            baselineEntries_.size());
        const float metaW = ImGui::CalcTextSize(baseMeta).x;
        const float winRight = ImGui::GetWindowContentRegionMax().x;
        const float lastEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        if (winRight - metaW > lastEnd + 16.0f) {
            ImGui::SameLine(winRight - metaW);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", baseMeta);
        }
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(
            "%s",
            hdm::Tr(
                "Проверка системных файлов:",
                "Перевірка системних файлів:",
                "System File Check:"));
        ImGui::BeginDisabled(busy);
        ImGui::SameLine(0.0f, 10.0f);
        if (RenderSecondaryButton("SFC /scannow")) {
            diagnostics_.RunMaintenanceAsync(
                hdm::MaintenanceAction::SfcScan, "", devicesCache_, MakeLogger());
        }
        if (isAppForeground &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "%s",
                hdm::Tr(
                    "Запускает встроенную проверку и восстановление защищённых файлов Windows (sfc.exe /scannow).",
                    "Запускає вбудовану перевірку та відновлення захищених файлів Windows (sfc.exe /scannow).",
                    "Runs built-in Windows protected system file verification and repair (sfc.exe /scannow)."));
        }

        ImGui::SameLine();
        if (RenderSecondaryButton("DISM CheckHealth")) {
            diagnostics_.RunMaintenanceAsync(
                hdm::MaintenanceAction::DismCheckHealth, "", devicesCache_, MakeLogger());
        }
        if (isAppForeground &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "%s",
                hdm::Tr(
                    "Быстрая проверка хранилища компонентов Windows без изменений (DISM /CheckHealth).",
                    "Швидка перевірка сховища компонентів Windows без змін (DISM /CheckHealth).",
                    "Quick non-destructive check of Windows component store health (DISM /CheckHealth)."));
        }

        ImGui::SameLine();
        if (RenderSecondaryButton("DISM RestoreHealth")) {
            diagnostics_.RunMaintenanceAsync(
                hdm::MaintenanceAction::DismRestoreHealth, "", devicesCache_, MakeLogger());
        }
        if (isAppForeground &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "%s",
                hdm::Tr(
                    "Глубокое восстановление хранилища компонентов Windows через Windows Update (DISM /RestoreHealth).",
                    "Глибоке відновлення сховища компонентів Windows через Windows Update (DISM /RestoreHealth).",
                    "Deep repair of the Windows component store via Windows Update (DISM /RestoreHealth)."));
        }
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}
