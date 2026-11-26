#include "ui/DriverLoomApp.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

void DriverLoomApp::RenderOverviewTab() {
    const float uiScale = EffectiveUiScale();
    const bool isAppForeground = (GetForegroundWindow() == hwnd_);

    const bool scanning = scanner_.IsScanning() || componentsScanner_.IsScanning() ||
                          componentsScanner_.IsCheckingOnline() || diagnostics_.IsScanning();
    const bool installing = componentsScanner_.IsInstalling() || wuManager_.IsInstalling() ||
                            localInstaller_.IsInstalling();
    const bool anyBusy = scanning || installing || scanner_.IsBusy() || diagnostics_.IsBusy();

    // Collect recommended actionable batch targets (excluding optional peripheral software)
    std::vector<hdm::SystemComponentItem> batchTargets;
    for (const auto& comp : componentsCache_) {
        if (IsComponentRecommendedAction(comp)) {
            batchTargets.push_back(comp);
        }
    }

    size_t wuCount = 0;
    for (const auto& u : wuCache_) {
        if (!u.installedOk && !u.isHidden) {
            ++wuCount;
        }
    }

    const size_t disabledSvcs = diagCache_.valid ? diagCache_.disabledCriticalServices : 0;
    const size_t attnCtrls = diagCache_.valid ? diagCache_.attentionControllers : 0;
    const size_t totalRecommendations =
        batchTargets.size() + disabledSvcs + attnCtrls + userRelevantIssueCount_;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild(
        "##OverviewScroll",
        ImVec2(0.0f, 0.0f),
        ImGuiChildFlags_None,
        ImGuiWindowFlags_None);
    ImGui::PopStyleColor();

    const float scrollGap = (ImGui::GetScrollMaxY() > 0.0f) ? std::round(12.0f * uiScale) : 0.0f;
    const float fullW = std::max(320.0f, ImGui::GetContentRegionAvail().x - scrollGap);
    const float gap = std::round(10.0f * uiScale);

    // =========================================================================
    // 1. HERO HEALTH CARD
    // =========================================================================
    {
        ImVec4 heroAccent = HealthyColor();
        AppIcon heroIcon = AppIcon::CheckCircle;
        const char* heroTitle = hdm::Tr(
            "С вашим компьютером всё в порядке",
            "З вашим комп'ютером усе гаразд",
            "Your PC is in great shape");
        char heroSub[256]{};

        if (installing) {
            heroAccent = InfoAccentColor();
            heroIcon = AppIcon::Updates;
            heroTitle = hdm::Tr(
                "Выполняется установка обновлений...",
                "Виконується встановлення оновлень...",
                "Installing updates...");
            const std::string phase =
                localInstaller_.IsInstalling()
                    ? hdm::TrDyn(localInstaller_.StatusMessage())
                    : (componentsScanner_.IsInstalling()
                           ? hdm::TrDyn(componentsScanner_.PhaseMessage())
                           : hdm::TrDyn(wuManager_.PhaseMessage()));
            std::snprintf(
                heroSub,
                sizeof(heroSub),
                "%s",
                phase.empty()
                    ? hdm::Tr(
                          "Пожалуйста, подождите — это займёт немного времени.",
                          "Будь ласка, зачекайте — це займе трохи часу.",
                          "Please wait — this will take a moment.")
                    : phase.c_str());
        } else if (scanning && lastDeviceGen_ == 0) {
            heroAccent = InfoAccentColor();
            heroIcon = AppIcon::Refresh;
            heroTitle = hdm::Tr(
                "Проверяем драйверы и компоненты ПК...",
                "Перевіряємо драйвери та компоненти ПК...",
                "Scanning your PC drivers & components...");
            std::snprintf(
                heroSub,
                sizeof(heroSub),
                "%s",
                hdm::Tr(
                    "Анализируем оборудование, мониторы, игровые библиотеки и состояние защиты Windows.",
                    "Аналізуємо обладнання, монітори, ігрові бібліотеки та стан захисту Windows.",
                    "Analyzing hardware, monitors, gaming runtimes, and Windows protection status."));
        } else if (disabledSvcs > 0) {
            heroAccent = DangerColor();
            heroIcon = AppIcon::AlertTriangle;
            heroTitle = hdm::Tr(
                "Отключены важные службы обновлений Windows",
                "Вимкнено важливі служби оновлень Windows",
                "Critical Windows update services are disabled");
            std::snprintf(
                heroSub,
                sizeof(heroSub),
                hdm::Tr(
                    "Обнаружено отключённых служб: %zu. Из-за этого драйверы и обновления могут не устанавливаться.",
                    "Виявлено вимкнених служб: %zu. Через це драйвери та оновлення можуть не встановлюватися.",
                    "%zu critical services are turned off. Drivers and updates may fail to install until enabled."),
                disabledSvcs);
        } else if (criticalDeviceErrorCount_ > 0) {
            heroAccent = DangerColor();
            heroIcon = AppIcon::AlertTriangle;
            heroTitle = hdm::Tr(
                "Обнаружены критичные ошибки драйверов",
                "Виявлено критичні помилки драйверів",
                "Critical driver errors detected");
            std::snprintf(
                heroSub,
                sizeof(heroSub),
                hdm::Tr(
                    "Устройств с ошибкой драйвера: %zu. Рекомендуется открыть список устройств и установить официальный драйвер.",
                    "Пристроїв з помилкою драйвера: %zu. Рекомендується відкрити список пристроїв і встановити офіційний драйвер.",
                    "%zu device(s) have driver errors. Open Devices to inspect and install an official driver."),
                criticalDeviceErrorCount_);
        } else if (!batchTargets.empty() || attnCtrls > 0 || userRelevantIssueCount_ > 0) {
            heroAccent = WarningColor();
            heroIcon = AppIcon::Updates;
            heroTitle = hdm::Tr(
                "Есть рекомендуемые обновления",
                "Є рекомендовані оновлення",
                "Recommended updates are available");
            if (!batchTargets.empty()) {
                std::snprintf(
                    heroSub,
                    sizeof(heroSub),
                    hdm::Tr(
                        "Найдено %zu обновлений для игр и оборудования. Перед установкой автоматически создастся точка отката.",
                        "Знайдено %zu оновлень для ігор та обладнання. Перед встановленням автоматично створиться точка відкату.",
                        "Found %zu updates for games and hardware. A System Restore point is created automatically before installing."),
                    batchTargets.size());
            } else {
                std::snprintf(
                    heroSub,
                    sizeof(heroSub),
                    hdm::Tr(
                        "Обнаружены устройства или контроллеры платы, для которых можно установить свежий официальный драйвер.",
                        "Виявлено пристрої або контролери плати, для яких можна встановити свіжий офіційний драйвер.",
                        "Some devices or board controllers can be updated with a newer official driver."));
            }
        } else {
            std::snprintf(
                heroSub,
                sizeof(heroSub),
                "%s",
                hdm::Tr(
                    "Основные драйверы, игровые библиотеки и защита системы работают исправно. Можно также проверить Windows Update.",
                    "Основні драйвери, ігрові бібліотеки та захист системи працюють справно. Можна також перевірити Windows Update.",
                    "Key drivers, gaming libraries, and system protection look healthy. You can also check Windows Update."));
        }

        const float heroPadX = std::round(20.0f * uiScale);
        const float heroPadY = std::round(13.0f * uiScale);
        const ImVec4 heroBg =
            g_isLightTheme
                ? ImVec4(
                      1.0f - (1.0f - heroAccent.x) * 0.045f,
                      1.0f - (1.0f - heroAccent.y) * 0.045f,
                      1.0f - (1.0f - heroAccent.z) * 0.045f,
                      1.00f)
                : ImVec4(
                      0.090f + heroAccent.x * 0.045f,
                      0.114f + heroAccent.y * 0.045f,
                      0.165f + heroAccent.z * 0.045f,
                      1.00f);
        const ImVec4 heroBorder =
            g_isLightTheme
                ? ImVec4(heroAccent.x * 0.78f, heroAccent.y * 0.78f, heroAccent.z * 0.78f, 0.36f)
                : ImVec4(heroAccent.x, heroAccent.y, heroAccent.z, 0.34f);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(heroPadX, heroPadY));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, heroBg);
        ImGui::PushStyleColor(ImGuiCol_Border, heroBorder);
        ImGui::BeginChild(
            "##HeroCard",
            ImVec2(fullW, 0.0f),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
            ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleColor(2);

        const float emblemDia = std::round(46.0f * uiScale);
        const ImVec2 startPos = ImGui::GetCursorScreenPos();
        const ImVec2 emblemCenter(startPos.x + emblemDia * 0.5f, startPos.y + emblemDia * 0.5f + 2.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec4 emblemBg(heroAccent.x, heroAccent.y, heroAccent.z, g_isLightTheme ? 0.14f : 0.18f);
        const ImVec4 emblemFg =
            g_isLightTheme
                ? ImVec4(heroAccent.x * 0.55f, heroAccent.y * 0.55f, heroAccent.z * 0.55f, 1.00f)
                : heroAccent;
        dl->AddCircleFilled(emblemCenter, emblemDia * 0.5f, ImGui::ColorConvertFloat4ToU32(emblemBg), 32);
        DrawAppIcon(dl, emblemCenter, emblemDia * 0.52f, heroIcon, ImGui::ColorConvertFloat4ToU32(emblemFg));

        // Text block to the right of emblem
        const float textStartX = emblemDia + std::round(16.0f * uiScale);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + textStartX);
        ImGui::BeginGroup();
        PushFontSafe(g_fontPrimaryHeader);
        ImGui::TextWrapped("%s", heroTitle);
        PopFontSafe(g_fontPrimaryHeader);
        ImGui::Dummy(ImVec2(0.0f, std::round(2.0f * uiScale)));
        ImGui::PushStyleColor(ImGuiCol_Text, MutedTextColor());
        ImGui::TextWrapped("%s", heroSub);
        ImGui::PopStyleColor();

        ImGui::Dummy(ImVec2(0.0f, std::round(10.0f * uiScale)));

        // Action Buttons Row
        const float btnH = std::round(34.0f * uiScale);
        bool hasPrevBtn = false;
        const char* instAllLblFmt = hdm::Tr(
            "Установить рекомендуемое (%zu)###HeroInstAll",
            "Встановити рекомендоване (%zu)###HeroInstAll",
            "Install Recommended (%zu)###HeroInstAll");
        const char* rescanLbl = hdm::Tr(
            "Проверить снова###HeroRescan",
            "Перевірити знову###HeroRescan",
            "Rescan PC###HeroRescan");
        const char* wuLbl = hdm::Tr(
            "Проверить Windows Update###HeroWu",
            "Перевірити Windows Update###HeroWu",
            "Check Windows Update###HeroWu");

        if (disabledSvcs > 0) {
            if (RenderPrimaryButton(
                    hdm::Tr("Перейти к службам###HeroFixSvc", "Перейти до служб###HeroFixSvc", "Fix Disabled Services###HeroFixSvc"),
                    ImVec2(0.0f, btnH))) {
                diagSectionIndex_ = 1;
                NavigateTo(PageDiagnostics);
            }
            hasPrevBtn = true;
        } else if (criticalDeviceErrorCount_ > 0) {
            if (RenderPrimaryButton(
                    hdm::Tr("Открыть проблемные устройства###HeroFixDev", "Відкрити проблемні пристрої###HeroFixDev", "View Failing Devices###HeroFixDev"),
                    ImVec2(0.0f, btnH))) {
                deviceFilterMode_ = DeviceFilterMode::ProblemOnly;
                deviceViewDirty_ = true;
                NavigateTo(PageDevices);
            }
            hasPrevBtn = true;
            if (!batchTargets.empty()) {
                char instAllLbl[96];
                std::snprintf(instAllLbl, sizeof(instAllLbl), instAllLblFmt, batchTargets.size());
                if (hasPrevBtn) {
                    SameLineWrapIfNeeded(CalcButtonWidth(instAllLbl));
                }
                ImGui::BeginDisabled(anyBusy);
                if (RenderSecondaryButton(instAllLbl, ImVec2(0.0f, btnH))) {
                    componentsScanner_.DownloadAndInstallBatchAsync(
                        batchTargets, devicesCache_, MakeLogger(), true);
                }
                ImGui::EndDisabled();
                hasPrevBtn = true;
            }
        } else if (!batchTargets.empty()) {
            char instAllLbl[96];
            std::snprintf(instAllLbl, sizeof(instAllLbl), instAllLblFmt, batchTargets.size());
            ImGui::BeginDisabled(anyBusy);
            if (RenderPrimaryButton(instAllLbl, ImVec2(0.0f, btnH))) {
                componentsScanner_.DownloadAndInstallBatchAsync(
                    batchTargets, devicesCache_, MakeLogger(), true);
            }
            ImGui::EndDisabled();
            hasPrevBtn = true;
        } else {
            ImGui::BeginDisabled(anyBusy);
            if (RenderPrimaryButton(
                    hdm::Tr("Проверить обновления###HeroScan", "Перевірити оновлення###HeroScan", "Check for Updates###HeroScan"),
                    ImVec2(0.0f, btnH))) {
                scanner_.StartScanAsync(MakeLogger());
            }
            ImGui::EndDisabled();
            hasPrevBtn = true;
        }

        if (!batchTargets.empty() || disabledSvcs > 0 || criticalDeviceErrorCount_ > 0) {
            if (hasPrevBtn) {
                SameLineWrapIfNeeded(CalcButtonWidth(rescanLbl));
            }
            ImGui::BeginDisabled(anyBusy);
            if (RenderSecondaryButton(rescanLbl, ImVec2(0.0f, btnH))) {
                scanner_.StartScanAsync(MakeLogger());
            }
            ImGui::EndDisabled();
            hasPrevBtn = true;
        }

        if (hasPrevBtn) {
            SameLineWrapIfNeeded(CalcButtonWidth(wuLbl));
        }
        ImGui::BeginDisabled(wuManager_.IsBusy());
        if (RenderSecondaryButton(wuLbl, ImVec2(0.0f, btnH))) {
            wuHasSearchedOnce_ = true;
            selectedWuIndex_ = -1;
            wuManager_.StartSearchAsync(MakeLogger(), !wuIncludeSystemUpdates_);
            NavigateTo(PageWindowsUpdate);
        }
        ImGui::EndDisabled();

        if (componentsScanner_.IsInstalling()) {
            ImGui::Dummy(ImVec2(0.0f, std::round(6.0f * uiScale)));
            const float prog = componentsScanner_.Progress();
            char ov[64];
            std::snprintf(ov, sizeof(ov), "%.0f%%", prog * 100.0f);
            const char* cLbl = hdm::Tr("Отмена", "Скасувати", "Cancel");
            const float cW = CalcButtonWidth(cLbl) + 8.0f * uiScale;
            ImGui::ProgressBar(
                prog,
                ImVec2(std::max(120.0f, ImGui::GetContentRegionAvail().x - cW - ImGui::GetStyle().ItemSpacing.x),
                       ImGui::GetFrameHeight()),
                ov);
            ImGui::SameLine();
            if (RenderSecondaryButton(hdm::Tr("Отмена###HeroCancel", "Скасувати###HeroCancel", "Cancel###HeroCancel"), ImVec2(cW, 0.0f))) {
                componentsScanner_.RequestCancelInstall();
            }
        }
        ImGui::EndGroup();

        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    ImGui::Dummy(ImVec2(0.0f, gap));

    // =========================================================================
    // 2. FOUR SUMMARY TILES (2x2 Grid)
    // =========================================================================
    const float tileW = std::min(fullW, std::max(std::round(220.0f * uiScale), (fullW - gap) * 0.5f));
    const float tilePadX = std::round(14.0f * uiScale);
    const float tilePadY = std::round(12.0f * uiScale);
    const float tileInnerW = std::max(80.0f, tileW - tilePadX * 2.0f);

    char compStat[64];
    ImVec4 compCol = HealthyColor();
    if (actionableComponentsCount_ > 0) {
        std::snprintf(
            compStat,
            sizeof(compStat),
            hdm::Tr("Доступно: %zu", "Доступно: %zu", "%zu available"),
            actionableComponentsCount_);
        compCol = WarningColor();
    } else {
        std::snprintf(compStat, sizeof(compStat), "%s", hdm::Tr("Актуально", "Актуально", "Up to date"));
    }
    const char* appsDesc = hdm::Tr(
        "DirectX, Visual C++, .NET, драйверы видеокарты и фирменное ПО для вашей периферии.",
        "DirectX, Visual C++, .NET, драйвери відеокарти та фірмове ПЗ для вашої периферії.",
        "DirectX, Visual C++, .NET, graphics drivers, and official software for your peripherals.");

    char wuStat[64];
    ImVec4 wuCol = MutedTextColor();
    if (wuManager_.IsSearching()) {
        std::snprintf(wuStat, sizeof(wuStat), "%s", hdm::Tr("Поиск...", "Пошук...", "Checking..."));
        wuCol = InfoAccentColor();
    } else if (wuCount > 0) {
        std::snprintf(
            wuStat,
            sizeof(wuStat),
            hdm::Tr("Найдено: %zu", "Знайдено: %zu", "%zu found"),
            wuCount);
        wuCol = InfoAccentColor();
    } else if (wuHasSearchedOnce_) {
        std::snprintf(wuStat, sizeof(wuStat), "%s", hdm::Tr("Всё установлено", "Усе встановлено", "Up to date"));
        wuCol = HealthyColor();
    } else {
        std::snprintf(wuStat, sizeof(wuStat), "%s", hdm::Tr("Не проверялось", "Не перевірялось", "Not checked"));
    }
    const char* wuDesc = hdm::Tr(
        "Официальные драйверы и системные обновления напрямую с серверов Microsoft.",
        "Офіційні драйвери та системні оновлення напряму з серверів Microsoft.",
        "Official hardware drivers and system updates directly from Microsoft servers.");

    char devStat[64];
    ImVec4 devCol = HealthyColor();
    if (userRelevantIssueCount_ > 0) {
        std::snprintf(
            devStat,
            sizeof(devStat),
            hdm::Tr("Внимание: %zu", "Увага: %zu", "%zu need attention"),
            userRelevantIssueCount_);
        devCol = WarningColor();
    } else {
        std::snprintf(devStat, sizeof(devStat), "%s", hdm::Tr("Всё работает", "Усе працює", "All working"));
    }
    char devDesc[192];
    std::snprintf(
        devDesc,
        sizeof(devDesc),
        hdm::Tr(
            "Подключено устройств: %zu  •  Мониторы: %zu  •  Периферия: %zu.",
            "Підключено пристроїв: %zu  •  Монітори: %zu  •  Периферія: %zu.",
            "Connected devices: %zu  •  Monitors: %zu  •  Peripherals: %zu."),
        totalDevicesCount_,
        monitorDevicesCount_,
        peripheralDevicesCount_);

    const bool srOk = diagCache_.valid && diagCache_.readiness.systemRestoreEnabled &&
                      !diagCache_.readiness.systemRestorePolicyDisabled;
    const char* protStat =
        !diagCache_.valid
            ? hdm::Tr("Проверка...", "Перевірка...", "Checking...")
            : (srOk ? hdm::Tr("Защита активна", "Захист активний", "Protected")
                    : hdm::Tr("Защита выкл.", "Захист вимкн.", "Protection off"));
    const ImVec4 protCol = !diagCache_.valid ? MutedTextColor() : (srOk ? HealthyColor() : WarningColor());

    std::string boardSummary;
    if (diagCache_.valid) {
        const auto& mb = diagCache_.motherboard;
        if (!mb.product.empty() && mb.product != "—") {
            boardSummary = mb.product;
        } else if (!mb.systemProduct.empty() && mb.systemProduct != "—") {
            boardSummary = mb.systemProduct;
        }
    }
    char protDesc[220];
    if (!boardSummary.empty()) {
        std::snprintf(
            protDesc,
            sizeof(protDesc),
            hdm::Tr(
                "Плата: %s  •  Точек восстановления: %zu.",
                "Плата: %s  •  Точок відновлення: %zu.",
                "Board: %s  •  Restore points: %zu."),
            boardSummary.c_str(),
            diagCache_.restorePoints.size());
    } else {
        std::snprintf(
            protDesc,
            sizeof(protDesc),
            "%s",
            hdm::Tr(
                "Точки восстановления, резервная копия драйверов и диагностика системы.",
                "Точки відновлення, резервна копія драйверів і діагностика системи.",
                "Restore points, driver backup export, and motherboard diagnostics."));
    }

    PushFontSafe(g_fontSecondaryHeader);
    const float measuredTitleH = ImGui::GetTextLineHeight();
    PopFontSafe(g_fontSecondaryHeader);

    PushFontSafe(g_fontSmall);
    float maxDescH = ImGui::GetTextLineHeight();
    const char* tileDescs[4] = {appsDesc, wuDesc, devDesc, protDesc};
    for (const char* d : tileDescs) {
        maxDescH = std::max(maxDescH, ImGui::CalcTextSize(d, nullptr, false, tileInnerW).y);
    }
    PopFontSafe(g_fontSmall);

    const float topToDescGap = std::round(6.0f * uiScale);
    const float descToBtnGap = std::round(12.0f * uiScale);
    const float tileH = std::max(
        std::round(122.0f * uiScale),
        std::ceil(
            tilePadY + measuredTitleH + topToDescGap + maxDescH + descToBtnGap +
            ImGui::GetFrameHeight() + tilePadY));

    auto renderSummaryTile = [&](const char* id,
                                 AppIcon icon,
                                 const char* title,
                                 const char* statusText,
                                 const ImVec4& statusColor,
                                 const char* desc,
                                 const char* actionLabel,
                                 auto&& onAction) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(tilePadX, tilePadY));
        ImGui::PushStyleColor(ImGuiCol_Border, PanelBorderColor());
        ImGui::BeginChild(
            id,
            ImVec2(tileW, tileH),
            true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();

        // Top Row: Icon + Title on left, Status pill on right (vertically aligned to title midline)
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float iconSz = std::round(18.0f * uiScale);
        const float availTileW = ImGui::GetContentRegionAvail().x;

        PushFontSafe(g_fontSecondaryHeader);
        const float titleH = ImGui::GetTextLineHeight();
        PopFontSafe(g_fontSecondaryHeader);

        float pillReserve = 0.0f;
        float pillW = 0.0f;
        float pillH = 0.0f;
        float pillPadX = std::round(8.0f * uiScale);
        ImVec2 statusSize(0.0f, 0.0f);
        if (statusText != nullptr && statusText[0] != '\0') {
            PushFontSafe(g_fontSmall);
            statusSize = ImGui::CalcTextSize(statusText);
            pillW = statusSize.x + pillPadX * 2.0f;
            pillH = statusSize.y + std::round(4.0f * uiScale);
            pillReserve = pillW + std::round(10.0f * uiScale);
            PopFontSafe(g_fontSmall);
        }

        DrawAppIcon(
            ImGui::GetWindowDrawList(),
            ImVec2(p0.x + iconSz * 0.5f, p0.y + titleH * 0.5f),
            iconSz,
            icon,
            ImGui::ColorConvertFloat4ToU32(AccentBarColor()));

        const float titleOffset = iconSz + std::round(9.0f * uiScale);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + titleOffset);
        PushFontSafe(g_fontSecondaryHeader);
        const float maxTitleW = std::max(40.0f, availTileW - titleOffset - pillReserve);
        const std::string shownTitle = EllipsizeUtf8(title != nullptr ? title : "", maxTitleW);
        ImGui::TextColored(CardHeaderTextColor(), "%s", shownTitle.c_str());
        PopFontSafe(g_fontSecondaryHeader);

        // Status text aligned on right of title row
        if (statusText != nullptr && statusText[0] != '\0') {
            PushFontSafe(g_fontSmall);
            const float rightEdge = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            const float pillY = p0.y + (titleH - pillH) * 0.5f;
            const ImVec2 bMin(rightEdge - pillW, pillY);
            const ImVec2 bMax(rightEdge, pillY + pillH);
            const ImVec4 bg(statusColor.x, statusColor.y, statusColor.z, g_isLightTheme ? 0.14f : 0.16f);
            const ImVec4 fg =
                g_isLightTheme
                    ? ImVec4(statusColor.x * 0.50f, statusColor.y * 0.50f, statusColor.z * 0.50f, 1.00f)
                    : statusColor;
            ImGui::GetWindowDrawList()->AddRectFilled(bMin, bMax, ImGui::ColorConvertFloat4ToU32(bg), pillH * 0.5f);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(bMin.x + pillPadX, bMin.y + (pillH - statusSize.y) * 0.5f),
                ImGui::ColorConvertFloat4ToU32(fg),
                statusText);
            PopFontSafe(g_fontSmall);
        }

        ImGui::SetCursorPosY(tilePadY + titleH + topToDescGap);
        PushFontSafe(g_fontSmall);
        ImGui::PushStyleColor(ImGuiCol_Text, MutedTextColor());
        ImGui::TextWrapped("%s", desc);
        ImGui::PopStyleColor();
        PopFontSafe(g_fontSmall);

        // Anchor action button consistently with equal bottom padding across all 4 tiles
        const float btnY = tileH - tilePadY - ImGui::GetFrameHeight();
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY() + std::round(6.0f * uiScale), btnY));
        if (RenderSecondaryButton(actionLabel)) {
            onAction();
        }

        ImGui::EndChild();
        ImGui::PopStyleVar();
    };

    // Row 1: Games & Software | Windows Update
    renderSummaryTile(
        "##TileApps",
        AppIcon::Apps,
        hdm::Tr("Игры и программы", "Ігри та ПЗ", "Games & Software"),
        compStat,
        compCol,
        appsDesc,
        hdm::Tr("Открыть список###TileAppsBtn", "Відкрити список###TileAppsBtn", "Open List###TileAppsBtn"),
        [&]() { NavigateTo(PageComponents); });

    ImGui::SameLine(0.0f, gap);

    renderSummaryTile(
        "##TileWu",
        AppIcon::Updates,
        hdm::Tr("Обновления Windows", "Оновлення Windows", "Windows Update"),
        wuStat,
        wuCol,
        wuDesc,
        wuHasSearchedOnce_
            ? hdm::Tr("Открыть###TileWuBtn", "Відкрити###TileWuBtn", "Open###TileWuBtn")
            : hdm::Tr("Проверить сейчас###TileWuBtn", "Перевірити зараз###TileWuBtn", "Check Now###TileWuBtn"),
        [&]() {
            if (!wuHasSearchedOnce_ && !wuManager_.IsBusy()) {
                wuHasSearchedOnce_ = true;
                selectedWuIndex_ = -1;
                wuManager_.StartSearchAsync(MakeLogger(), !wuIncludeSystemUpdates_);
            }
            NavigateTo(PageWindowsUpdate);
        });

    ImGui::Dummy(ImVec2(0.0f, gap));

    // Row 2: Devices & Monitors | Protection & System
    renderSummaryTile(
        "##TileDev",
        AppIcon::Devices,
        hdm::Tr("Устройства и мониторы", "Пристрої та монітори", "Devices & Monitors"),
        devStat,
        devCol,
        devDesc,
        userRelevantIssueCount_ > 0
            ? hdm::Tr("Показать проблемы###TileDevBtn", "Показати проблеми###TileDevBtn", "View Issues###TileDevBtn")
            : hdm::Tr("Все устройства###TileDevBtn", "Усі пристрої###TileDevBtn", "Browse Devices###TileDevBtn"),
        [&]() {
            if (userRelevantIssueCount_ > 0) {
                deviceFilterMode_ = DeviceFilterMode::ProblemOnly;
                deviceViewDirty_ = true;
            }
            NavigateTo(PageDevices);
        });

    ImGui::SameLine(0.0f, gap);

    renderSummaryTile(
        "##TileProt",
        AppIcon::Shield,
        hdm::Tr("Защита и система", "Захист і система", "Protection & System"),
        protStat,
        protCol,
        protDesc,
        hdm::Tr("Открыть диагностику###TileProtBtn", "Відкрити діагностику###TileProtBtn", "Open Diagnostics###TileProtBtn"),
        [&]() { NavigateTo(PageDiagnostics); });

    ImGui::Dummy(ImVec2(0.0f, gap));

    // =========================================================================
    // 3. QUICK RECOMMENDATIONS LIST (Top actionable items right on Home!)
    // =========================================================================
    size_t actionableCtrlCount = 0;
    if (diagCache_.valid) {
        for (const auto& ctrl : diagCache_.controllers) {
            if (ctrl.health != hdm::ControllerHealth::Ok &&
                (!ctrl.searchKey.empty() || !ctrl.hardwareId.empty())) {
                ++actionableCtrlCount;
            }
        }
    }
    if (totalRecommendations > 0 && (!batchTargets.empty() || actionableCtrlCount > 0)) {
        const float cardPad = std::round(15.0f * uiScale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(cardPad, std::round(13.0f * uiScale)));
        ImGui::PushStyleColor(ImGuiCol_Border, PanelBorderColor());
        ImGui::BeginChild(
            "##QuickRecCard",
            ImVec2(fullW, 0.0f),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
            ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleColor();

        RenderSecondaryCardHeader(
            hdm::Tr("Что можно улучшить сейчас", "Що можна покращити зараз", "Quick Recommendations"));
        ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));

        int shownRows = 0;
        constexpr int kMaxQuickRows = 4;
        const float btnW = CalcButtonWidth(hdm::Tr("Установить", "Встановити", "Install")) + 12.0f * uiScale;
        const bool busyInstall = componentsScanner_.IsInstalling() || diagnostics_.IsBusy();

        // First: Outdated / missing board controllers
        if (diagCache_.valid) {
            for (size_t i = 0; i < diagCache_.controllers.size() && shownRows < kMaxQuickRows; ++i) {
                const auto& ctrl = diagCache_.controllers[i];
                if (ctrl.health == hdm::ControllerHealth::Ok) {
                    continue;
                }
                const bool hasKey = !ctrl.searchKey.empty() || !ctrl.hardwareId.empty();
                if (!hasKey) {
                    continue;
                }

                if (shownRows > 0) {
                    ImGui::Separator();
                }
                ImGui::PushID(static_cast<int>(1000 + i));
                const float availW = ImGui::GetContentRegionAvail().x;
                const float textW = std::max(120.0f, availW - btnW - std::round(14.0f * uiScale));

                ImGui::BeginGroup();
                const std::string titleStr = hdm::TrDyn(ctrl.role) + " — " + ctrl.deviceName;
                ImGui::TextUnformatted(EllipsizeUtf8(titleStr, textW).c_str());
                PushFontSafe(g_fontSmall);
                ImGui::TextColored(
                    WarningTextColor(),
                    "%s",
                    EllipsizeUtf8(hdm::TrDyn(ctrl.statusText), textW).c_str());
                PopFontSafe(g_fontSmall);
                ImGui::EndGroup();

                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - btnW);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::round(4.0f * uiScale));
                ImGui::BeginDisabled(busyInstall);
                if (RenderPrimaryButton(
                        hdm::Tr("Обновить###QCtrl", "Оновити###QCtrl", "Update###QCtrl"),
                        ImVec2(btnW, 0.0f))) {
                    componentsScanner_.SearchAndInstallFromCatalogAsync(
                        ctrl.deviceName,
                        ctrl.searchKey,
                        ctrl.hardwareId,
                        ctrl.driverVersion,
                        devicesCache_,
                        MakeLogger(),
                        [this](bool ok, bool rebootReq) {
                            if (rebootReq) rebootRequired_.store(true);
                            if (ok) pendingDeviceRescan_.store(true);
                        });
                }
                ImGui::EndDisabled();
                ImGui::PopID();
                ++shownRows;
            }
        }

        // Next: Top Games & Software items
        for (size_t i = 0; i < batchTargets.size() && shownRows < kMaxQuickRows; ++i) {
            const auto& comp = batchTargets[i];
            if (shownRows > 0) {
                ImGui::Separator();
            }
            ImGui::PushID(static_cast<int>(2000 + i));
            const float availW = ImGui::GetContentRegionAvail().x;
            const float textW = std::max(120.0f, availW - btnW - std::round(14.0f * uiScale));

            ImGui::BeginGroup();
            ImGui::TextUnformatted(EllipsizeUtf8(hdm::TrDyn(comp.name), textW).c_str());
            PushFontSafe(g_fontSmall);
            std::string subLine = hdm::TrDyn(comp.statusText);
            if (!comp.purposeDescription.empty()) {
                subLine += "  •  " + hdm::TrDyn(comp.purposeDescription);
            }
            ImGui::TextColored(MutedTextColor(), "%s", EllipsizeUtf8(subLine, textW).c_str());
            PopFontSafe(g_fontSmall);
            ImGui::EndGroup();

            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - btnW);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::round(4.0f * uiScale));
            ImGui::BeginDisabled(busyInstall);
            const char* actLbl =
                !comp.installed
                    ? hdm::Tr("Установить###QComp", "Встановити###QComp", "Install###QComp")
                    : hdm::Tr("Обновить###QComp", "Оновити###QComp", "Update###QComp");
            if (RenderPrimaryButton(actLbl, ImVec2(btnW, 0.0f))) {
                componentsScanner_.DownloadAndInstallAsync(comp, devicesCache_, MakeLogger());
            }
            ImGui::EndDisabled();
            ImGui::PopID();
            ++shownRows;
        }

        if (batchTargets.size() + attnCtrls > static_cast<size_t>(shownRows)) {
            ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));
            if (RenderSecondaryButton(hdm::Tr(
                    "Посмотреть все обновления...###QMore",
                    "Переглянути всі оновлення...###QMore",
                    "View All Updates...###QMore"))) {
                NavigateTo(batchTargets.empty() && attnCtrls > 0 ? PageDiagnostics : PageComponents);
            }
        }

        ImGui::EndChild();
        ImGui::PopStyleVar();
    } else if (diagCache_.valid) {
        // Calm PC Snapshot Card when everything is up to date!
        const float cardPad = std::round(15.0f * uiScale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(cardPad, std::round(13.0f * uiScale)));
        ImGui::PushStyleColor(ImGuiCol_Border, PanelBorderColor());
        ImGui::BeginChild(
            "##PcInfoSummaryCard",
            ImVec2(fullW, 0.0f),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
            ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleColor();

        RenderSecondaryCardHeader(hdm::Tr("Кратко о вашем ПК", "Коротко про ваш ПК", "Your PC Summary"));
        ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));

        const auto& mb = diagCache_.motherboard;
        std::string boardName = mb.manufacturer;
        if (!mb.product.empty() && mb.product != "—") {
            if (!boardName.empty() && boardName != "—") boardName += " ";
            else boardName.clear();
            boardName += mb.product;
        }
        if (!boardName.empty()) {
            ImGui::TextDisabled("%s", hdm::Tr("Материнская плата:", "Материнська плата:", "Motherboard:"));
            ImGui::SameLine();
            ImGui::TextWrapped("%s", boardName.c_str());
        }
        for (const auto& d : devicesCache_) {
            if (d.gpuVramBytes >= (64ull * 1024ull * 1024ull)) {
                ImGui::TextDisabled("%s", hdm::Tr("Видеокарта:", "Відеокарта:", "Graphics Card:"));
                ImGui::SameLine();
                ImGui::TextWrapped("%s (%s)", d.name.c_str(), hdm::FormatBytes(d.gpuVramBytes).c_str());
                break;
            }
        }
        if (!diagCache_.activePowerSchemeName.empty()) {
            ImGui::TextDisabled("%s", hdm::Tr("Режим питания:", "Режим живлення:", "Power Plan:"));
            ImGui::SameLine();
            ImGui::TextWrapped("%s", hdm::TrDyn(diagCache_.activePowerSchemeName).c_str());
        }

        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    (void)isAppForeground;
    ImGui::EndChild(); // ##OverviewScroll
}
