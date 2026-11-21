#include "ui/AppWidgets.hpp"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

std::atomic<bool> g_windowRegainedFocus{false};
bool g_isLightTheme = false;
bool g_isAdvancedMode = false;
bool g_autoCheckAppUpdates = true;
float g_dpiScale = 1.0f;

ImFont* g_fontRegular = nullptr;
ImFont* g_fontSecondaryHeader = nullptr;
ImFont* g_fontMainTab = nullptr;
ImFont* g_fontPrimaryHeader = nullptr;

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
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
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
        if (FAILED(fn(hwnd, kDwmwaUseImmersiveDarkMode, &mode, sizeof(mode)))) {
            fn(hwnd, kDwmwaUseImmersiveDarkModeOld, &mode, sizeof(mode));
        }
    }
    FreeLibrary(dwm);
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
    const std::wstring wUrl = hdm::Utf8ToWide(url);
    ShellExecuteW(nullptr, L"open", wUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// --- Theme & Typography Helpers ---
ImVec4 AccentTextColor() {
    return g_isLightTheme
               ? ImVec4(0.11f, 0.38f, 0.78f, 1.00f)
               : ImVec4(0.48f, 0.76f, 1.00f, 1.00f);
}

ImVec4 WarningTextColor() {
    return g_isLightTheme
               ? ImVec4(0.68f, 0.42f, 0.02f, 1.00f)
               : ImVec4(0.98f, 0.75f, 0.14f, 1.00f);
}

ImVec4 PanelBorderColor() {
    return g_isLightTheme
               ? ImVec4(0.72f, 0.78f, 0.87f, 0.95f)
               : ImVec4(0.20f, 0.27f, 0.38f, 0.95f);
}

ImVec4 AccentBarColor() {
    return g_isLightTheme
               ? ImVec4(0.14f, 0.48f, 0.82f, 1.00f)
               : ImVec4(0.36f, 0.74f, 0.96f, 1.00f);
}

ImVec4 CardHeaderTextColor() {
    return g_isLightTheme
               ? ImVec4(0.09f, 0.20f, 0.38f, 1.00f)
               : ImVec4(0.80f, 0.90f, 1.00f, 1.00f);
}

void ApplyBaseLayoutStyle(ImGuiStyle& style, float scale) {
    style = ImGuiStyle();

    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(11.0f, 5.0f);
    style.CellPadding = ImVec2(10.0f, 6.0f);
    style.ItemSpacing = ImVec2(8.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.ScrollbarSize = 11.0f;
    style.GrabMinSize = 10.0f;

    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 1.0f;

    style.WindowRounding = 0.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 7.0f;

    if (scale > 0.5f && scale != 1.0f) {
        style.ScaleAllSizes(scale);
    }
}

void ApplyModernDarkThemeColors(ImGuiStyle& style) {
    ImGui::StyleColorsDark(&style);
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = ImVec4(0.93f, 0.95f, 0.98f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.56f, 0.62f, 0.72f, 1.00f);
    c[ImGuiCol_WindowBg] = ImVec4(0.059f, 0.071f, 0.098f, 1.00f);
    c[ImGuiCol_ChildBg] = ImVec4(0.082f, 0.098f, 0.133f, 1.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.098f, 0.118f, 0.161f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.176f, 0.216f, 0.290f, 0.90f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    c[ImGuiCol_FrameBg] = ImVec4(0.110f, 0.133f, 0.180f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.145f, 0.176f, 0.239f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.180f, 0.220f, 0.298f, 1.00f);

    c[ImGuiCol_TitleBg] = ImVec4(0.059f, 0.071f, 0.098f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.082f, 0.098f, 0.133f, 1.00f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.075f, 0.090f, 0.125f, 1.00f);

    c[ImGuiCol_ScrollbarBg] = ImVec4(0.059f, 0.071f, 0.098f, 0.45f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.22f, 0.27f, 0.36f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30f, 0.36f, 0.47f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.36f, 0.44f, 0.58f, 1.00f);

    c[ImGuiCol_CheckMark] = ImVec4(0.38f, 0.66f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.38f, 0.66f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.50f, 0.74f, 1.00f, 1.00f);

    c[ImGuiCol_Button] = ImVec4(0.20f, 0.45f, 0.86f, 0.95f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.27f, 0.54f, 0.96f, 1.00f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.16f, 0.37f, 0.73f, 1.00f);

    c[ImGuiCol_Header] = ImVec4(0.22f, 0.46f, 0.84f, 0.30f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.50f, 0.90f, 0.44f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.25f, 0.50f, 0.90f, 0.60f);

    c[ImGuiCol_Separator] = ImVec4(0.176f, 0.216f, 0.290f, 0.90f);
    c[ImGuiCol_Tab] = ImVec4(0.094f, 0.114f, 0.157f, 1.00f);
    c[ImGuiCol_TabHovered] = ImVec4(0.22f, 0.46f, 0.85f, 0.88f);
    c[ImGuiCol_TabSelected] = ImVec4(0.19f, 0.43f, 0.82f, 1.00f);

    c[ImGuiCol_TableHeaderBg] = ImVec4(0.125f, 0.155f, 0.215f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.23f, 0.31f, 0.45f, 1.00f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.133f, 0.165f, 0.224f, 0.85f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.026f);

    c[ImGuiCol_PlotHistogram] = ImVec4(0.27f, 0.56f, 0.96f, 1.00f);
}

void ApplyModernLightThemeColors(ImGuiStyle& style) {
    ImGui::StyleColorsLight(&style);
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = ImVec4(0.08f, 0.11f, 0.18f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.41f, 0.47f, 0.57f, 1.00f);
    c[ImGuiCol_WindowBg] = ImVec4(0.941f, 0.953f, 0.973f, 1.00f);
    c[ImGuiCol_ChildBg] = ImVec4(0.988f, 0.992f, 1.000f, 1.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.992f, 0.996f, 1.000f, 0.99f);
    c[ImGuiCol_Border] = ImVec4(0.75f, 0.80f, 0.87f, 1.00f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    c[ImGuiCol_FrameBg] = ImVec4(0.92f, 0.94f, 0.97f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.87f, 0.90f, 0.95f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.82f, 0.87f, 0.94f, 1.00f);

    c[ImGuiCol_TitleBg] = ImVec4(0.92f, 0.94f, 0.97f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.96f, 0.97f, 0.99f, 1.00f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.93f, 0.95f, 0.98f, 1.00f);

    c[ImGuiCol_ScrollbarBg] = ImVec4(0.93f, 0.95f, 0.98f, 0.70f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.72f, 0.77f, 0.85f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.58f, 0.65f, 0.76f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.44f, 0.53f, 0.68f, 1.00f);

    c[ImGuiCol_CheckMark] = ImVec4(0.14f, 0.45f, 0.88f, 1.00f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.16f, 0.48f, 0.90f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.11f, 0.39f, 0.80f, 1.00f);

    c[ImGuiCol_Button] = ImVec4(0.74f, 0.85f, 0.99f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.65f, 0.80f, 0.98f, 1.00f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.57f, 0.74f, 0.96f, 1.00f);

    c[ImGuiCol_Header] = ImVec4(0.18f, 0.48f, 0.88f, 0.16f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.48f, 0.88f, 0.26f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.18f, 0.48f, 0.88f, 0.38f);

    c[ImGuiCol_Separator] = ImVec4(0.76f, 0.81f, 0.88f, 0.95f);
    c[ImGuiCol_Tab] = ImVec4(0.90f, 0.92f, 0.96f, 1.00f);
    c[ImGuiCol_TabHovered] = ImVec4(0.80f, 0.88f, 0.98f, 1.00f);
    c[ImGuiCol_TabSelected] = ImVec4(0.76f, 0.86f, 0.99f, 1.00f);

    c[ImGuiCol_TableHeaderBg] = ImVec4(0.89f, 0.92f, 0.96f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.72f, 0.78f, 0.86f, 1.00f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.83f, 0.87f, 0.92f, 0.95f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(0.0f, 0.15f, 0.35f, 0.035f);

    c[ImGuiCol_PlotHistogram] = ImVec4(0.20f, 0.54f, 0.92f, 1.00f);
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
        std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 2, L'\0');
        if (RegQueryValueExW(
                key.Get(),
                name,
                nullptr,
                &type,
                reinterpret_cast<LPBYTE>(buf.data()),
                &bytes) != ERROR_SUCCESS) {
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

// Rounded tinted pill status badge with UTF-8 ellipsis protection and optional vertical frame centering.
void RenderStatusBadge(const ImVec4& color, const char* text, bool alignToFrame) {
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
    const float textH = ImGui::GetTextLineHeight();
    const float frameH = ImGui::GetFrameHeight();
    const float badgeH = textH + std::round(6.0f * uiScale);
    const float topOffset = alignToFrame ? std::max(0.0f, (frameH - badgeH) * 0.5f) : 0.0f;

    const float padLeft = std::round(8.0f * uiScale);
    const float padRight = std::round(9.0f * uiScale);
    const float radius = std::max(3.2f, textH * 0.21f);
    const float dotToText = std::round(6.0f * uiScale);
    const float availW = std::max(44.0f * uiScale, ImGui::GetContentRegionAvail().x - 2.0f);
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
    const ImVec4 bgVec(color.x, color.y, color.z, g_isLightTheme ? 0.16f : 0.14f);
    const ImVec4 borderVec =
        g_isLightTheme
            ? ImVec4(color.x * 0.72f, color.y * 0.72f, color.z * 0.72f, 0.50f)
            : ImVec4(color.x, color.y, color.z, 0.42f);
    const ImVec4 fgVec =
        g_isLightTheme
            ? ImVec4(color.x * 0.50f, color.y * 0.50f, color.z * 0.50f, 1.00f)
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
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, std::round(10.0f * uiScale));
    if (g_isLightTheme) {
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.82f, 0.89f, 0.98f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.84f, 0.97f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.69f, 0.80f, 0.95f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.28f, 0.50f, 0.82f, 0.88f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.18f, 0.36f, 1.00f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.94f, 0.95f, 0.98f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.88f, 0.91f, 0.96f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.83f, 0.87f, 0.94f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.77f, 0.82f, 0.88f, 0.90f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.32f, 0.38f, 0.48f, 1.00f));
        }
    } else {
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.21f, 0.33f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.27f, 0.42f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.12f, 0.18f, 0.29f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.34f, 0.54f, 0.80f, 0.82f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.88f, 0.94f, 1.00f, 1.00f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.102f, 0.122f, 0.165f, 0.92f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.145f, 0.175f, 0.238f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.090f, 0.108f, 0.148f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.19f, 0.23f, 0.31f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.68f, 0.74f, 0.84f, 1.00f));
        }
    }
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar();
    return clicked;
}

float CalcSubSectionTabWidth(const char* label) {
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
    PushFontSafe(g_fontSecondaryHeader);
    const float w = ImGui::CalcTextSize(label, nullptr, true).x + std::round(12.0f * uiScale) * 2.0f;
    PopFontSafe(g_fontSecondaryHeader);
    return w;
}

bool RenderSubSectionHeaderTab(const char* label, bool active) {
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
    PushFontSafe(g_fontSecondaryHeader);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, std::round(5.0f * uiScale));
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding,
        ImVec2(std::round(12.0f * uiScale), std::round(5.5f * uiScale)));
    if (g_isLightTheme) {
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.83f, 0.93f, 0.97f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.76f, 0.89f, 0.95f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.70f, 0.85f, 0.93f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.16f, 0.56f, 0.74f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.06f, 0.22f, 0.32f, 1.00f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.93f, 0.95f, 0.98f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.87f, 0.91f, 0.96f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.82f, 0.87f, 0.93f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.76f, 0.81f, 0.88f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.34f, 0.41f, 0.52f, 1.00f));
        }
    } else {
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.105f, 0.185f, 0.265f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.135f, 0.235f, 0.335f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.090f, 0.160f, 0.230f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.26f, 0.68f, 0.84f, 0.92f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.97f, 1.00f, 1.00f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.092f, 0.112f, 0.152f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.125f, 0.160f, 0.220f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.080f, 0.100f, 0.138f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.17f, 0.21f, 0.28f, 0.75f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.64f, 0.72f, 0.84f, 1.00f));
        }
    }
    const bool clicked = ImGui::Button(label);
    if (active && ImGui::IsItemVisible()) {
        const ImVec2 rMin = ImGui::GetItemRectMin();
        const ImVec2 rMax = ImGui::GetItemRectMax();
        const float lineH = std::max(2.0f, 2.5f * uiScale);
        const float y = rMax.y - lineH * 0.5f;
        const float pad = std::round(4.0f * uiScale);
        const ImVec4 lineCol = g_isLightTheme
                                   ? ImVec4(0.09f, 0.54f, 0.76f, 1.00f)
                                   : ImVec4(0.32f, 0.78f, 0.94f, 1.00f);
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(rMin.x + pad, y),
            ImVec2(rMax.x - pad, y),
            ImGui::ColorConvertFloat4ToU32(lineCol),
            lineH);
    }
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(2);
    PopFontSafe(g_fontSecondaryHeader);
    return clicked;
}

void RenderSecondaryCardHeader(const char* title, const char* rightMeta) {
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
    PushFontSafe(g_fontSecondaryHeader);
    const float textH = ImGui::GetTextLineHeight();
    const float padX = std::round(9.0f * uiScale);
    const float padY = std::round(4.0f * uiScale);
    const float bannerH = textH + padY * 2.0f;
    const float availW = std::max(60.0f, ImGui::GetContentRegionAvail().x);

    const ImVec2 pMin = ImGui::GetCursorScreenPos();
    const ImVec2 pMax(pMin.x + availW, pMin.y + bannerH);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float rounding = std::round(4.0f * uiScale);
    const ImVec4 fillCol = g_isLightTheme
                               ? ImVec4(0.90f, 0.93f, 0.97f, 1.00f)
                               : ImVec4(0.115f, 0.145f, 0.205f, 0.92f);
    const ImVec4 borderCol = g_isLightTheme
                                 ? ImVec4(0.72f, 0.79f, 0.88f, 0.95f)
                                 : ImVec4(0.22f, 0.32f, 0.46f, 0.80f);
    dl->AddRectFilled(pMin, pMax, ImGui::ColorConvertFloat4ToU32(fillCol), rounding);
    dl->AddRect(pMin, pMax, ImGui::ColorConvertFloat4ToU32(borderCol), rounding, 0, 1.0f);

    const float barW = std::max(2.5f, std::round(3.0f * uiScale));
    const float barPadY = std::round(3.0f * uiScale);
    dl->AddRectFilled(
        ImVec2(pMin.x + 2.0f, pMin.y + barPadY),
        ImVec2(pMin.x + 2.0f + barW, pMax.y - barPadY),
        ImGui::ColorConvertFloat4ToU32(AccentBarColor()),
        barW * 0.5f);

    float rightReserve = 0.0f;
    if (rightMeta != nullptr && rightMeta[0] != '\0') {
        const ImVec2 metaSize = ImGui::CalcTextSize(rightMeta);
        rightReserve = metaSize.x + padX * 1.5f;
        const ImVec4 metaCol = g_isLightTheme
                                   ? ImVec4(0.38f, 0.45f, 0.56f, 1.00f)
                                   : ImVec4(0.56f, 0.64f, 0.76f, 1.00f);
        dl->AddText(
            ImVec2(pMax.x - padX - metaSize.x, pMin.y + padY),
            ImGui::ColorConvertFloat4ToU32(metaCol),
            rightMeta);
    }

    const float maxTitleW = std::max(24.0f, availW - (padX + barW + 4.0f + padX + rightReserve));
    const std::string shownTitle = EllipsizeUtf8(title != nullptr ? title : "", maxTitleW);
    dl->AddText(
        ImVec2(pMin.x + 2.0f + barW + padX, pMin.y + padY),
        ImGui::ColorConvertFloat4ToU32(CardHeaderTextColor()),
        shownTitle.c_str());

    ImGui::Dummy(ImVec2(availW, bannerH));
    PopFontSafe(g_fontSecondaryHeader);
}

void RenderVerticalDivider(float padX) {
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
    const float gap = std::round(padX * uiScale);
    const float h = ImGui::GetFrameHeight();
    ImGui::SameLine(0.0f, gap);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float x = std::floor(p.x + 1.0f) + 0.5f;
    const float yPad = std::round(3.0f * uiScale);
    const ImVec4 divCol = g_isLightTheme
                              ? ImVec4(0.73f, 0.78f, 0.86f, 0.95f)
                              : ImVec4(0.24f, 0.31f, 0.44f, 0.90f);
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(x, p.y + yPad),
        ImVec2(x, p.y + h - yPad),
        ImGui::ColorConvertFloat4ToU32(divCol),
        1.0f);
    ImGui::Dummy(ImVec2(2.0f + gap, h));
}

void RenderSecondaryGroupLabel(const char* text) {
    PushFontSafe(g_fontSecondaryHeader);
    ImGui::AlignTextToFramePadding();
    const ImVec4 lblCol = g_isLightTheme
                              ? ImVec4(0.32f, 0.40f, 0.52f, 1.00f)
                              : ImVec4(0.58f, 0.67f, 0.80f, 1.00f);
    ImGui::TextColored(lblCol, "%s", text);
    PopFontSafe(g_fontSecondaryHeader);
}

void RenderTableHeadersRow() {
    PushFontSafe(g_fontSecondaryHeader);
    const ImVec4 hdrCol = g_isLightTheme
                              ? ImVec4(0.12f, 0.20f, 0.34f, 1.00f)
                              : ImVec4(0.78f, 0.87f, 0.98f, 1.00f);
    ImGui::PushStyleColor(ImGuiCol_Text, hdrCol);
    ImGui::TableHeadersRow();
    ImGui::PopStyleColor();
    PopFontSafe(g_fontSecondaryHeader);
}

bool RenderSecondaryButton(const char* label, const ImVec2& size) {
    if (g_isLightTheme) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.93f, 0.95f, 0.98f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.87f, 0.91f, 0.96f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.81f, 0.86f, 0.94f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.70f, 0.76f, 0.85f, 0.95f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.125f, 0.149f, 0.204f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.173f, 0.208f, 0.278f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.106f, 0.129f, 0.180f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.23f, 0.28f, 0.37f, 0.95f));
    }
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

bool RenderActionButton(const char* label, const ImVec2& size, bool primary) {
    if (primary) {
        return ImGui::Button(label, size);
    }
    return RenderSecondaryButton(label, size);
}

float CalcSummaryInfoBadgeWidth(const SummaryBadgeSegment* segments, size_t count) {
    if (segments == nullptr || count == 0) {
        return 0.0f;
    }
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
    const float padX = std::round(7.0f * uiScale);
    const float gap = std::round(3.0f * uiScale);
    const float sepPad = std::round(5.0f * uiScale);
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
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
    const float padX = std::round(7.0f * uiScale);
    const float gap = std::round(3.0f * uiScale);
    const float sepPad = std::round(5.0f * uiScale);
    const float frameH = ImGui::GetFrameHeight();
    const float badgeW = CalcSummaryInfoBadgeWidth(segments, count);

    const ImVec2 pMin = ImGui::GetCursorScreenPos();
    const ImVec2 pMax(pMin.x + badgeW, pMin.y + frameH);
    ImGui::Dummy(ImVec2(badgeW, frameH));

    const ImVec4 bgCol =
        g_isLightTheme ? ImVec4(0.92f, 0.95f, 0.99f, 1.00f) : ImVec4(0.09f, 0.14f, 0.23f, 0.96f);
    const ImVec4 borderCol =
        g_isLightTheme ? ImVec4(0.62f, 0.72f, 0.86f, 0.90f) : ImVec4(0.24f, 0.37f, 0.56f, 0.78f);
    const ImU32 sepCol =
        g_isLightTheme ? IM_COL32(150, 170, 198, 170) : IM_COL32(72, 98, 134, 150);
    const ImVec4 defaultLabelCol =
        g_isLightTheme ? ImVec4(0.32f, 0.38f, 0.48f, 1.00f) : ImVec4(0.71f, 0.78f, 0.89f, 1.00f);
    const ImVec4 tintedLabelCol =
        g_isLightTheme ? ImVec4(0.55f, 0.34f, 0.02f, 1.00f) : ImVec4(0.96f, 0.82f, 0.50f, 1.00f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = std::round(6.0f * uiScale);
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
                    ? ImVec4(seg.accentColor.x * 0.50f, seg.accentColor.y * 0.50f, seg.accentColor.z * 0.50f, 1.00f)
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

int CompareVersionOrString(const std::string& a, const std::string& b) {
    if (a.empty() && b.empty()) return 0;
    if (a.empty()) return 1; // Put empty values ("—") last in ascending order
    if (b.empty()) return -1;

    auto extractNums = [](const std::string& s) {
        std::vector<unsigned long long> nums;
        unsigned long long cur = 0;
        bool inNum = false;
        for (char ch : s) {
            if (ch >= '0' && ch <= '9') {
                cur = cur * 10 + static_cast<unsigned long long>(ch - '0');
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

    const auto va = extractNums(a);
    const auto vb = extractNums(b);
    if (!va.empty() && !vb.empty()) {
        const size_t n = std::max(va.size(), vb.size());
        for (size_t i = 0; i < n; ++i) {
            const auto na = i < va.size() ? va[i] : 0ULL;
            const auto nb = i < vb.size() ? vb[i] : 0ULL;
            if (na != nb) return (na < nb) ? -1 : 1;
        }
        return 0;
    }
    return a.compare(b);
}
