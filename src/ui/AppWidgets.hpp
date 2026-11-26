#pragma once

#include "common/AppCommon.hpp"
#include "imgui.h"

#include <atomic>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

extern std::atomic<bool> g_windowRegainedFocus;
extern bool g_isLightTheme;
extern bool g_isAdvancedMode;
extern bool g_autoCheckAppUpdates;
extern bool g_autoCheckOnlineUpdates;
extern bool g_pinLogPanel;
extern float g_dpiScale;
extern int g_uiScalePercent;
extern bool g_pendingScaleReload;
extern bool g_cliOverrideAdvancedMode;
extern bool g_cliOverrideLightTheme;
extern bool g_cliOverrideUiScale;
extern bool g_cliOverrideLanguage;
extern std::unordered_set<std::string> g_mutedComponentIds;
float EffectiveUiScale() noexcept;

extern ImFont* g_fontSmall;
extern ImFont* g_fontRegular;
extern ImFont* g_fontSecondaryHeader;
extern ImFont* g_fontMainTab;
extern ImFont* g_fontPrimaryHeader;
extern ImTextureID g_appLogoTexture;

enum class AppIcon {
    BrandLogo,
    Home,
    Apps,
    Updates,
    Devices,
    Shield,
    Folder,
    Settings,
    Info,
    Sun,
    Moon,
    CheckCircle,
    AlertTriangle,
    Refresh,
    Heart,
    ChevronUp
};

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

// Semantic theme colors
ImVec4 SidebarBgColor();
ImVec4 CanvasBgColor();
ImVec4 CardBgColor();
ImVec4 CardElevatedBgColor();
ImVec4 MutedTextColor();
ImVec4 AccentTextColor();
ImVec4 WarningTextColor();
ImVec4 PanelBorderColor();
ImVec4 AccentBarColor();
ImVec4 CardHeaderTextColor();
ImVec4 HealthyColor();
ImVec4 WarningColor();
ImVec4 DangerColor();
ImVec4 InfoAccentColor();

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
void RenderStatusBadge(
    const ImVec4& color,
    const char* text,
    bool alignToFrame = false,
    float maxWidth = 0.0f);
bool RenderPillButton(const char* label, bool active, const ImVec2& size = ImVec2(0.0f, 0.0f));
float CalcSubSectionTabWidth(const char* label);
bool RenderSubSectionHeaderTab(const char* label, bool active);
void RenderSecondaryCardHeader(const char* title, const char* rightMeta = nullptr);
void RenderSecondaryGroupLabel(const char* text);
void RenderTableHeadersRow();
bool RenderPrimaryButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));
bool RenderSecondaryButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f));
bool RenderSecondaryButtonWithIcon(
    const char* label,
    AppIcon icon,
    const ImVec4& iconColor,
    const ImVec2& size = ImVec2(0.0f, 0.0f));
bool RenderActionButton(const char* label, const ImVec2& size, bool primary);

enum class TaskbarOverlayState {
    None,
    Attention,
    Critical
};
void SetTaskbarOverlayBadge(HWND hwnd, TaskbarOverlayState state);
void ShutdownTaskbarOverlay();
float CalcSummaryInfoBadgeWidth(const SummaryBadgeSegment* segments, size_t count);
void RenderSummaryInfoBadge(const SummaryBadgeSegment* segments, size_t count);

// Vector icons & modern layout widgets
void DrawAppIcon(ImDrawList* dl, ImVec2 center, float size, AppIcon icon, ImU32 color);
bool RenderSidebarNavItem(
    const char* id,
    AppIcon icon,
    const char* label,
    bool active,
    size_t badgeCount = 0,
    const ImVec4& badgeColor = ImVec4(0.96f, 0.65f, 0.14f, 1.00f),
    float width = 0.0f);
bool RenderIconButton(
    const char* id,
    AppIcon icon,
    const ImVec2& size,
    const char* tooltip = nullptr,
    bool active = false);
void RenderPageHeader(const char* title, const char* subtitle = nullptr);

int CompareVersionOrString(const std::string& a, const std::string& b);
