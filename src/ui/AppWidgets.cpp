#include "ui/AppWidgets.hpp"

#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <sstream>

std::atomic<bool> g_windowRegainedFocus{false};
bool g_isLightTheme = false;
bool g_isAdvancedMode = false;
bool g_autoCheckAppUpdates = true;
bool g_autoCheckOnlineUpdates = true;
bool g_pinLogPanel = false;
float g_dpiScale = 1.0f;
int g_uiScalePercent = 100;
bool g_pendingScaleReload = false;

float EffectiveUiScale() noexcept {
    const float dpi = (g_dpiScale > 0.5f ? g_dpiScale : 1.0f);
    const int clampedPct = std::clamp(g_uiScalePercent, 80, 175);
    return std::clamp(dpi * (static_cast<float>(clampedPct) / 100.0f), 0.75f, 3.0f);
}

ImFont* g_fontSmall = nullptr;
ImFont* g_fontRegular = nullptr;
ImFont* g_fontSecondaryHeader = nullptr;
ImFont* g_fontMainTab = nullptr;
ImFont* g_fontPrimaryHeader = nullptr;
ImTextureID g_appLogoTexture = 0;

void PushFontSafe(ImFont* font) {
    if (font != nullptr) {
        ImGui::PushFont(font, font->LegacySize);
    }
}

void PopFontSafe(ImFont* font) {
    if (font != nullptr) {
        ImGui::PopFont();
    }
}

void SetWindowTitleBarDark(HWND hwnd, bool useDark) {
    if (hwnd == nullptr) {
        return;
    }
    HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dwm == nullptr) {
        return;
    }
    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto fn = reinterpret_cast<DwmSetWindowAttributeFn>(
        reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute")));
    if (fn != nullptr) {
        const BOOL mode = useDark ? TRUE : FALSE;
        constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
        constexpr DWORD kDwmwaUseImmersiveDarkModeOld = 19;
        constexpr DWORD kDwmwaCaptionColor = 35;
        constexpr DWORD kDwmwaTextColor = 36;
        if (FAILED(fn(hwnd, kDwmwaUseImmersiveDarkMode, &mode, sizeof(mode)))) {
            fn(hwnd, kDwmwaUseImmersiveDarkModeOld, &mode, sizeof(mode));
        }
        // Windows 11 instant caption repaint matching the active theme
        const COLORREF captionCol = useDark ? RGB(12, 15, 22) : RGB(236, 240, 246);
        const COLORREF textCol = useDark ? RGB(238, 243, 250) : RGB(18, 25, 38);
        fn(hwnd, kDwmwaCaptionColor, &captionCol, sizeof(captionCol));
        fn(hwnd, kDwmwaTextColor, &textCol, sizeof(textCol));
    }
    FreeLibrary(dwm);
    SetWindowPos(
        hwnd,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
}

std::string UrlEncode(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 3);
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            encoded.push_back('+');
        } else {
            encoded.push_back('%');
            encoded.push_back(kHex[(c >> 4) & 0x0F]);
            encoded.push_back(kHex[c & 0x0F]);
        }
    }
    return encoded;
}

void OpenUrlInBrowser(const std::string& url) {
    size_t start = 0;
    size_t end = url.size();
    while (start < end && (url[start] == ' ' || url[start] == '\t' ||
                           url[start] == '\r' || url[start] == '\n')) {
        ++start;
    }
    while (end > start && (url[end - 1] == ' ' || url[end - 1] == '\t' ||
                           url[end - 1] == '\r' || url[end - 1] == '\n')) {
        --end;
    }
    const std::string_view clean(url.data() + start, end - start);
    if (clean.empty()) {
        return;
    }
    for (unsigned char ch : clean) {
        if (ch < 0x20 || ch == 0x7F) {
            return;
        }
    }
    const std::string lower = hdm::ToLowerAscii(clean.substr(0, 10));
    const bool allowed = (lower.rfind("https://", 0) == 0) ||
                         (lower.rfind("http://", 0) == 0) ||
                         (lower.rfind("mailto:", 0) == 0);
    if (!allowed) {
        return;
    }
    const std::wstring wUrl = hdm::Utf8ToWide(clean);
    ShellExecuteW(nullptr, L"open", wUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// --- Semantic Theme & Typography Helpers ---
ImVec4 SidebarBgColor() {
    return g_isLightTheme
               ? ImVec4(0.925f, 0.941f, 0.965f, 1.00f)
               : ImVec4(0.047f, 0.060f, 0.088f, 1.00f);
}

ImVec4 CanvasBgColor() {
    return g_isLightTheme
               ? ImVec4(0.961f, 0.969f, 0.984f, 1.00f)
               : ImVec4(0.063f, 0.080f, 0.118f, 1.00f);
}

ImVec4 CardBgColor() {
    return g_isLightTheme
               ? ImVec4(1.000f, 1.000f, 1.000f, 1.00f)
               : ImVec4(0.090f, 0.114f, 0.165f, 1.00f);
}

ImVec4 CardElevatedBgColor() {
    return g_isLightTheme
               ? ImVec4(0.969f, 0.978f, 0.992f, 1.00f)
               : ImVec4(0.114f, 0.145f, 0.208f, 1.00f);
}

ImVec4 MutedTextColor() {
    return g_isLightTheme
               ? ImVec4(0.37f, 0.43f, 0.53f, 1.00f)
               : ImVec4(0.58f, 0.65f, 0.76f, 1.00f);
}

ImVec4 AccentTextColor() {
    return g_isLightTheme
               ? ImVec4(0.09f, 0.38f, 0.80f, 1.00f)
               : ImVec4(0.42f, 0.73f, 1.00f, 1.00f);
}

ImVec4 WarningTextColor() {
    return g_isLightTheme
               ? ImVec4(0.68f, 0.42f, 0.02f, 1.00f)
               : ImVec4(0.98f, 0.74f, 0.18f, 1.00f);
}

ImVec4 PanelBorderColor() {
    return g_isLightTheme
               ? ImVec4(0.83f, 0.87f, 0.92f, 1.00f)
               : ImVec4(0.16f, 0.21f, 0.30f, 0.95f);
}

ImVec4 AccentBarColor() {
    return g_isLightTheme
               ? ImVec4(0.10f, 0.43f, 0.85f, 1.00f)
               : ImVec4(0.28f, 0.66f, 1.00f, 1.00f);
}

ImVec4 CardHeaderTextColor() {
    return g_isLightTheme
               ? ImVec4(0.09f, 0.16f, 0.28f, 1.00f)
               : ImVec4(0.90f, 0.94f, 1.00f, 1.00f);
}

ImVec4 HealthyColor() {
    return ImVec4(0.16f, 0.78f, 0.56f, 1.00f);
}

ImVec4 WarningColor() {
    return ImVec4(0.96f, 0.68f, 0.14f, 1.00f);
}

ImVec4 DangerColor() {
    return ImVec4(0.94f, 0.37f, 0.41f, 1.00f);
}

ImVec4 InfoAccentColor() {
    return g_isLightTheme
               ? ImVec4(0.10f, 0.43f, 0.85f, 1.00f)
               : ImVec4(0.32f, 0.68f, 1.00f, 1.00f);
}

void ApplyBaseLayoutStyle(ImGuiStyle& style, float scale) {
    style = ImGuiStyle();

    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(12.0f, 6.0f);
    style.CellPadding = ImVec2(10.0f, 7.0f);
    style.ItemSpacing = ImVec2(8.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.ScrollbarSize = 10.0f;
    style.GrabMinSize = 10.0f;

    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 1.0f;

    style.WindowRounding = 0.0f;
    style.ChildRounding = 10.0f;
    style.FrameRounding = 7.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 7.0f;

    if (scale > 0.5f && scale != 1.0f) {
        style.ScaleAllSizes(scale);
    }
    // ScaleAllSizes uses ImTrunc, which shrinks 1px borders/separators/caret to 0 at <100% scale.
    style.ChildBorderSize = std::max(1.0f, style.ChildBorderSize);
    style.PopupBorderSize = std::max(1.0f, style.PopupBorderSize);
    style.FrameBorderSize = std::max(1.0f, style.FrameBorderSize);
    style.TabBorderSize = std::max(1.0f, style.TabBorderSize);
    style.TabBarBorderSize = std::max(1.0f, style.TabBarBorderSize);
    style.SeparatorSize = std::max(1.0f, style.SeparatorSize);
    style.InputTextCursorSize = std::max(1.0f, style.InputTextCursorSize);
    style.MouseCursorScale = std::max(1.0f, style.MouseCursorScale);
}

void ApplyModernDarkThemeColors(ImGuiStyle& style) {
    ImGui::StyleColorsDark(&style);
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = ImVec4(0.94f, 0.96f, 0.99f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.58f, 0.65f, 0.76f, 1.00f);
    c[ImGuiCol_WindowBg] = CanvasBgColor();
    c[ImGuiCol_ChildBg] = CardBgColor();
    c[ImGuiCol_PopupBg] = ImVec4(0.102f, 0.129f, 0.184f, 0.99f);
    c[ImGuiCol_Border] = PanelBorderColor();
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    c[ImGuiCol_FrameBg] = ImVec4(0.118f, 0.145f, 0.204f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.153f, 0.188f, 0.263f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.188f, 0.231f, 0.322f, 1.00f);

    c[ImGuiCol_TitleBg] = SidebarBgColor();
    c[ImGuiCol_TitleBgActive] = CardBgColor();
    c[ImGuiCol_MenuBarBg] = SidebarBgColor();

    c[ImGuiCol_ScrollbarBg] = ImVec4(0.05f, 0.07f, 0.10f, 0.35f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.22f, 0.28f, 0.38f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30f, 0.38f, 0.50f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.38f, 0.47f, 0.62f, 1.00f);

    c[ImGuiCol_CheckMark] = ImVec4(0.36f, 0.68f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.36f, 0.68f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.48f, 0.76f, 1.00f, 1.00f);

    c[ImGuiCol_Button] = ImVec4(0.18f, 0.47f, 0.92f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.54f, 0.98f, 1.00f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.14f, 0.39f, 0.78f, 1.00f);

    c[ImGuiCol_Header] = ImVec4(0.20f, 0.47f, 0.88f, 0.28f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.52f, 0.92f, 0.40f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.24f, 0.52f, 0.92f, 0.54f);

    c[ImGuiCol_Separator] = ImVec4(0.16f, 0.21f, 0.30f, 0.85f);
    c[ImGuiCol_Tab] = ImVec4(0.094f, 0.118f, 0.169f, 1.00f);
    c[ImGuiCol_TabHovered] = ImVec4(0.20f, 0.46f, 0.85f, 0.88f);
    c[ImGuiCol_TabSelected] = ImVec4(0.18f, 0.44f, 0.84f, 1.00f);

    c[ImGuiCol_TableHeaderBg] = ImVec4(0.114f, 0.145f, 0.208f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.19f, 0.25f, 0.36f, 1.00f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.13f, 0.17f, 0.24f, 0.80f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.022f);

    c[ImGuiCol_PlotHistogram] = ImVec4(0.24f, 0.56f, 0.98f, 1.00f);
}

void ApplyModernLightThemeColors(ImGuiStyle& style) {
    ImGui::StyleColorsLight(&style);
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = ImVec4(0.07f, 0.10f, 0.16f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.39f, 0.45f, 0.55f, 1.00f);
    c[ImGuiCol_WindowBg] = CanvasBgColor();
    c[ImGuiCol_ChildBg] = CardBgColor();
    c[ImGuiCol_PopupBg] = ImVec4(1.000f, 1.000f, 1.000f, 0.99f);
    c[ImGuiCol_Border] = PanelBorderColor();
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    c[ImGuiCol_FrameBg] = ImVec4(0.95f, 0.965f, 0.985f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.90f, 0.93f, 0.97f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.85f, 0.89f, 0.95f, 1.00f);

    c[ImGuiCol_TitleBg] = SidebarBgColor();
    c[ImGuiCol_TitleBgActive] = CardBgColor();
    c[ImGuiCol_MenuBarBg] = SidebarBgColor();

    c[ImGuiCol_ScrollbarBg] = ImVec4(0.93f, 0.95f, 0.98f, 0.65f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.74f, 0.79f, 0.86f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.60f, 0.67f, 0.77f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.46f, 0.55f, 0.68f, 1.00f);

    c[ImGuiCol_CheckMark] = ImVec4(0.10f, 0.42f, 0.85f, 1.00f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.10f, 0.42f, 0.85f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.07f, 0.34f, 0.72f, 1.00f);

    // Default ImGui button stays light-neutral so dark ImGuiCol_Text always contrasts cleanly;
    // primary CTAs explicitly use RenderPrimaryButton() with crisp white text.
    c[ImGuiCol_Button] = ImVec4(0.94f, 0.96f, 0.98f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.88f, 0.92f, 0.97f, 1.00f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.82f, 0.87f, 0.95f, 1.00f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.06f, 0.09f, 0.16f, 0.38f);

    c[ImGuiCol_Header] = ImVec4(0.10f, 0.43f, 0.85f, 0.14f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.10f, 0.43f, 0.85f, 0.22f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.10f, 0.43f, 0.85f, 0.32f);

    c[ImGuiCol_Separator] = ImVec4(0.84f, 0.88f, 0.93f, 0.95f);
    c[ImGuiCol_Tab] = ImVec4(0.91f, 0.93f, 0.96f, 1.00f);
    c[ImGuiCol_TabHovered] = ImVec4(0.82f, 0.89f, 0.98f, 1.00f);
    c[ImGuiCol_TabSelected] = ImVec4(0.78f, 0.87f, 0.99f, 1.00f);

    c[ImGuiCol_TableHeaderBg] = ImVec4(0.94f, 0.955f, 0.98f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.82f, 0.86f, 0.92f, 1.00f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.89f, 0.92f, 0.95f, 0.95f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(0.0f, 0.15f, 0.35f, 0.025f);

    c[ImGuiCol_PlotHistogram] = ImVec4(0.10f, 0.43f, 0.85f, 1.00f);
}

void ApplyAppTheme(HWND hwnd, float scale, bool lightTheme, bool initLayout) {
    g_isLightTheme = lightTheme;
    ImGuiStyle& style = ImGui::GetStyle();
    if (initLayout) {
        ApplyBaseLayoutStyle(style, scale);
    }
    if (lightTheme) {
        ApplyModernLightThemeColors(style);
    } else {
        ApplyModernDarkThemeColors(style);
    }
    SetWindowTitleBarDark(hwnd, !lightTheme);
}

constexpr wchar_t kPrefsRegSubKey[] = L"Software\\DriverLoom";

void LoadUserPreferences() noexcept {
    hdm::RegKeyGuard key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kPrefsRegSubKey, 0, KEY_READ, key.Put()) != ERROR_SUCCESS) {
        return;
    }

    auto readDword = [&key](const wchar_t* name, DWORD& outVal) -> bool {
        DWORD type = 0;
        DWORD val = 0;
        DWORD cb = sizeof(val);
        if (RegQueryValueExW(
                key.Get(),
                name,
                nullptr,
                &type,
                reinterpret_cast<LPBYTE>(&val),
                &cb) == ERROR_SUCCESS &&
            type == REG_DWORD && cb == sizeof(DWORD)) {
            outVal = val;
            return true;
        }
        return false;
    };

    DWORD dw = 0;
    if (readDword(L"AdvancedMode", dw)) {
        g_isAdvancedMode = (dw != 0);
    }
    if (readDword(L"LightTheme", dw)) {
        g_isLightTheme = (dw != 0);
    }
    if (readDword(L"AutoCheckUpdates", dw)) {
        g_autoCheckAppUpdates = (dw != 0);
    }
    if (readDword(L"AutoCheckOnlineUpdates", dw)) {
        g_autoCheckOnlineUpdates = (dw != 0);
    }
    if (readDword(L"PinLogPanel", dw)) {
        g_pinLogPanel = (dw != 0);
    }
    if (readDword(L"UiScalePercent", dw)) {
        if (dw >= 80 && dw <= 175) {
            g_uiScalePercent = static_cast<int>(dw);
        }
    }
    if (readDword(L"Language", dw)) {
        if (dw == static_cast<DWORD>(hdm::Lang::Ru)) {
            hdm::SetLang(hdm::Lang::Ru);
        } else if (dw == static_cast<DWORD>(hdm::Lang::Uk)) {
            hdm::SetLang(hdm::Lang::Uk);
        } else if (dw == static_cast<DWORD>(hdm::Lang::En)) {
            hdm::SetLang(hdm::Lang::En);
        }
    }
}

void SaveUserPreferences() noexcept {
    hdm::RegKeyGuard key;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kPrefsRegSubKey,
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_WRITE,
            nullptr,
            key.Put(),
            nullptr) != ERROR_SUCCESS) {
        return;
    }

    auto writeDword = [&key](const wchar_t* name, DWORD val) {
        RegSetValueExW(
            key.Get(),
            name,
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&val),
            sizeof(val));
    };

    writeDword(L"AdvancedMode", g_isAdvancedMode ? 1u : 0u);
    writeDword(L"LightTheme", g_isLightTheme ? 1u : 0u);
    writeDword(L"AutoCheckUpdates", g_autoCheckAppUpdates ? 1u : 0u);
    writeDword(L"AutoCheckOnlineUpdates", g_autoCheckOnlineUpdates ? 1u : 0u);
    writeDword(L"PinLogPanel", g_pinLogPanel ? 1u : 0u);
    writeDword(L"UiScalePercent", static_cast<DWORD>(std::clamp(g_uiScalePercent, 80, 175)));
    writeDword(L"Language", static_cast<DWORD>(hdm::GetLang()));
}

constexpr wchar_t kSnapshotRegSubKey[] = L"Software\\DriverLoom\\BaselineSnapshot";

std::string SanitizeTabField(std::string s) {
    for (char& ch : s) {
        if (ch == '\t' || ch == '\r' || ch == '\n') {
            ch = ' ';
        }
    }
    return s;
}

std::string FormatSnapshotTimestampNow() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[32];
    std::snprintf(
        buf,
        sizeof(buf),
        "%04u-%02u-%02u %02u:%02u:%02u",
        static_cast<unsigned>(st.wYear),
        static_cast<unsigned>(st.wMonth),
        static_cast<unsigned>(st.wDay),
        static_cast<unsigned>(st.wHour),
        static_cast<unsigned>(st.wMinute),
        static_cast<unsigned>(st.wSecond));
    return buf;
}

void SaveBaselineSnapshotToRegistry(
    const std::string& timestamp,
    const std::vector<DriverBaselineEntry>& entries) {
    hdm::RegKeyGuard key;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kSnapshotRegSubKey,
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_WRITE,
            nullptr,
            key.Put(),
            nullptr) != ERROR_SUCCESS) {
        return;
    }

    auto writeWideSz = [&key](const wchar_t* name, const std::wstring& val) {
        const DWORD bytes = static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t));
        RegSetValueExW(
            key.Get(),
            name,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(val.c_str()),
            bytes);
    };

    std::string payload;
    payload.reserve(entries.size() * 140);
    for (const auto& e : entries) {
        if (!payload.empty()) {
            payload.push_back('\n');
        }
        payload += SanitizeTabField(e.instanceId);
        payload.push_back('\t');
        payload += SanitizeTabField(e.name);
        payload.push_back('\t');
        payload += SanitizeTabField(e.deviceClass);
        payload.push_back('\t');
        payload += SanitizeTabField(e.driverVersion);
        payload.push_back('\t');
        payload += SanitizeTabField(e.driverDate);
        payload.push_back('\t');
        payload += SanitizeTabField(e.infName);
        payload.push_back('\t');
        payload += SanitizeTabField(e.statusText);
        payload.push_back('\t');
        payload += std::to_string(e.health);
    }

    writeWideSz(L"Timestamp", hdm::Utf8ToWide(timestamp));
    writeWideSz(L"Payload", hdm::Utf8ToWide(payload));
}

bool LoadBaselineSnapshotFromRegistry(
    std::string& outTimestamp,
    std::vector<DriverBaselineEntry>& outEntries) {
    outTimestamp.clear();
    outEntries.clear();

    hdm::RegKeyGuard key;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            kSnapshotRegSubKey,
            0,
            KEY_READ,
            key.Put()) != ERROR_SUCCESS) {
        return false;
    }

    auto readUtf8Sz = [&key](const wchar_t* name, std::string& outStr) -> bool {
        DWORD type = 0;
        DWORD bytes = 0;
        if (RegQueryValueExW(key.Get(), name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
            type != REG_SZ || bytes < sizeof(wchar_t)) {
            return false;
        }
        std::vector<wchar_t> buf;
        LSTATUS st = ERROR_MORE_DATA;
        for (int attempt = 0; attempt < 4; ++attempt) {
            buf.assign(bytes / sizeof(wchar_t) + 2, L'\0');
            DWORD bufBytes = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
            st = RegQueryValueExW(
                key.Get(),
                name,
                nullptr,
                &type,
                reinterpret_cast<LPBYTE>(buf.data()),
                &bufBytes);
            if (st == ERROR_SUCCESS && type == REG_SZ) {
                bytes = bufBytes;
                break;
            }
            if (st != ERROR_MORE_DATA) {
                return false;
            }
            bytes = bufBytes;
        }
        if (st != ERROR_SUCCESS) {
            return false;
        }
        const size_t maxChars = bytes / sizeof(wchar_t);
        size_t len = 0;
        while (len < maxChars && buf[len] != L'\0') {
            ++len;
        }
        outStr = hdm::WideToUtf8(std::wstring_view(buf.data(), len));
        return true;
    };

    if (!readUtf8Sz(L"Timestamp", outTimestamp)) {
        return false;
    }
    std::string payload;
    if (!readUtf8Sz(L"Payload", payload) || payload.empty()) {
        return false;
    }

    std::istringstream iss(payload);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        std::string fields[8];
        size_t fieldIdx = 0;
        size_t start = 0;
        while (fieldIdx < 7) {
            const size_t tabPos = line.find('\t', start);
            if (tabPos == std::string::npos) {
                break;
            }
            fields[fieldIdx++] = line.substr(start, tabPos - start);
            start = tabPos + 1;
        }
        fields[fieldIdx] = line.substr(start);
        if (fieldIdx < 7 || fields[0].empty()) {
            continue;
        }

        DriverBaselineEntry entry;
        entry.instanceId = std::move(fields[0]);
        entry.name = std::move(fields[1]);
        entry.deviceClass = std::move(fields[2]);
        entry.driverVersion = std::move(fields[3]);
        entry.driverDate = std::move(fields[4]);
        entry.infName = std::move(fields[5]);
        entry.statusText = std::move(fields[6]);
        entry.health = std::atoi(fields[7].c_str());
        outEntries.push_back(std::move(entry));
    }
    return !outEntries.empty();
}

std::string EllipsizeUtf8(const std::string& text, float maxWidth) {
    if (text.empty() || maxWidth <= 8.0f) {
        return text;
    }
    if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth) {
        return text;
    }
    const char* ellipsis = "...";
    const float ellipsisW = ImGui::CalcTextSize(ellipsis).x;
    if (maxWidth <= ellipsisW) {
        return ellipsis;
    }

    size_t len = text.size();
    while (len > 0) {
        --len;
        while (len > 0 && (static_cast<unsigned char>(text[len]) & 0xC0u) == 0x80u) {
            --len;
        }
        size_t cleanLen = len;
        while (cleanLen > 0 && text[cleanLen - 1] == ' ') {
            --cleanLen;
        }
        std::string candidate = text.substr(0, cleanLen);
        candidate += ellipsis;
        if (ImGui::CalcTextSize(candidate.c_str()).x <= maxWidth) {
            return candidate;
        }
    }
    return ellipsis;
}

bool RenderCellText(const std::string& text, bool dimmed, bool alignToFrame) {
    if (alignToFrame) {
        ImGui::AlignTextToFramePadding();
    }
    const std::string raw = text.empty() ? "—" : text;
    const float availW = ImGui::GetContentRegionAvail().x - 2.0f;
    const std::string shown = EllipsizeUtf8(raw, availW);
    if (dimmed) {
        ImGui::TextDisabled("%s", shown.c_str());
    } else {
        ImGui::TextUnformatted(shown.c_str());
    }
    return (shown != raw);
}

void RenderStatusBadge(const ImVec4& color, const char* text, bool alignToFrame, float maxWidth) {
    const float uiScale = EffectiveUiScale();
    const float textH = ImGui::GetTextLineHeight();
    const float frameH = ImGui::GetFrameHeight();
    const float badgeH = textH + std::round(6.0f * uiScale);
    const float topOffset = alignToFrame ? std::max(0.0f, (frameH - badgeH) * 0.5f) : 0.0f;

    const float padLeft = std::round(8.0f * uiScale);
    const float padRight = std::round(9.0f * uiScale);
    const float radius = std::max(3.2f, textH * 0.21f);
    const float dotToText = std::round(6.0f * uiScale);
    float availW = std::max(44.0f * uiScale, ImGui::GetContentRegionAvail().x - 2.0f);
    if (maxWidth > 0.0f) {
        availW = std::min(availW, std::max(44.0f * uiScale, maxWidth));
    }
    const float maxTextW = std::max(12.0f, availW - (padLeft + radius * 2.0f + dotToText + padRight));

    const std::string shown = EllipsizeUtf8(text != nullptr ? std::string(text) : "", maxTextW);
    const ImVec2 textSize = ImGui::CalcTextSize(shown.c_str());
    const float pillW = std::min(
        availW,
        padLeft + radius * 2.0f + dotToText + textSize.x + padRight);

    const ImVec2 startScreen = ImGui::GetCursorScreenPos();
    const ImVec2 pMin(startScreen.x, startScreen.y + topOffset);
    const ImVec2 pMax(pMin.x + pillW, pMin.y + badgeH);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec4 bgVec(color.x, color.y, color.z, g_isLightTheme ? 0.14f : 0.14f);
    const ImVec4 borderVec =
        g_isLightTheme
            ? ImVec4(color.x * 0.72f, color.y * 0.72f, color.z * 0.72f, 0.38f)
            : ImVec4(color.x, color.y, color.z, 0.36f);
    const ImVec4 fgVec =
        g_isLightTheme
            ? ImVec4(color.x * 0.48f, color.y * 0.48f, color.z * 0.48f, 1.00f)
            : color;
    const float rounding = badgeH * 0.5f;
    dl->AddRectFilled(pMin, pMax, ImGui::ColorConvertFloat4ToU32(bgVec), rounding);
    dl->AddRect(pMin, pMax, ImGui::ColorConvertFloat4ToU32(borderVec), rounding, 0, 1.0f);

    const ImVec2 dotCenter(pMin.x + padLeft + radius, pMin.y + badgeH * 0.5f);
    dl->AddCircleFilled(dotCenter, radius, ImGui::ColorConvertFloat4ToU32(fgVec));

    const ImVec2 textPos(
        pMin.x + padLeft + radius * 2.0f + dotToText,
        pMin.y + (badgeH - textH) * 0.5f);
    dl->AddText(textPos, ImGui::ColorConvertFloat4ToU32(fgVec), shown.c_str());

    ImGui::Dummy(ImVec2(pillW, alignToFrame ? std::max(frameH, badgeH) : badgeH));
}

bool RenderPillButton(const char* label, bool active, const ImVec2& size) {
    const float uiScale = EffectiveUiScale();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, std::round(14.0f * uiScale));
    if (g_isLightTheme) {
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.86f, 0.92f, 0.99f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.88f, 0.98f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.74f, 0.84f, 0.97f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.18f, 0.48f, 0.86f, 0.70f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.28f, 0.62f, 1.00f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.93f, 0.95f, 0.98f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.88f, 0.91f, 0.96f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.82f, 0.86f, 0.92f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30f, 0.36f, 0.46f, 1.00f));
        }
    } else {
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.24f, 0.40f, 0.98f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.30f, 0.49f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.21f, 0.35f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.32f, 0.60f, 0.95f, 0.78f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.96f, 1.00f, 1.00f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.105f, 0.130f, 0.184f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.145f, 0.180f, 0.250f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.090f, 0.112f, 0.160f, 0.98f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.18f, 0.23f, 0.32f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.66f, 0.73f, 0.84f, 1.00f));
        }
    }
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar();
    return clicked;
}

float CalcSubSectionTabWidth(const char* label) {
    const float uiScale = EffectiveUiScale();
    PushFontSafe(g_fontSecondaryHeader);
    const float w = ImGui::CalcTextSize(label, nullptr, true).x + std::round(14.0f * uiScale) * 2.0f;
    PopFontSafe(g_fontSecondaryHeader);
    return w;
}

bool RenderSubSectionHeaderTab(const char* label, bool active) {
    const float uiScale = EffectiveUiScale();
    PushFontSafe(g_fontSecondaryHeader);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, std::round(7.0f * uiScale));
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding,
        ImVec2(std::round(14.0f * uiScale), std::round(6.0f * uiScale)));
    if (g_isLightTheme) {
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.88f, 0.93f, 1.00f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.83f, 0.90f, 0.99f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.78f, 0.86f, 0.98f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.20f, 0.49f, 0.86f, 0.72f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.27f, 0.62f, 1.00f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.00f, 1.00f, 1.00f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.93f, 0.95f, 0.98f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.88f, 0.91f, 0.96f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.83f, 0.87f, 0.92f, 0.90f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.32f, 0.39f, 0.50f, 1.00f));
        }
    } else {
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.125f, 0.215f, 0.355f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.155f, 0.260f, 0.425f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.105f, 0.185f, 0.305f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.28f, 0.60f, 0.95f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.98f, 1.00f, 1.00f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.094f, 0.118f, 0.169f, 0.92f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.130f, 0.165f, 0.235f, 0.98f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.082f, 0.104f, 0.148f, 0.98f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.17f, 0.22f, 0.30f, 0.80f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.64f, 0.72f, 0.84f, 1.00f));
        }
    }
    const bool clicked = ImGui::Button(label);
    if (active && ImGui::IsItemVisible()) {
        const ImVec2 rMin = ImGui::GetItemRectMin();
        const ImVec2 rMax = ImGui::GetItemRectMax();
        const float lineH = std::max(2.0f, 2.5f * uiScale);
        const float y = rMax.y - lineH * 0.5f;
        const float pad = std::round(6.0f * uiScale);
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(rMin.x + pad, y),
            ImVec2(rMax.x - pad, y),
            ImGui::ColorConvertFloat4ToU32(AccentBarColor()),
            lineH);
    }
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(2);
    PopFontSafe(g_fontSecondaryHeader);
    return clicked;
}

void RenderSecondaryCardHeader(const char* title, const char* rightMeta) {
    const float uiScale = EffectiveUiScale();
    PushFontSafe(g_fontSecondaryHeader);
    const float textH = ImGui::GetTextLineHeight();
    const float availW = std::max(60.0f, ImGui::GetContentRegionAvail().x);
    const float headerH = textH + std::round(4.0f * uiScale);

    const ImVec2 pMin = ImGui::GetCursorScreenPos();
    const ImVec2 pMax(pMin.x + availW, pMin.y + headerH);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float barW = std::max(2.5f, std::round(3.0f * uiScale));
    const float barPadY = std::round(2.0f * uiScale);
    dl->AddRectFilled(
        ImVec2(pMin.x, pMin.y + barPadY),
        ImVec2(pMin.x + barW, pMax.y - barPadY),
        ImGui::ColorConvertFloat4ToU32(AccentBarColor()),
        barW * 0.5f);

    float rightReserve = 0.0f;
    if (rightMeta != nullptr && rightMeta[0] != '\0') {
        const ImVec2 metaSize = ImGui::CalcTextSize(rightMeta);
        rightReserve = metaSize.x + std::round(10.0f * uiScale);
        dl->AddText(
            ImVec2(pMax.x - metaSize.x, pMin.y + (headerH - textH) * 0.5f),
            ImGui::ColorConvertFloat4ToU32(MutedTextColor()),
            rightMeta);
    }

    const float titleOffset = barW + std::round(8.0f * uiScale);
    const float maxTitleW = std::max(24.0f, availW - titleOffset - rightReserve);
    const std::string shownTitle = EllipsizeUtf8(title != nullptr ? title : "", maxTitleW);
    dl->AddText(
        ImVec2(pMin.x + titleOffset, pMin.y + (headerH - textH) * 0.5f),
        ImGui::ColorConvertFloat4ToU32(CardHeaderTextColor()),
        shownTitle.c_str());

    ImGui::Dummy(ImVec2(availW, headerH));
    PopFontSafe(g_fontSecondaryHeader);
}

void RenderSecondaryGroupLabel(const char* text) {
    PushFontSafe(g_fontSecondaryHeader);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(MutedTextColor(), "%s", text);
    PopFontSafe(g_fontSecondaryHeader);
}

void RenderTableHeadersRow() {
    PushFontSafe(g_fontSecondaryHeader);
    const ImVec4 hdrCol = g_isLightTheme
                              ? ImVec4(0.16f, 0.23f, 0.35f, 1.00f)
                              : ImVec4(0.76f, 0.84f, 0.96f, 1.00f);
    ImGui::PushStyleColor(ImGuiCol_Text, hdrCol);
    ImGui::TableHeadersRow();
    ImGui::PopStyleColor();
    PopFontSafe(g_fontSecondaryHeader);
}

bool RenderPrimaryButton(const char* label, const ImVec2& size) {
    const bool isDisabled = (ImGui::GetStyle().Alpha < 0.95f);
    if (g_isLightTheme) {
        if (isDisabled) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.84f, 0.88f, 0.93f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.84f, 0.88f, 0.93f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.84f, 0.88f, 0.93f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.75f, 0.80f, 0.88f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.24f, 0.31f, 0.42f, 1.00f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.43f, 0.85f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.08f, 0.36f, 0.76f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.06f, 0.30f, 0.66f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.08f, 0.36f, 0.75f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
        }
    } else {
        if (isDisabled) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.22f, 0.33f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.22f, 0.33f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.22f, 0.33f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.24f, 0.31f, 0.43f, 0.75f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.76f, 0.82f, 0.92f, 1.00f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.48f, 0.94f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.55f, 1.00f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.14f, 0.40f, 0.82f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.32f, 0.62f, 1.00f, 0.55f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
        }
    }
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(5);
    return clicked;
}

bool RenderSecondaryButton(const char* label, const ImVec2& size) {
    if (g_isLightTheme) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.93f, 0.95f, 0.98f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.87f, 0.91f, 0.96f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.78f, 0.83f, 0.90f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.12f, 0.17f, 0.26f, 1.00f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.125f, 0.155f, 0.218f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.170f, 0.212f, 0.294f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.105f, 0.132f, 0.188f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.22f, 0.28f, 0.39f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.89f, 0.93f, 0.98f, 1.00f));
    }
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(5);
    return clicked;
}

bool RenderActionButton(const char* label, const ImVec2& size, bool primary) {
    if (primary) {
        return RenderPrimaryButton(label, size);
    }
    return RenderSecondaryButton(label, size);
}

bool RenderSecondaryButtonWithIcon(
    const char* label,
    AppIcon icon,
    const ImVec4& iconColor,
    const ImVec2& size) {
    if (label == nullptr) {
        return false;
    }
    const std::string_view full(label);
    const size_t hashPos = full.find("##");
    const std::string_view displayPart =
        (hashPos != std::string_view::npos) ? full.substr(0, hashPos) : full;
    const std::string hiddenId =
        (hashPos != std::string_view::npos) ? std::string(full.substr(hashPos))
                                            : (std::string("##") + std::string(full));

    const ImGuiStyle& style = ImGui::GetStyle();
    const float uiScale = EffectiveUiScale();
    const float iconSz = std::round(13.5f * uiScale);
    const float iconGap = std::round(7.0f * uiScale);
    const ImVec2 txtSz =
        displayPart.empty()
            ? ImVec2(0.0f, ImGui::GetFontSize())
            : ImGui::CalcTextSize(displayPart.data(), displayPart.data() + displayPart.size());
    const float contentW = iconSz + (displayPart.empty() ? 0.0f : (iconGap + txtSz.x));
    const float autoW = style.FramePadding.x * 2.0f + contentW;
    const float btnW = (size.x > 0.0f) ? size.x : autoW;
    const float btnH = (size.y > 0.0f) ? size.y : ImGui::GetFrameHeight();

    const bool clicked = RenderSecondaryButton(hiddenId.c_str(), ImVec2(btnW, btnH));
    if (ImGui::IsItemVisible()) {
        const ImVec2 rMin = ImGui::GetItemRectMin();
        const ImVec2 rMax = ImGui::GetItemRectMax();
        const float startX =
            rMin.x + std::max(style.FramePadding.x, (rMax.x - rMin.x - contentW) * 0.5f);
        const float centerY = (rMin.y + rMax.y) * 0.5f;

        const float alpha = style.Alpha;
        ImVec4 icCol = iconColor;
        icCol.w *= alpha;
        const ImVec4 txtCol = g_isLightTheme ? ImVec4(0.10f, 0.14f, 0.21f, alpha)
                                             : ImVec4(0.89f, 0.93f, 0.98f, alpha);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        DrawAppIcon(
            dl,
            ImVec2(startX + iconSz * 0.5f, centerY),
            iconSz,
            icon,
            ImGui::ColorConvertFloat4ToU32(icCol));
        if (!displayPart.empty()) {
            dl->AddText(
                ImVec2(startX + iconSz + iconGap, centerY - txtSz.y * 0.5f),
                ImGui::ColorConvertFloat4ToU32(txtCol),
                displayPart.data(),
                displayPart.data() + displayPart.size());
        }
    }
    return clicked;
}

namespace {

HICON CreateTaskbarBadgeIcon(bool critical) {
    constexpr int kSize = 16;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = kSize;
    bi.bV5Height = -kSize;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000u;
    bi.bV5GreenMask = 0x0000FF00u;
    bi.bV5BlueMask = 0x000000FFu;
    bi.bV5AlphaMask = 0xFF000000u;

    void* bits = nullptr;
    HDC screenDc = GetDC(nullptr);
    HBITMAP colorBmp = CreateDIBSection(
        screenDc, reinterpret_cast<const BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screenDc);
    if (colorBmp == nullptr || bits == nullptr) {
        if (colorBmp != nullptr) DeleteObject(colorBmp);
        return nullptr;
    }

    auto premulPixel = [](uint8_t r, uint8_t g, uint8_t b, float aFrac) -> uint32_t {
        const float a = std::clamp(aFrac, 0.0f, 1.0f);
        const auto au = static_cast<uint32_t>(std::lround(a * 255.0f));
        const auto ru = static_cast<uint32_t>(std::lround(static_cast<float>(r) * a));
        const auto gu = static_cast<uint32_t>(std::lround(static_cast<float>(g) * a));
        const auto bu = static_cast<uint32_t>(std::lround(static_cast<float>(b) * a));
        return (au << 24) | (ru << 16) | (gu << 8) | bu;
    };

    const uint8_t fillR = critical ? 239 : 245;
    const uint8_t fillG = critical ? 68 : 158;
    const uint8_t fillB = critical ? 68 : 11;

    auto* px = static_cast<uint32_t*>(bits);
    constexpr float cx = 7.5f;
    constexpr float cy = 7.5f;
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float dx = static_cast<float>(x) - cx;
            const float dy = static_cast<float>(y) - cy;
            const float dist = std::sqrt(dx * dx + dy * dy);
            uint32_t col = 0;
            if (dist <= 7.3f) {
                const float edgeAlpha = std::clamp(7.3f - dist, 0.0f, 1.0f);
                if (dist > 6.1f) {
                    // Dark translucent rim so badge stands out on both light and dark taskbars.
                    col = premulPixel(16, 20, 28, edgeAlpha * 0.90f);
                } else {
                    const bool drawExclamation =
                        critical && (x == 7 || x == 8) && ((y >= 3 && y <= 9) || (y >= 11 && y <= 12));
                    col = drawExclamation ? premulPixel(255, 255, 255, 1.0f)
                                          : premulPixel(fillR, fillG, fillB, 1.0f);
                }
            }
            px[y * kSize + x] = col;
        }
    }

    HBITMAP maskBmp = CreateBitmap(kSize, kSize, 1, 1, nullptr);
    if (maskBmp == nullptr) {
        DeleteObject(colorBmp);
        return nullptr;
    }

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = maskBmp;
    ii.hbmColor = colorBmp;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(colorBmp);
    DeleteObject(maskBmp);
    return icon;
}

struct TaskbarOverlayCache {
    hdm::ComPtr<ITaskbarList3> taskbar;
    bool initAttempted{false};
    HICON attentionIcon{nullptr};
    HICON criticalIcon{nullptr};
    HWND lastHwnd{nullptr};
} g_taskbarCache;

} // namespace

void SetTaskbarOverlayBadge(HWND hwnd, TaskbarOverlayState state) {
    if (hwnd == nullptr) {
        return;
    }
    if (!g_taskbarCache.initAttempted) {
        g_taskbarCache.initAttempted = true;
        if (SUCCEEDED(CoCreateInstance(
                CLSID_TaskbarList,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_ITaskbarList3,
                reinterpret_cast<void**>(g_taskbarCache.taskbar.Put()))) &&
            g_taskbarCache.taskbar) {
            if (FAILED(g_taskbarCache.taskbar->HrInit())) {
                g_taskbarCache.taskbar.Reset();
            }
        }
    }
    if (!g_taskbarCache.taskbar) {
        return;
    }

    g_taskbarCache.lastHwnd = hwnd;
    HICON icon = nullptr;
    const wchar_t* desc = nullptr;
    if (state == TaskbarOverlayState::Critical) {
        if (g_taskbarCache.criticalIcon == nullptr) {
            g_taskbarCache.criticalIcon = CreateTaskbarBadgeIcon(true);
        }
        icon = g_taskbarCache.criticalIcon;
        desc = L"Critical driver or service issue";
    } else if (state == TaskbarOverlayState::Attention) {
        if (g_taskbarCache.attentionIcon == nullptr) {
            g_taskbarCache.attentionIcon = CreateTaskbarBadgeIcon(false);
        }
        icon = g_taskbarCache.attentionIcon;
        desc = L"Updates available";
    }
    g_taskbarCache.taskbar->SetOverlayIcon(hwnd, icon, desc);
}

void ShutdownTaskbarOverlay() {
    if (g_taskbarCache.taskbar && g_taskbarCache.lastHwnd != nullptr && IsWindow(g_taskbarCache.lastHwnd)) {
        g_taskbarCache.taskbar->SetOverlayIcon(g_taskbarCache.lastHwnd, nullptr, nullptr);
    }
    if (g_taskbarCache.attentionIcon != nullptr) {
        DestroyIcon(g_taskbarCache.attentionIcon);
        g_taskbarCache.attentionIcon = nullptr;
    }
    if (g_taskbarCache.criticalIcon != nullptr) {
        DestroyIcon(g_taskbarCache.criticalIcon);
        g_taskbarCache.criticalIcon = nullptr;
    }
    g_taskbarCache.taskbar.Reset();
    g_taskbarCache.lastHwnd = nullptr;
    g_taskbarCache.initAttempted = false;
}

float CalcSummaryInfoBadgeWidth(const SummaryBadgeSegment* segments, size_t count) {
    if (segments == nullptr || count == 0) {
        return 0.0f;
    }
    const float uiScale = EffectiveUiScale();
    const float padX = std::round(9.0f * uiScale);
    const float gap = std::round(4.0f * uiScale);
    const float sepPad = std::round(6.0f * uiScale);
    const float sepTotal = sepPad * 2.0f + 1.0f;

    float total = padX * 2.0f;
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) {
            total += sepTotal;
        }
        const SummaryBadgeSegment& seg = segments[i];
        const float labelW =
            (seg.label != nullptr && seg.label[0] != '\0') ? ImGui::CalcTextSize(seg.label).x : 0.0f;
        float valueW = 0.0f;
        if (seg.value != nullptr && seg.value[0] != '\0') {
            PushFontSafe(g_fontSecondaryHeader);
            valueW = ImGui::CalcTextSize(seg.value).x;
            PopFontSafe(g_fontSecondaryHeader);
        }
        total += labelW + (labelW > 0.0f && valueW > 0.0f ? gap : 0.0f) + valueW;
    }
    return total;
}

void RenderSummaryInfoBadge(const SummaryBadgeSegment* segments, size_t count) {
    if (segments == nullptr || count == 0) {
        return;
    }
    const float uiScale = EffectiveUiScale();
    const float padX = std::round(9.0f * uiScale);
    const float gap = std::round(4.0f * uiScale);
    const float sepPad = std::round(6.0f * uiScale);
    const float frameH = ImGui::GetFrameHeight();
    const float badgeW = CalcSummaryInfoBadgeWidth(segments, count);

    const ImVec2 pMin = ImGui::GetCursorScreenPos();
    const ImVec2 pMax(pMin.x + badgeW, pMin.y + frameH);
    ImGui::Dummy(ImVec2(badgeW, frameH));

    const ImVec4 bgCol = CardBgColor();
    const ImVec4 borderCol = PanelBorderColor();
    const ImU32 sepCol = ImGui::ColorConvertFloat4ToU32(PanelBorderColor());
    const ImVec4 defaultLabelCol = MutedTextColor();
    const ImVec4 tintedLabelCol = WarningTextColor();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = std::round(7.0f * uiScale);
    dl->AddRectFilled(pMin, pMax, ImGui::ColorConvertFloat4ToU32(bgCol), rounding);
    dl->AddRect(pMin, pMax, ImGui::ColorConvertFloat4ToU32(borderCol), rounding, 0, 1.0f);

    const float sepMarginY = std::round(5.0f * uiScale);
    float curX = pMin.x + padX;
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) {
            curX += sepPad;
            dl->AddLine(
                ImVec2(curX, pMin.y + sepMarginY),
                ImVec2(curX, pMax.y - sepMarginY),
                sepCol,
                1.0f);
            curX += 1.0f + sepPad;
        }

        const SummaryBadgeSegment& seg = segments[i];
        const bool hasLabel = (seg.label != nullptr && seg.label[0] != '\0');
        const bool hasValue = (seg.value != nullptr && seg.value[0] != '\0');
        if (hasLabel) {
            const ImVec2 lblSize = ImGui::CalcTextSize(seg.label);
            const ImVec4& lblCol = seg.tintLabel ? tintedLabelCol : defaultLabelCol;
            dl->AddText(
                ImVec2(curX, pMin.y + (frameH - lblSize.y) * 0.5f),
                ImGui::ColorConvertFloat4ToU32(lblCol),
                seg.label);
            curX += lblSize.x;
            if (hasValue) {
                curX += gap;
            }
        }
        if (hasValue) {
            const ImVec4 valCol =
                g_isLightTheme
                    ? ImVec4(seg.accentColor.x * 0.52f, seg.accentColor.y * 0.52f, seg.accentColor.z * 0.52f, 1.00f)
                    : seg.accentColor;
            PushFontSafe(g_fontSecondaryHeader);
            const ImVec2 valSize = ImGui::CalcTextSize(seg.value);
            dl->AddText(
                ImVec2(curX, pMin.y + (frameH - valSize.y) * 0.5f),
                ImGui::ColorConvertFloat4ToU32(valCol),
                seg.value);
            PopFontSafe(g_fontSecondaryHeader);
            curX += valSize.x;
        }
    }
}

void DrawAppIcon(ImDrawList* dl, ImVec2 c, float s, AppIcon icon, ImU32 col) {
    if (dl == nullptr) {
        return;
    }
    const float r = s * 0.5f;
    const float t = std::max(1.4f, s * 0.105f);

    switch (icon) {
    case AppIcon::BrandLogo: {
        // Fallback vector icon matching app_icon.png: rounded microchip inside circular update ring
        const float ar = r * 0.84f;
        dl->PathClear();
        dl->PathArcTo(c, ar, 0.35f, 5.55f, 28);
        dl->PathStroke(col, 0, t * 1.15f);
        const float chipR = r * 0.38f;
        dl->AddRect(
            ImVec2(c.x - chipR, c.y - chipR),
            ImVec2(c.x + chipR, c.y + chipR),
            col,
            std::max(1.5f, s * 0.08f),
            0,
            t);
        break;
    }
    case AppIcon::Home: {
        // Clean house outline
        const ImVec2 roofL(c.x - r * 0.85f, c.y - r * 0.05f);
        const ImVec2 roofTop(c.x, c.y - r * 0.82f);
        const ImVec2 roofR(c.x + r * 0.85f, c.y - r * 0.05f);
        dl->AddLine(roofL, roofTop, col, t);
        dl->AddLine(roofTop, roofR, col, t);
        const ImVec2 wallTL(c.x - r * 0.58f, c.y - r * 0.10f);
        const ImVec2 wallBL(c.x - r * 0.58f, c.y + r * 0.72f);
        const ImVec2 wallBR(c.x + r * 0.58f, c.y + r * 0.72f);
        const ImVec2 wallTR(c.x + r * 0.58f, c.y - r * 0.10f);
        dl->AddLine(wallTL, wallBL, col, t);
        dl->AddLine(wallBL, wallBR, col, t);
        dl->AddLine(wallBR, wallTR, col, t);
        break;
    }
    case AppIcon::Apps: {
        // 2x2 rounded grid (Games & Software)
        const float g = r * 0.16f;
        const float b = r * 0.68f;
        const float rnd = std::max(1.5f, s * 0.12f);
        dl->AddRect(ImVec2(c.x - b - g, c.y - b - g), ImVec2(c.x - g, c.y - g), col, rnd, 0, t);
        dl->AddRect(ImVec2(c.x + g, c.y - b - g), ImVec2(c.x + b + g, c.y - g), col, rnd, 0, t);
        dl->AddRect(ImVec2(c.x - b - g, c.y + g), ImVec2(c.x - g, c.y + b + g), col, rnd, 0, t);
        dl->AddRect(ImVec2(c.x + g, c.y + g), ImVec2(c.x + b + g, c.y + b + g), col, rnd, 0, t);
        break;
    }
    case AppIcon::Updates:
    case AppIcon::Refresh: {
        // Crisp symmetric circular update arrows with exact tangent-aligned triangular heads
        const float ar = r * 0.72f;
        const float headLen = std::max(3.6f, r * 0.44f);
        const float headHalfW = std::max(2.6f, r * 0.30f);

        auto drawArcWithArrow = [&](float aStart, float aEnd) {
            dl->PathClear();
            dl->PathArcTo(c, ar, aStart, aEnd, 20);
            dl->PathStroke(col, 0, t);

            // Exact unit tangent (clockwise in screen Y-down coords) and outward radial normal at aEnd
            const float ca = std::cos(aEnd);
            const float sa = std::sin(aEnd);
            const ImVec2 endPt(c.x + ca * ar, c.y + sa * ar);
            const ImVec2 tanVec(-sa, ca);
            const ImVec2 radVec(ca, sa);

            const ImVec2 tip(endPt.x + tanVec.x * headLen * 0.62f, endPt.y + tanVec.y * headLen * 0.62f);
            const ImVec2 base(endPt.x - tanVec.x * headLen * 0.38f, endPt.y - tanVec.y * headLen * 0.38f);
            const ImVec2 wingOut(base.x + radVec.x * headHalfW, base.y + radVec.y * headHalfW);
            const ImVec2 wingIn(base.x - radVec.x * headHalfW, base.y - radVec.y * headHalfW);
            dl->AddTriangleFilled(tip, wingOut, wingIn, col);
        };

        drawArcWithArrow(-2.45f, -0.35f);
        drawArcWithArrow(0.69f, 2.79f);
        break;
    }
    case AppIcon::Devices: {
        // Monitor with stand
        const float rnd = std::max(1.5f, s * 0.10f);
        dl->AddRect(
            ImVec2(c.x - r * 0.82f, c.y - r * 0.66f),
            ImVec2(c.x + r * 0.82f, c.y + r * 0.36f),
            col,
            rnd,
            0,
            t);
        dl->AddLine(ImVec2(c.x, c.y + r * 0.36f), ImVec2(c.x, c.y + r * 0.72f), col, t);
        dl->AddLine(ImVec2(c.x - r * 0.44f, c.y + r * 0.72f), ImVec2(c.x + r * 0.44f, c.y + r * 0.72f), col, t);
        break;
    }
    case AppIcon::Shield: {
        // Protective shield with checkmark
        ImVec2 pts[5] = {
            ImVec2(c.x, c.y - r * 0.84f),
            ImVec2(c.x + r * 0.74f, c.y - r * 0.50f),
            ImVec2(c.x + r * 0.60f, c.y + r * 0.25f),
            ImVec2(c.x, c.y + r * 0.86f),
            ImVec2(c.x - r * 0.60f, c.y + r * 0.25f),
        };
        ImVec2 ptsFull[6] = {
            pts[0],
            pts[1],
            pts[2],
            pts[3],
            pts[4],
            ImVec2(c.x - r * 0.74f, c.y - r * 0.50f),
        };
        dl->AddPolyline(ptsFull, 6, col, ImDrawFlags_Closed, t);
        dl->AddLine(
            ImVec2(c.x - r * 0.30f, c.y + r * 0.02f),
            ImVec2(c.x - r * 0.06f, c.y + r * 0.26f),
            col,
            t);
        dl->AddLine(
            ImVec2(c.x - r * 0.06f, c.y + r * 0.26f),
            ImVec2(c.x + r * 0.34f, c.y - r * 0.22f),
            col,
            t);
        break;
    }
    case AppIcon::Folder: {
        ImVec2 pts[6] = {
            ImVec2(c.x - r * 0.82f, c.y - r * 0.56f),
            ImVec2(c.x - r * 0.22f, c.y - r * 0.56f),
            ImVec2(c.x - r * 0.02f, c.y - r * 0.32f),
            ImVec2(c.x + r * 0.82f, c.y - r * 0.32f),
            ImVec2(c.x + r * 0.82f, c.y + r * 0.62f),
            ImVec2(c.x - r * 0.82f, c.y + r * 0.62f),
        };
        dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, t);
        break;
    }
    case AppIcon::Settings: {
        // Sliders / Gear icon (3 horizontal bars with knobs — ultra crisp at small sizes)
        const float w = r * 0.78f;
        const float y1 = c.y - r * 0.50f;
        const float y2 = c.y;
        const float y3 = c.y + r * 0.50f;
        const float kr = std::max(1.8f, r * 0.22f);
        dl->AddLine(ImVec2(c.x - w, y1), ImVec2(c.x + w, y1), col, t);
        dl->AddLine(ImVec2(c.x - w, y2), ImVec2(c.x + w, y2), col, t);
        dl->AddLine(ImVec2(c.x - w, y3), ImVec2(c.x + w, y3), col, t);
        dl->AddCircleFilled(ImVec2(c.x - w * 0.35f, y1), kr, col);
        dl->AddCircleFilled(ImVec2(c.x + w * 0.35f, y2), kr, col);
        dl->AddCircleFilled(ImVec2(c.x - w * 0.15f, y3), kr, col);
        break;
    }
    case AppIcon::Info: {
        dl->AddCircle(c, r * 0.82f, col, 24, t);
        dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.34f), std::max(1.1f, r * 0.14f), col, 12);
        dl->AddLine(ImVec2(c.x, c.y - r * 0.06f), ImVec2(c.x, c.y + r * 0.42f), col, t * 1.05f);
        break;
    }
    case AppIcon::Sun: {
        dl->AddCircle(c, r * 0.42f, col, 20, t);
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * 0.78539816f;
            dl->AddLine(
                ImVec2(c.x + std::cos(a) * r * 0.62f, c.y + std::sin(a) * r * 0.62f),
                ImVec2(c.x + std::cos(a) * r * 0.88f, c.y + std::sin(a) * r * 0.88f),
                col,
                t * 0.9f);
        }
        break;
    }
    case AppIcon::Moon: {
        dl->PathClear();
        dl->PathArcTo(c, r * 0.74f, 0.55f, 4.90f, 20);
        dl->PathStroke(col, 0, t);
        dl->PathClear();
        dl->PathArcTo(ImVec2(c.x + r * 0.36f, c.y - r * 0.24f), r * 0.58f, 1.30f, 4.20f, 16);
        dl->PathStroke(col, 0, t);
        break;
    }
    case AppIcon::CheckCircle: {
        dl->AddCircle(c, r * 0.84f, col, 28, t);
        dl->AddLine(
            ImVec2(c.x - r * 0.36f, c.y + r * 0.02f),
            ImVec2(c.x - r * 0.08f, c.y + r * 0.30f),
            col,
            t * 1.1f);
        dl->AddLine(
            ImVec2(c.x - r * 0.08f, c.y + r * 0.30f),
            ImVec2(c.x + r * 0.40f, c.y - r * 0.26f),
            col,
            t * 1.1f);
        break;
    }
    case AppIcon::AlertTriangle: {
        ImVec2 tri[3] = {
            ImVec2(c.x, c.y - r * 0.82f),
            ImVec2(c.x + r * 0.84f, c.y + r * 0.72f),
            ImVec2(c.x - r * 0.84f, c.y + r * 0.72f),
        };
        dl->AddPolyline(tri, 3, col, ImDrawFlags_Closed, t);
        dl->AddLine(ImVec2(c.x, c.y - r * 0.26f), ImVec2(c.x, c.y + r * 0.22f), col, t * 1.05f);
        dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.46f), std::max(1.2f, r * 0.12f), col, 12);
        break;
    }
    case AppIcon::Heart: {
        const float rad = r * 0.38f;
        const float topY = c.y - r * 0.22f;
        const ImVec2 lc(c.x - rad, topY);
        const ImVec2 rc(c.x + rad, topY);
        dl->PathClear();
        dl->PathLineTo(ImVec2(c.x, c.y + r * 0.74f));
        dl->PathLineTo(ImVec2(c.x - rad * 2.0f, topY + r * 0.14f));
        dl->PathArcTo(lc, rad, 3.05f, 6.28f, 12);
        dl->PathArcTo(rc, rad, 3.14f, 6.37f, 12);
        dl->PathLineTo(ImVec2(c.x + rad * 2.0f, topY + r * 0.14f));
        dl->PathStroke(col, ImDrawFlags_Closed, t);
        break;
    }
    case AppIcon::ChevronUp: {
        dl->AddLine(ImVec2(c.x - r * 0.55f, c.y + r * 0.22f), ImVec2(c.x, c.y - r * 0.30f), col, t);
        dl->AddLine(ImVec2(c.x, c.y - r * 0.30f), ImVec2(c.x + r * 0.55f, c.y + r * 0.22f), col, t);
        break;
    }
    }
}

bool RenderSidebarNavItem(
    const char* id,
    AppIcon icon,
    const char* label,
    bool active,
    size_t badgeCount,
    const ImVec4& badgeColor,
    float width) {
    const float uiScale = EffectiveUiScale();
    const float itemW = (width > 0.0f) ? width : ImGui::GetContentRegionAvail().x;
    const float itemH = std::round(38.0f * uiScale);

    const ImVec2 pMin = ImGui::GetCursorScreenPos();
    const ImVec2 pMax(pMin.x + itemW, pMin.y + itemH);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(itemW, itemH));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = std::round(8.0f * uiScale);

    if (active) {
        const ImVec4 bg = g_isLightTheme
                              ? ImVec4(0.84f, 0.90f, 0.99f, 1.00f)
                              : ImVec4(0.125f, 0.205f, 0.340f, 0.95f);
        dl->AddRectFilled(pMin, pMax, ImGui::ColorConvertFloat4ToU32(bg), rounding);
        const float barW = std::max(3.0f, std::round(3.2f * uiScale));
        const float barPadY = std::round(7.0f * uiScale);
        dl->AddRectFilled(
            ImVec2(pMin.x + 2.0f, pMin.y + barPadY),
            ImVec2(pMin.x + 2.0f + barW, pMax.y - barPadY),
            ImGui::ColorConvertFloat4ToU32(AccentBarColor()),
            barW * 0.5f);
    } else if (hovered) {
        const ImVec4 bg = g_isLightTheme
                              ? ImVec4(0.87f, 0.90f, 0.95f, 0.72f)
                              : ImVec4(0.105f, 0.135f, 0.195f, 0.85f);
        dl->AddRectFilled(pMin, pMax, ImGui::ColorConvertFloat4ToU32(bg), rounding);
    }

    const ImVec4 iconColVec =
        active
            ? AccentBarColor()
            : (hovered ? (g_isLightTheme ? ImVec4(0.14f, 0.20f, 0.32f, 1.00f)
                                         : ImVec4(0.84f, 0.90f, 0.98f, 1.00f))
                       : MutedTextColor());
    const ImVec4 textColVec =
        active
            ? (g_isLightTheme ? ImVec4(0.06f, 0.22f, 0.52f, 1.00f)
                              : ImVec4(0.96f, 0.98f, 1.00f, 1.00f))
            : (hovered ? (g_isLightTheme ? ImVec4(0.08f, 0.12f, 0.20f, 1.00f)
                                         : ImVec4(0.92f, 0.95f, 1.00f, 1.00f))
                       : (g_isLightTheme ? ImVec4(0.24f, 0.30f, 0.40f, 1.00f)
                                         : ImVec4(0.73f, 0.80f, 0.90f, 1.00f)));

    const float iconSize = std::round(16.0f * uiScale);
    const float iconCenterX = pMin.x + std::round(22.0f * uiScale);
    const float centerY = (pMin.y + pMax.y) * 0.5f;
    DrawAppIcon(dl, ImVec2(iconCenterX, centerY), iconSize, icon, ImGui::ColorConvertFloat4ToU32(iconColVec));

    float rightReserve = std::round(10.0f * uiScale);
    if (badgeCount > 0) {
        char bBuf[24];
        if (badgeCount > 99) {
            std::snprintf(bBuf, sizeof(bBuf), "99+");
        } else {
            std::snprintf(bBuf, sizeof(bBuf), "%zu", badgeCount);
        }
        PushFontSafe(g_fontSmall);
        const ImVec2 bSize = ImGui::CalcTextSize(bBuf);
        const float bPadX = std::round(6.5f * uiScale);
        const float bH = std::round(19.0f * uiScale);
        const float bW = std::max(bH, bSize.x + bPadX * 2.0f);
        const ImVec2 bMin(pMax.x - std::round(10.0f * uiScale) - bW, centerY - bH * 0.5f);
        const ImVec2 bMax(bMin.x + bW, bMin.y + bH);
        const ImVec4 bBg(badgeColor.x, badgeColor.y, badgeColor.z, g_isLightTheme ? 0.18f : 0.22f);
        const ImVec4 bFg =
            g_isLightTheme
                ? ImVec4(badgeColor.x * 0.50f, badgeColor.y * 0.50f, badgeColor.z * 0.50f, 1.00f)
                : badgeColor;
        dl->AddRectFilled(bMin, bMax, ImGui::ColorConvertFloat4ToU32(bBg), bH * 0.5f);
        dl->AddText(
            ImVec2(bMin.x + (bW - bSize.x) * 0.5f, bMin.y + (bH - bSize.y) * 0.5f),
            ImGui::ColorConvertFloat4ToU32(bFg),
            bBuf);
        PopFontSafe(g_fontSmall);
        rightReserve = bW + std::round(16.0f * uiScale);
    }

    ImFont* lblFont = active ? g_fontSecondaryHeader : g_fontRegular;
    PushFontSafe(lblFont);
    const float textX = pMin.x + std::round(40.0f * uiScale);
    const float maxTextW = std::max(24.0f, itemW - (textX - pMin.x) - rightReserve);
    const std::string shown = EllipsizeUtf8(label != nullptr ? label : "", maxTextW);
    const ImVec2 ts = ImGui::CalcTextSize(shown.c_str());
    dl->AddText(
        ImVec2(textX, centerY - ts.y * 0.5f),
        ImGui::ColorConvertFloat4ToU32(textColVec),
        shown.c_str());
    PopFontSafe(lblFont);

    return clicked;
}

bool RenderIconButton(
    const char* id,
    AppIcon icon,
    const ImVec2& size,
    const char* tooltip,
    bool active) {
    const bool clicked = active ? RenderPillButton(id, true, size)
                                : RenderSecondaryButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (ImGui::IsItemVisible()) {
        const ImVec2 rMin = ImGui::GetItemRectMin();
        const ImVec2 rMax = ImGui::GetItemRectMax();
        const ImVec2 center((rMin.x + rMax.x) * 0.5f, (rMin.y + rMax.y) * 0.5f);
        const float iconSz = std::min(rMax.x - rMin.x, rMax.y - rMin.y) * 0.52f;
        const ImVec4 col =
            (active || hovered)
                ? AccentTextColor()
                : (g_isLightTheme ? ImVec4(0.24f, 0.32f, 0.44f, 1.00f)
                                  : ImVec4(0.75f, 0.83f, 0.94f, 1.00f));
        DrawAppIcon(ImGui::GetWindowDrawList(), center, iconSz, icon, ImGui::ColorConvertFloat4ToU32(col));
    }
    if (tooltip != nullptr && tooltip[0] != '\0' && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return clicked;
}

void RenderPageHeader(const char* title, const char* subtitle) {
    const float uiScale = EffectiveUiScale();
    PushFontSafe(g_fontMainTab);
    ImGui::TextUnformatted(title != nullptr ? title : "");
    PopFontSafe(g_fontMainTab);
    if (subtitle != nullptr && subtitle[0] != '\0') {
        PushFontSafe(g_fontSmall);
        ImGui::PushStyleColor(ImGuiCol_Text, MutedTextColor());
        ImGui::TextWrapped("%s", subtitle);
        ImGui::PopStyleColor();
        PopFontSafe(g_fontSmall);
    }
    ImGui::Dummy(ImVec2(0.0f, std::round(4.0f * uiScale)));
}

int CompareVersionOrString(const std::string& a, const std::string& b) {
    if (a.empty() && b.empty()) return 0;
    if (a.empty()) return 1;
    if (b.empty()) return -1;

    auto splitSemVer = [](std::string_view s) -> std::pair<std::string_view, std::string_view> {
        const size_t plusPos = s.find('+');
        if (plusPos != std::string_view::npos) {
            s = s.substr(0, plusPos);
        }
        const size_t dashPos = s.find('-');
        if (dashPos == std::string_view::npos) {
            return {s, {}};
        }
        return {s.substr(0, dashPos), s.substr(dashPos + 1)};
    };

    auto extractNums = [](std::string_view s) {
        std::vector<unsigned long long> nums;
        unsigned long long cur = 0;
        bool inNum = false;
        for (char ch : s) {
            if (ch >= '0' && ch <= '9') {
                const unsigned long long d = static_cast<unsigned long long>(ch - '0');
                constexpr unsigned long long kMaxVal = (std::numeric_limits<unsigned long long>::max)();
                if (cur > (kMaxVal - d) / 10ULL) {
                    cur = kMaxVal;
                } else {
                    cur = cur * 10ULL + d;
                }
                inNum = true;
            } else if (inNum) {
                nums.push_back(cur);
                cur = 0;
                inNum = false;
            }
        }
        if (inNum) nums.push_back(cur);
        return nums;
    };

    auto comparePrerelease = [](std::string_view pa, std::string_view pb) -> int {
        auto splitParts = [](std::string_view sv) {
            std::vector<std::string_view> parts;
            size_t i = 0;
            while (i < sv.size()) {
                while (i < sv.size() && (sv[i] == '.' || sv[i] == '-')) ++i;
                if (i >= sv.size()) break;
                size_t j = i;
                while (j < sv.size() && sv[j] != '.' && sv[j] != '-') ++j;
                parts.push_back(sv.substr(i, j - i));
                i = j;
            }
            return parts;
        };
        auto isDigits = [](std::string_view tok) {
            if (tok.empty()) return false;
            for (char c : tok) {
                if (c < '0' || c > '9') return false;
            }
            return true;
        };
        auto parseUint = [](std::string_view tok) {
            unsigned long long v = 0;
            constexpr unsigned long long kMaxVal = (std::numeric_limits<unsigned long long>::max)();
            for (char c : tok) {
                const unsigned long long d = static_cast<unsigned long long>(c - '0');
                v = (v > (kMaxVal - d) / 10ULL) ? kMaxVal : (v * 10ULL + d);
            }
            return v;
        };

        const auto sa = splitParts(pa);
        const auto sb = splitParts(pb);
        const size_t n = std::min(sa.size(), sb.size());
        for (size_t i = 0; i < n; ++i) {
            const bool da = isDigits(sa[i]);
            const bool db = isDigits(sb[i]);
            if (da && db) {
                const auto va = parseUint(sa[i]);
                const auto vb = parseUint(sb[i]);
                if (va != vb) return (va < vb) ? -1 : 1;
            } else if (da != db) {
                return da ? -1 : 1;
            } else {
                const std::string la = hdm::ToLowerAscii(sa[i]);
                const std::string lb = hdm::ToLowerAscii(sb[i]);
                if (la != lb) return (la < lb) ? -1 : 1;
            }
        }
        if (sa.size() != sb.size()) {
            return (sa.size() < sb.size()) ? -1 : 1;
        }
        return 0;
    };

    const auto [coreA, preA] = splitSemVer(a);
    const auto [coreB, preB] = splitSemVer(b);
    const auto va = extractNums(coreA);
    const auto vb = extractNums(coreB);
    if (!va.empty() && !vb.empty()) {
        const size_t n = std::max(va.size(), vb.size());
        for (size_t i = 0; i < n; ++i) {
            const auto na = i < va.size() ? va[i] : 0ULL;
            const auto nb = i < vb.size() ? vb[i] : 0ULL;
            if (na != nb) return (na < nb) ? -1 : 1;
        }
        if (preA.empty() != preB.empty()) {
            return preA.empty() ? 1 : -1;
        }
        if (!preA.empty() && !preB.empty()) {
            return comparePrerelease(preA, preB);
        }
        return 0;
    }
    return a.compare(b);
}
