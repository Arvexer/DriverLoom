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

#include <cfgmgr32.h>
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
#include <unordered_set>
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
bool g_isLightTheme = false;
float g_dpiScale = 1.0f;

class DriverLoomApp;
DriverLoomApp* g_appInstance = nullptr;
bool g_isRenderingFrame = false;

void RenderSingleFrame(
    const std::wstring* screenshotPath = nullptr,
    bool* screenshotTaken = nullptr,
    bool* outCaptureDone = nullptr);

// --- Multi-tier Typography ---
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
    case WM_ENTERSIZEMOVE:
        SetTimer(hWnd, 1, 16, nullptr);
        return 0;
    case WM_EXITSIZEMOVE:
        KillTimer(hWnd, 1);
        return 0;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            return 0;
        }
        g_resizeWidth = LOWORD(lParam);
        g_resizeHeight = HIWORD(lParam);
        if (g_appInstance != nullptr && g_pd3dDevice != nullptr) {
            RenderSingleFrame();
        }
        return 0;
    case WM_TIMER:
        if (wParam == 1 && g_appInstance != nullptr && g_pd3dDevice != nullptr) {
            RenderSingleFrame();
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
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
        KillTimer(hWnd, 1);
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

void ApplyAppTheme(HWND hwnd, float scale, bool lightTheme, bool initLayout = false) {
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

bool RenderPillButton(const char* label, bool active) {
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
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar();
    return clicked;
}

float CalcSubSectionTabWidth(const char* label) {
    const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
    PushFontSafe(g_fontSecondaryHeader);
    const float w = ImGui::CalcTextSize(label).x + std::round(12.0f * uiScale) * 2.0f;
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

void RenderSecondaryCardHeader(const char* title, const char* rightMeta = nullptr) {
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

void RenderVerticalDivider(float padX = 4.0f) {
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

bool RenderSecondaryButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f)) {
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

struct SummaryBadgeSegment {
    const char* label = nullptr;
    const char* value = nullptr;
    ImVec4 accentColor = ImVec4(0.46f, 0.74f, 0.98f, 1.00f);
    bool tintLabel = false;
};

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
        WuHide,
        WuUnhide,
        EnableService,
        ResetWuCache,
        SfcScan,
        DismCheck,
        DismRestore,
        DeviceAction
    };
    Mode mode = Mode::None;
    std::wstring targetArg;
    std::wstring instanceArg;
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
        } else if (arg == L"--cli-wu-hide" && i + 1 < argc) {
            mode = Mode::WuHide;
            targetArg = argv[++i];
        } else if (arg == L"--cli-wu-unhide" && i + 1 < argc) {
            mode = Mode::WuUnhide;
            targetArg = argv[++i];
        } else if (arg == L"--cli-enable-service" && i + 1 < argc) {
            mode = Mode::EnableService;
            targetArg = argv[++i];
        } else if (arg == L"--cli-device-action" && i + 1 < argc) {
            mode = Mode::DeviceAction;
            targetArg = argv[++i];
        } else if (arg == L"--instance" && i + 1 < argc) {
            instanceArg = argv[++i];
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

    if (mode == Mode::DeviceAction) {
        hdm::DeviceAction devAct = hdm::DeviceAction::Restart;
        if (!hdm::DeviceScanner::ParseDeviceActionCliVerb(
                hdm::WideToUtf8(targetArg), devAct)) {
            fileLogger(hdm::LogLevel::Error, "Неизвестная команда управления устройством.");
            return 1;
        }
        bool rebootReq = false;
        const bool ok = hdm::DeviceScanner::RunDeviceActionSync(
            devAct, hdm::WideToUtf8(instanceArg), fileLogger, &rebootReq);
        if (!ok) {
            return 1;
        }
        return rebootReq ? 2 : 0;
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

    if (mode == Mode::WuHide || mode == Mode::WuUnhide) {
        std::vector<std::string> ids;
        std::stringstream ss(hdm::WideToUtf8(targetArg));
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (!token.empty()) {
                ids.push_back(token);
            }
        }
        const bool ok = hdm::WindowsUpdateManager::SetUpdatesHiddenSync(
            ids, mode == Mode::WuHide, fileLogger, driversOnly);
        return ok ? 0 : 1;
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
            std::string(hdm::Tr(
                "DriverLoom запущен. Режим прав: ",
                "DriverLoom запущено. Режим прав: ",
                "DriverLoom started. Privilege mode: ")) +
                (isElevated_
                     ? hdm::Tr("Администратор", "Адміністратор", "Administrator")
                     : hdm::Tr("Обычный (UAC по запросу)", "Звичайний (UAC за запитом)", "Standard (UAC on demand)")));
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

        const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
        auto pushMainTabBarStyle = [uiScale]() {
            PushFontSafe(g_fontMainTab);
            ImGui::PushStyleVar(
                ImGuiStyleVar_FramePadding,
                ImVec2(std::round(14.0f * uiScale), std::round(7.0f * uiScale)));
            ImGui::PushStyleVar(ImGuiStyleVar_TabBorderSize, 1.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_TabBarBorderSize, std::round(1.0f * uiScale));
            ImGui::PushStyleVar(ImGuiStyleVar_TabBarOverlineSize, std::max(2.0f, 2.5f * uiScale));
            if (g_isLightTheme) {
                ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.89f, 0.92f, 0.96f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.93f, 0.96f, 0.99f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_TabSelected, ImVec4(0.99f, 0.995f, 1.00f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, ImVec4(0.14f, 0.45f, 0.88f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_TabDimmed, ImVec4(0.89f, 0.92f, 0.96f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, ImVec4(0.99f, 0.995f, 1.00f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_TabDimmedSelectedOverline, ImVec4(0.14f, 0.45f, 0.88f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.75f, 0.80f, 0.87f, 0.90f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.072f, 0.088f, 0.122f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_TabHovered, ImVec4(0.108f, 0.138f, 0.195f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_TabSelected, ImVec4(0.128f, 0.172f, 0.248f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, ImVec4(0.36f, 0.70f, 1.00f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_TabDimmed, ImVec4(0.072f, 0.088f, 0.122f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, ImVec4(0.128f, 0.172f, 0.248f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_TabDimmedSelectedOverline, ImVec4(0.36f, 0.70f, 1.00f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.15f, 0.19f, 0.26f, 0.85f));
            }
        };
        auto popMainTabBarStyle = []() {
            ImGui::PopStyleColor(8);
            ImGui::PopStyleVar(4);
            PopFontSafe(g_fontMainTab);
        };

        pushMainTabBarStyle();
        if (ImGui::BeginTabBar("##MainTabs", ImGuiTabBarFlags_DrawSelectedOverline)) {
            auto tabFlag = [this](int idx) {
                return (pendingInitialTab_ == idx)
                           ? ImGuiTabItemFlags_SetSelected
                           : ImGuiTabItemFlags_None;
            };
            auto beginMainTab = [&](const char* label, int idx) -> bool {
                const bool isSel =
                    (pendingInitialTab_ >= 0) ? (pendingInitialTab_ == idx) : (activeTabIndex_ == idx);
                const ImVec4 borderCol =
                    g_isLightTheme
                        ? (isSel ? ImVec4(0.32f, 0.54f, 0.84f, 0.95f) : ImVec4(0.75f, 0.80f, 0.87f, 0.90f))
                        : (isSel ? ImVec4(0.28f, 0.46f, 0.72f, 0.95f) : ImVec4(0.15f, 0.19f, 0.26f, 0.85f));
                const ImVec4 textCol =
                    g_isLightTheme
                        ? (isSel ? ImVec4(0.06f, 0.14f, 0.28f, 1.00f) : ImVec4(0.36f, 0.42f, 0.52f, 1.00f))
                        : (isSel ? ImVec4(0.97f, 0.99f, 1.00f, 1.00f) : ImVec4(0.60f, 0.67f, 0.78f, 1.00f));
                ImGui::PushStyleColor(ImGuiCol_Border, borderCol);
                ImGui::PushStyleColor(ImGuiCol_Text, textCol);
                const bool open = ImGui::BeginTabItem(label, nullptr, tabFlag(idx));
                ImGui::PopStyleColor(2);
                return open;
            };
            auto runTabCard = [&](const char* childId, auto&& drawFn) {
                popMainTabBarStyle();
                const ImVec4 cardBorder = g_isLightTheme
                                              ? ImVec4(0.68f, 0.75f, 0.85f, 0.95f)
                                              : ImVec4(0.22f, 0.31f, 0.44f, 0.95f);
                ImGui::PushStyleColor(ImGuiCol_Border, cardBorder);
                ImGui::BeginChild(
                    childId,
                    ImVec2(0.0f, 0.0f),
                    true,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                ImGui::PopStyleColor();
                drawFn();
                ImGui::EndChild();
                pushMainTabBarStyle();
            };

            if (beginMainTab(
                    hdm::Tr(
                        "  Устройства и мониторы  ###Tab0",
                        "  Пристрої та монітори  ###Tab0",
                        "  Devices & Monitors  ###Tab0"),
                    0)) {
                activeTabIndex_ = 0;
                runTabCard("##TabCardDevices", [&]() { RenderDevicesTab(); });
                ImGui::EndTabItem();
            }
            if (beginMainTab(
                    hdm::Tr(
                        "  Компоненты и периферия  ###Tab1",
                        "  Компоненти та периферія  ###Tab1",
                        "  Components & Peripherals  ###Tab1"),
                    1)) {
                activeTabIndex_ = 1;
                runTabCard("##TabCardComponents", [&]() { RenderComponentsTab(); });
                ImGui::EndTabItem();
            }
            if (beginMainTab("  Windows Update  ###Tab2", 2)) {
                activeTabIndex_ = 2;
                runTabCard("##TabCardWu", [&]() { RenderWindowsUpdateTab(); });
                ImGui::EndTabItem();
            }
            if (beginMainTab(
                    hdm::Tr(
                        "  Локальные (.inf)  ###Tab3",
                        "  Локальні (.inf)  ###Tab3",
                        "  Local (.inf)  ###Tab3"),
                    3)) {
                activeTabIndex_ = 3;
                runTabCard("##TabCardLocal", [&]() { RenderLocalInstallerTab(); });
                ImGui::EndTabItem();
            }
            if (beginMainTab(
                    hdm::Tr(
                        "  Система и диагностика  ###Tab4",
                        "  Система та діагностика  ###Tab4",
                        "  System & Diagnostics  ###Tab4"),
                    4)) {
                activeTabIndex_ = 4;
                runTabCard("##TabCardDiag", [&]() { RenderDiagnosticsTab(); });
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        popMainTabBarStyle();
        pendingInitialTab_ = -1;
        HandleKeyboardShortcuts();
        ImGui::EndChild();

        RenderLogPanel();

        ImGui::End();
    }

    bool ShouldExitApp() const noexcept { return exitRequested_; }
    void SetInitialDiagSection(int sec) noexcept { diagSectionIndex_ = sec; }
    void SetInitialLocalSection(int sec) noexcept { localSectionIndex_ = sec; }
    void SetInitialSelectedDeviceRow(int row) noexcept { pendingSelectDeviceRow_ = row; }
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
        const bool isAppForeground = (GetForegroundWindow() == hwnd_);
        const float padY = std::round(9.0f * uiScale);
        const float baseFrameH = ImGui::GetFrameHeight();
        const float headerHeight = baseFrameH + padY * 2.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, padY));
        ImGui::PushStyleColor(
            ImGuiCol_Border,
            g_isLightTheme ? ImVec4(0.70f, 0.77f, 0.87f, 0.95f) : ImVec4(0.24f, 0.36f, 0.54f, 0.95f));
        ImGui::BeginChild(
            "##HeaderBar",
            ImVec2(0.0f, headerHeight),
            true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();

        {
            PushFontSafe(g_fontPrimaryHeader);
            const ImVec2 titleSize = ImGui::CalcTextSize("DriverLoom");
            const ImVec2 startScreen = ImGui::GetCursorScreenPos();
            const float yOff = std::max(0.0f, (baseFrameH - titleSize.y) * 0.5f);
            const ImVec4 titleCol = g_isLightTheme
                                        ? ImVec4(0.08f, 0.34f, 0.74f, 1.00f)
                                        : ImVec4(0.50f, 0.78f, 1.00f, 1.00f);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(startScreen.x, startScreen.y + yOff),
                ImGui::ColorConvertFloat4ToU32(titleCol),
                "DriverLoom");
            ImGui::Dummy(ImVec2(titleSize.x, baseFrameH));
            PopFontSafe(g_fontPrimaryHeader);
        }
        RenderVerticalDivider(6.0f);
        ImGui::SameLine(0.0f, 2.0f * uiScale);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("v1.0.0");

        ImGui::SameLine(0.0f, 12.0f * uiScale);
        if (isElevated_) {
            RenderStatusBadge(
                ImVec4(0.20f, 0.83f, 0.60f, 1.00f),
                hdm::Tr("Администратор", "Адміністратор", "Administrator"),
                true);
        } else {
            RenderStatusBadge(
                ImVec4(0.98f, 0.75f, 0.14f, 1.00f),
                hdm::Tr("Обычный режим", "Звичайний режим", "Standard mode"),
                true);
            ImGui::SameLine(0.0f, 10.0f * uiScale);
            if (RenderSecondaryButton(hdm::Tr(
                    "Перезапустить как администратор###ElevBtn",
                    "Перезапустити як адміністратор###ElevBtn",
                    "Restart as Administrator###ElevBtn"))) {
                const hdm::ElevatedRunResult run = hdm::ExecuteElevatedSelf(L"", false);
                if (run.launched) {
                    exitRequested_ = true;
                } else if (run.launchError != ERROR_CANCELLED) {
                    logBuffer_.Push(
                        hdm::LogLevel::Error,
                        std::string(hdm::Tr(
                            "Не удалось повысить права: ",
                            "Не вдалося підвищити права: ",
                            "Failed to elevate privileges: ")) +
                            hdm::FormatWin32Error(run.launchError));
                }
            }
        }

        const char* srLabel = systemRestore_.IsBusy()
                                  ? hdm::Tr(
                                        "Создание точки восстановления...###SrBtn",
                                        "Створення точки відновлення...###SrBtn",
                                        "Creating restore point...###SrBtn")
                                  : hdm::Tr(
                                        "Точка восстановления###SrBtn",
                                        "Точка відновлення###SrBtn",
                                        "Restore Point###SrBtn");
        const char* srMeasure = systemRestore_.IsBusy()
                                    ? hdm::Tr(
                                          "Создание точки восстановления...",
                                          "Створення точки відновлення...",
                                          "Creating restore point...")
                                    : hdm::Tr(
                                          "Точка восстановления",
                                          "Точка відновлення",
                                          "Restore Point");
        const float btnWidth = CalcButtonWidth(srMeasure) + 6.0f * uiScale;

        struct LangOption {
            hdm::Lang lang;
            const char* label;
        };
        static constexpr LangOption kLangOptions[] = {
            {hdm::Lang::Ru, "RU — Русский"},
            {hdm::Lang::Uk, "UA — Українська"},
            {hdm::Lang::En, "EN — English"},
        };
        const hdm::Lang curLang = hdm::GetLang();
        const char* curLangLabel = kLangOptions[0].label;
        float maxLangLabelW = 0.0f;
        for (const auto& opt : kLangOptions) {
            maxLangLabelW = std::max(maxLangLabelW, ImGui::CalcTextSize(opt.label).x);
            if (opt.lang == curLang) {
                curLangLabel = opt.label;
            }
        }
        const float langComboW =
            maxLangLabelW + style.FramePadding.x * 2.0f + baseFrameH + 6.0f * uiScale;

        const float fullThemeBtnW = std::max(
            CalcButtonWidth(hdm::Tr("Тема: Светлая", "Тема: Світла", "Theme: Light")),
            CalcButtonWidth(hdm::Tr("Тема: Тёмная", "Тема: Темна", "Theme: Dark")));
        const float shortThemeBtnW = std::max(
            CalcButtonWidth(hdm::Tr("Светлая", "Світла", "Light")),
            CalcButtonWidth(hdm::Tr("Тёмная", "Темна", "Dark")));
        const float hdrGap = std::round(8.0f * uiScale);
        const float prevItemEndX = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        const float winContentMaxX = ImGui::GetWindowContentRegionMax().x;
        const float fullRightGroupW = fullThemeBtnW + hdrGap + langComboW + hdrGap + btnWidth;
        const bool useShortThemeLbl = (winContentMaxX - prevItemEndX - 12.0f * uiScale) < fullRightGroupW;
        const float themeBtnW = useShortThemeLbl ? shortThemeBtnW : fullThemeBtnW;
        const char* themeBtnLbl =
            useShortThemeLbl
                ? (g_isLightTheme
                       ? hdm::Tr("Светлая###ThemeBtn", "Світла###ThemeBtn", "Light###ThemeBtn")
                       : hdm::Tr("Тёмная###ThemeBtn", "Темна###ThemeBtn", "Dark###ThemeBtn"))
                : (g_isLightTheme
                       ? hdm::Tr("Тема: Светлая###ThemeBtn", "Тема: Світла###ThemeBtn", "Theme: Light###ThemeBtn")
                       : hdm::Tr("Тема: Тёмная###ThemeBtn", "Тема: Темна###ThemeBtn", "Theme: Dark###ThemeBtn"));

        const float rightGroupW = themeBtnW + hdrGap + langComboW + hdrGap + btnWidth;
        const float rightX = winContentMaxX - rightGroupW;
        if (rightX > prevItemEndX + 12.0f * uiScale) {
            ImGui::SameLine(rightX);
        } else {
            ImGui::SameLine();
        }

        if (RenderSecondaryButton(themeBtnLbl, ImVec2(themeBtnW, 0.0f))) {
            ApplyAppTheme(hwnd_, g_dpiScale, !g_isLightTheme);
        }
        if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip(
                "%s",
                hdm::Tr(
                    "Переключить оформление: тёмная / светлая тема",
                    "Перемкнути оформлення: темна / світла тема",
                    "Switch appearance: Dark / Light theme"));
        }

        ImGui::SameLine(0.0f, hdrGap);
        ImGui::SetNextItemWidth(langComboW);
        if (g_isLightTheme) {
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.68f, 0.75f, 0.86f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.94f, 0.96f, 0.99f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.88f, 0.91f, 0.97f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.83f, 0.88f, 0.95f, 1.00f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.36f, 0.52f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.106f, 0.133f, 0.184f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.145f, 0.184f, 0.255f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.169f, 0.216f, 0.298f, 1.00f));
        }
        if (ImGui::BeginCombo("##LangCombo", curLangLabel, ImGuiComboFlags_HeightSmall)) {
            for (const auto& opt : kLangOptions) {
                const bool isSelected = (opt.lang == curLang);
                if (ImGui::Selectable(opt.label, isSelected)) {
                    hdm::SetLang(opt.lang);
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleColor(4);

        ImGui::SameLine(0.0f, hdrGap);
        ImGui::BeginDisabled(systemRestore_.IsBusy());
        if (RenderSecondaryButton(srLabel, ImVec2(btnWidth, 0.0f))) {
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

        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    void RenderRebootBanner() {
        const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
        const ImGuiStyle& style = ImGui::GetStyle();
        const float padY = std::round(7.0f * uiScale);
        const float bannerHeight = ImGui::GetFrameHeight() + padY * 2.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, padY));
        ImGui::PushStyleColor(
            ImGuiCol_ChildBg,
            g_isLightTheme ? ImVec4(1.00f, 0.96f, 0.84f, 0.98f) : ImVec4(0.24f, 0.16f, 0.05f, 0.95f));
        ImGui::PushStyleColor(
            ImGuiCol_Border,
            g_isLightTheme ? ImVec4(0.86f, 0.58f, 0.10f, 0.95f) : ImVec4(0.96f, 0.68f, 0.16f, 0.90f));
        ImGui::BeginChild(
            "##RebootBanner",
            ImVec2(0.0f, bannerHeight),
            true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor(2);
        RenderStatusBadge(
            ImVec4(0.98f, 0.75f, 0.14f, 1.00f),
            hdm::Tr(
                "Требуется перезагрузка Windows для завершения установки драйверов.",
                "Потрібне перезавантаження Windows для завершення встановлення драйверів.",
                "Windows restart is required to finish driver installation."),
            true);
        const char* hideLabel = hdm::Tr("Скрыть###RebHide", "Приховати###RebHide", "Dismiss###RebHide");
        const float hideW = CalcButtonWidth(hdm::Tr("Скрыть", "Приховати", "Dismiss"));
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0f,
                                 ImGui::GetWindowContentRegionMax().x - hideW));
        if (RenderSecondaryButton(hideLabel, ImVec2(hideW, 0.0f))) {
            rebootRequired_.store(false);
        }
        ImGui::EndChild();
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

    const hdm::DeviceInfo* FindSelectedDevice() const {
        if (selectedDeviceInstanceId_.empty()) {
            return nullptr;
        }
        for (const auto& d : devicesCache_) {
            if (d.instanceId == selectedDeviceInstanceId_) {
                return &d;
            }
        }
        return nullptr;
    }

    void HandleKeyboardShortcuts() {
        const ImGuiIO& io = ImGui::GetIO();

        if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
            switch (activeTabIndex_) {
            case 0:
                if (!scanner_.IsBusy() && !componentsScanner_.IsInstalling()) {
                    scanner_.StartScanAsync(MakeLogger());
                }
                break;
            case 1:
                if (!componentsScanner_.IsScanning() &&
                    !componentsScanner_.IsCheckingOnline() &&
                    !componentsScanner_.IsInstalling()) {
                    componentsScanner_.StartLocalScanAsync(devicesCache_, MakeLogger());
                }
                break;
            case 2:
                if (!wuManager_.IsBusy()) {
                    selectedWuIndex_ = -1;
                    wuManager_.StartSearchAsync(MakeLogger(), !wuIncludeSystemUpdates_);
                }
                break;
            case 3:
                if (localSectionIndex_ == 1) {
                    if (!localInstaller_.IsScanningStore() &&
                        !localInstaller_.IsExportingBackup() &&
                        !localInstaller_.IsRemovingStorePackages()) {
                        localInstaller_.ScanDriverStoreAsync(devicesCache_, MakeLogger());
                    }
                } else if (std::strlen(localFolderBuf_) > 0 && !localInstaller_.IsScanningInf()) {
                    localInstaller_.ScanInfFilesAsync(
                        hdm::Utf8ToWide(localFolderBuf_), MakeLogger());
                }
                break;
            case 4:
                if (!diagnostics_.IsBusy()) {
                    diagnostics_.StartScanAsync(devicesCache_, MakeLogger());
                }
                break;
            default:
                break;
            }
        }

        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            if (activeTabIndex_ != 0) {
                pendingInitialTab_ = 0;
            }
            focusDeviceSearchNextFrame_ = true;
        }

        if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            if (activeTabIndex_ == 0) {
                if (const hdm::DeviceInfo* sel = FindSelectedDevice()) {
                    if (!sel->hardwareId.empty()) {
                        ImGui::SetClipboardText(sel->hardwareId.c_str());
                        logBuffer_.Push(
                            hdm::LogLevel::Info,
                            std::string(hdm::Tr(
                                "Скопирован Hardware ID (Ctrl+C): ",
                                "Скопійовано Hardware ID (Ctrl+C): ",
                                "Copied Hardware ID (Ctrl+C): ")) +
                                sel->hardwareId);
                    }
                }
            } else if (activeTabIndex_ == 2) {
                if (selectedWuIndex_ >= 0 &&
                    selectedWuIndex_ < static_cast<int>(wuCache_.size())) {
                    const auto& wu = wuCache_[static_cast<size_t>(selectedWuIndex_)];
                    if (!wu.title.empty()) {
                        ImGui::SetClipboardText(wu.title.c_str());
                        logBuffer_.Push(
                            hdm::LogLevel::Info,
                            std::string(hdm::Tr(
                                "Скопировано название обновления (Ctrl+C): ",
                                "Скопійовано назву оновлення (Ctrl+C): ",
                                "Copied update title (Ctrl+C): ")) +
                                wu.title);
                    }
                }
            }
        }

        if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (activeTabIndex_ == 0 && !selectedDeviceInstanceId_.empty()) {
                selectedDeviceInstanceId_.clear();
            } else if (activeTabIndex_ == 2 && selectedWuIndex_ >= 0) {
                selectedWuIndex_ = -1;
            }
        }
    }

    void RenderDevicesTab() {
        const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
        const bool isAppForeground = (GetForegroundWindow() == hwnd_);

        // Row 1: Scan button + PnP rescan + Category filter pills (auto-wrap on narrow windows)
        const bool scanning = scanner_.IsScanning();
        const bool catalogBusy = componentsScanner_.IsInstalling();
        const bool devBusy = scanner_.IsBusy() || catalogBusy;
        ImGui::BeginDisabled(devBusy);
        const char* devScanLbl =
            scanning
                ? hdm::Tr("Сканирование...###DevScan", "Сканування...###DevScan", "Scanning...###DevScan")
                : (devBusy
                       ? hdm::Tr("Выполнение...###DevScan", "Виконання...###DevScan", "Working...###DevScan")
                       : hdm::Tr("Сканировать###DevScan", "Сканувати###DevScan", "Scan###DevScan"));
        if (ImGui::Button(devScanLbl)) {
            scanner_.StartScanAsync(MakeLogger());
        }
        ImGui::EndDisabled();

        const char* pnpTxt = hdm::Tr("Обновить PnP", "Оновити PnP", "Rescan PnP");
        SameLineWrapIfNeeded(CalcButtonWidth(pnpTxt));
        ImGui::BeginDisabled(devBusy);
        if (RenderSecondaryButton(hdm::Tr(
                "Обновить PnP###DevPnp",
                "Оновити PnP###DevPnp",
                "Rescan PnP###DevPnp"))) {
            scanner_.RunDeviceActionAsync(
                hdm::DeviceAction::Reenumerate,
                {},
                hdm::Tr("Шина PnP", "Шина PnP", "PnP Bus"),
                MakeLogger(),
                [this](bool, bool rebootReq) {
                    if (rebootReq) {
                        rebootRequired_.store(true);
                    }
                });
        }
        if (isAppForeground &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "%s",
                hdm::Tr(
                    "Повторный опрос шины PnP (CM_Reenumerate_DevNode) для поиска подключённых или удалённых устройств.",
                    "Повторне опитування шини PnP (CM_Reenumerate_DevNode) для пошуку підключених або видалених пристроїв.",
                    "Rescan the PnP bus (CM_Reenumerate_DevNode) to detect connected or removed hardware."));
        }
        ImGui::EndDisabled();

        const char* fAllTxt = hdm::Tr("Все", "Усі", "All");
        const char* fProbTxt = hdm::Tr("Проблемные", "Проблемні", "Issues");
        const char* fMonTxt = hdm::Tr("Мониторы", "Монітори", "Monitors");
        const char* fPerTxt = hdm::Tr("Периферия", "Периферія", "Peripherals");
        const char* fGpuTxt = hdm::Tr("Видео и чипсет", "Відео та чипсет", "GPU & Chipset");

        const char* filterLbl = hdm::Tr("Фильтр:", "Фільтр:", "Filter:");
        float filterLblW = 0.0f;
        {
            PushFontSafe(g_fontSecondaryHeader);
            filterLblW = ImGui::CalcTextSize(filterLbl).x;
            PopFontSafe(g_fontSecondaryHeader);
        }
        const float divCheckW = 14.0f * uiScale + filterLblW + 6.0f * uiScale + CalcButtonWidth(fAllTxt);
        const float lastX = ImGui::GetItemRectMax().x;
        const float winRightLim = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        if (lastX + ImGui::GetStyle().ItemSpacing.x + divCheckW <= winRightLim) {
            RenderVerticalDivider(3.0f);
            ImGui::SameLine(0.0f, 3.0f * uiScale);
        }
        RenderSecondaryGroupLabel(filterLbl);
        ImGui::SameLine(0.0f, 6.0f * uiScale);
        if (RenderPillButton(hdm::Tr("Все###DfAll", "Усі###DfAll", "All###DfAll"),
                             deviceFilterMode_ == DeviceFilterMode::All)) {
            deviceFilterMode_ = DeviceFilterMode::All;
            deviceViewDirty_ = true;
        }
        SameLineWrapIfNeeded(CalcButtonWidth(fProbTxt));
        if (RenderPillButton(hdm::Tr("Проблемные###DfProb", "Проблемні###DfProb", "Issues###DfProb"),
                             deviceFilterMode_ == DeviceFilterMode::ProblemOnly)) {
            deviceFilterMode_ = DeviceFilterMode::ProblemOnly;
            deviceViewDirty_ = true;
        }
        SameLineWrapIfNeeded(CalcButtonWidth(fMonTxt));
        if (RenderPillButton(hdm::Tr("Мониторы###DfMon", "Монітори###DfMon", "Monitors###DfMon"),
                             deviceFilterMode_ == DeviceFilterMode::Monitors)) {
            deviceFilterMode_ = DeviceFilterMode::Monitors;
            deviceViewDirty_ = true;
        }
        SameLineWrapIfNeeded(CalcButtonWidth(fPerTxt));
        if (RenderPillButton(hdm::Tr("Периферия###DfPer", "Периферія###DfPer", "Peripherals###DfPer"),
                             deviceFilterMode_ == DeviceFilterMode::Peripherals)) {
            deviceFilterMode_ = DeviceFilterMode::Peripherals;
            deviceViewDirty_ = true;
        }
        SameLineWrapIfNeeded(CalcButtonWidth(fGpuTxt));
        if (RenderPillButton(hdm::Tr("Видео и чипсет###DfGpu", "Відео та чипсет###DfGpu", "GPU & Chipset###DfGpu"),
                             deviceFilterMode_ == DeviceFilterMode::DisplayAndChipset)) {
            deviceFilterMode_ = DeviceFilterMode::DisplayAndChipset;
            deviceViewDirty_ = true;
        }

        {
            char totStr[32], monStr[32], probStr[32];
            std::snprintf(totStr, sizeof(totStr), "%zu", totalDevicesCount_);
            std::snprintf(monStr, sizeof(monStr), "%zu", monitorDevicesCount_);
            std::snprintf(probStr, sizeof(probStr), "%zu", problemDevicesCount_);

            const ImVec4 probAccent =
                (problemDevicesCount_ > 0)
                    ? ImVec4(0.98f, 0.72f, 0.16f, 1.00f)
                    : ImVec4(0.20f, 0.83f, 0.60f, 1.00f);
            const SummaryBadgeSegment segments[3] = {
                {hdm::Tr("Всего:", "Усього:", "Total:"), totStr, ImVec4(0.46f, 0.74f, 0.98f, 1.00f), false},
                {hdm::Tr("Мониторы:", "Монітори:", "Monitors:"), monStr, ImVec4(0.26f, 0.82f, 0.92f, 1.00f), false},
                {hdm::Tr("Проблемные:", "Проблемні:", "Issues:"), probStr, probAccent, problemDevicesCount_ > 0},
            };

            const float summaryW = CalcSummaryInfoBadgeWidth(segments, 3);
            const float lastItemEndLocalX = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
            const float winRightX = ImGui::GetWindowContentRegionMax().x;
            if (winRightX - summaryW > lastItemEndLocalX + 10.0f * uiScale) {
                ImGui::SameLine(winRightX - summaryW);
            } else {
                ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), winRightX - summaryW));
            }
            RenderSummaryInfoBadge(segments, 3);
        }

        // Row 2: Responsive full-width search bar
        ImGui::SetNextItemWidth(std::max(180.0f, ImGui::GetContentRegionAvail().x));
        if (focusDeviceSearchNextFrame_) {
            ImGui::SetKeyboardFocusHere();
            focusDeviceSearchNextFrame_ = false;
        }
        if (ImGui::InputTextWithHint(
                "##DeviceSearch",
                hdm::Tr(
                    "Поиск по названию, модели монитора (EDID), бренду, версии или Hardware ID... (Ctrl+F)",
                    "Пошук за назвою, моделлю монітора (EDID), брендом, версією або Hardware ID... (Ctrl+F)",
                    "Search by name, monitor model (EDID), brand, version, or Hardware ID... (Ctrl+F)"),
                deviceFilterBuf_,
                sizeof(deviceFilterBuf_))) {
            deviceViewDirty_ = true;
        }

        if (catalogBusy) {
            const float prog = componentsScanner_.Progress();
            const std::string phase = hdm::TrDyn(componentsScanner_.PhaseMessage());
            char overlay[256];
            std::snprintf(
                overlay,
                sizeof(overlay),
                "%s (%.0f%%)",
                phase.empty()
                    ? hdm::Tr(
                          "Установка драйвера из Microsoft Update Catalog...",
                          "Встановлення драйвера з Microsoft Update Catalog...",
                          "Installing driver from Microsoft Update Catalog...")
                    : phase.c_str(),
                prog * 100.0f);
            const char* cancelTxt = hdm::Tr("Отмена", "Скасувати", "Cancel");
            const float cancelW = CalcButtonWidth(cancelTxt) + 8.0f * uiScale;
            ImGui::ProgressBar(
                prog,
                ImVec2(std::max(120.0f, ImGui::GetContentRegionAvail().x - cancelW - ImGui::GetStyle().ItemSpacing.x),
                       ImGui::GetFrameHeight()),
                overlay);
            ImGui::SameLine();
            if (RenderSecondaryButton(hdm::Tr("Отмена###CatCancel", "Скасувати###CatCancel", "Cancel###CatCancel"),
                                      ImVec2(cancelW, 0.0f))) {
                componentsScanner_.RequestCancelInstall();
            }
        }

        ImGui::Spacing();

        if (pendingSelectDeviceRow_ >= 0) {
            if (deviceViewDirty_ && !devicesCache_.empty()) {
                RebuildDeviceViewCache(nullptr);
            }
            if (!visibleDeviceIndices_.empty()) {
                const size_t pick = std::min(
                    static_cast<size_t>(pendingSelectDeviceRow_),
                    visibleDeviceIndices_.size() - 1);
                selectedDeviceInstanceId_ = devicesCache_[visibleDeviceIndices_[pick]].instanceId;
                pendingSelectDeviceRow_ = -1;
            }
        }

        const hdm::DeviceInfo* selectedDevBefore = FindSelectedDevice();
        const ImGuiStyle& devStyle = ImGui::GetStyle();
        const float cardPadY = std::round(8.0f * uiScale);
        const float detailsH =
            (selectedDevBefore != nullptr)
                ? (ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetFrameHeight() +
                   cardPadY * 2.0f + 8.0f * uiScale)
                : 0.0f;
        const float tableH = std::max(
            110.0f,
            ImGui::GetContentRegionAvail().y -
                (detailsH > 0.0f ? detailsH + devStyle.ItemSpacing.y : 0.0f));

        constexpr ImGuiTableFlags kTableFlags =
            ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
            ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY;

        if (ImGui::BeginTable("##DevicesTable", 7, kTableFlags, ImVec2(0.0f, tableH))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(
                hdm::Tr("Статус", "Статус", "Status"),
                ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch,
                1.28f,
                0);
            ImGui::TableSetupColumn(
                hdm::Tr("Устройство / Модель", "Пристрій / Модель", "Device / Model"),
                ImGuiTableColumnFlags_WidthStretch,
                1.45f,
                1);
            ImGui::TableSetupColumn(
                hdm::Tr("Производитель", "Виробник", "Manufacturer"),
                ImGuiTableColumnFlags_WidthStretch,
                0.90f,
                2);
            ImGui::TableSetupColumn(
                hdm::Tr("Класс", "Клас", "Class"),
                ImGuiTableColumnFlags_WidthStretch,
                0.60f,
                3);
            ImGui::TableSetupColumn(
                hdm::Tr("Версия", "Версія", "Version"),
                ImGuiTableColumnFlags_WidthStretch,
                0.76f,
                4);
            ImGui::TableSetupColumn(
                hdm::Tr("Дата", "Дата", "Date"),
                ImGuiTableColumnFlags_WidthStretch,
                0.60f,
                5);
            ImGui::TableSetupColumn("Hardware ID", ImGuiTableColumnFlags_WidthStretch, 1.28f, 6);
            RenderTableHeadersRow();

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
                    const bool isRowSelected =
                        (!selectedDeviceInstanceId_.empty() &&
                         dev.instanceId == selectedDeviceInstanceId_);
                    if (ImGui::Selectable(
                            "##rowSel",
                            isRowSelected,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                            ImVec2(0.0f, rowBadgeH))) {
                        if (isRowSelected) {
                            selectedDeviceInstanceId_.clear();
                        } else if (!dev.instanceId.empty()) {
                            selectedDeviceInstanceId_ = dev.instanceId;
                        }
                    }
                    if (ImGui::BeginPopupContextItem("##DevCtxMenu")) {
                        if (!dev.instanceId.empty()) {
                            selectedDeviceInstanceId_ = dev.instanceId;
                        }
                        ImGui::TextDisabled("%s", dev.name.c_str());
                        ImGui::Separator();

                        const bool hasHwKey = !dev.searchHardwareKey.empty() || !dev.hardwareId.empty();
                        if (ImGui::MenuItem(
                                hdm::Tr(
                                    "Скачать и установить драйвер (Catalog WHQL)",
                                    "Завантажити та встановити драйвер (Catalog WHQL)",
                                    "Download & Install Driver (Catalog WHQL)"),
                                nullptr,
                                false,
                                !devBusy && hasHwKey)) {
                            componentsScanner_.SearchAndInstallFromCatalogAsync(
                                dev.name,
                                dev.searchHardwareKey,
                                dev.hardwareId,
                                dev.driverVersion,
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
                        ImGui::Separator();

                        const bool canControl = !devBusy && !dev.instanceId.empty();
                        auto triggerAction = [&](hdm::DeviceAction act) {
                            scanner_.RunDeviceActionAsync(
                                act,
                                dev.instanceId,
                                dev.name,
                                MakeLogger(),
                                [this](bool, bool rebootReq) {
                                    if (rebootReq) {
                                        rebootRequired_.store(true);
                                    }
                                });
                        };

                        if (ImGui::MenuItem(
                                hdm::Tr("Перезапустить устройство", "Перезапустити пристрій", "Restart device"),
                                nullptr,
                                false,
                                canControl)) {
                            triggerAction(hdm::DeviceAction::Restart);
                        }
                        if (dev.problemCode == CM_PROB_DISABLED) {
                            if (ImGui::MenuItem(
                                    hdm::Tr("Включить устройство", "Увімкнути пристрій", "Enable device"),
                                    nullptr,
                                    false,
                                    canControl)) {
                                triggerAction(hdm::DeviceAction::Enable);
                            }
                        } else {
                            if (ImGui::MenuItem(
                                    hdm::Tr("Отключить устройство", "Вимкнути пристрій", "Disable device"),
                                    nullptr,
                                    false,
                                    canControl)) {
                                triggerAction(hdm::DeviceAction::Disable);
                            }
                        }
                        if (ImGui::MenuItem(
                                hdm::Tr(
                                    "Откатить драйвер (предыдущая версия)",
                                    "Відкотити драйвер (попередня версія)",
                                    "Roll back driver (previous version)"),
                                nullptr,
                                false,
                                canControl && dev.hasDriver)) {
                            triggerAction(hdm::DeviceAction::RollbackDriver);
                        }
                        if (ImGui::MenuItem(
                                hdm::Tr(
                                    "Удалить устройство (переустановка PnP)",
                                    "Видалити пристрій (перевстановлення PnP)",
                                    "Uninstall device (PnP reinstall)"),
                                nullptr,
                                false,
                                canControl)) {
                            triggerAction(hdm::DeviceAction::UninstallDevice);
                        }
                        if (ImGui::MenuItem(
                                hdm::Tr(
                                    "Открыть Диспетчер устройств Windows",
                                    "Відкрити Диспетчер пристроїв Windows",
                                    "Open Windows Device Manager"))) {
                            ShellExecuteW(nullptr, L"open", L"devmgmt.msc", nullptr, nullptr, SW_SHOWNORMAL);
                        }
                        ImGui::Separator();

                        if (ImGui::MenuItem(
                                hdm::Tr("Копировать Hardware ID", "Копіювати Hardware ID", "Copy Hardware ID"),
                                nullptr,
                                false,
                                !dev.hardwareId.empty())) {
                            ImGui::SetClipboardText(dev.hardwareId.c_str());
                            logBuffer_.Push(
                                hdm::LogLevel::Info,
                                std::string(hdm::Tr("Скопирован Hardware ID: ", "Скопійовано Hardware ID: ", "Copied Hardware ID: ")) +
                                    dev.hardwareId);
                        }
                        if (ImGui::MenuItem(
                                hdm::Tr("Копировать короткий ID (VEN/DEV)", "Копіювати короткий ID (VEN/DEV)", "Copy Short ID (VEN/DEV)"),
                                nullptr,
                                false,
                                !dev.searchHardwareKey.empty())) {
                            ImGui::SetClipboardText(dev.searchHardwareKey.c_str());
                        }
                        if (ImGui::MenuItem(
                                hdm::Tr("Копировать название устройства", "Копіювати назву пристрою", "Copy Device Name"))) {
                            ImGui::SetClipboardText(dev.name.c_str());
                        }
                        ImGui::Separator();
                        const std::string queryId =
                            !dev.edidModel.empty()
                                ? (dev.manufacturer + " " + dev.edidModel + " monitor driver")
                                : (!dev.searchHardwareKey.empty()
                                       ? (dev.searchHardwareKey + " Windows driver")
                                       : (dev.name + " Windows driver"));
                        if (ImGui::MenuItem(
                                hdm::Tr("Найти драйвер в браузере (Google)", "Знайти драйвер у браузері (Google)", "Search driver in browser (Google)"))) {
                            OpenUrlInBrowser("https://www.google.com/search?q=" + UrlEncode(queryId));
                        }
                        if (ImGui::MenuItem(
                                hdm::Tr("Найти в каталоге Microsoft Update", "Знайти в каталозі Microsoft Update", "Search in Microsoft Update Catalog"),
                                nullptr,
                                false,
                                !dev.searchHardwareKey.empty())) {
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
                    const std::string devStatusShown = hdm::TrDyn(dev.statusText);
                    RenderStatusBadge(statusColor, devStatusShown.c_str(), false);

                    // Col 1: Device Name
                    ImGui::TableSetColumnIndex(1);
                    const std::string devNameShown = hdm::TrDyn(dev.name);
                    RenderCellText(devNameShown);
                    if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(460.0f * uiScale);
                        ImGui::TextColored(AccentTextColor(), "%s", devNameShown.c_str());
                        ImGui::TextDisabled(
                            hdm::Tr(
                                "Производитель: %s  |  Класс: %s",
                                "Виробник: %s  |  Клас: %s",
                                "Manufacturer: %s  |  Class: %s"),
                            dev.manufacturer.empty() ? "—" : dev.manufacturer.c_str(),
                            dev.deviceClass.empty() ? "—" : dev.deviceClass.c_str());
                        ImGui::TextDisabled(
                            hdm::Tr(
                                "INF: %s  |  Статус: %s",
                                "INF: %s  |  Статус: %s",
                                "INF: %s  |  Status: %s"),
                            dev.infName.empty() ? "—" : dev.infName.c_str(),
                            devStatusShown.c_str());
                        if (!dev.hardwareId.empty()) {
                            ImGui::TextWrapped("Hardware ID: %s", dev.hardwareId.c_str());
                        }
                        if (!dev.edidModel.empty()) {
                            ImGui::Text(
                                hdm::Tr("Модель (EDID): %s", "Модель (EDID): %s", "Model (EDID): %s"),
                                dev.edidModel.c_str());
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

        if (const hdm::DeviceInfo* selectedDev = FindSelectedDevice()) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(devStyle.WindowPadding.x, cardPadY));
            ImGui::PushStyleColor(ImGuiCol_Border, PanelBorderColor());
            ImGui::BeginChild(
                "##DeviceDetailsCard",
                ImVec2(0.0f, detailsH),
                true,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleColor();

            {
                PushFontSafe(g_fontSecondaryHeader);
                const float lineH = ImGui::GetTextLineHeight();
                const float barW = std::max(2.5f, std::round(3.0f * uiScale));
                const ImVec2 barMin = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(barMin.x, barMin.y + 1.0f),
                    ImVec2(barMin.x + barW, barMin.y + lineH - 1.0f),
                    ImGui::ColorConvertFloat4ToU32(AccentBarColor()),
                    barW * 0.5f);
                ImGui::Dummy(ImVec2(barW + std::round(5.0f * uiScale), lineH));
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(
                    CardHeaderTextColor(),
                    "%s",
                    EllipsizeUtf8(selectedDev->name, 420.0f * uiScale).c_str());
                PopFontSafe(g_fontSecondaryHeader);
            }
            ImGui::SameLine(0.0f, 12.0f);
            const std::string metaLine =
                std::string(hdm::Tr("Статус: ", "Статус: ", "Status: ")) + hdm::TrDyn(selectedDev->statusText) +
                hdm::Tr("  |  Класс: ", "  |  Клас: ", "  |  Class: ") +
                (selectedDev->deviceClass.empty() ? "—" : selectedDev->deviceClass) +
                "  |  INF: " + (selectedDev->infName.empty() ? "—" : selectedDev->infName);
            RenderCellText(metaLine, true);

            const std::string idLine =
                "Hardware ID: " + (selectedDev->hardwareId.empty() ? "—" : selectedDev->hardwareId) +
                "   |   Instance ID: " +
                (selectedDev->instanceId.empty() ? "—" : selectedDev->instanceId);
            if (RenderCellText(idLine, true) && isAppForeground &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", idLine.c_str());
            }

            const bool hasHwKey =
                !selectedDev->searchHardwareKey.empty() || !selectedDev->hardwareId.empty();
            const bool canControl = !devBusy && !selectedDev->instanceId.empty();
            const bool highlightCatalog =
                (selectedDev->health != hdm::DeviceHealth::Ok || selectedDev->isGenericMonitorDriver);

            ImGui::BeginDisabled(devBusy || !hasHwKey);
            if (RenderActionButton(
                    hdm::Tr(
                        "Установить из Catalog (WHQL)###DevCatBtn",
                        "Встановити з Catalog (WHQL)###DevCatBtn",
                        "Install from Catalog (WHQL)###DevCatBtn"),
                    ImVec2(0.0f, 0.0f),
                    highlightCatalog)) {
                componentsScanner_.SearchAndInstallFromCatalogAsync(
                    selectedDev->name,
                    selectedDev->searchHardwareKey,
                    selectedDev->hardwareId,
                    selectedDev->driverVersion,
                    devicesCache_,
                    MakeLogger(),
                    [this](bool ok, bool rebootReq) {
                        if (rebootReq) rebootRequired_.store(true);
                        if (ok) pendingDeviceRescan_.store(true);
                    });
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(!canControl);
            if (RenderSecondaryButton(
                    hdm::Tr("Перезапустить###DevRst", "Перезапустити###DevRst", "Restart###DevRst"))) {
                scanner_.RunDeviceActionAsync(
                    hdm::DeviceAction::Restart,
                    selectedDev->instanceId,
                    selectedDev->name,
                    MakeLogger(),
                    [this](bool, bool rebootReq) {
                        if (rebootReq) rebootRequired_.store(true);
                    });
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!selectedDev->hasDriver);
            if (RenderSecondaryButton(
                    hdm::Tr("Откатить драйвер###DevRb", "Відкотити драйвер###DevRb", "Roll Back###DevRb"))) {
                scanner_.RunDeviceActionAsync(
                    hdm::DeviceAction::RollbackDriver,
                    selectedDev->instanceId,
                    selectedDev->name,
                    MakeLogger(),
                    [this](bool, bool rebootReq) {
                        if (rebootReq) rebootRequired_.store(true);
                    });
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(selectedDev->hardwareId.empty());
            if (RenderSecondaryButton(
                    hdm::Tr("Копировать HWID (Ctrl+C)###DevCpHw", "Копіювати HWID (Ctrl+C)###DevCpHw", "Copy HWID (Ctrl+C)###DevCpHw"))) {
                ImGui::SetClipboardText(selectedDev->hardwareId.c_str());
                logBuffer_.Push(
                    hdm::LogLevel::Info,
                    std::string(hdm::Tr("Скопирован Hardware ID: ", "Скопійовано Hardware ID: ", "Copied Hardware ID: ")) +
                        selectedDev->hardwareId);
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::BeginDisabled(selectedDev->instanceId.empty());
            if (RenderSecondaryButton(
                    hdm::Tr("Копировать Instance ID###DevCpInst", "Копіювати Instance ID###DevCpInst", "Copy Instance ID###DevCpInst"))) {
                ImGui::SetClipboardText(selectedDev->instanceId.c_str());
                logBuffer_.Push(
                    hdm::LogLevel::Info,
                    std::string(hdm::Tr("Скопирован Instance ID: ", "Скопійовано Instance ID: ", "Copied Instance ID: ")) +
                        selectedDev->instanceId);
            }
            ImGui::EndDisabled();

            const char* closeTxt = hdm::Tr("Закрыть (Esc)", "Закрити (Esc)", "Close (Esc)");
            const float closeW = CalcButtonWidth(closeTxt);
            const float winRight = ImGui::GetWindowContentRegionMax().x;
            const float lastEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
            if (winRight - closeW > lastEnd + devStyle.ItemSpacing.x) {
                ImGui::SameLine(winRight - closeW);
            } else {
                ImGui::SameLine();
            }
            if (RenderSecondaryButton(
                    hdm::Tr("Закрыть (Esc)###DevClose", "Закрити (Esc)###DevClose", "Close (Esc)###DevClose"),
                    ImVec2(closeW, 0.0f))) {
                selectedDeviceInstanceId_.clear();
            }

            ImGui::EndChild();
            ImGui::PopStyleVar();
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
        std::vector<hdm::SystemComponentItem> selectedTargets;
        for (const auto& comp : componentsCache_) {
            if ((!comp.installed || comp.updateAvailable) && !comp.directInstallerUrl.empty()) {
                batchTargets.push_back(comp);
            }
            if (!comp.directInstallerUrl.empty() && selectedComponentIds_.count(comp.id) > 0) {
                selectedTargets.push_back(comp);
            }
        }

        ImGui::BeginDisabled(busy);
        if (RenderSecondaryButton(hdm::Tr(
                "Обновить проверку###CompScan",
                "Оновити перевірку###CompScan",
                "Rescan###CompScan"))) {
            componentsScanner_.StartLocalScanAsync(devicesCache_, MakeLogger());
        }
        const char* chkOnlineTxt = hdm::Tr("Проверить онлайн", "Перевірити онлайн", "Check Online");
        SameLineWrapIfNeeded(CalcButtonWidth(chkOnlineTxt));
        if (RenderSecondaryButton(hdm::Tr(
                "Проверить онлайн###CompOnline",
                "Перевірити онлайн###CompOnline",
                "Check Online###CompOnline"))) {
            componentsScanner_.StartOnlineVersionCheckAsync(MakeLogger());
        }
        ImGui::EndDisabled();

        char batchLbl[128];
        if (!batchTargets.empty()) {
            std::snprintf(
                batchLbl,
                sizeof(batchLbl),
                hdm::Tr(
                    "Обновить все доступные (%zu)###BatchCompBtn",
                    "Оновити всі доступні (%zu)###BatchCompBtn",
                    "Update All Available (%zu)###BatchCompBtn"),
                batchTargets.size());
        } else {
            std::snprintf(
                batchLbl,
                sizeof(batchLbl),
                "%s",
                hdm::Tr(
                    "Обновить все доступные (0)###BatchCompBtn",
                    "Оновити всі доступні (0)###BatchCompBtn",
                    "Update All Available (0)###BatchCompBtn"));
        }
        SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr(
            "Обновить все доступные (99)",
            "Оновити всі доступні (99)",
            "Update All Available (99)")));
        ImGui::BeginDisabled(busy || batchTargets.empty());
        if (RenderActionButton(batchLbl, ImVec2(0.0f, 0.0f), selectedTargets.empty())) {
            componentsScanner_.DownloadAndInstallBatchAsync(
                std::move(batchTargets), devicesCache_, MakeLogger());
        }
        ImGui::EndDisabled();

        char selBatchLbl[128];
        if (!selectedTargets.empty()) {
            std::snprintf(
                selBatchLbl,
                sizeof(selBatchLbl),
                hdm::Tr(
                    "Обновить выбранные (%zu)###BatchSelCompBtn",
                    "Оновити вибрані (%zu)###BatchSelCompBtn",
                    "Update Selected (%zu)###BatchSelCompBtn"),
                selectedTargets.size());
        } else {
            std::snprintf(
                selBatchLbl,
                sizeof(selBatchLbl),
                "%s",
                hdm::Tr(
                    "Обновить выбранные (0)###BatchSelCompBtn",
                    "Оновити вибрані (0)###BatchSelCompBtn",
                    "Update Selected (0)###BatchSelCompBtn"));
        }
        SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr(
            "Обновить выбранные (99)",
            "Оновити вибрані (99)",
            "Update Selected (99)")));
        ImGui::BeginDisabled(busy || selectedTargets.empty());
        if (RenderActionButton(selBatchLbl, ImVec2(0.0f, 0.0f), !selectedTargets.empty())) {
            componentsScanner_.DownloadAndInstallBatchAsync(
                std::move(selectedTargets), devicesCache_, MakeLogger());
        }
        ImGui::EndDisabled();

        const char* selAvailTxt =
            hdm::Tr("Выбрать доступные", "Вибрати доступні", "Select Available");
        SameLineWrapIfNeeded(CalcButtonWidth(selAvailTxt));
        ImGui::BeginDisabled(busy || batchTargets.empty());
        if (RenderSecondaryButton(hdm::Tr(
                "Выбрать доступные###CompSelAvail",
                "Вибрати доступні###CompSelAvail",
                "Select Available###CompSelAvail"))) {
            for (const auto& comp : batchTargets) {
                selectedComponentIds_.insert(comp.id);
            }
        }
        ImGui::EndDisabled();

        const char* clrSelTxt = hdm::Tr("Снять все", "Зняти всі", "Clear Selection");
        SameLineWrapIfNeeded(CalcButtonWidth(clrSelTxt));
        ImGui::BeginDisabled(busy || selectedComponentIds_.empty());
        if (RenderSecondaryButton(hdm::Tr(
                "Снять все###CompClrSel",
                "Зняти всі###CompClrSel",
                "Clear Selection###CompClrSel"))) {
            selectedComponentIds_.clear();
        }
        ImGui::EndDisabled();

        if (componentsScanner_.IsInstalling()) {
            SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Отмена", "Скасувати", "Cancel")));
            if (RenderSecondaryButton(hdm::Tr("Отмена###CompCancel", "Скасувати###CompCancel", "Cancel###CompCancel"))) {
                componentsScanner_.RequestCancelInstall();
            }
        }

        const std::string phase = hdm::TrDyn(componentsScanner_.PhaseMessage());
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
        RenderSecondaryGroupLabel(hdm::Tr("Фильтр:", "Фільтр:", "Filter:"));
        ImGui::SameLine(0.0f, 8.0f * uiScale);
        if (RenderPillButton(hdm::Tr("Все###CfAll", "Усі###CfAll", "All###CfAll"),
                             compFilterMode_ == ComponentFilterMode::All)) {
            compFilterMode_ = ComponentFilterMode::All;
            compViewDirty_ = true;
        }
        const char* cfGameTxt = hdm::Tr("Игры и библиотеки", "Ігри та бібліотеки", "Games & Runtimes");
        SameLineWrapIfNeeded(CalcButtonWidth(cfGameTxt));
        if (RenderPillButton(hdm::Tr("Игры и библиотеки###CfGame", "Ігри та бібліотеки###CfGame", "Games & Runtimes###CfGame"),
                             compFilterMode_ == ComponentFilterMode::GamingRuntimes)) {
            compFilterMode_ = ComponentFilterMode::GamingRuntimes;
            compViewDirty_ = true;
        }
        const char* cfPerTxt = hdm::Tr("Клавиатуры / Мыши / Аудио", "Клавіатури / Миші / Аудіо", "Keyboards / Mice / Audio");
        SameLineWrapIfNeeded(CalcButtonWidth(cfPerTxt));
        if (RenderPillButton(hdm::Tr("Клавиатуры / Мыши / Аудио###CfPer", "Клавіатури / Миші / Аудіо###CfPer", "Keyboards / Mice / Audio###CfPer"),
                             compFilterMode_ == ComponentFilterMode::Peripherals)) {
            compFilterMode_ = ComponentFilterMode::Peripherals;
            compViewDirty_ = true;
        }
        const char* cfMonTxt = hdm::Tr("Мониторы, GPU и плата", "Монітори, GPU та плата", "Monitors, GPU & Board");
        SameLineWrapIfNeeded(CalcButtonWidth(cfMonTxt));
        if (RenderPillButton(hdm::Tr("Мониторы, GPU и плата###CfMon", "Монітори, GPU та плата###CfMon", "Monitors, GPU & Board###CfMon"),
                             compFilterMode_ == ComponentFilterMode::MonitorsAndGpu)) {
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

        const float primaryW = CalcButtonWidth(hdm::Tr("Установить", "Встановити", "Download")) + 8.0f * uiScale;
        const float siteW = CalcButtonWidth(hdm::Tr("Сайт", "Сайт", "Site")) + 6.0f * uiScale;
        const float fullActionW = primaryW + compStyle.ItemSpacing.x + siteW;
        const float checkW = ImGui::GetFrameHeight();
        const float actionColW =
            fullActionW + compStyle.ItemSpacing.x + checkW + compStyle.CellPadding.x * 2.0f + 4.0f;

        if (ImGui::BeginTable("##ComponentsTable", 6, kTableFlags, ImGui::GetContentRegionAvail())) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(
                hdm::Tr("Статус", "Статус", "Status"),
                ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch,
                1.46f,
                0);
            ImGui::TableSetupColumn(
                hdm::Tr("Компонент / Официальное ПО", "Компонент / Офіційне ПЗ", "Component / Official Software"),
                ImGuiTableColumnFlags_WidthStretch,
                1.36f,
                1);
            ImGui::TableSetupColumn(
                hdm::Tr("Устройство / Назначение", "Пристрій / Призначення", "Hardware / Purpose"),
                ImGuiTableColumnFlags_WidthStretch,
                1.10f,
                2);
            ImGui::TableSetupColumn(
                hdm::Tr("Установлено", "Встановлено", "Installed"),
                ImGuiTableColumnFlags_WidthStretch,
                0.70f,
                3);
            ImGui::TableSetupColumn(
                hdm::Tr("Актуальная", "Актуальна", "Latest"),
                ImGuiTableColumnFlags_WidthStretch,
                0.76f,
                4);
            ImGui::TableSetupColumn(
                hdm::Tr("Действие", "Дія", "Action"),
                ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed,
                actionColW,
                5);
            RenderTableHeadersRow();

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
                const bool canSelectItem = !item.directInstallerUrl.empty();
                bool isSelected = canSelectItem && selectedComponentIds_.count(item.id) > 0;

                ImGui::TableNextRow();
                if (isSelected) {
                    const ImU32 selRowBg = ImGui::ColorConvertFloat4ToU32(
                        g_isLightTheme
                            ? ImVec4(0.16f, 0.48f, 0.90f, 0.12f)
                            : ImVec4(0.22f, 0.50f, 0.90f, 0.18f));
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, selRowBg);
                }
                ImGui::PushID(static_cast<int>(origIdx));

                ImGui::TableSetColumnIndex(0);
                ImVec4 statusCol(0.20f, 0.83f, 0.60f, 1.00f);
                if (!item.installed || item.updateAvailable) {
                    statusCol = ImVec4(0.98f, 0.75f, 0.14f, 1.00f);
                }
                const std::string compStatusShown = hdm::TrDyn(item.statusText);
                RenderStatusBadge(statusCol, compStatusShown.c_str(), true);
                if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("%s", compStatusShown.c_str());
                }

                ImGui::TableSetColumnIndex(1);
                RenderCellText(hdm::TrDyn(item.name), false, true);
                if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(440.0f * uiScale);
                    ImGui::TextColored(AccentTextColor(), "%s", hdm::TrDyn(item.name).c_str());
                    ImGui::TextDisabled(
                        hdm::Tr("Издатель: %s", "Видавець: %s", "Publisher: %s"),
                        item.vendor.c_str());
                    if (!item.purposeDescription.empty()) {
                        ImGui::Separator();
                        ImGui::TextUnformatted(hdm::Tr("За что отвечает:", "За що відповідає:", "Purpose:"));
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

                ImGui::TableSetColumnIndex(2);
                const std::string targetHw = hdm::TrDyn(
                    item.detectedHardware.empty() ? "Игры и системные библиотеки Windows"
                                                  : item.detectedHardware);
                if (RenderCellText(targetHw, false, true) && isAppForeground &&
                    ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("%s", targetHw.c_str());
                }

                ImGui::TableSetColumnIndex(3);
                const std::string instShown = hdm::TrDyn(item.installedVersion);
                if (RenderCellText(instShown, item.installedVersion.empty(), true) &&
                    isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("%s", instShown.c_str());
                }

                ImGui::TableSetColumnIndex(4);
                const std::string latestShown = hdm::TrDyn(item.latestVersion);
                if (RenderCellText(latestShown, item.latestVersion.empty(), true) &&
                    isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip("%s", latestShown.c_str());
                }

                ImGui::TableSetColumnIndex(5);
                if (!item.directInstallerUrl.empty()) {
                    const bool needAction = !item.installed || item.updateAvailable;
                    const char* btnLbl =
                        !item.installed
                            ? hdm::Tr("Установить###CAct", "Встановити###CAct", "Install###CAct")
                            : (item.updateAvailable
                                   ? hdm::Tr("Обновить###CAct", "Оновити###CAct", "Update###CAct")
                                   : hdm::Tr("Скачать###CAct", "Завантажити###CAct", "Download###CAct"));
                    ImGui::BeginDisabled(busy);
                    if (RenderActionButton(btnLbl, ImVec2(primaryW, 0.0f), needAction)) {
                        componentsScanner_.DownloadAndInstallAsync(
                            item, devicesCache_, MakeLogger());
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (RenderSecondaryButton(
                            hdm::Tr("Сайт###CSite", "Сайт###CSite", "Site###CSite"),
                            ImVec2(siteW, 0.0f))) {
                        OpenUrlInBrowser(item.officialPageUrl);
                    }
                    ImGui::SameLine();
                    ImGui::BeginDisabled(busy);
                    if (ImGui::Checkbox("##compSel", &isSelected)) {
                        if (isSelected) {
                            selectedComponentIds_.insert(item.id);
                        } else {
                            selectedComponentIds_.erase(item.id);
                        }
                    }
                    ImGui::EndDisabled();
                    if (isAppForeground &&
                        ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::SetTooltip(
                            "%s",
                            hdm::Tr(
                                "Выбрать компонент для пакетной установки / обновления",
                                "Вибрати компонент для пакетного встановлення / оновлення",
                                "Select component for batch install / update"));
                    }
                } else if (!item.officialPageUrl.empty()) {
                    if (RenderSecondaryButton(
                            hdm::Tr("Открыть сайт###COpenSite", "Відкрити сайт###COpenSite", "Open Website###COpenSite"),
                            ImVec2(fullActionW, 0.0f))) {
                        OpenUrlInBrowser(item.officialPageUrl);
                    }
                    ImGui::SameLine();
                    ImGui::BeginDisabled(true);
                    bool noDirectSel = false;
                    ImGui::Checkbox("##compSel", &noDirectSel);
                    ImGui::EndDisabled();
                } else {
                    ImGui::Dummy(ImVec2(fullActionW, 0.0f));
                    ImGui::SameLine();
                    ImGui::BeginDisabled(true);
                    bool noDirectSel = false;
                    ImGui::Checkbox("##compSel", &noDirectSel);
                    ImGui::EndDisabled();
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
        if (ImGui::Button(hdm::Tr(
                "Найти обновления###WuFindBtn",
                "Знайти оновлення###WuFindBtn",
                "Check for Updates###WuFindBtn"))) {
            selectedWuIndex_ = -1;
            wuManager_.StartSearchAsync(MakeLogger(), !wuIncludeSystemUpdates_);
        }
        SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Выбрать все", "Вибрати всі", "Select All")));
        if (RenderSecondaryButton(hdm::Tr(
                "Выбрать все###WuSelAll",
                "Вибрати всі###WuSelAll",
                "Select All###WuSelAll"))) {
            wuManager_.SelectAll(true, wuShowHidden_);
            for (auto& u : wuCache_) {
                if (!u.installedOk && (wuShowHidden_ || !u.isHidden)) {
                    u.selected = true;
                }
            }
        }
        SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Снять все", "Зняти всі", "Clear Selection")));
        if (RenderSecondaryButton(hdm::Tr(
                "Снять все###WuClearAll",
                "Зняти всі###WuClearAll",
                "Clear Selection###WuClearAll"))) {
            wuManager_.SelectAll(false, true);
            for (auto& u : wuCache_) u.selected = false;
        }
        ImGui::EndDisabled();

        size_t hiddenCount = 0;
        std::vector<std::string> selectedIds;
        std::vector<std::string> selectedToHide;
        std::vector<std::string> selectedToUnhide;
        for (const auto& u : wuCache_) {
            if (u.isHidden) {
                ++hiddenCount;
            }
            if (!u.installedOk && u.selected) {
                if (wuShowHidden_ || !u.isHidden) {
                    selectedIds.push_back(u.updateId);
                }
                if (u.isHidden) {
                    selectedToUnhide.push_back(u.updateId);
                } else {
                    selectedToHide.push_back(u.updateId);
                }
            }
        }

        char installLbl[128];
        if (!selectedIds.empty()) {
            std::snprintf(
                installLbl,
                sizeof(installLbl),
                hdm::Tr(
                    "Установить выбранные (%zu)###WuInstallBtn",
                    "Встановити вибрані (%zu)###WuInstallBtn",
                    "Install Selected (%zu)###WuInstallBtn"),
                selectedIds.size());
        } else {
            std::snprintf(
                installLbl,
                sizeof(installLbl),
                "%s",
                hdm::Tr(
                    "Установить выбранные###WuInstallBtn",
                    "Встановити вибрані###WuInstallBtn",
                    "Install Selected###WuInstallBtn"));
        }
        const float installBtnW = CalcButtonWidth(
            selectedIds.empty()
                ? hdm::Tr("Установить выбранные", "Встановити вибрані", "Install Selected")
                : hdm::Tr("Установить выбранные (99)", "Встановити вибрані (99)", "Install Selected (99)"));
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

        char hideLbl[96];
        if (!selectedToHide.empty()) {
            std::snprintf(
                hideLbl,
                sizeof(hideLbl),
                hdm::Tr("Скрыть (%zu)###WuHideBtn", "Приховати (%zu)###WuHideBtn", "Hide (%zu)###WuHideBtn"),
                selectedToHide.size());
        } else {
            std::snprintf(
                hideLbl,
                sizeof(hideLbl),
                "%s",
                hdm::Tr("Скрыть###WuHideBtn", "Приховати###WuHideBtn", "Hide###WuHideBtn"));
        }
        const float hideBtnW = CalcButtonWidth(
            selectedToHide.empty()
                ? hdm::Tr("Скрыть", "Приховати", "Hide")
                : hdm::Tr("Скрыть (99)", "Приховати (99)", "Hide (99)"));
        SameLineWrapIfNeeded(hideBtnW);
        ImGui::BeginDisabled(busy || selectedToHide.empty());
        if (RenderSecondaryButton(hideLbl)) {
            wuManager_.SetUpdatesHiddenAsync(
                selectedToHide, true, MakeLogger(), !wuIncludeSystemUpdates_);
        }
        ImGui::EndDisabled();
        if (isAppForeground &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(
                "%s",
                hdm::Tr(
                    "Запретить Windows Update предлагать и автоматически устанавливать выбранные обновления (IUpdate::put_IsHidden).",
                    "Заборонити Windows Update пропонувати й автоматично встановлювати вибрані оновлення (IUpdate::put_IsHidden).",
                    "Prevent Windows Update from offering and auto-installing selected updates (IUpdate::put_IsHidden)."));
        }

        if (wuShowHidden_ && !selectedToUnhide.empty()) {
            char unhideLbl[96];
            std::snprintf(
                unhideLbl,
                sizeof(unhideLbl),
                hdm::Tr(
                    "Вернуть (%zu)###WuUnhideBtn",
                    "Повернути (%zu)###WuUnhideBtn",
                    "Unhide (%zu)###WuUnhideBtn"),
                selectedToUnhide.size());
            SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Вернуть (99)", "Повернути (99)", "Unhide (99)")));
            ImGui::BeginDisabled(busy);
            if (RenderSecondaryButton(unhideLbl)) {
                wuManager_.SetUpdatesHiddenAsync(
                    selectedToUnhide, false, MakeLogger(), !wuIncludeSystemUpdates_);
            }
            ImGui::EndDisabled();
        }

        if (busy) {
            SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Отмена", "Скасувати", "Cancel")));
            if (RenderSecondaryButton(hdm::Tr("Отмена###WuCancel", "Скасувати###WuCancel", "Cancel###WuCancel"))) {
                wuManager_.RequestCancel();
            }
        }

        const std::string phase = hdm::TrDyn(wuManager_.PhaseMessage());
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

        ImGui::Checkbox(
            hdm::Tr(
                "Включать системные обновления и библиотеки (не только драйверы)###WuSysCb",
                "Включати системні оновлення та бібліотеки (не лише драйвери)###WuSysCb",
                "Include system updates & runtimes (not only drivers)###WuSysCb"),
            &wuIncludeSystemUpdates_);
        SameLineWrapIfNeeded(
            CalcButtonWidth(hdm::Tr(
                "Точка восстановления перед установкой",
                "Точка відновлення перед встановленням",
                "Create restore point before installing")) +
            32.0f * uiScale);
        ImGui::Checkbox(
            hdm::Tr(
                "Точка восстановления перед установкой###WuRpCb",
                "Точка відновлення перед встановленням###WuRpCb",
                "Create restore point before installing###WuRpCb"),
            &wuRestoreBefore_);
        {
            char showHidLbl[96];
            std::snprintf(
                showHidLbl,
                sizeof(showHidLbl),
                hdm::Tr(
                    "Показывать скрытые (%zu)###WuShowHiddenCb",
                    "Показувати приховані (%zu)###WuShowHiddenCb",
                    "Show hidden (%zu)###WuShowHiddenCb"),
                hiddenCount);
            SameLineWrapIfNeeded(
                CalcButtonWidth(hdm::Tr("Показывать скрытые (99)", "Показувати приховані (99)", "Show hidden (99)")) +
                32.0f * uiScale);
            ImGui::Checkbox(showHidLbl, &wuShowHidden_);
            if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip(
                    "%s",
                    hdm::Tr(
                        "Показать скрытые через IUpdate::IsHidden обновления, чтобы вернуть их в список.",
                        "Показати приховані через IUpdate::IsHidden оновлення, щоб повернути їх до списку.",
                        "Show updates hidden via IUpdate::IsHidden so you can restore them."));
            }
        }

        if (wuManager_.IsInstalling() || wuManager_.IsSearching()) {
            if (wuManager_.IsSearching()) {
                const float anim = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
                ImGui::ProgressBar(
                    anim,
                    ImVec2(-1.0f, 18.0f * uiScale),
                    hdm::Tr(
                        "Поиск в Windows Update...",
                        "Пошук у Windows Update...",
                        "Searching Windows Update..."));
            } else {
                const float prog = wuManager_.Progress();
                char overlay[64];
                std::snprintf(overlay, sizeof(overlay), "%.0f%%", prog * 100.0f);
                ImGui::ProgressBar(prog, ImVec2(-1.0f, 18.0f * uiScale), overlay);
            }
        }

        ImGui::Spacing();

        const bool hasVisibleDetails =
            (selectedWuIndex_ >= 0 && selectedWuIndex_ < static_cast<int>(wuCache_.size()) &&
             (wuShowHidden_ || !wuCache_[static_cast<size_t>(selectedWuIndex_)].isHidden));
        const float availY = ImGui::GetContentRegionAvail().y;
        const float wuSpacingY = ImGui::GetStyle().ItemSpacing.y;
        const float detailsH = hasVisibleDetails ? (90.0f * uiScale) : 0.0f;
        const float tableH = std::max(120.0f, availY - (detailsH > 0.0f ? detailsH + wuSpacingY : 0.0f));

        constexpr ImGuiTableFlags kTableFlags =
            ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY;

        if (ImGui::BeginTable("##WuTable", 5, kTableFlags, ImVec2(0.0f, tableH))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(
                hdm::Tr("Выбор", "Вибір", "Select"),
                ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed,
                58.0f * uiScale,
                0);
            ImGui::TableSetupColumn(
                hdm::Tr("Обновление", "Оновлення", "Update"),
                ImGuiTableColumnFlags_WidthStretch,
                1.80f,
                1);
            ImGui::TableSetupColumn(
                hdm::Tr("Производитель / Модель", "Виробник / Модель", "Vendor / Model"),
                ImGuiTableColumnFlags_WidthStretch,
                1.05f,
                2);
            ImGui::TableSetupColumn(
                hdm::Tr("Размер", "Розмір", "Size"),
                ImGuiTableColumnFlags_WidthStretch,
                0.50f,
                3);
            ImGui::TableSetupColumn(
                hdm::Tr("Статус", "Статус", "Status"),
                ImGuiTableColumnFlags_WidthStretch,
                0.98f,
                4);
            RenderTableHeadersRow();

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
                if (item.isHidden && !wuShowHidden_) {
                    continue;
                }
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
                if (ImGui::BeginPopupContextItem("##WuRowCtx")) {
                    selectedWuIndex_ = i;
                    ImGui::TextDisabled("%s", EllipsizeUtf8(item.title, 340.0f * uiScale).c_str());
                    ImGui::Separator();
                    if (!item.isHidden) {
                        if (ImGui::MenuItem(
                                hdm::Tr(
                                    "Скрыть обновление (запретить автоустановку)",
                                    "Приховати оновлення (заборонити автовстановлення)",
                                    "Hide update (block automatic install)"),
                                nullptr,
                                false,
                                !busy && !item.installedOk)) {
                            wuManager_.SetUpdatesHiddenAsync(
                                {item.updateId}, true, MakeLogger(), !wuIncludeSystemUpdates_);
                        }
                    } else {
                        if (ImGui::MenuItem(
                                hdm::Tr(
                                    "Вернуть обновление (разрешить установку)",
                                    "Повернути оновлення (дозволити встановлення)",
                                    "Unhide update (allow installation)"),
                                nullptr,
                                false,
                                !busy)) {
                            wuManager_.SetUpdatesHiddenAsync(
                                {item.updateId}, false, MakeLogger(), !wuIncludeSystemUpdates_);
                        }
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem(
                            hdm::Tr(
                                "Копировать название обновления",
                                "Копіювати назву оновлення",
                                "Copy update title"),
                            nullptr,
                            false,
                            !item.title.empty())) {
                        ImGui::SetClipboardText(item.title.c_str());
                    }
                    if (ImGui::MenuItem(
                            hdm::Tr("Копировать Update ID", "Копіювати Update ID", "Copy Update ID"),
                            nullptr,
                            false,
                            !item.updateId.empty())) {
                        ImGui::SetClipboardText(item.updateId.c_str());
                    }
                    ImGui::EndPopup();
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
                RenderCellText(hdm::TrDyn(item.sizeFormatted), true, true);

                ImGui::TableSetColumnIndex(4);
                ImVec4 col(0.60f, 0.72f, 0.90f, 1.00f);
                if (item.installedOk) {
                    col = ImVec4(0.20f, 0.83f, 0.60f, 1.00f);
                } else if (item.isHidden) {
                    col = ImVec4(0.58f, 0.68f, 0.86f, 1.00f);
                }
                const std::string wuStatusShown = hdm::TrDyn(item.statusText);
                RenderStatusBadge(col, wuStatusShown.c_str(), true);

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
                hdm::Tr(
                    "Hardware ID: %s   |   Дата: %s   |   Update ID: %s",
                    "Hardware ID: %s   |   Дата: %s   |   Update ID: %s",
                    "Hardware ID: %s   |   Date: %s   |   Update ID: %s"),
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
        if (ImGui::Button(hdm::Tr(
                "Экспорт бэкапа в папку...###DsExp",
                "Експорт бекапу в папку...###DsExp",
                "Export Backup Folder...###DsExp"))) {
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
            } else {
                ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), winRight - sumW));
            }
            RenderSummaryInfoBadge(segments, 3);
        }

        // Row 2: Filter pills + checkboxes
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

        SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Без дубликатов", "Без дублікатів", "Skip duplicates")) + 36.0f * uiScale);
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

        if (localInstaller_.IsExportingBackup() || localInstaller_.IsRemovingStorePackages()) {
            const float prog = localInstaller_.Progress();
            const std::string statusMsg = hdm::TrDyn(localInstaller_.StatusMessage());
            char overlay[128];
            std::snprintf(overlay, sizeof(overlay), "%.0f%% — %s", prog * 100.0f, statusMsg.c_str());
            ImGui::ProgressBar(prog, ImVec2(-1.0f, 18.0f * uiScale), overlay);
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
        const float actionColW = rmBtnW + style.CellPadding.x * 2.0f + 4.0f;

        if (ImGui::BeginTable("##DriverStoreTable", 9, kTableFlags, ImGui::GetContentRegionAvail())) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(hdm::Tr("Выбор", "Вибір", "Select"), ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, 54.0f * uiScale, 8);
            ImGui::TableSetupColumn(hdm::Tr("Статус", "Статус", "Status"), ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 1.44f, 0);
            ImGui::TableSetupColumn(hdm::Tr("Пакет", "Пакет", "Package"), ImGuiTableColumnFlags_WidthStretch, 0.66f, 1);
            ImGui::TableSetupColumn(hdm::Tr("INF / Активное устройство", "INF / Активний пристрій", "INF / Active Device"), ImGuiTableColumnFlags_WidthStretch, 1.28f, 2);
            ImGui::TableSetupColumn(hdm::Tr("Поставщик", "Виробник", "Provider"), ImGuiTableColumnFlags_WidthStretch, 0.76f, 3);
            ImGui::TableSetupColumn(hdm::Tr("Класс", "Клас", "Class"), ImGuiTableColumnFlags_WidthStretch, 0.52f, 4);
            ImGui::TableSetupColumn(hdm::Tr("Версия / Дата", "Версія / Дата", "Version / Date"), ImGuiTableColumnFlags_WidthStretch, 1.14f, 5);
            ImGui::TableSetupColumn(hdm::Tr("Размер", "Розмір", "Size"), ImGuiTableColumnFlags_WidthStretch, 0.60f, 6);
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
                const std::string title =
                    pkg.activeDeviceName.empty()
                        ? pkg.originalInfName
                        : (pkg.originalInfName + " — " + pkg.activeDeviceName);
                RenderCellText(title, false, true);
                if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(460.0f * uiScale);
                    ImGui::TextColored(AccentTextColor(), "%s (%s)", pkg.originalInfName.c_str(), pkg.oemInfName.c_str());
                    if (!pkg.activeDeviceName.empty()) {
                        ImGui::Text(
                            hdm::Tr("Устройство: %s", "Пристрій: %s", "Device: %s"),
                            pkg.activeDeviceName.c_str());
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

    void RenderLocalInstallerTab() {
        const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
        const ImGuiStyle& style = ImGui::GetStyle();
        const bool isAppForeground = (GetForegroundWindow() == hwnd_);

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

        const char* pickLbl = hdm::Tr("Обзор папки...###LocPick", "Огляд папки...###LocPick", "Browse Folder...###LocPick");
        const char* scanLbl = hdm::Tr("Найти .inf###LocScan", "Знайти .inf###LocScan", "Scan .inf###LocScan");
        const float buttonsW =
            CalcButtonWidth(hdm::Tr("Обзор папки...", "Огляд папки...", "Browse Folder...")) +
            CalcButtonWidth(hdm::Tr("Найти .inf", "Знайти .inf", "Scan .inf")) +
            style.ItemSpacing.x * 2.0f + 4.0f;

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(hdm::Tr("Папка с драйверами:", "Папка з драйверами:", "Drivers Folder:"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(180.0f, ImGui::GetContentRegionAvail().x - buttonsW));
        ImGui::InputTextWithHint(
            "##FolderInput",
            hdm::Tr(
                "Выберите папку с распакованными .inf драйверами...",
                "Виберіть папку з розпакованими .inf драйверами...",
                "Select folder containing extracted .inf drivers..."),
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
        if (ImGui::Button(hdm::Tr(
                "Установить все .inf (DiInstallDriverW)###LocInstAll",
                "Встановити всі .inf (DiInstallDriverW)###LocInstAll",
                "Install All .inf (DiInstallDriverW)###LocInstAll"))) {
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

    void RenderDiagnosticsTab() {
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
        SameLineWrapIfNeeded(CalcButtonWidth(hdm::Tr("Очистить кэш WU", "Очистити кеш WU", "Reset WU Cache")));
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
                "Ключевые контроллеры платы (%zu)###CtrlPill",
                "Ключові контролери плати (%zu)###CtrlPill",
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
                    "Службы Windows и обновления (%zu | %zu откл.!)###SvcPill",
                    "Служби Windows та оновлення (%zu | %zu вимкн.!)###SvcPill",
                    "Windows & Update Services (%zu | %zu off!)###SvcPill"),
                diagCache_.services.size(),
                diagCache_.disabledCriticalServices);
        } else {
            std::snprintf(
                svcPill,
                sizeof(svcPill),
                hdm::Tr(
                    "Службы Windows и обновления (%zu)###SvcPill",
                    "Служби Windows та оновлення (%zu)###SvcPill",
                    "Windows & Update Services (%zu)###SvcPill"),
                diagCache_.services.size());
        }
        SameLineWrapIfNeeded(CalcSubSectionTabWidth(hdm::Tr(
            "Службы Windows и обновления (99 | 9 откл.!)",
            "Служби Windows та оновлення (99 | 9 вимкн.!)",
            "Windows & Update Services (99 | 9 off!)")));
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
        SameLineWrapIfNeeded(CalcSubSectionTabWidth(hdm::Tr(
            "Точки восстановления (99)",
            "Точки відновлення (99)",
            "Restore Points (99)")));
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
        SameLineWrapIfNeeded(CalcSubSectionTabWidth(hdm::Tr(
            "Накопители и питание (99)",
            "Накопичувачі та живлення (99)",
            "Storage & Power (99)")));
        if (RenderSubSectionHeaderTab(storPill, diagSectionIndex_ == 3)) {
            diagSectionIndex_ = 3;
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
        } else {
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

    void RenderLogPanel() {
        const float uiScale = std::max(1.0f, ImGui::GetFontSize() / 16.5f);
        const ImGuiStyle& style = ImGui::GetStyle();
        bool wasDirty = false;
        const auto entries = logBuffer_.Snapshot(&wasDirty);

        ImGui::PushStyleColor(ImGuiCol_Border, PanelBorderColor());
        ImGui::BeginChild(
            "##LogPanel",
            ImVec2(0.0f, 0.0f),
            true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();

        const float frameH = ImGui::GetFrameHeight();
        {
            const float barW = std::max(2.5f, std::round(3.0f * uiScale));
            const float barPadY = std::round(4.0f * uiScale);
            const ImVec2 pMin = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(pMin.x, pMin.y + barPadY),
                ImVec2(pMin.x + barW, pMin.y + frameH - barPadY),
                ImGui::ColorConvertFloat4ToU32(AccentBarColor()),
                barW * 0.5f);
            ImGui::Dummy(ImVec2(barW + std::round(6.0f * uiScale), frameH));
        }
        ImGui::SameLine(0.0f, 0.0f);
        {
            PushFontSafe(g_fontSecondaryHeader);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(
                CardHeaderTextColor(),
                "%s",
                hdm::Tr("Журнал событий", "Журнал подій", "Event Log"));
            PopFontSafe(g_fontSecondaryHeader);
        }
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("(%zu)", entries.size());

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9.0f * uiScale, 3.0f * uiScale));
        const char* copyLogTxt = hdm::Tr("Копировать", "Копіювати", "Copy");
        const char* clearLogTxt = hdm::Tr("Очистить", "Очистити", "Clear");
        const char* autoLogTxt = hdm::Tr("Автопрокрутка", "Автопрокрутка", "Auto-scroll");
        const float copyW = ImGui::CalcTextSize(copyLogTxt).x + 18.0f * uiScale;
        const float clearW = ImGui::CalcTextSize(clearLogTxt).x + 18.0f * uiScale;
        const float autoW = ImGui::CalcTextSize(autoLogTxt).x + ImGui::GetFrameHeight() + 8.0f;
        const float totalRightW = autoW + copyW + clearW + style.ItemSpacing.x * 2.0f;
        const float winRight = ImGui::GetWindowContentRegionMax().x;
        if (winRight - totalRightW > ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 12.0f) {
            ImGui::SameLine(winRight - totalRightW);
        } else {
            ImGui::SameLine(0.0f, 16.0f);
        }

        ImGui::Checkbox(
            hdm::Tr("Автопрокрутка###LogAuto", "Автопрокрутка###LogAuto", "Auto-scroll###LogAuto"),
            &logAutoScroll_);
        ImGui::SameLine();
        if (RenderSecondaryButton(
                hdm::Tr("Копировать###LogCopy", "Копіювати###LogCopy", "Copy###LogCopy"),
                ImVec2(copyW, 0.0f))) {
            std::string all;
            for (const auto& e : entries) {
                all += "[" + e.timestamp + "] " + hdm::TrDyn(e.message) + "\n";
            }
            ImGui::SetClipboardText(all.c_str());
        }
        ImGui::SameLine();
        if (RenderSecondaryButton(
                hdm::Tr("Очистить###LogClr", "Очистити###LogClr", "Clear###LogClr"),
                ImVec2(clearW, 0.0f))) {
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
                ImVec4 col = g_isLightTheme
                                 ? ImVec4(0.14f, 0.18f, 0.26f, 1.00f)
                                 : ImVec4(0.84f, 0.88f, 0.94f, 1.00f);
                if (entry.level == hdm::LogLevel::Success) {
                    col = g_isLightTheme
                              ? ImVec4(0.05f, 0.48f, 0.30f, 1.00f)
                              : ImVec4(0.20f, 0.83f, 0.60f, 1.00f);
                } else if (entry.level == hdm::LogLevel::Warning) {
                    col = g_isLightTheme
                              ? ImVec4(0.68f, 0.42f, 0.02f, 1.00f)
                              : ImVec4(0.98f, 0.75f, 0.14f, 1.00f);
                } else if (entry.level == hdm::LogLevel::Error) {
                    col = g_isLightTheme
                              ? ImVec4(0.75f, 0.14f, 0.16f, 1.00f)
                              : ImVec4(0.97f, 0.44f, 0.44f, 1.00f);
                }
                const ImVec4 tsCol = g_isLightTheme
                                         ? ImVec4(0.44f, 0.50f, 0.60f, 1.00f)
                                         : ImVec4(0.49f, 0.56f, 0.67f, 1.00f);
                ImGui::TextColored(tsCol, "[%s]", entry.timestamp.c_str());
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::TextUnformatted(hdm::TrDyn(entry.message).c_str());
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
    int activeTabIndex_{0};
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
    std::string selectedDeviceInstanceId_;
    int pendingSelectDeviceRow_{-1};
    bool focusDeviceSearchNextFrame_{false};

    // Components & Peripherals UI state
    std::vector<hdm::SystemComponentItem> componentsCache_;
    std::vector<size_t> visibleComponentIndices_;
    std::unordered_set<std::string> selectedComponentIds_;
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
    bool wuShowHidden_{false};

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

void RenderSingleFrame(
    const std::wstring* screenshotPath,
    bool* screenshotTaken,
    bool* outCaptureDone) {
    if (g_isRenderingFrame || g_appInstance == nullptr ||
        g_pd3dDevice == nullptr || g_pd3dDeviceContext == nullptr || g_pSwapChain == nullptr) {
        return;
    }
    g_isRenderingFrame = true;

    if (g_resizeWidth != 0 && g_resizeHeight != 0) {
        CleanupRenderTarget();
        g_pSwapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
        g_resizeWidth = 0;
        g_resizeHeight = 0;
        CreateRenderTarget();
    }
    if (g_mainRenderTargetView == nullptr) {
        g_isRenderingFrame = false;
        return;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    g_appInstance->RenderFrame();

    ImGui::Render();
    const ImVec4 clearColor = g_isLightTheme
                                  ? ImVec4(0.941f, 0.953f, 0.973f, 1.00f)
                                  : ImVec4(0.059f, 0.071f, 0.098f, 1.00f);
    const float clearColorWithAlpha[4] = {
        clearColor.x, clearColor.y, clearColor.z, clearColor.w};
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColorWithAlpha);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    if (screenshotPath != nullptr && !screenshotPath->empty() &&
        screenshotTaken != nullptr && !(*screenshotTaken)) {
        const double elapsed = ImGui::GetTime();
        if ((elapsed >= 1.8 && g_appInstance->IsInitialScanSettled()) || elapsed >= 4.0) {
            SaveBackBufferToBmp(*screenshotPath);
            *screenshotTaken = true;
            if (outCaptureDone != nullptr) {
                *outCaptureDone = true;
            }
        }
    }

    const HRESULT hr = g_pSwapChain->Present(1, 0);
    g_swapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    g_isRenderingFrame = false;
}

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

    SetWindowTitleBarDark(hwnd, true);

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

    g_dpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    ApplyAppTheme(hwnd, g_dpiScale, false, true);

    const float fontScale = (g_dpiScale > 0.5f ? g_dpiScale : 1.0f);
    auto tryLoadCyrillicFont = [&](const char* path, float sizePx) -> ImFont* {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            return nullptr;
        }
        ImFontConfig cfg{};
        cfg.Flags |= ImFontFlags_NoLoadError;
        return io.Fonts->AddFontFromFileTTF(
            path, sizePx, &cfg, io.Fonts->GetGlyphRangesCyrillic());
    };

    g_fontRegular = tryLoadCyrillicFont("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f * fontScale);
    if (g_fontRegular == nullptr) {
        g_fontRegular = io.Fonts->AddFontDefault();
    }
    io.FontDefault = g_fontRegular;

    g_fontSecondaryHeader =
        tryLoadCyrillicFont("C:\\Windows\\Fonts\\seguisb.ttf", 16.5f * fontScale);
    if (g_fontSecondaryHeader == nullptr) {
        g_fontSecondaryHeader =
            tryLoadCyrillicFont("C:\\Windows\\Fonts\\segoeuib.ttf", 16.5f * fontScale);
    }
    if (g_fontSecondaryHeader == nullptr) {
        g_fontSecondaryHeader = g_fontRegular;
    }

    g_fontMainTab = tryLoadCyrillicFont("C:\\Windows\\Fonts\\segoeuib.ttf", 17.5f * fontScale);
    if (g_fontMainTab == nullptr) {
        g_fontMainTab = g_fontSecondaryHeader;
    }

    g_fontPrimaryHeader =
        tryLoadCyrillicFont("C:\\Windows\\Fonts\\segoeuib.ttf", 19.5f * fontScale);
    if (g_fontPrimaryHeader == nullptr) {
        g_fontPrimaryHeader = g_fontMainTab;
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    int initialTab = -1;
    int initialDiagSection = 0;
    int initialLocalSection = 0;
    int initialSelectedDeviceRow = -1;
    std::wstring screenshotPath;
    hdm::DetectSystemLanguage();
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
                } else if (arg == L"--select-device-row" && i + 1 < argc) {
                    initialSelectedDeviceRow = _wtoi(argv[++i]);
                } else if (arg == L"--screenshot" && i + 1 < argc) {
                    screenshotPath = argv[++i];
                } else if (arg == L"--lang" && i + 1 < argc) {
                    const std::wstring langArg = argv[++i];
                    if (langArg == L"uk" || langArg == L"ua" || langArg == L"UK" || langArg == L"UA") {
                        hdm::SetLang(hdm::Lang::Uk);
                    } else if (langArg == L"en" || langArg == L"EN" || langArg == L"eng" || langArg == L"ENG") {
                        hdm::SetLang(hdm::Lang::En);
                    } else if (langArg == L"ru" || langArg == L"RU") {
                        hdm::SetLang(hdm::Lang::Ru);
                    }
                }
            }
            LocalFree(argv);
        }
    }

    {
        DriverLoomApp app(hwnd, initialTab);
        app.SetInitialDiagSection(initialDiagSection);
        app.SetInitialLocalSection(initialLocalSection);
        app.SetInitialSelectedDeviceRow(initialSelectedDeviceRow);

        struct AppInstanceGuard {
            explicit AppInstanceGuard(DriverLoomApp* ptr) { g_appInstance = ptr; }
            ~AppInstanceGuard() { g_appInstance = nullptr; }
        } appGuard(&app);

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

            RenderSingleFrame(&screenshotPath, &screenshotTaken, &done);
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
