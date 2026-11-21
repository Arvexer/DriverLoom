#pragma once

#include "common/AppCommon.hpp"
#include "imgui.h"

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

extern std::atomic<bool> g_windowRegainedFocus;
extern bool g_isLightTheme;
extern bool g_isAdvancedMode;
extern bool g_autoCheckAppUpdates;
extern float g_dpiScale;

extern ImFont* g_fontRegular;
extern ImFont* g_fontSecondaryHeader;
extern ImFont* g_fontMainTab;
extern ImFont* g_fontPrimaryHeader;

struct DriverBaselineEntry {
    std::string instanceId;
    std::string name;
    std::string deviceClass;
    std::string driverVersion;
    std::string driverDate;
    std::string infName;
    std::string statusText;
    int health{0};
};

enum class SnapshotDiffKind {
    DriverUpdated,
    IssueFixed,
    NewIssue,
    DeviceAdded,
    DeviceRemoved,
    Unchanged
};

struct DriverSnapshotDiffRow {
    SnapshotDiffKind kind{SnapshotDiffKind::Unchanged};
    std::string instanceId;
    std::string name;
    std::string deviceClass;
    std::string beforeSummary;
    std::string afterSummary;
    bool canRollback{false};
};

struct SummaryBadgeSegment {
    const char* label = nullptr;
    const char* value = nullptr;
    ImVec4 accentColor = ImVec4(0.46f, 0.74f, 0.98f, 1.00f);
    bool tintLabel = false;
};

void PushFontSafe(ImFont* font);
void PopFontSafe(ImFont* font);
void SetWindowTitleBarDark(HWND hwnd, bool useDark);

std::string UrlEncode(const std::string& value);
void OpenUrlInBrowser(const std::string& url);

ImVec4 AccentTextColor();
ImVec4 WarningTextColor();
ImVec4 PanelBorderColor();
ImVec4 AccentBarColor();
ImVec4 CardHeaderTextColor();

void ApplyBaseLayoutStyle(ImGuiStyle& style, float scale);
void ApplyModernDarkThemeColors(ImGuiStyle& style);
void ApplyModernLightThemeColors(ImGuiStyle& style);
void ApplyAppTheme(HWND hwnd, float scale, bool lightTheme, bool initLayout = false);

void LoadUserPreferences() noexcept;
void SaveUserPreferences() noexcept;

std::string FormatSnapshotTimestampNow();
void SaveBaselineSnapshotToRegistry(
    const std::string& timestamp,
    const std::vector<DriverBaselineEntry>& entries);
bool LoadBaselineSnapshotFromRegistry(
    std::string& outTimestamp,
    std::vector<DriverBaselineEntry>& outEntries);

std::string EllipsizeUtf8(const std::string& text, float maxWidth);
bool RenderCellText(const std::string& text, bool dimmed = false, bool alignToFrame = false);
void RenderStatusBadge(const ImVec4& color, const char* text, bool alignToFrame = false);
bool RenderPillButton(const char* label, bool active, const ImVec2& size = ImVec2(0.0f, 0.0f));
float CalcSubSectionTabWidth(const char* label);
bool RenderSubSectionHeaderTab(const char* label, bool active);
void RenderSecondaryCardHeader(const char* title, const char* rightMeta = nullptr);
void RenderVerticalDivider(float padX = 4.0f);
void RenderSecondaryGroupLabel(const char* text);
void RenderTableHeadersRow();
bool RenderSecondaryButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));
bool RenderActionButton(const char* label, const ImVec2& size, bool primary);
float CalcSummaryInfoBadgeWidth(const SummaryBadgeSegment* segments, size_t count);
void RenderSummaryInfoBadge(const SummaryBadgeSegment* segments, size_t count);

int CompareVersionOrString(const std::string& a, const std::string& b);
