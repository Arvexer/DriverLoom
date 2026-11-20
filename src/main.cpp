#include "common/AppCommon.hpp"
#include "core/DeviceScanner.hpp"
#include "core/LocalDriverInstaller.hpp"
#include "core/SystemComponentsScanner.hpp"
#include "core/SystemDiagnostics.hpp"
#include "core/SystemRestore.hpp"
#include "core/WindowsUpdateManager.hpp"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

// --- DirectX 11 Global State ---
ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
bool g_swapChainOccluded = false;
UINT g_resizeWidth = 0;
UINT g_resizeHeight = 0;
std::atomic<bool> g_windowRegainedFocus{false};

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    if (SUCCEEDED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer))) && pBackBuffer != nullptr) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView != nullptr) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL featureLevelArray[2] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL featureLevel{};

    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        featureLevelArray,
        2,
        D3D11_SDK_VERSION,
        &sd,
        &g_pSwapChain,
        &g_pd3dDevice,
        &featureLevel,
        &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED) {
        res = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            featureLevelArray,
            2,
            D3D11_SDK_VERSION,
            &sd,
            &g_pSwapChain,
            &g_pd3dDevice,
            &featureLevel,
            &g_pd3dDeviceContext);
    }
    if (res != S_OK) {
        return false;
    }
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain != nullptr) {
        g_pSwapChain->Release();
        g_pSwapChain = nullptr;
    }
    if (g_pd3dDeviceContext != nullptr) {
        g_pd3dDeviceContext->Release();
        g_pd3dDeviceContext = nullptr;
    }
    if (g_pd3dDevice != nullptr) {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
    }
}

bool SaveBackBufferToBmp(const std::wstring& bmpPath) {
    if (g_pSwapChain == nullptr || g_pd3dDevice == nullptr || g_pd3dDeviceContext == nullptr) {
        return false;
    }
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) || backBuffer == nullptr) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_pd3dDevice->CreateTexture2D(&desc, nullptr, &staging)) || staging == nullptr) {
        backBuffer->Release();
        return false;
    }
    g_pd3dDeviceContext->CopyResource(staging, backBuffer);
    backBuffer->Release();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_pd3dDeviceContext->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        staging->Release();
        return false;
    }

    const UINT w = desc.Width;
    const UINT h = desc.Height;
    const size_t rowBytes = static_cast<size_t>(w) * 4u;
    std::vector<unsigned char> pixels(rowBytes * h);
    const bool swapRB =
        (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
         desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);

    for (UINT y = 0; y < h; ++y) {
        const auto* src = static_cast<const unsigned char*>(mapped.pData) +
                          static_cast<size_t>(y) * mapped.RowPitch;
        unsigned char* dst = pixels.data() + static_cast<size_t>(y) * rowBytes;
        for (UINT x = 0; x < w; ++x) {
            const unsigned char c0 = src[x * 4 + 0];
            const unsigned char c1 = src[x * 4 + 1];
            const unsigned char c2 = src[x * 4 + 2];
            dst[x * 4 + 0] = swapRB ? c2 : c0;
            dst[x * 4 + 1] = c1;
            dst[x * 4 + 2] = swapRB ? c0 : c2;
            dst[x * 4 + 3] = 255;
        }
    }

    g_pd3dDeviceContext->Unmap(staging, 0);
    staging->Release();

    BITMAPFILEHEADER bfh{};
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = static_cast<LONG>(w);
    bih.biHeight = -static_cast<LONG>(h); // Top-down DIB
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = static_cast<DWORD>(pixels.size());

    bfh.bfType = 0x4D42; // 'BM'
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + bih.biSizeImage;

    std::ofstream ofs(std::filesystem::path(bmpPath), std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return false;
    }
    ofs.write(reinterpret_cast<const char*>(&bfh), sizeof(bfh));
    ofs.write(reinterpret_cast<const char*>(&bih), sizeof(bih));
    ofs.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    return ofs.good();
}

void EnableDarkTitleBar(HWND hwnd) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm == nullptr) {
        return;
    }
    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto fn = reinterpret_cast<DwmSetWindowAttributeFn>(
        reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute")));
    if (fn != nullptr) {
        const BOOL useDark = TRUE;
        constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
        constexpr DWORD kDwmwaUseImmersiveDarkModeOld = 19;
        if (FAILED(fn(hwnd, kDwmwaUseImmersiveDarkMode, &useDark, sizeof(useDark)))) {
            fn(hwnd, kDwmwaUseImmersiveDarkModeOld, &useDark, sizeof(useDark));
        }
    }
    FreeLibrary(dwm);
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
        return TRUE;
    }

    switch (msg) {
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) {
            g_windowRegainedFocus.store(true);
        }
        break;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            return 0;
        }
        g_resizeWidth = LOWORD(lParam);
        g_resizeHeight = HIWORD(lParam);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = 980;
        mmi->ptMinTrackSize.y = 640;
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) {
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
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

// --- Refined Modern Dark Slate Theme ---
void ApplyModernDarkTheme(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
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

    c[ImGuiCol_TableHeaderBg] = ImVec4(0.118f, 0.141f, 0.192f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.180f, 0.220f, 0.298f, 1.00f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.133f, 0.165f, 0.224f, 0.85f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.026f);

    c[ImGuiCol_PlotHistogram] = ImVec4(0.27f, 0.56f, 0.96f, 1.00f);
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

bool RenderCellText(const std::string& text, bool dimmed = false, bool alignToFrame = false) {
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
void RenderStatusBadge(const ImVec4& color, const char* text, bool alignToFrame = false) {
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
    const ImVec4 bgVec(color.x, color.y, color.z, 0.14f);
    const ImVec4 borderVec(color.x, color.y, color.z, 0.42f);
    const float rounding = badgeH * 0.5f;
    dl->AddRectFilled(pMin, pMax, ImGui::ColorConvertFloat4ToU32(bgVec), rounding);
    dl->AddRect(pMin, pMax, ImGui::ColorConvertFloat4ToU32(borderVec), rounding, 0, 1.0f);

    const ImVec2 dotCenter(pMin.x + padLeft + radius, pMin.y + badgeH * 0.5f);
    dl->AddCircleFilled(dotCenter, radius, ImGui::ColorConvertFloat4ToU32(color));

    const ImVec2 textPos(
        pMin.x + padLeft + radius * 2.0f + dotToText,
        pMin.y + (badgeH - textH) * 0.5f);
    dl->AddText(textPos, ImGui::ColorConvertFloat4ToU32(color), shown.c_str());

    ImGui::Dummy(ImVec2(pillW, alignToFrame ? std::max(frameH, badgeH) : badgeH));
}

bool RenderPillButton(const char* label, bool active) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.19f, 0.44f, 0.84f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.52f, 0.94f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.37f, 0.72f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.38f, 0.65f, 1.00f, 0.85f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.118f, 0.141f, 0.192f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.165f, 0.196f, 0.267f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.102f, 0.125f, 0.173f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.21f, 0.25f, 0.34f, 0.90f));
    }
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    return clicked;
}

bool RenderSecondaryButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f)) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.125f, 0.149f, 0.204f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.173f, 0.208f, 0.278f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.106f, 0.129f, 0.180f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.23f, 0.28f, 0.37f, 0.95f));
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

// --- Elevated CLI Worker Mode ---
int RunCliModeIfRequested() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return -1;
    }

    struct ArgvGuard {
        LPWSTR* ptr;
        ~ArgvGuard() { LocalFree(ptr); }
    } guard{argv};

    enum class Mode {
        None,
        RestorePoint,
        InstallInf,
        RemoveOemInfs,
        WuInstall,
        EnableService,
        ResetWuCache,
        SfcScan,
        DismCheck,
        DismRestore
    };
    Mode mode = Mode::None;
    std::wstring targetArg;
    std::wstring logPath;
    std::wstring cancelFilePath;
    bool forceInf = false;
    bool restoreBefore = false;
    bool driversOnly = false;
    bool autoStartService = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--cli-restore-point" && i + 1 < argc) {
            mode = Mode::RestorePoint;
            targetArg = argv[++i];
        } else if (arg == L"--cli-install-inf" && i + 1 < argc) {
            mode = Mode::InstallInf;
            targetArg = argv[++i];
        } else if (arg == L"--cli-remove-oem-infs" && i + 1 < argc) {
            mode = Mode::RemoveOemInfs;
            targetArg = argv[++i];
        } else if (arg == L"--cli-wu-install" && i + 1 < argc) {
            mode = Mode::WuInstall;
            targetArg = argv[++i];
        } else if (arg == L"--cli-enable-service" && i + 1 < argc) {
            mode = Mode::EnableService;
            targetArg = argv[++i];
        } else if (arg == L"--cli-reset-wucache") {
            mode = Mode::ResetWuCache;
        } else if (arg == L"--cli-sfc") {
            mode = Mode::SfcScan;
        } else if (arg == L"--cli-dism-check") {
            mode = Mode::DismCheck;
        } else if (arg == L"--cli-dism-restore") {
            mode = Mode::DismRestore;
        } else if (arg == L"--log" && i + 1 < argc) {
            logPath = argv[++i];
        } else if (arg == L"--cancel-file" && i + 1 < argc) {
            cancelFilePath = argv[++i];
        } else if (arg == L"--force") {
            forceInf = true;
        } else if (arg == L"--restore-before") {
            restoreBefore = true;
        } else if (arg == L"--drivers-only") {
            driversOnly = true;
        } else if (arg == L"--auto-start") {
            autoStartService = true;
        }
    }

    if (mode == Mode::None) {
        return -1;
    }

    std::mutex fileLogMutex;
    std::ofstream ofs(std::filesystem::path(logPath), std::ios::binary | std::ios::app);
    auto writeRawLine = [&](const std::string& rawLine) {
        std::lock_guard<std::mutex> lock(fileLogMutex);
        if (!ofs.is_open()) {
            return;
        }
        ofs << rawLine << "\n";
        ofs.flush();
    };
    hdm::LogCallback fileLogger = [&](hdm::LogLevel lvl, const std::string& msg) {
        const char* tag = "[INFO] ";
        if (lvl == hdm::LogLevel::Success) tag = "[OK] ";
        else if (lvl == hdm::LogLevel::Warning) tag = "[WARN] ";
        else if (lvl == hdm::LogLevel::Error) tag = "[ERR] ";
        writeRawLine(std::string(tag) + msg);
    };

    if (mode == Mode::RestorePoint) {
        const bool ok = hdm::SystemRestore::CreateRestorePointSync(
            hdm::WideToUtf8(targetArg), fileLogger);
        return ok ? 0 : 1;
    }

    if (mode == Mode::InstallInf) {
        const hdm::LocalInstallSummary summary =
            hdm::LocalDriverInstaller::InstallFromFolderSync(
                targetArg, forceInf, restoreBefore, fileLogger);
        if (summary.failedCount > 0) {
            return 1;
        }
        return summary.rebootRequired ? 2 : 0;
    }

    if (mode == Mode::RemoveOemInfs) {
        std::vector<std::string> names;
        std::stringstream ss(hdm::WideToUtf8(targetArg));
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (!token.empty()) {
                names.push_back(token);
            }
        }
        size_t removed = 0;
        size_t failed = 0;
        bool rebootReq = false;
        hdm::LocalDriverInstaller::RemoveDriverStorePackagesSync(
            names, restoreBefore, fileLogger, &removed, &failed, &rebootReq);
        if (failed > 0 && removed == 0) {
            return 1;
        }
        return rebootReq ? 2 : 0;
    }

    if (mode == Mode::EnableService) {
        return hdm::SystemDiagnostics::EnableServiceSync(
                   hdm::WideToUtf8(targetArg), autoStartService, fileLogger)
                   ? 0
                   : 1;
    }

    if (mode == Mode::ResetWuCache) {
        return hdm::SystemDiagnostics::ResetWuDownloadCacheSync(fileLogger) ? 0 : 1;
    }

    struct CancelFileWatcherGuard {
        std::atomic<bool> cancel{false};
        std::atomic<bool> stop{false};
        std::thread watcher;

        explicit CancelFileWatcherGuard(const std::wstring& path) {
            if (!path.empty()) {
                const std::filesystem::path marker(path);
                watcher = std::thread([marker, this]() {
                    while (!stop.load()) {
                        std::error_code ec;
                        if (std::filesystem::exists(marker, ec)) {
                            cancel.store(true);
                            break;
                        }
                        Sleep(200);
                    }
                });
            }
        }
        ~CancelFileWatcherGuard() {
            stop.store(true);
            if (watcher.joinable()) {
                watcher.join();
            }
        }
    };

    if (mode == Mode::SfcScan || mode == Mode::DismCheck || mode == Mode::DismRestore) {
        CancelFileWatcherGuard cancelGuard(cancelFilePath);
        if (mode == Mode::SfcScan) {
            return hdm::SystemDiagnostics::RunSystemToolSync(
                       L"sfc.exe", L"/scannow", "SFC /scannow", fileLogger, &cancelGuard.cancel)
                       ? 0
                       : 1;
        }
        if (mode == Mode::DismCheck) {
            return hdm::SystemDiagnostics::RunSystemToolSync(
                       L"dism.exe",
                       L"/Online /Cleanup-Image /CheckHealth",
                       "DISM /CheckHealth",
                       fileLogger,
                       &cancelGuard.cancel)
                       ? 0
                       : 1;
        }
        return hdm::SystemDiagnostics::RunSystemToolSync(
                   L"dism.exe",
                   L"/Online /Cleanup-Image /RestoreHealth",
                   "DISM /RestoreHealth",
                   fileLogger,
                   &cancelGuard.cancel)
                   ? 0
                   : 1;
    }

    if (mode == Mode::WuInstall) {
        std::vector<std::string> ids;
        std::stringstream ss(hdm::WideToUtf8(targetArg));
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (!token.empty()) {
                ids.push_back(token);
            }
        }

        auto sanitizeMeta = [](std::string text) {
            for (char& ch : text) {
                if (ch == '\r' || ch == '\n' || ch == '|') {
                    ch = ' ';
                }
            }
            return text;
        };

        std::atomic<float> workerProgress{0.0f};
        CancelFileWatcherGuard cancelGuard(cancelFilePath);

        const hdm::WuInstallSummary summary =
            hdm::WindowsUpdateManager::DownloadAndInstallSync(
                ids,
                restoreBefore,
                fileLogger,
                &workerProgress,
                &cancelGuard.cancel,
                [&](const std::string& phase) {
                    const int milli = static_cast<int>(
                        std::clamp(workerProgress.load(), 0.0f, 1.0f) * 1000.0f);
                    writeRawLine(
                        "[META] PROG|" + std::to_string(milli) + "|" + sanitizeMeta(phase));
                },
                [&](const std::string& updateId,
                    bool installedOk,
                    bool rebootReq,
                    const std::string& statusText) {
                    writeRawLine(
                        std::string("[META] ITEM|") + sanitizeMeta(updateId) + "|" +
                        (installedOk ? "1" : "0") + "|" +
                        (rebootReq ? "1" : "0") + "|" +
                        sanitizeMeta(statusText));
                },
                driversOnly);

        if (summary.failedCount > 0 && summary.installedCount == 0) {
            return 1;
        }
        return summary.rebootRequired ? 2 : 0;
    }

    return 1;
}

// --- Application UI Controller ---
class DriverLoomApp {
public:
    explicit DriverLoomApp(HWND mainHwnd, int initialTab = -1)
        : hwnd_(mainHwnd),
          isElevated_(hdm::IsProcessElevated()),
          pendingInitialTab_(initialTab) {
        logBuffer_.Push(
            hdm::LogLevel::Info,
            std::string("DriverLoom запущен. Режим прав: ") +
                (isElevated_ ? "Администратор" : "Обычный (UAC по запросу)"));
        scanner_.StartScanAsync(MakeLogger());
    }

    void RenderFrame() {
        SyncScannerSnapshots();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);

        constexpr ImGuiWindowFlags kRootFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings;

        ImGui::Begin("##MainWorkspace", nullptr, kRootFlags);

        RenderHeaderBar();
        if (rebootRequired_.load()) {
            RenderRebootBanner();
        }

        const float availHeight = ImGui::GetContentRegionAvail().y;
        const float spacingY = ImGui::GetStyle().ItemSpacing.y;
        const float logPanelHeight = std::clamp(availHeight * 0.20f, 128.0f, 195.0f);
        const float tabAreaHeight = std::max(280.0f, availHeight - logPanelHeight - spacingY);

        ImGui::BeginChild(
            "##TabsRegion",
            ImVec2(0.0f, tabAreaHeight),
            false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        if (ImGui::BeginTabBar("##MainTabs")) {
            auto beginTabCard = [](const char* childId) {
                ImGui::BeginChild(
                    childId,
                    ImVec2(0.0f, 0.0f),
                    true,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            };
            auto tabFlag = [this](int idx) {
                return (pendingInitialTab_ == idx)
                           ? ImGuiTabItemFlags_SetSelected
                           : ImGuiTabItemFlags_None;
            };

            if (ImGui::BeginTabItem("  Устройства и мониторы  ", nullptr, tabFlag(0))) {
                beginTabCard("##TabCardDevices");
                RenderDevicesTab();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("  Компоненты и периферия  ", nullptr, tabFlag(1))) {
                beginTabCard("##TabCardComponents");
                RenderComponentsTab();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("  Windows Update  ", nullptr, tabFlag(2))) {
                beginTabCard("##TabCardWu");
                RenderWindowsUpdateTab();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("  Локальные (.inf)  ", nullptr, tabFlag(3))) {
                beginTabCard("##TabCardLocal");
                RenderLocalInstallerTab();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("  Система и диагностика  ", nullptr, tabFlag(4))) {
                beginTabCard("##TabCardDiag");
                RenderDiagnosticsTab();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        pendingInitialTab_ = -1;
        ImGui::EndChild();

        RenderLogPanel();

        ImGui::End();
    }

    bool ShouldExitApp() const noexcept { return exitRequested_; }
    void SetInitialDiagSection(int sec) noexcept { diagSectionIndex_ = sec; }
    void SetInitialLocalSection(int sec) noexcept { localSectionIndex_ = sec; }
    bool IsInitialScanSettled() const noexcept {
        return lastDeviceGen_ > 0 && lastComponentsGen_ > 0 && lastDiagGen_ > 0 &&
               lastDriverStoreGen_ > 0 &&
               !scanner_.IsScanning() && !componentsScanner_.IsScanning() &&
               !diagnostics_.IsScanning() && !localInstaller_.IsScanningStore();
    }

private:
    enum class DeviceFilterMode {
        All,
        ProblemOnly,
        Monitors,
        Peripherals,
        DisplayAndChipset
    };

    enum class ComponentFilterMode {
        All,
        GamingRuntimes,
        Peripherals,
        MonitorsAndGpu
    };

    enum class DriverStoreFilterMode {
        All,
        OldDuplicates,
        ActiveOnly,
        UnusedOnly
    };

    hdm::LogCallback MakeLogger() {
        return [this](hdm::LogLevel lvl, const std::string& msg) {
            logBuffer_.Push(lvl, msg);
        };
    }

    void SyncScannerSnapshots() {
        const uint64_t devGen = scanner_.Generation();
        if (devGen != lastDeviceGen_) {
            devicesCache_ = scanner_.GetDevicesSnapshot();
            lastDeviceGen_ = devGen;
            totalDevicesCount_ = devicesCache_.size();
            problemDevicesCount_ = 0;
            monitorDevicesCount_ = 0;
            peripheralDevicesCount_ = 0;
            for (const auto& d : devicesCache_) {
                if (d.health != hdm::DeviceHealth::Ok) ++problemDevicesCount_;
                if (d.category == hdm::DeviceCategory::Monitor) ++monitorDevicesCount_;
                if (d.category == hdm::DeviceCategory::Peripheral) ++peripheralDevicesCount_;
            }
            deviceViewDirty_ = true;

            // Automatically refresh gaming runtimes / peripheral software when device list updates.
            if (!componentsScanner_.IsScanning() && !componentsScanner_.IsInstalling()) {
                componentsScanner_.StartLocalScanAsync(devicesCache_, MakeLogger());
            }
            if (!diagnostics_.IsBusy()) {
                diagnostics_.StartScanAsync(devicesCache_, MakeLogger());
            }
            if (!localInstaller_.IsScanningStore() && !localInstaller_.IsRemovingStorePackages()) {
                localInstaller_.ScanDriverStoreAsync(devicesCache_, nullptr);
            }
        }

        const uint64_t compGen = componentsScanner_.Generation();
        if (compGen != lastComponentsGen_) {
            componentsCache_ = componentsScanner_.GetItemsSnapshot();
            lastComponentsGen_ = compGen;
            compViewDirty_ = true;
        }

        const uint64_t diagGen = diagnostics_.Generation();
        if (diagGen != lastDiagGen_) {
            diagCache_ = diagnostics_.GetSnapshot();
            lastDiagGen_ = diagGen;
        }

        const uint64_t storeGen = localInstaller_.StoreGeneration();
        if (storeGen != lastDriverStoreGen_) {
            driverStoreCache_ = localInstaller_.GetDriverStoreSnapshot();
            lastDriverStoreGen_ = storeGen;
            storeViewDirty_ = true;
        }

        if (pendingDeviceRescan_.exchange(false) && !scanner_.IsScanning()) {
            scanner_.StartScanAsync(MakeLogger());
        }

        if (g_windowRegainedFocus.exchange(false)) {
            const double now = ImGui::GetTime();
            if (now - lastFocusRefreshTime_ > 2.5 &&
                !devicesCache_.empty() &&
                !componentsScanner_.IsScanning() &&
                !componentsScanner_.IsInstalling() &&
                !componentsScanner_.IsCheckingOnline()) {
                lastFocusRefreshTime_ = now;
                componentsScanner_.RefreshLocalPreserveOnlineAsync(devicesCache_);
                if (!diagnostics_.IsBusy()) {
                    diagnostics_.StartScanAsync(devicesCache_, nullptr);
                }
            }
        }
    }

    void RenderHeaderBar() {
        const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
        const ImGuiStyle& style = ImGui::GetStyle();
        const float padY = std::round(9.0f * uiScale);
        const float headerHeight = ImGui::GetFrameHeight() + padY * 2.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, padY));
        ImGui::BeginChild(
            "##HeaderBar",
            ImVec2(0.0f, headerHeight),
            true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.45f, 0.73f, 1.00f, 1.00f), "DriverLoom");
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("v1.0.0");

        ImGui::SameLine(0.0f, 12.0f);
        if (isElevated_) {
            RenderStatusBadge(
                ImVec4(0.20f, 0.83f, 0.60f, 1.00f),
                "Администратор",
                true);
        } else {
            RenderStatusBadge(
                ImVec4(0.98f, 0.75f, 0.14f, 1.00f),
                "Обычный режим",
                true);
            ImGui::SameLine(0.0f, 10.0f);
            if (RenderSecondaryButton("Перезапустить как администратор")) {
                const hdm::ElevatedRunResult run = hdm::ExecuteElevatedSelf(L"", false);
                if (run.launched) {
                    exitRequested_ = true;
                } else if (run.launchError != ERROR_CANCELLED) {
                    logBuffer_.Push(
                        hdm::LogLevel::Error,
                        "Не удалось повысить права: " + hdm::FormatWin32Error(run.launchError));
                }
            }
        }

        const char* srLabel = systemRestore_.IsBusy()
                                  ? "Создание точки восстановления..."
                                  : "Точка восстановления";
        const float btnWidth = CalcButtonWidth(srLabel) + 6.0f * uiScale;
        const float rightX = ImGui::GetWindowContentRegionMax().x - btnWidth;
        if (rightX > ImGui::GetCursorPosX() + 12.0f) {
            ImGui::SameLine(rightX);
        } else {
            ImGui::SameLine();
        }

        ImGui::BeginDisabled(systemRestore_.IsBusy());
        if (RenderSecondaryButton(srLabel, ImVec2(btnWidth, 0.0f))) {
            systemRestore_.CreateRestorePointAsync(
                "DriverLoom (ручная точка восстановления)",
                MakeLogger());
        }
        ImGui::EndDisabled();

        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    void RenderRebootBanner() {
        const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
        const ImGuiStyle& style = ImGui::GetStyle();
        const float padY = std::round(7.0f * uiScale);
        const float bannerHeight = ImGui::GetFrameHeight() + padY * 2.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, padY));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.24f, 0.16f, 0.05f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.96f, 0.68f, 0.16f, 0.90f));
        ImGui::BeginChild(
            "##RebootBanner",
            ImVec2(0.0f, bannerHeight),
            true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        RenderStatusBadge(
            ImVec4(1.00f, 0.87f, 0.45f, 1.00f),
            "Требуется перезагрузка Windows для завершения установки драйверов.",
            true);
        const char* hideLabel = "Скрыть";
        const float hideW = CalcButtonWidth(hideLabel);
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0f,
                                 ImGui::GetWindowContentRegionMax().x - hideW));
        if (RenderSecondaryButton(hideLabel, ImVec2(hideW, 0.0f))) {
            rebootRequired_.store(false);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
    }

    void RebuildDeviceViewCache(ImGuiTableSortSpecs* sortSpecs) {
        visibleDeviceIndices_.clear();
        visibleDeviceIndices_.reserve(devicesCache_.size());

        std::wstring needleLower = hdm::Utf8ToWide(deviceFilterBuf_);
        if (!needleLower.empty()) {
            CharLowerBuffW(needleLower.data(), static_cast<DWORD>(needleLower.size()));
        }

        for (size_t i = 0; i < devicesCache_.size(); ++i) {
            const auto& d = devicesCache_[i];
            switch (deviceFilterMode_) {
            case DeviceFilterMode::ProblemOnly:
                if (d.health == hdm::DeviceHealth::Ok && !d.isGenericMonitorDriver) continue;
                break;
            case DeviceFilterMode::Monitors:
                if (d.category != hdm::DeviceCategory::Monitor) continue;
                break;
            case DeviceFilterMode::Peripherals:
                if (d.category != hdm::DeviceCategory::Peripheral) continue;
                break;
            case DeviceFilterMode::DisplayAndChipset:
                if (d.category != hdm::DeviceCategory::DisplayAndChipset) continue;
                break;
            case DeviceFilterMode::All:
            default:
                break;
            }

            if (!needleLower.empty() &&
                d.searchIndexLower.find(needleLower) == std::wstring::npos) {
                continue;
            }
            visibleDeviceIndices_.push_back(i);
        }

        if (sortSpecs != nullptr && sortSpecs->SpecsCount > 0) {
            const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
            const bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
            std::sort(
                visibleDeviceIndices_.begin(),
                visibleDeviceIndices_.end(),
                [&](size_t ia, size_t ib) {
                    const auto& a = devicesCache_[ia];
                    const auto& b = devicesCache_[ib];
                    int cmp = 0;
                    switch (spec.ColumnUserID) {
                    case 0: {
                        auto rank = [](hdm::DeviceHealth h) {
                            return h == hdm::DeviceHealth::Error ? 0
                                   : (h == hdm::DeviceHealth::Warning ? 1 : 2);
                        };
                        cmp = rank(a.health) - rank(b.health);
                        if (cmp == 0) cmp = a.statusText.compare(b.statusText);
                        break;
                    }
                    case 1: cmp = a.name.compare(b.name); break;
                    case 2: cmp = a.manufacturer.compare(b.manufacturer); break;
                    case 3: cmp = a.deviceClass.compare(b.deviceClass); break;
                    case 4: cmp = CompareVersionOrString(a.driverVersion, b.driverVersion); break;
                    case 5: cmp = a.driverDate.compare(b.driverDate); break;
                    case 6: cmp = a.hardwareId.compare(b.hardwareId); break;
                    default: break;
                    }
                    return asc ? (cmp < 0) : (cmp > 0);
                });
        }

        deviceViewDirty_ = false;
    }

    static void SameLineWrapIfNeeded(float nextItemWidth) {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float lastX = ImGui::GetItemRectMax().x;
        const float winRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        if (lastX + style.ItemSpacing.x + nextItemWidth <= winRight) {
            ImGui::SameLine();
        }
    }

    static float CalcButtonWidth(const char* label) {
        return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    }

    void RenderDevicesTab() {
        const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
        const bool isAppForeground = (GetForegroundWindow() == hwnd_);

        // Row 1: Scan button + Category filter pills (auto-wrap on narrow windows)
        const bool scanning = scanner_.IsScanning();
        ImGui::BeginDisabled(scanning);
        if (ImGui::Button(scanning ? "Сканирование..." : "Сканировать устройства")) {
            scanner_.StartScanAsync(MakeLogger());
        }
        ImGui::EndDisabled();

        SameLineWrapIfNeeded(CalcButtonWidth("Все"));
        if (RenderPillButton("Все", deviceFilterMode_ == DeviceFilterMode::All)) {
            deviceFilterMode_ = DeviceFilterMode::All;
            deviceViewDirty_ = true;
        }
        SameLineWrapIfNeeded(CalcButtonWidth("Требуют внимания"));
        if (RenderPillButton("Требуют внимания", deviceFilterMode_ == DeviceFilterMode::ProblemOnly)) {
            deviceFilterMode_ = DeviceFilterMode::ProblemOnly;
            deviceViewDirty_ = true;
        }
        SameLineWrapIfNeeded(CalcButtonWidth("Мониторы"));
        if (RenderPillButton("Мониторы", deviceFilterMode_ == DeviceFilterMode::Monitors)) {
            deviceFilterMode_ = DeviceFilterMode::Monitors;
            deviceViewDirty_ = true;
        }
        SameLineWrapIfNeeded(CalcButtonWidth("Периферия"));
        if (RenderPillButton("Периферия", deviceFilterMode_ == DeviceFilterMode::Peripherals)) {
            deviceFilterMode_ = DeviceFilterMode::Peripherals;
            deviceViewDirty_ = true;
        }
        SameLineWrapIfNeeded(CalcButtonWidth("Видео и чипсет"));
        if (RenderPillButton("Видео и чипсет", deviceFilterMode_ == DeviceFilterMode::DisplayAndChipset)) {
            deviceFilterMode_ = DeviceFilterMode::DisplayAndChipset;
            deviceViewDirty_ = true;
        }

        char summaryBuf[128];
        std::snprintf(
            summaryBuf,
            sizeof(summaryBuf),
            "Всего: %zu  |  Мониторы: %zu  |  Проблемные: %zu",
            totalDevicesCount_,
            monitorDevicesCount_,
            problemDevicesCount_);
        const float summaryW = ImGui::CalcTextSize(summaryBuf).x;
        const float lastItemEndLocalX = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        const float winRightX = ImGui::GetWindowContentRegionMax().x;
        if (winRightX - summaryW > lastItemEndLocalX + 16.0f) {
            ImGui::SameLine(winRightX - summaryW);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", summaryBuf);
        } else {
            ImGui::TextDisabled("%s", summaryBuf);
        }

        // Row 2: Responsive full-width search bar
        ImGui::SetNextItemWidth(std::max(180.0f, ImGui::GetContentRegionAvail().x));
        if (ImGui::InputTextWithHint(
                "##DeviceSearch",
                "Поиск по названию, модели монитора (EDID), бренду, версии или Hardware ID...",
                deviceFilterBuf_,
                sizeof(deviceFilterBuf_))) {
            deviceViewDirty_ = true;
        }

        ImGui::Spacing();

        constexpr ImGuiTableFlags kTableFlags =
            ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
            ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY;

        if (ImGui::BeginTable("##DevicesTable", 7, kTableFlags, ImGui::GetContentRegionAvail())) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(
                "Статус",
                ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch,
                1.28f,
                0);
            ImGui::TableSetupColumn("Устройство / Модель", ImGuiTableColumnFlags_WidthStretch, 1.45f, 1);
            ImGui::TableSetupColumn("Производитель", ImGuiTableColumnFlags_WidthStretch, 0.90f, 2);
            ImGui::TableSetupColumn("Класс", ImGuiTableColumnFlags_WidthStretch, 0.60f, 3);
            ImGui::TableSetupColumn("Версия", ImGuiTableColumnFlags_WidthStretch, 0.76f, 4);
            ImGui::TableSetupColumn("Дата", ImGuiTableColumnFlags_WidthStretch, 0.60f, 5);
            ImGui::TableSetupColumn("Hardware ID", ImGuiTableColumnFlags_WidthStretch, 1.28f, 6);
            ImGui::TableHeadersRow();

            ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
            if (sortSpecs != nullptr && sortSpecs->SpecsDirty) {
                deviceViewDirty_ = true;
                sortSpecs->SpecsDirty = false;
            }
            if (deviceViewDirty_) {
                RebuildDeviceViewCache(sortSpecs);
            }

            const float rowBadgeH = ImGui::GetTextLineHeight() + std::round(6.0f * uiScale);
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(visibleDeviceIndices_.size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const hdm::DeviceInfo& dev =
                        devicesCache_[visibleDeviceIndices_[static_cast<size_t>(row)]];
                    ImGui::TableNextRow();
                    ImGui::PushID(row);

                    // Col 0: Full-row selectable behind rounded status badge (zero X-shift)
                    ImGui::TableSetColumnIndex(0);
                    const ImVec2 cellStart = ImGui::GetCursorPos();
                    ImGui::Selectable(
                        "##rowSel",
                        false,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                        ImVec2(0.0f, rowBadgeH));
                    if (ImGui::BeginPopupContextItem("##DevCtxMenu")) {
                        ImGui::TextDisabled("%s", dev.name.c_str());
                        ImGui::Separator();
                        if (ImGui::MenuItem("Копировать Hardware ID", nullptr, false, !dev.hardwareId.empty())) {
                            ImGui::SetClipboardText(dev.hardwareId.c_str());
                            logBuffer_.Push(hdm::LogLevel::Info, "Скопирован Hardware ID: " + dev.hardwareId);
                        }
                        if (ImGui::MenuItem("Копировать короткий ID (VEN/DEV)", nullptr, false, !dev.searchHardwareKey.empty())) {
                            ImGui::SetClipboardText(dev.searchHardwareKey.c_str());
                        }
                        if (ImGui::MenuItem("Копировать название устройства")) {
                            ImGui::SetClipboardText(dev.name.c_str());
                        }
                        ImGui::Separator();
                        const std::string queryId =
                            !dev.edidModel.empty()
                                ? (dev.manufacturer + " " + dev.edidModel + " monitor driver")
                                : (!dev.searchHardwareKey.empty()
                                       ? (dev.searchHardwareKey + " Windows driver")
                                       : (dev.name + " Windows driver"));
                        if (ImGui::MenuItem("Найти драйвер в браузере (Google)")) {
                            OpenUrlInBrowser("https://www.google.com/search?q=" + UrlEncode(queryId));
                        }
                        if (ImGui::MenuItem("Найти в каталоге Microsoft Update", nullptr, false, !dev.searchHardwareKey.empty())) {
                            OpenUrlInBrowser(
                                "https://www.catalog.update.microsoft.com/Search.aspx?q=" +
                                UrlEncode(dev.searchHardwareKey));
                        }
                        ImGui::EndPopup();
                    }

                    ImGui::SetCursorPos(cellStart);
                    ImVec4 statusColor(0.20f, 0.83f, 0.60f, 1.00f); // Emerald
                    if (dev.health == hdm::DeviceHealth::Error) {
                        statusColor = ImVec4(0.97f, 0.44f, 0.44f, 1.00f); // Coral Red
                    } else if (dev.health == hdm::DeviceHealth::Warning || dev.isGenericMonitorDriver) {
                        statusColor = ImVec4(0.98f, 0.75f, 0.14f, 1.00f); // Amber
                    }
                    RenderStatusBadge(statusColor, dev.statusText.c_str(), false);

                    // Col 1: Device Name
                    ImGui::TableSetColumnIndex(1);
                    RenderCellText(dev.name);
                    if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(460.0f * uiScale);
                        ImGui::TextColored(ImVec4(0.45f, 0.73f, 1.00f, 1.00f), "%s", dev.name.c_str());
                        ImGui::TextDisabled(
                            "Производитель: %s  |  Класс: %s",
                            dev.manufacturer.empty() ? "—" : dev.manufacturer.c_str(),
                            dev.deviceClass.empty() ? "—" : dev.deviceClass.c_str());
                        ImGui::TextDisabled(
                            "INF: %s  |  Статус: %s",
                            dev.infName.empty() ? "—" : dev.infName.c_str(),
                            dev.statusText.c_str());
                        if (!dev.hardwareId.empty()) {
                            ImGui::TextWrapped("Hardware ID: %s", dev.hardwareId.c_str());
                        }
                        if (!dev.edidModel.empty()) {
                            ImGui::Text("Модель (EDID): %s", dev.edidModel.c_str());
                        }
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }

                    // Col 2: Manufacturer
                    ImGui::TableSetColumnIndex(2);
                    if (RenderCellText(dev.manufacturer) && isAppForeground &&
                        ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("%s", dev.manufacturer.c_str());
                    }

                    // Col 3: Class
                    ImGui::TableSetColumnIndex(3);
                    RenderCellText(dev.deviceClass);

                    // Col 4: Version
                    ImGui::TableSetColumnIndex(4);
                    RenderCellText(dev.driverVersion, dev.driverVersion.empty());

                    // Col 5: Date
                    ImGui::TableSetColumnIndex(5);
                    RenderCellText(dev.driverDate, true);

                    // Col 6: Hardware ID
                    ImGui::TableSetColumnIndex(6);
                    if (RenderCellText(dev.hardwareId, dev.hardwareId.empty()) &&
                        isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("%s", dev.hardwareId.c_str());
                    }

                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }

    void RebuildComponentsViewCache(ImGuiTableSortSpecs* sortSpecs) {
        visibleComponentIndices_.clear();
        visibleComponentIndices_.reserve(componentsCache_.size());

        for (size_t i = 0; i < componentsCache_.size(); ++i) {
            const auto& item = componentsCache_[i];
            if (compFilterMode_ == ComponentFilterMode::GamingRuntimes &&
                item.category != hdm::ComponentCategory::GamingRuntime) {
                continue;
            }
            if (compFilterMode_ == ComponentFilterMode::Peripherals &&
                item.category != hdm::ComponentCategory::PeripheralSoftware) {
                continue;
            }
            if (compFilterMode_ == ComponentFilterMode::MonitorsAndGpu &&
                item.category != hdm::ComponentCategory::MonitorAndGpu) {
                continue;
            }
            visibleComponentIndices_.push_back(i);
        }

        if (sortSpecs != nullptr && sortSpecs->SpecsCount > 0) {
            const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[0];
            const bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
            std::sort(
                visibleComponentIndices_.begin(),
                visibleComponentIndices_.end(),
                [&](size_t ia, size_t ib) {
                    const auto& a = componentsCache_[ia];
                    const auto& b = componentsCache_[ib];
                    int cmp = 0;
                    switch (spec.ColumnUserID) {
                    case 0: {
                        auto statusRank = [](const hdm::SystemComponentItem& x) {
                            if (!x.installed) return 0;
                            if (x.updateAvailable) return 1;
                            return 2;
                        };
                        cmp = statusRank(a) - statusRank(b);
                        if (cmp == 0) cmp = a.statusText.compare(b.statusText);
                        break;
                    }
                    case 1:
                        cmp = a.name.compare(b.name);
                        break;
                    case 2: {
                        const std::string& ha = a.detectedHardware.empty()
                                                    ? std::string("Игры и системные библиотеки Windows")
                                                    : a.detectedHardware;
                        const std::string& hb = b.detectedHardware.empty()
                                                    ? std::string("Игры и системные библиотеки Windows")
                                                    : b.detectedHardware;
                        cmp = ha.compare(hb);
                        break;
                    }
                    case 3:
                        cmp = CompareVersionOrString(a.installedVersion, b.installedVersion);
                        break;
                    case 4:
                        cmp = CompareVersionOrString(a.latestVersion, b.latestVersion);
                        break;
                    default:
                        break;
                    }
                    if (cmp == 0) {
                        cmp = a.name.compare(b.name);
                    }
                    return asc ? (cmp < 0) : (cmp > 0);
                });
        }

        compViewDirty_ = false;
    }

    void RenderComponentsTab() {
        const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
        const ImGuiStyle& compStyle = ImGui::GetStyle();
        const bool busy =
            componentsScanner_.IsScanning() ||
            componentsScanner_.IsCheckingOnline() ||
            componentsScanner_.IsInstalling();

        std::vector<hdm::SystemComponentItem> batchTargets;
        for (const auto& comp : componentsCache_) {
            if ((!comp.installed || comp.updateAvailable) && !comp.directInstallerUrl.empty()) {
                batchTargets.push_back(comp);
            }
        }

        ImGui::BeginDisabled(busy);
        if (RenderSecondaryButton("Обновить проверку")) {
            componentsScanner_.StartLocalScanAsync(devicesCache_, MakeLogger());
        }
        SameLineWrapIfNeeded(CalcButtonWidth("Проверить онлайн"));
        if (RenderSecondaryButton("Проверить онлайн")) {
            componentsScanner_.StartOnlineVersionCheckAsync(MakeLogger());
        }
        ImGui::EndDisabled();

        char batchLbl[96];
        if (!batchTargets.empty()) {
            std::snprintf(
                batchLbl,
                sizeof(batchLbl),
                "Обновить все доступные (%zu)###BatchCompBtn",
                batchTargets.size());
        } else {
            std::snprintf(batchLbl, sizeof(batchLbl), "Обновить все доступные (0)###BatchCompBtn");
        }
        SameLineWrapIfNeeded(CalcButtonWidth("Обновить все доступные (99)"));
        ImGui::BeginDisabled(busy || batchTargets.empty());
        if (ImGui::Button(batchLbl)) {
            componentsScanner_.DownloadAndInstallBatchAsync(
                std::move(batchTargets), devicesCache_, MakeLogger());
        }
        ImGui::EndDisabled();

        if (componentsScanner_.IsInstalling()) {
            SameLineWrapIfNeeded(CalcButtonWidth("Отмена"));
            if (RenderSecondaryButton("Отмена")) {
                componentsScanner_.RequestCancelInstall();
            }
        }

        const std::string phase = componentsScanner_.PhaseMessage();
        if (busy || !phase.empty()) {
            const float winRight = ImGui::GetWindowContentRegionMax().x;
            const float lastEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
            const float remain = winRight - lastEnd - 16.0f;
            const float phaseW = ImGui::CalcTextSize(phase.c_str()).x;
            if (remain >= phaseW) {
                ImGui::SameLine(winRight - phaseW);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", phase.c_str());
            } else if (remain >= 150.0f * uiScale) {
                ImGui::SameLine(0.0f, 16.0f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", EllipsizeUtf8(phase, remain).c_str());
            } else {
                ImGui::TextDisabled("%s", phase.c_str());
            }
        }

        // Row 2: Filter pills
        if (RenderPillButton("Все", compFilterMode_ == ComponentFilterMode::All)) {
            compFilterMode_ = ComponentFilterMode::All;
            compViewDirty_ = true;
        }
        SameLineWrapIfNeeded(CalcButtonWidth("Игры и библиотеки"));
        if (RenderPillButton("Игры и библиотеки", compFilterMode_ == ComponentFilterMode::GamingRuntimes)) {
            compFilterMode_ = ComponentFilterMode::GamingRuntimes;
            compViewDirty_ = true;
        }
        SameLineWrapIfNeeded(CalcButtonWidth("Клавиатуры / Мыши / Аудио"));
        if (RenderPillButton("Клавиатуры / Мыши / Аудио", compFilterMode_ == ComponentFilterMode::Peripherals)) {
            compFilterMode_ = ComponentFilterMode::Peripherals;
            compViewDirty_ = true;
        }
        SameLineWrapIfNeeded(CalcButtonWidth("Мониторы и GPU"));
        if (RenderPillButton("Мониторы и GPU", compFilterMode_ == ComponentFilterMode::MonitorsAndGpu)) {
            compFilterMode_ = ComponentFilterMode::MonitorsAndGpu;
            compViewDirty_ = true;
        }

        if (componentsScanner_.IsInstalling()) {
            const float prog = componentsScanner_.Progress();
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "%.0f%%", prog * 100.0f);
            ImGui::ProgressBar(prog, ImVec2(-1.0f, 18.0f * uiScale), overlay);
        }

        ImGui::Spacing();

        constexpr ImGuiTableFlags kTableFlags =
            ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
            ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY;

        const float primaryW = CalcButtonWidth("Установить") + 6.0f * uiScale;
        const float siteW = CalcButtonWidth("Сайт") + 6.0f * uiScale;
        const float fullActionW = primaryW + compStyle.ItemSpacing.x + siteW;
        const float actionColW = fullActionW + compStyle.CellPadding.x * 2.0f + 4.0f;

        if (ImGui::BeginTable("##ComponentsTable", 6, kTableFlags, ImGui::GetContentRegionAvail())) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(
                "Статус",
                ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch,
                1.46f,
                0);
            ImGui::TableSetupColumn(
                "Компонент / Официальное ПО", ImGuiTableColumnFlags_WidthStretch, 1.36f, 1);
            ImGui::TableSetupColumn(
                "Устройство / Назначение", ImGuiTableColumnFlags_WidthStretch, 1.10f, 2);
            ImGui::TableSetupColumn(
                "Установлено", ImGuiTableColumnFlags_WidthStretch, 0.70f, 3);
            ImGui::TableSetupColumn(
                "Актуальная", ImGuiTableColumnFlags_WidthStretch, 0.76f, 4);
            ImGui::TableSetupColumn(
                "Действие",
                ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed,
                actionColW,
                5);
            ImGui::TableHeadersRow();

            ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
            if (sortSpecs != nullptr && sortSpecs->SpecsDirty) {
                compViewDirty_ = true;
                sortSpecs->SpecsDirty = false;
            }
            if (compViewDirty_) {
                RebuildComponentsViewCache(sortSpecs);
            }

            const bool isAppForeground = (GetForegroundWindow() == hwnd_);

            for (size_t viewIdx = 0; viewIdx < visibleComponentIndices_.size(); ++viewIdx) {
                const size_t origIdx = visibleComponentIndices_[viewIdx];
                const auto& item = componentsCache_[origIdx];

                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(origIdx));

                ImGui::TableSetColumnIndex(0);
                ImVec4 statusCol(0.20f, 0.83f, 0.60f, 1.00f);
                if (!item.installed || item.updateAvailable) {
                    statusCol = ImVec4(0.98f, 0.75f, 0.14f, 1.00f);
                }
                RenderStatusBadge(statusCol, item.statusText.c_str(), true);
                if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("%s", item.statusText.c_str());
                }

                ImGui::TableSetColumnIndex(1);
                RenderCellText(item.name, false, true);
                if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(440.0f * uiScale);
                    ImGui::TextColored(ImVec4(0.45f, 0.73f, 1.00f, 1.00f), "%s", item.name.c_str());
                    ImGui::TextDisabled("Издатель: %s", item.vendor.c_str());
                    if (!item.purposeDescription.empty()) {
                        ImGui::Separator();
                        ImGui::TextUnformatted("За что отвечает:");
                        ImGui::TextWrapped("%s", item.purposeDescription.c_str());
                    }
                    if (!item.detectedHardware.empty()) {
                        ImGui::Separator();
                        ImGui::TextWrapped("Оборудование: %s", item.detectedHardware.c_str());
                    }
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }

                ImGui::TableSetColumnIndex(2);
                const std::string targetHw =
                    item.detectedHardware.empty() ? "Игры и системные библиотеки Windows"
                                                  : item.detectedHardware;
                if (RenderCellText(targetHw, false, true) && isAppForeground &&
                    ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("%s", targetHw.c_str());
                }

                ImGui::TableSetColumnIndex(3);
                if (RenderCellText(item.installedVersion, item.installedVersion.empty(), true) &&
                    isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("%s", item.installedVersion.c_str());
                }

                ImGui::TableSetColumnIndex(4);
                if (RenderCellText(item.latestVersion, item.latestVersion.empty(), true) &&
                    isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("%s", item.latestVersion.c_str());
                }

                ImGui::TableSetColumnIndex(5);
                if (!item.directInstallerUrl.empty()) {
                    const bool needAction = !item.installed || item.updateAvailable;
                    const char* btnLbl = !item.installed
                                             ? "Установить"
                                             : (item.updateAvailable ? "Обновить" : "Скачать");
                    ImGui::BeginDisabled(busy);
                    if (RenderActionButton(btnLbl, ImVec2(primaryW, 0.0f), needAction)) {
                        componentsScanner_.DownloadAndInstallAsync(
                            item, devicesCache_, MakeLogger());
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (RenderSecondaryButton("Сайт", ImVec2(siteW, 0.0f))) {
                        OpenUrlInBrowser(item.officialPageUrl);
                    }
                } else if (!item.officialPageUrl.empty()) {
                    if (RenderSecondaryButton("Открыть сайт", ImVec2(fullActionW, 0.0f))) {
                        OpenUrlInBrowser(item.officialPageUrl);
                    }
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    void RenderWindowsUpdateTab() {
        const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
        const bool isAppForeground = (GetForegroundWindow() == hwnd_);
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

        const bool busy = wuManager_.IsBusy();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Найти обновления")) {
            selectedWuIndex_ = -1;
            wuManager_.StartSearchAsync(MakeLogger(), !wuIncludeSystemUpdates_);
        }
        SameLineWrapIfNeeded(CalcButtonWidth("Выбрать все"));
        if (RenderSecondaryButton("Выбрать все")) {
            wuManager_.SelectAll(true);
            for (auto& u : wuCache_) u.selected = true;
        }
        SameLineWrapIfNeeded(CalcButtonWidth("Снять все"));
        if (RenderSecondaryButton("Снять все")) {
            wuManager_.SelectAll(false);
            for (auto& u : wuCache_) u.selected = false;
        }
        ImGui::EndDisabled();

        std::vector<std::string> selectedIds;
        for (const auto& u : wuCache_) {
            if (u.selected && !u.installedOk) {
                selectedIds.push_back(u.updateId);
            }
        }

        char installLbl[96];
        if (!selectedIds.empty()) {
            std::snprintf(
                installLbl,
                sizeof(installLbl),
                "Установить выбранные (%zu)###WuInstallBtn",
                selectedIds.size());
        } else {
            std::snprintf(installLbl, sizeof(installLbl), "Установить выбранные###WuInstallBtn");
        }
        const float installBtnW = CalcButtonWidth(
            selectedIds.empty() ? "Установить выбранные" : "Установить выбранные (99)");
        SameLineWrapIfNeeded(installBtnW);
        ImGui::BeginDisabled(busy || selectedIds.empty());
        if (ImGui::Button(installLbl)) {
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

        if (busy) {
            SameLineWrapIfNeeded(CalcButtonWidth("Отмена"));
            if (RenderSecondaryButton("Отмена")) {
                wuManager_.RequestCancel();
            }
        }

        const std::string phase = wuManager_.PhaseMessage();
        if (busy || !phase.empty()) {
            const float winRight = ImGui::GetWindowContentRegionMax().x;
            const float lastEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
            const float remain = winRight - lastEnd - 16.0f;
            const float phaseW = ImGui::CalcTextSize(phase.c_str()).x;
            if (remain >= phaseW) {
                ImGui::SameLine(winRight - phaseW);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", phase.c_str());
            } else if (remain >= 150.0f * uiScale) {
                ImGui::SameLine(0.0f, 14.0f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", EllipsizeUtf8(phase, remain).c_str());
            }
        }

        ImGui::Checkbox("Включать системные обновления и библиотеки (не только драйверы)", &wuIncludeSystemUpdates_);
        SameLineWrapIfNeeded(CalcButtonWidth("Точка восстановления перед установкой") + 32.0f * uiScale);
        ImGui::Checkbox("Точка восстановления перед установкой", &wuRestoreBefore_);

        if (wuManager_.IsInstalling() || wuManager_.IsSearching()) {
            if (wuManager_.IsSearching()) {
                const float anim = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
                ImGui::ProgressBar(anim, ImVec2(-1.0f, 18.0f * uiScale), "Поиск в Windows Update...");
            } else {
                const float prog = wuManager_.Progress();
                char overlay[64];
                std::snprintf(overlay, sizeof(overlay), "%.0f%%", prog * 100.0f);
                ImGui::ProgressBar(prog, ImVec2(-1.0f, 18.0f * uiScale), overlay);
            }
        }

        ImGui::Spacing();

        const float availY = ImGui::GetContentRegionAvail().y;
        const float wuSpacingY = ImGui::GetStyle().ItemSpacing.y;
        const float detailsH = (selectedWuIndex_ >= 0 && selectedWuIndex_ < static_cast<int>(wuCache_.size()))
                                   ? 90.0f * uiScale
                                   : 0.0f;
        const float tableH = std::max(120.0f, availY - (detailsH > 0.0f ? detailsH + wuSpacingY : 0.0f));

        constexpr ImGuiTableFlags kTableFlags =
            ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY;

        if (ImGui::BeginTable("##WuTable", 5, kTableFlags, ImVec2(0.0f, tableH))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(
                "Выбор",
                ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed,
                58.0f * uiScale,
                0);
            ImGui::TableSetupColumn("Обновление", ImGuiTableColumnFlags_WidthStretch, 1.80f, 1);
            ImGui::TableSetupColumn(
                "Производитель / Модель", ImGuiTableColumnFlags_WidthStretch, 1.05f, 2);
            ImGui::TableSetupColumn(
                "Размер", ImGuiTableColumnFlags_WidthStretch, 0.50f, 3);
            ImGui::TableSetupColumn(
                "Статус", ImGuiTableColumnFlags_WidthStretch, 0.98f, 4);
            ImGui::TableHeadersRow();

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
                            case 3:
                                cmp = (a.maxSizeBytes < b.maxSizeBytes)
                                          ? -1
                                          : (a.maxSizeBytes > b.maxSizeBytes ? 1 : 0);
                                break;
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
                RenderCellText(item.sizeFormatted, true, true);

                ImGui::TableSetColumnIndex(4);
                const ImVec4 col = item.installedOk
                                       ? ImVec4(0.20f, 0.83f, 0.60f, 1.00f)
                                       : ImVec4(0.60f, 0.72f, 0.90f, 1.00f);
                RenderStatusBadge(col, item.statusText.c_str(), true);

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        if (detailsH > 0.0f) {
            const auto& sel = wuCache_[static_cast<size_t>(selectedWuIndex_)];
            ImGui::BeginChild(
                "##WuDetails",
                ImVec2(0.0f, detailsH),
                true,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::TextWrapped("%s", sel.description.empty() ? sel.title.c_str() : sel.description.c_str());
            ImGui::TextDisabled(
                "Hardware ID: %s   |   Дата: %s   |   Update ID: %s",
                sel.driverHardwareId.empty() ? "—" : sel.driverHardwareId.c_str(),
                sel.driverVerDate.empty() ? "—" : sel.driverVerDate.c_str(),
                sel.updateId.c_str());
            ImGui::EndChild();
        }
    }

    void RebuildDriverStoreViewCache(ImGuiTableSortSpecs* sortSpecs) {
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

    void RenderDriverStoreSection() {
        const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
        const ImGuiStyle& style = ImGui::GetStyle();
        const bool isAppForeground = (GetForegroundWindow() == hwnd_);
        const bool busy = localInstaller_.IsPickingFolder() ||
                          localInstaller_.IsScanningStore() ||
                          localInstaller_.IsExportingBackup() ||
                          localInstaller_.IsRemovingStorePackages() ||
                          localInstaller_.IsInstalling();

        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Экспорт бэкапа в папку...")) {
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
                        localInstaller_.ExportDriverStoreBackupAsync(
                            path,
                            backupSkipDuplicates_,
                            MakeLogger(),
                            [this, path](bool ok, size_t exportedCount) {
                                if (ok && exportedCount > 0) {
                                    localInstaller_.ScanInfFilesAsync(path, nullptr);
                                }
                            });
                    }
                });
        }
        if (isAppForeground &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "Копирует установленные сторонние пакеты драйверов (FileRepository) в выбранную папку.\n"
                "После переустановки Windows выберите эту папку во вкладке «Установка из папки (.inf)».");
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

        char rmSelLbl[96];
        if (selectedCount > 0) {
            std::snprintf(
                rmSelLbl,
                sizeof(rmSelLbl),
                "Удалить выбранные (%zu / %s)###RmDupBtn",
                selectedCount,
                hdm::FormatBytes(selectedBytes).c_str());
        } else {
            std::snprintf(rmSelLbl, sizeof(rmSelLbl), "Удалить выбранные (0)###RmDupBtn");
        }
        SameLineWrapIfNeeded(CalcButtonWidth("Удалить выбранные (99 / 999.9 МБ)") + 6.0f);
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

        SameLineWrapIfNeeded(CalcButtonWidth("Выбрать дубликаты"));
        ImGui::BeginDisabled(busy || driverStoreCache_.duplicateCount == 0);
        if (RenderSecondaryButton("Выбрать дубликаты")) {
            for (auto& pkg : driverStoreCache_.packages) {
                pkg.selected = (!pkg.inUse && pkg.isOldDuplicate);
            }
        }
        ImGui::EndDisabled();

        SameLineWrapIfNeeded(CalcButtonWidth("Снять все"));
        ImGui::BeginDisabled(busy || selectedCount == 0);
        if (RenderSecondaryButton("Снять все")) {
            for (auto& pkg : driverStoreCache_.packages) {
                pkg.selected = false;
            }
        }
        ImGui::EndDisabled();

        SameLineWrapIfNeeded(CalcButtonWidth("Обновить"));
        ImGui::BeginDisabled(busy);
        if (RenderSecondaryButton("Обновить")) {
            localInstaller_.ScanDriverStoreAsync(devicesCache_, MakeLogger());
        }
        ImGui::EndDisabled();

        if (driverStoreCache_.valid) {
            char storeSummary[128];
            std::snprintf(
                storeSummary,
                sizeof(storeSummary),
                "Всего: %s  |  Активных: %zu  |  Дубликатов: %zu",
                hdm::FormatBytes(driverStoreCache_.totalStoreBytes).c_str(),
                driverStoreCache_.activeCount,
                driverStoreCache_.duplicateCount);
            const float sumW = ImGui::CalcTextSize(storeSummary).x;
            const float winRight = ImGui::GetWindowContentRegionMax().x;
            const float lastEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
            if (winRight - sumW > lastEnd + 16.0f) {
                ImGui::SameLine(winRight - sumW);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", storeSummary);
            } else {
                ImGui::TextDisabled("%s", storeSummary);
            }
        }

        // Row 2: Filter pills + checkboxes
        char allLbl[48], dupFiltLbl[48], actLbl[48];
        std::snprintf(allLbl, sizeof(allLbl), "Все (%zu)###DsAll", driverStoreCache_.packages.size());
        std::snprintf(dupFiltLbl, sizeof(dupFiltLbl), "Дубликаты (%zu)###DsDup", driverStoreCache_.duplicateCount);
        std::snprintf(actLbl, sizeof(actLbl), "Активные (%zu)###DsAct", driverStoreCache_.activeCount);

        if (RenderPillButton(allLbl, storeFilterMode_ == DriverStoreFilterMode::All)) {
            storeFilterMode_ = DriverStoreFilterMode::All;
            storeViewDirty_ = true;
        }
        SameLineWrapIfNeeded(CalcButtonWidth("Дубликаты (999)"));
        if (RenderPillButton(dupFiltLbl, storeFilterMode_ == DriverStoreFilterMode::OldDuplicates)) {
            storeFilterMode_ = DriverStoreFilterMode::OldDuplicates;
            storeViewDirty_ = true;
        }
        SameLineWrapIfNeeded(CalcButtonWidth("Активные (999)"));
        if (RenderPillButton(actLbl, storeFilterMode_ == DriverStoreFilterMode::ActiveOnly)) {
            storeFilterMode_ = DriverStoreFilterMode::ActiveOnly;
            storeViewDirty_ = true;
        }
        SameLineWrapIfNeeded(CalcButtonWidth("Неиспользуемые"));
        if (RenderPillButton("Неиспользуемые", storeFilterMode_ == DriverStoreFilterMode::UnusedOnly)) {
            storeFilterMode_ = DriverStoreFilterMode::UnusedOnly;
            storeViewDirty_ = true;
        }

        SameLineWrapIfNeeded(CalcButtonWidth("Без дубликатов") + 36.0f * uiScale);
        ImGui::Checkbox("Без дубликатов", &backupSkipDuplicates_);
        if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip("Не включать старые дубликаты пакетов при экспорте бэкапа в папку.");
        }
        SameLineWrapIfNeeded(CalcButtonWidth("Точка восстановления") + 32.0f * uiScale);
        ImGui::Checkbox("Точка восстановления", &storeRestoreBefore_);

        if (localInstaller_.IsExportingBackup() || localInstaller_.IsRemovingStorePackages()) {
            const float prog = localInstaller_.Progress();
            const std::string statusMsg = localInstaller_.StatusMessage();
            char overlay[128];
            std::snprintf(overlay, sizeof(overlay), "%.0f%% — %s", prog * 100.0f, statusMsg.c_str());
            ImGui::ProgressBar(prog, ImVec2(-1.0f, 18.0f * uiScale), overlay);
        }

        ImGui::Spacing();

        if (!driverStoreCache_.valid) {
            ImGui::TextDisabled("Выполняется анализ хранилища драйверов DriverStore (oem*.inf)...");
            return;
        }

        constexpr ImGuiTableFlags kTableFlags =
            ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY;

        const float rmBtnW = CalcButtonWidth("Удалить") + 8.0f * uiScale;
        const float actionColW = rmBtnW + style.CellPadding.x * 2.0f + 4.0f;

        if (ImGui::BeginTable("##DriverStoreTable", 9, kTableFlags, ImGui::GetContentRegionAvail())) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Выбор", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, 54.0f * uiScale, 8);
            ImGui::TableSetupColumn("Статус", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 1.44f, 0);
            ImGui::TableSetupColumn("Пакет", ImGuiTableColumnFlags_WidthStretch, 0.66f, 1);
            ImGui::TableSetupColumn("INF / Активное устройство", ImGuiTableColumnFlags_WidthStretch, 1.28f, 2);
            ImGui::TableSetupColumn("Поставщик", ImGuiTableColumnFlags_WidthStretch, 0.76f, 3);
            ImGui::TableSetupColumn("Класс", ImGuiTableColumnFlags_WidthStretch, 0.52f, 4);
            ImGui::TableSetupColumn("Версия / Дата", ImGuiTableColumnFlags_WidthStretch, 1.14f, 5);
            ImGui::TableSetupColumn("Размер", ImGuiTableColumnFlags_WidthStretch, 0.60f, 6);
            ImGui::TableSetupColumn("Действие", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, actionColW, 7);
            ImGui::TableHeadersRow();

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
                    RenderStatusBadge(ImVec4(0.98f, 0.75f, 0.14f, 1.00f), "Старый дубликат", true);
                } else if (pkg.inUse) {
                    RenderStatusBadge(ImVec4(0.20f, 0.83f, 0.60f, 1.00f), "Активен", true);
                } else {
                    RenderStatusBadge(ImVec4(0.58f, 0.68f, 0.86f, 1.00f), "В хранилище", true);
                }

                ImGui::TableSetColumnIndex(2);
                RenderCellText(pkg.oemInfName, false, true);

                ImGui::TableSetColumnIndex(3);
                const std::string title =
                    pkg.activeDeviceName.empty()
                        ? pkg.originalInfName
                        : (pkg.originalInfName + " — " + pkg.activeDeviceName);
                RenderCellText(title, false, true);
                if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(460.0f * uiScale);
                    ImGui::TextColored(ImVec4(0.45f, 0.73f, 1.00f, 1.00f), "%s (%s)", pkg.originalInfName.c_str(), pkg.oemInfName.c_str());
                    if (!pkg.activeDeviceName.empty()) {
                        ImGui::Text("Устройство: %s", pkg.activeDeviceName.c_str());
                    }
                    ImGui::TextDisabled("Поставщик: %s  |  Класс: %s",
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
                    if (RenderSecondaryButton("Удалить", ImVec2(rmBtnW, 0.0f))) {
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
                    const float usedW = ImGui::CalcTextSize("Занят").x;
                    const float availCell = ImGui::GetContentRegionAvail().x;
                    if (availCell > usedW) {
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availCell - usedW) * 0.5f);
                    }
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("Занят");
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    void RenderLocalInstallerTab() {
        const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
        const ImGuiStyle& style = ImGui::GetStyle();
        const bool isAppForeground = (GetForegroundWindow() == hwnd_);

        if (RenderPillButton("Установка из папки (.inf)", localSectionIndex_ == 0)) {
            localSectionIndex_ = 0;
        }
        char storePill[96];
        if (driverStoreCache_.duplicateCount > 0) {
            std::snprintf(
                storePill,
                sizeof(storePill),
                "Бэкап и хранилище DriverStore (%zu | %zu дубл.)###StorePill",
                driverStoreCache_.packages.size(),
                driverStoreCache_.duplicateCount);
        } else {
            std::snprintf(
                storePill,
                sizeof(storePill),
                "Бэкап и хранилище DriverStore (%zu)###StorePill",
                driverStoreCache_.packages.size());
        }
        SameLineWrapIfNeeded(CalcButtonWidth("Бэкап и хранилище DriverStore (999 | 99 дубл.)"));
        if (RenderPillButton(storePill, localSectionIndex_ == 1)) {
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

        const char* pickLbl = "Обзор папки...";
        const char* scanLbl = "Найти .inf";
        const float buttonsW =
            CalcButtonWidth(pickLbl) + CalcButtonWidth(scanLbl) + style.ItemSpacing.x * 2.0f + 4.0f;

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Папка с драйверами:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(180.0f, ImGui::GetContentRegionAvail().x - buttonsW));
        ImGui::InputTextWithHint(
            "##FolderInput",
            "Выберите папку с распакованными .inf драйверами...",
            localFolderBuf_,
            sizeof(localFolderBuf_));

        ImGui::SameLine();
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
        ImGui::SameLine();
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

        ImGui::Checkbox("Принудительная установка (DIIRFLAG_FORCE_INF)", &localForceInstall_);
        SameLineWrapIfNeeded(CalcButtonWidth("Точка восстановления перед установкой") + 32.0f * uiScale);
        ImGui::Checkbox("Точка восстановления перед установкой", &localRestoreBefore_);

        auto infFiles = localInstaller_.GetInfFilesSnapshot();
        SameLineWrapIfNeeded(CalcButtonWidth("Установить все .inf (DiInstallDriverW)"));
        ImGui::BeginDisabled(busy || std::strlen(localFolderBuf_) == 0);
        if (ImGui::Button("Установить все .inf (DiInstallDriverW)")) {
            localInstaller_.InstallFromFolderAsync(
                hdm::Utf8ToWide(localFolderBuf_),
                localForceInstall_,
                localRestoreBefore_,
                MakeLogger(),
                [this](const hdm::LocalInstallSummary& summary) {
                    if (summary.rebootRequired) {
                        rebootRequired_.store(true);
                    }
                    if (summary.installedCount > 0 || summary.stagedOnlyCount > 0) {
                        pendingDeviceRescan_.store(true);
                    }
                });
        }
        ImGui::EndDisabled();

        char infCountBuf[64];
        std::snprintf(infCountBuf, sizeof(infCountBuf), "Найдено .inf: %zu", infFiles.size());
        const float countW = ImGui::CalcTextSize(infCountBuf).x;
        const float winRight = ImGui::GetWindowContentRegionMax().x;
        const float lastEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        if (winRight - countW > lastEnd + 16.0f) {
            ImGui::SameLine(winRight - countW);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", infCountBuf);
        } else {
            ImGui::TextDisabled("%s", infCountBuf);
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
            ImGui::TableSetupColumn("Имя файла", ImGuiTableColumnFlags_WidthStretch, 1.00f, 0);
            ImGui::TableSetupColumn("Относительный путь", ImGuiTableColumnFlags_WidthStretch, 2.00f, 1);
            ImGui::TableSetupColumn("Размер", ImGuiTableColumnFlags_WidthStretch, 0.50f, 2);
            ImGui::TableHeadersRow();

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

    void RenderDiagnosticsTab() {
        const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
        const ImGuiStyle& style = ImGui::GetStyle();
        const bool busy = diagnostics_.IsBusy();
        const bool isAppForeground = (GetForegroundWindow() == hwnd_);

        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Обновить диагностику")) {
            diagnostics_.StartScanAsync(devicesCache_, MakeLogger());
        }
        SameLineWrapIfNeeded(CalcButtonWidth("Очистить кэш WU"));
        if (RenderSecondaryButton("Очистить кэш WU")) {
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
                "Останавливает wuauserv и BITS, очищает папку загрузок Windows Update "
                "(SoftwareDistribution\\Download) и запускает службы заново.\n"
                "Помогает, если скачивание обновлений зависло или повреждено.");
        }

        if (diagnostics_.IsRunningAction()) {
            SameLineWrapIfNeeded(CalcButtonWidth("Отмена"));
            if (RenderSecondaryButton("Отмена")) {
                diagnostics_.RequestCancel();
            }
            const std::string phase = diagnostics_.ActionPhase();
            if (!phase.empty()) {
                ImGui::SameLine(0.0f, 12.0f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(ImVec4(0.98f, 0.75f, 0.14f, 1.00f), "%s", phase.c_str());
            }
        } else if (diagCache_.valid) {
            ImGui::SameLine(0.0f, 12.0f);
            if (diagCache_.disabledCriticalServices > 0) {
                char msg[96];
                std::snprintf(
                    msg,
                    sizeof(msg),
                    "Отключено критичных служб: %zu",
                    diagCache_.disabledCriticalServices);
                RenderStatusBadge(ImVec4(0.97f, 0.44f, 0.44f, 1.00f), msg, true);
            } else if (diagCache_.attentionControllers > 0) {
                char msg[96];
                std::snprintf(
                    msg,
                    sizeof(msg),
                    "Контроллеры требуют внимания: %zu",
                    diagCache_.attentionControllers);
                RenderStatusBadge(ImVec4(0.98f, 0.75f, 0.14f, 1.00f), msg, true);
            } else {
                RenderStatusBadge(
                    ImVec4(0.20f, 0.83f, 0.60f, 1.00f),
                    "Системное окружение готово к обновлениям",
                    true);
            }
        }

        if (!diagCache_.valid) {
            ImGui::TextDisabled("Выполняется сбор сведений о материнской плате, службах и драйверах...");
            return;
        }

        // Top cards: Motherboard/BIOS & Windows Readiness (compact padding so table & footer fit cleanly)
        const float cardPadY = std::round(9.0f * uiScale);
        const float cardSpacingY = std::round(5.0f * uiScale);
        const float badgeH = ImGui::GetTextLineHeight() + std::round(6.0f * uiScale);
        const float cardH = std::max(
            126.0f * uiScale,
            ImGui::GetTextLineHeight() + cardSpacingY * 2.0f + 4.0f +
                (badgeH + cardSpacingY) * 3.0f + cardPadY * 2.0f);
        const float cardGap = style.ItemSpacing.x;
        const float cardW = std::max(260.0f, (ImGui::GetContentRegionAvail().x - cardGap) * 0.5f);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, cardPadY));
        ImGui::BeginChild(
            "##MoboDiagCard",
            ImVec2(cardW, cardH),
            true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, cardSpacingY));
        {
            const auto& mb = diagCache_.motherboard;
            ImGui::TextColored(ImVec4(0.45f, 0.73f, 1.00f, 1.00f), "Материнская плата и BIOS");
            ImGui::Separator();

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
                boardTitle = "Модель не определена в SMBIOS";
            }

            ImGui::TextDisabled("Плата:");
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
                if (RenderSecondaryButton("Сайт платы")) {
                    OpenUrlInBrowser(mb.supportUrl);
                }
                ImGui::SameLine();
            }
            if (RenderSecondaryButton("Поиск драйверов")) {
                OpenUrlInBrowser(
                    "https://www.google.com/search?q=" +
                    UrlEncode(boardTitle + " chipset lan audio drivers Windows"));
            }
            ImGui::SameLine();
            if (RenderSecondaryButton("Копировать")) {
                const std::string copyText = boardTitle + " | BIOS: " + biosInfo;
                ImGui::SetClipboardText(copyText.c_str());
                logBuffer_.Push(hdm::LogLevel::Info, "Скопировано в буфер: " + copyText);
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::SameLine(0.0f, cardGap);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, cardPadY));
        ImGui::BeginChild(
            "##ReadinessDiagCard",
            ImVec2(cardW, cardH),
            true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, cardSpacingY));
        {
            const auto& rd = diagCache_.readiness;
            ImGui::TextColored(ImVec4(0.45f, 0.73f, 1.00f, 1.00f), "Готовность Windows и защита");
            ImGui::Separator();

            if (rd.cbsRebootPending || rd.wuRebootRequired) {
                RenderStatusBadge(
                    ImVec4(0.98f, 0.75f, 0.14f, 1.00f),
                    "Ожидается перезагрузка Windows (WU / CBS)");
            } else if (rd.pendingFileRenames) {
                RenderStatusBadge(
                    ImVec4(0.98f, 0.75f, 0.14f, 1.00f),
                    "Есть отложенные операции замены системных файлов");
            } else {
                RenderStatusBadge(
                    ImVec4(0.20f, 0.83f, 0.60f, 1.00f),
                    "Перезагрузка не требуется");
            }

            if (rd.systemRestorePolicyDisabled) {
                RenderStatusBadge(
                    ImVec4(0.97f, 0.44f, 0.44f, 1.00f),
                    "Точки восстановления запрещены политикой (DisableSR)");
            } else if (rd.systemRestoreEnabled) {
                RenderStatusBadge(
                    ImVec4(0.20f, 0.83f, 0.60f, 1.00f),
                    "Защита системы (точки восстановления) активна");
            } else {
                RenderStatusBadge(
                    ImVec4(0.98f, 0.75f, 0.14f, 1.00f),
                    "Защита системы отключена — точки могут не создаться");
            }

            if (diagCache_.disabledCriticalServices > 0) {
                char svcWarn[128];
                std::snprintf(
                    svcWarn,
                    sizeof(svcWarn),
                    "Отключены критичные службы: %zu шт. (см. вкладку служб)",
                    diagCache_.disabledCriticalServices);
                RenderStatusBadge(ImVec4(0.97f, 0.44f, 0.44f, 1.00f), svcWarn);
            } else {
                RenderStatusBadge(
                    ImVec4(0.20f, 0.83f, 0.60f, 1.00f),
                    "Критичные службы обновлений и защиты не отключены");
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::PopStyleVar();

        // Section switcher pills: Controllers vs Windows Services
        char ctrlPill[96];
        std::snprintf(
            ctrlPill,
            sizeof(ctrlPill),
            "Ключевые контроллеры платы (%zu)",
            diagCache_.controllers.size());
        if (RenderPillButton(ctrlPill, diagSectionIndex_ == 0)) {
            diagSectionIndex_ = 0;
        }

        char svcPill[96];
        if (diagCache_.disabledCriticalServices > 0) {
            std::snprintf(
                svcPill,
                sizeof(svcPill),
                "Службы Windows и обновления (%zu | %zu откл.!)",
                diagCache_.services.size(),
                diagCache_.disabledCriticalServices);
        } else {
            std::snprintf(
                svcPill,
                sizeof(svcPill),
                "Службы Windows и обновления (%zu)",
                diagCache_.services.size());
        }
        SameLineWrapIfNeeded(CalcButtonWidth(svcPill));
        if (RenderPillButton(svcPill, diagSectionIndex_ == 1)) {
            diagSectionIndex_ = 1;
        }

        const float footerPadY = std::round(7.0f * uiScale);
        const float footerH = ImGui::GetFrameHeight() + footerPadY * 2.0f;
        const float tableH = std::max(84.0f, ImGui::GetContentRegionAvail().y - footerH - style.ItemSpacing.y);
        constexpr ImGuiTableFlags kDiagTableFlags =
            ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY;

        if (diagSectionIndex_ == 0) {
            const float catW = CalcButtonWidth("Каталог MS") + 6.0f * uiScale;
            const float searchW = CalcButtonWidth("Поиск") + 6.0f * uiScale;
            const float actionColW = catW + style.ItemSpacing.x + searchW + style.CellPadding.x * 2.0f + 4.0f;

            if (ImGui::BeginTable("##DiagControllersTable", 5, kDiagTableFlags, ImVec2(0.0f, tableH))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Состояние", ImGuiTableColumnFlags_WidthStretch, 1.34f);
                ImGui::TableSetupColumn("Роль", ImGuiTableColumnFlags_WidthStretch, 0.62f);
                ImGui::TableSetupColumn("Устройство", ImGuiTableColumnFlags_WidthStretch, 1.20f);
                ImGui::TableSetupColumn("Драйвер / Дата", ImGuiTableColumnFlags_WidthStretch, 0.94f);
                ImGui::TableSetupColumn("Действие", ImGuiTableColumnFlags_WidthFixed, actionColW);
                ImGui::TableHeadersRow();

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
                    RenderStatusBadge(col, item.statusText.c_str(), true);

                    ImGui::TableSetColumnIndex(1);
                    RenderCellText(item.role, false, true);

                    ImGui::TableSetColumnIndex(2);
                    RenderCellText(item.deviceName, false, true);
                    if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(440.0f * uiScale);
                        ImGui::TextColored(ImVec4(0.45f, 0.73f, 1.00f, 1.00f), "%s", item.deviceName.c_str());
                        ImGui::TextDisabled(
                            "Поставщик: %s | INF: %s",
                            item.manufacturer.empty() ? "—" : item.manufacturer.c_str(),
                            item.infName.empty() ? "—" : item.infName.c_str());
                        if (!item.hardwareId.empty()) {
                            ImGui::TextWrapped("Hardware ID: %s", item.hardwareId.c_str());
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
                    const bool hasKey = !item.searchKey.empty();
                    ImGui::BeginDisabled(!hasKey);
                    if (RenderSecondaryButton("Каталог MS", ImVec2(catW, 0.0f))) {
                        OpenUrlInBrowser(
                            "https://www.catalog.update.microsoft.com/Search.aspx?q=" +
                            UrlEncode(item.searchKey));
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (RenderSecondaryButton("Поиск", ImVec2(searchW, 0.0f))) {
                        const std::string q =
                            (hasKey ? (item.searchKey + " " + item.deviceName) : item.deviceName) +
                            " Windows driver";
                        OpenUrlInBrowser("https://www.google.com/search?q=" + UrlEncode(q));
                    }

                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        } else {
            const float svcBtnW = CalcButtonWidth("Включить") + 10.0f * uiScale;
            const float svcActionW = svcBtnW + style.CellPadding.x * 2.0f + 4.0f;
            if (ImGui::BeginTable("##DiagServicesTable", 5, kDiagTableFlags, ImVec2(0.0f, tableH))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Статус", ImGuiTableColumnFlags_WidthStretch, 1.18f);
                ImGui::TableSetupColumn("Служба", ImGuiTableColumnFlags_WidthStretch, 1.12f);
                ImGui::TableSetupColumn("Назначение", ImGuiTableColumnFlags_WidthStretch, 1.15f);
                ImGui::TableSetupColumn("Тип запуска", ImGuiTableColumnFlags_WidthStretch, 0.62f);
                ImGui::TableSetupColumn("Действие", ImGuiTableColumnFlags_WidthFixed, svcActionW);
                ImGui::TableHeadersRow();

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
                    RenderStatusBadge(col, svc.stateText.c_str(), true);

                    ImGui::TableSetColumnIndex(1);
                    const std::string svcLabel = svc.serviceName + " — " + svc.displayName;
                    if (RenderCellText(svcLabel, false, true) && isAppForeground &&
                        ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("%s", svcLabel.c_str());
                    }

                    ImGui::TableSetColumnIndex(2);
                    if (RenderCellText(svc.purpose, true, true) && isAppForeground &&
                        ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip("%s", svc.purpose.c_str());
                    }

                    ImGui::TableSetColumnIndex(3);
                    RenderCellText(svc.startTypeText, false, true);

                    ImGui::TableSetColumnIndex(4);
                    if (svc.health == hdm::ServiceHealthState::Disabled) {
                        ImGui::BeginDisabled(busy);
                        if (ImGui::Button("Включить", ImVec2(svcBtnW, 0.0f))) {
                            diagnostics_.RunMaintenanceAsync(
                                hdm::MaintenanceAction::EnableService,
                                svc.serviceName,
                                devicesCache_,
                                MakeLogger());
                        }
                        ImGui::EndDisabled();
                    } else {
                        const float okW = ImGui::CalcTextSize("ОК").x;
                        const float availCell = ImGui::GetContentRegionAvail().x;
                        if (availCell > okW) {
                            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availCell - okW) * 0.5f);
                        }
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("ОК");
                    }

                    ImGui::PopID();
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
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Проверка системных файлов:");
        ImGui::BeginDisabled(busy);
        ImGui::SameLine(0.0f, 10.0f);
        if (RenderSecondaryButton("SFC /scannow")) {
            diagnostics_.RunMaintenanceAsync(
                hdm::MaintenanceAction::SfcScan, "", devicesCache_, MakeLogger());
        }
        if (isAppForeground &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Запускает встроенную проверку и восстановление защищённых файлов Windows (sfc.exe /scannow).");
        }

        ImGui::SameLine();
        if (RenderSecondaryButton("DISM CheckHealth")) {
            diagnostics_.RunMaintenanceAsync(
                hdm::MaintenanceAction::DismCheckHealth, "", devicesCache_, MakeLogger());
        }
        if (isAppForeground &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Быстрая проверка хранилища компонентов Windows без изменений (DISM /CheckHealth).");
        }

        ImGui::SameLine();
        if (RenderSecondaryButton("DISM RestoreHealth")) {
            diagnostics_.RunMaintenanceAsync(
                hdm::MaintenanceAction::DismRestoreHealth, "", devicesCache_, MakeLogger());
        }
        if (isAppForeground &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Глубокое восстановление хранилища компонентов Windows через Windows Update (DISM /RestoreHealth).");
        }
        ImGui::EndDisabled();
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    void RenderLogPanel() {
        const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
        const ImGuiStyle& style = ImGui::GetStyle();
        bool wasDirty = false;
        const auto entries = logBuffer_.Snapshot(&wasDirty);

        ImGui::BeginChild(
            "##LogPanel",
            ImVec2(0.0f, 0.0f),
            true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.62f, 0.74f, 0.90f, 1.00f), "Журнал событий");
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("(%zu)", entries.size());

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9.0f * uiScale, 3.0f * uiScale));
        const float copyW = ImGui::CalcTextSize("Копировать").x + 18.0f * uiScale;
        const float clearW = ImGui::CalcTextSize("Очистить").x + 18.0f * uiScale;
        const float autoW = ImGui::CalcTextSize("Автопрокрутка").x + ImGui::GetFrameHeight() + 8.0f;
        const float totalRightW = autoW + copyW + clearW + style.ItemSpacing.x * 2.0f;
        const float winRight = ImGui::GetWindowContentRegionMax().x;
        if (winRight - totalRightW > ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 12.0f) {
            ImGui::SameLine(winRight - totalRightW);
        } else {
            ImGui::SameLine(0.0f, 16.0f);
        }

        ImGui::Checkbox("Автопрокрутка", &logAutoScroll_);
        ImGui::SameLine();
        if (RenderSecondaryButton("Копировать", ImVec2(copyW, 0.0f))) {
            std::string all;
            for (const auto& e : entries) {
                all += "[" + e.timestamp + "] " + e.message + "\n";
            }
            ImGui::SetClipboardText(all.c_str());
        }
        ImGui::SameLine();
        if (RenderSecondaryButton("Очистить", ImVec2(clearW, 0.0f))) {
            logBuffer_.Clear();
        }
        ImGui::PopStyleVar();
        ImGui::Separator();

        ImGui::BeginChild("##LogScroll", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_HorizontalScrollbar);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(entries.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& entry = entries[static_cast<size_t>(i)];
                ImVec4 col(0.84f, 0.88f, 0.94f, 1.00f);
                if (entry.level == hdm::LogLevel::Success) {
                    col = ImVec4(0.20f, 0.83f, 0.60f, 1.00f);
                } else if (entry.level == hdm::LogLevel::Warning) {
                    col = ImVec4(0.98f, 0.75f, 0.14f, 1.00f);
                } else if (entry.level == hdm::LogLevel::Error) {
                    col = ImVec4(0.97f, 0.44f, 0.44f, 1.00f);
                }
                ImGui::TextColored(ImVec4(0.49f, 0.56f, 0.67f, 1.00f), "[%s]", entry.timestamp.c_str());
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::TextUnformatted(entry.message.c_str());
                ImGui::PopStyleColor();
            }
        }
        if (logAutoScroll_ && wasDirty) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();

        ImGui::EndChild();
    }

    HWND hwnd_{nullptr};
    bool isElevated_{false};
    int pendingInitialTab_{-1};
    bool exitRequested_{false};
    std::atomic<bool> rebootRequired_{false};
    std::atomic<bool> pendingDeviceRescan_{false};
    double lastFocusRefreshTime_{-10.0};

    hdm::LogBuffer logBuffer_;
    hdm::DeviceScanner scanner_;
    hdm::SystemComponentsScanner componentsScanner_;
    hdm::SystemDiagnostics diagnostics_;
    hdm::WindowsUpdateManager wuManager_;
    hdm::LocalDriverInstaller localInstaller_;
    hdm::SystemRestore systemRestore_;

    // System diagnostics snapshot & tab subview state
    hdm::SystemDiagnosticsSnapshot diagCache_;
    uint64_t lastDiagGen_{0};
    int diagSectionIndex_{0};

    // Cached Devices UI state (zero per-frame filtering/sorting allocations)
    std::vector<hdm::DeviceInfo> devicesCache_;
    std::vector<size_t> visibleDeviceIndices_;
    uint64_t lastDeviceGen_{0};
    size_t totalDevicesCount_{0};
    size_t problemDevicesCount_{0};
    size_t monitorDevicesCount_{0};
    size_t peripheralDevicesCount_{0};
    char deviceFilterBuf_[256]{};
    DeviceFilterMode deviceFilterMode_{DeviceFilterMode::All};
    bool deviceViewDirty_{true};

    // Components & Peripherals UI state
    std::vector<hdm::SystemComponentItem> componentsCache_;
    std::vector<size_t> visibleComponentIndices_;
    uint64_t lastComponentsGen_{0};
    ComponentFilterMode compFilterMode_{ComponentFilterMode::All};
    bool compViewDirty_{true};

    // WU UI state
    std::vector<hdm::DriverUpdateItem> wuCache_;
    uint64_t lastWuGen_{0};
    int selectedWuIndex_{-1};
    bool wuSortDirty_{false};
    bool wuRestoreBefore_{true};
    bool wuIncludeSystemUpdates_{false};

    // Local INF & DriverStore backup UI state
    int localSectionIndex_{0};
    char localFolderBuf_[1024]{};
    bool localForceInstall_{false};
    bool localRestoreBefore_{true};
    bool backupSkipDuplicates_{true};
    bool storeRestoreBefore_{true};
    hdm::DriverStoreSnapshot driverStoreCache_;
    std::vector<size_t> visibleStoreIndices_;
    uint64_t lastDriverStoreGen_{0};
    DriverStoreFilterMode storeFilterMode_{DriverStoreFilterMode::All};
    bool storeViewDirty_{true};
    std::mutex pendingPathMutex_;
    std::string pendingFolderUtf8_;
    bool hasPendingFolder_{false};

    // Log UI state
    bool logAutoScroll_{true};
};

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    const int cliExit = RunCliModeIfRequested();
    if (cliExit >= 0) {
        return cliExit;
    }

    ImGui_ImplWin32_EnableDpiAwareness();

    constexpr int kAppIconId = 101;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = static_cast<HICON>(LoadImageW(
        hInstance,
        MAKEINTRESOURCEW(kAppIconId),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXICON),
        GetSystemMetrics(SM_CYICON),
        LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(
        hInstance,
        MAKEINTRESOURCEW(kAppIconId),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DriverLoomWndClass";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"DriverLoom",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1280,
        820,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr);

    if (hwnd == nullptr) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    EnableDarkTitleBar(hwnd);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    const float dpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    ApplyModernDarkTheme(dpiScale);

    const float fontSize = 16.5f * (dpiScale > 0.5f ? dpiScale : 1.0f);
    ImFont* loadedFont = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\segoeui.ttf",
        fontSize,
        nullptr,
        io.Fonts->GetGlyphRangesCyrillic());
    if (loadedFont == nullptr) {
        io.Fonts->AddFontDefault();
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    int initialTab = -1;
    int initialDiagSection = 0;
    int initialLocalSection = 0;
    std::wstring screenshotPath;
    {
        int argc = 0;
        if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
            for (int i = 1; i < argc; ++i) {
                const std::wstring arg = argv[i];
                if (arg == L"--start-tab" && i + 1 < argc) {
                    initialTab = _wtoi(argv[++i]);
                } else if (arg == L"--diag-section" && i + 1 < argc) {
                    initialDiagSection = _wtoi(argv[++i]);
                } else if (arg == L"--local-section" && i + 1 < argc) {
                    initialLocalSection = _wtoi(argv[++i]);
                } else if (arg == L"--screenshot" && i + 1 < argc) {
                    screenshotPath = argv[++i];
                }
            }
            LocalFree(argv);
        }
    }

    {
        DriverLoomApp app(hwnd, initialTab);
        app.SetInitialDiagSection(initialDiagSection);
        app.SetInitialLocalSection(initialLocalSection);
        const ImVec4 clearColor(0.059f, 0.071f, 0.098f, 1.00f);

        bool done = false;
        bool screenshotTaken = false;
        while (!done && !app.ShouldExitApp()) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT) {
                    done = true;
                }
            }
            if (done) {
                break;
            }

            if (screenshotPath.empty() && g_swapChainOccluded &&
                g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
                Sleep(10);
                continue;
            }
            g_swapChainOccluded = false;

            if (g_resizeWidth != 0 && g_resizeHeight != 0) {
                CleanupRenderTarget();
                g_pSwapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
                g_resizeWidth = 0;
                g_resizeHeight = 0;
                CreateRenderTarget();
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            app.RenderFrame();

            ImGui::Render();
            const float clearColorWithAlpha[4] = {
                clearColor.x, clearColor.y, clearColor.z, clearColor.w};
            g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
            g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColorWithAlpha);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

            if (!screenshotPath.empty() && !screenshotTaken) {
                const double elapsed = ImGui::GetTime();
                if ((elapsed >= 1.8 && app.IsInitialScanSettled()) || elapsed >= 4.0) {
                    SaveBackBufferToBmp(screenshotPath);
                    screenshotTaken = true;
                    done = true;
                }
            }

            const HRESULT hr = g_pSwapChain->Present(1, 0);
            g_swapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
        }
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}
