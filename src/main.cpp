#include "app/CliWorker.hpp"
#include "common/AppCommon.hpp"
#include "ui/AppWidgets.hpp"
#include "ui/DriverLoomApp.hpp"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

// --- DirectX 11 Global State ---
ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
ID3D11ShaderResourceView* g_appLogoSrv = nullptr;
bool g_swapChainOccluded = false;
UINT g_resizeWidth = 0;
UINT g_resizeHeight = 0;

DriverLoomApp* g_appInstance = nullptr;
HWND g_mainHwnd = nullptr;
bool g_isRenderingFrame = false;
bool g_pendingDpiReload = false;
float g_pendingDpiScale = 1.0f;

void LoadAppFonts(float dpiScale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    const float fontScale = (dpiScale > 0.5f ? dpiScale : 1.0f);
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

    g_fontRegular = tryLoadCyrillicFont("C:\\Windows\\Fonts\\segoeui.ttf", 16.5f * fontScale);
    if (g_fontRegular == nullptr) {
        g_fontRegular = io.Fonts->AddFontDefault();
    }
    io.FontDefault = g_fontRegular;

    g_fontSmall = tryLoadCyrillicFont("C:\\Windows\\Fonts\\segoeui.ttf", 15.0f * fontScale);
    if (g_fontSmall == nullptr) {
        g_fontSmall = g_fontRegular;
    }

    g_fontSecondaryHeader =
        tryLoadCyrillicFont("C:\\Windows\\Fonts\\seguisb.ttf", 17.5f * fontScale);
    if (g_fontSecondaryHeader == nullptr) {
        g_fontSecondaryHeader =
            tryLoadCyrillicFont("C:\\Windows\\Fonts\\segoeuib.ttf", 17.5f * fontScale);
    }
    if (g_fontSecondaryHeader == nullptr) {
        g_fontSecondaryHeader = g_fontRegular;
    }

    g_fontMainTab = tryLoadCyrillicFont("C:\\Windows\\Fonts\\segoeuib.ttf", 20.5f * fontScale);
    if (g_fontMainTab == nullptr) {
        g_fontMainTab = g_fontSecondaryHeader;
    }

    g_fontPrimaryHeader =
        tryLoadCyrillicFont("C:\\Windows\\Fonts\\segoeuib.ttf", 23.5f * fontScale);
    if (g_fontPrimaryHeader == nullptr) {
        g_fontPrimaryHeader = g_fontMainTab;
    }
}

void RenderSingleFrame(
    const std::wstring* screenshotPath = nullptr,
    bool* screenshotTaken = nullptr,
    bool* outCaptureDone = nullptr,
    const std::wstring* captureSuiteDir = nullptr,
    int* suiteStep = nullptr);

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

void CreateAppLogoTexture(HINSTANCE hInstance, int iconResId) {
    if (g_pd3dDevice == nullptr || g_appLogoSrv != nullptr) {
        return;
    }
    HICON hIcon = static_cast<HICON>(LoadImageW(
        hInstance,
        MAKEINTRESOURCEW(iconResId),
        IMAGE_ICON,
        128,
        128,
        LR_DEFAULTCOLOR));
    if (hIcon == nullptr) {
        return;
    }

    ICONINFO ii{};
    if (!GetIconInfo(hIcon, &ii)) {
        DestroyIcon(hIcon);
        return;
    }

    BITMAP bm{};
    if (ii.hbmColor != nullptr && GetObjectW(ii.hbmColor, sizeof(bm), &bm) > 0 &&
        bm.bmWidth > 0 && bm.bmHeight > 0) {
        const UINT w = static_cast<UINT>(bm.bmWidth);
        const UINT h = static_cast<UINT>(bm.bmHeight);
        std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4u, 0);

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = static_cast<LONG>(w);
        bmi.bmiHeader.biHeight = -static_cast<LONG>(h); // Top-down DIB
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        HDC hdc = GetDC(nullptr);
        if (hdc != nullptr) {
            if (GetDIBits(hdc, ii.hbmColor, 0, h, pixels.data(), &bmi, DIB_RGB_COLORS) > 0) {
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = w;
                desc.Height = h;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_IMMUTABLE;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

                D3D11_SUBRESOURCE_DATA initData{};
                initData.pSysMem = pixels.data();
                initData.SysMemPitch = w * 4u;

                ID3D11Texture2D* tex = nullptr;
                if (SUCCEEDED(g_pd3dDevice->CreateTexture2D(&desc, &initData, &tex)) && tex != nullptr) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                    srvDesc.Format = desc.Format;
                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Texture2D.MipLevels = 1;
                    if (SUCCEEDED(g_pd3dDevice->CreateShaderResourceView(tex, &srvDesc, &g_appLogoSrv)) &&
                        g_appLogoSrv != nullptr) {
                        g_appLogoTexture = reinterpret_cast<ImTextureID>(g_appLogoSrv);
                    }
                    tex->Release();
                }
            }
            ReleaseDC(nullptr, hdc);
        }
    }

    if (ii.hbmColor != nullptr) DeleteObject(ii.hbmColor);
    if (ii.hbmMask != nullptr) DeleteObject(ii.hbmMask);
    DestroyIcon(hIcon);
}

void CleanupDeviceD3D() {
    if (g_appLogoSrv != nullptr) {
        g_appLogoSrv->Release();
        g_appLogoSrv = nullptr;
    }
    g_appLogoTexture = 0;
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
    case WM_DPICHANGED: {
        const float newScale = static_cast<float>(HIWORD(wParam)) / 96.0f;
        if (newScale >= 0.5f && newScale <= 4.0f) {
            g_pendingDpiScale = newScale;
            g_pendingDpiReload = true;
        }
        const auto* rc = reinterpret_cast<const RECT*>(lParam);
        if (rc != nullptr) {
            SetWindowPos(
                hWnd,
                nullptr,
                rc->left,
                rc->top,
                rc->right - rc->left,
                rc->bottom - rc->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        const float dpi = (g_dpiScale > 0.5f ? g_dpiScale : 1.0f);
        const float minScale = std::min(dpi, EffectiveUiScale());
        LONG minW = static_cast<LONG>(std::round(980.0f * minScale));
        LONG minH = static_cast<LONG>(std::round(640.0f * minScale));
        HMONITOR mon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (mon != nullptr && GetMonitorInfoW(mon, &mi)) {
            const LONG workW = mi.rcWork.right - mi.rcWork.left;
            const LONG workH = mi.rcWork.bottom - mi.rcWork.top;
            if (workW > 320 && minW > workW) minW = workW;
            if (workH > 240 && minH > workH) minH = workH;
        }
        mmi->ptMinTrackSize.x = minW;
        mmi->ptMinTrackSize.y = minH;
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

void RenderSingleFrame(
    const std::wstring* screenshotPath,
    bool* screenshotTaken,
    bool* outCaptureDone,
    const std::wstring* captureSuiteDir,
    int* suiteStep) {
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

    if (g_pendingDpiReload || g_pendingScaleReload) {
        if (g_pendingDpiReload) {
            g_dpiScale = g_pendingDpiScale;
            g_pendingDpiReload = false;
        }
        g_pendingScaleReload = false;
        const float effScale = EffectiveUiScale();
        ApplyAppTheme(g_mainHwnd, effScale, g_isLightTheme, true);
        ImGui_ImplDX11_InvalidateDeviceObjects();
        LoadAppFonts(effScale);
        ImGui_ImplDX11_CreateDeviceObjects();
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    g_appInstance->RenderFrame();

    ImGui::Render();
    const ImVec4 clearColor = CanvasBgColor();
    const float clearColorWithAlpha[4] = {
        clearColor.x, clearColor.y, clearColor.z, clearColor.w};
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColorWithAlpha);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    if (captureSuiteDir != nullptr && !captureSuiteDir->empty() &&
        suiteStep != nullptr && *suiteStep >= 0 && *suiteStep <= 23) {
        const double elapsed = ImGui::GetTime();
        if ((elapsed >= 1.8 && g_appInstance->IsInitialScanSettled()) || elapsed >= 6.0) {
            const std::filesystem::path dirPath(*captureSuiteDir);
            const int viewIdx = *suiteStep / 4;
            const int phase = *suiteStep % 4;
            if (phase == 0) {
                switch (viewIdx) {
                case 0:
                    g_appInstance->ConfigureCaptureState(0, 0, 0, true, false, -1);
                    break;
                case 1:
                    g_appInstance->ConfigureCaptureState(1, 0, 0, true, false, -1);
                    break;
                case 2:
                    g_appInstance->ConfigureCaptureState(2, 0, 0, true, false, -1);
                    break;
                case 3:
                    g_appInstance->ConfigureCaptureState(3, 0, 0, true, false, 0);
                    break;
                case 4:
                    g_appInstance->ConfigureCaptureState(4, 0, 0, true, false, -1);
                    break;
                case 5:
                    g_appInstance->ConfigureCaptureState(0, 0, 0, false, true, -1);
                    break;
                default:
                    break;
                }
            } else if (phase == 3) {
                static constexpr const wchar_t* kSuiteFiles[6] = {
                    L"01-overview.bmp",
                    L"02-components.bmp",
                    L"03-windows-update.bmp",
                    L"04-devices.bmp",
                    L"05-diagnostics.bmp",
                    L"06-light-theme.bmp",
                };
                if (viewIdx >= 0 && viewIdx < 6) {
                    SaveBackBufferToBmp((dirPath / kSuiteFiles[viewIdx]).wstring());
                }
                if (viewIdx == 5 && outCaptureDone != nullptr) {
                    *outCaptureDone = true;
                }
            }
            ++(*suiteStep);
        }
    } else if (screenshotPath != nullptr && !screenshotPath->empty() &&
               screenshotTaken != nullptr && !(*screenshotTaken)) {
        const double elapsed = ImGui::GetTime();
        if ((elapsed >= 1.8 && g_appInstance->IsInitialScanSettled()) || elapsed >= 6.0) {
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
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);

    const int cliExit = RunCliModeIfRequested();
    if (cliExit >= 0) {
        return cliExit;
    }

    int initialTab = -1;
    int initialDiagSection = 0;
    int initialLocalSection = 0;
    int initialSelectedDeviceRow = -1;
    std::wstring screenshotPath;
    std::wstring captureSuiteDir;
    bool hasLocalSectionArg = false;
    bool hasModeArg = false;

    hdm::DetectSystemLanguage();
    LoadUserPreferences();
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
                    hasLocalSectionArg = true;
                } else if (arg == L"--select-device-row" && i + 1 < argc) {
                    initialSelectedDeviceRow = _wtoi(argv[++i]);
                } else if (arg == L"--screenshot" && i + 1 < argc) {
                    screenshotPath = argv[++i];
                } else if (arg == L"--capture-suite" && i + 1 < argc) {
                    captureSuiteDir = argv[++i];
                } else if (arg == L"--lang" && i + 1 < argc) {
                    const std::wstring langArg = argv[++i];
                    if (langArg == L"uk" || langArg == L"ua" || langArg == L"UK" || langArg == L"UA") {
                        hdm::SetLang(hdm::Lang::Uk);
                    } else if (langArg == L"en" || langArg == L"EN" || langArg == L"eng" || langArg == L"ENG") {
                        hdm::SetLang(hdm::Lang::En);
                    } else if (langArg == L"ru" || langArg == L"RU") {
                        hdm::SetLang(hdm::Lang::Ru);
                    }
                } else if (arg == L"--mode" && i + 1 < argc) {
                    const std::wstring modeArg = argv[++i];
                    if (modeArg == L"advanced" || modeArg == L"adv" || modeArg == L"1") {
                        g_isAdvancedMode = true;
                        hasModeArg = true;
                    } else if (modeArg == L"standard" || modeArg == L"safe" || modeArg == L"0") {
                        g_isAdvancedMode = false;
                        hasModeArg = true;
                    }
                } else if (arg == L"--theme" && i + 1 < argc) {
                    const std::wstring themeArg = argv[++i];
                    if (themeArg == L"light" || themeArg == L"LIGHT") {
                        g_isLightTheme = true;
                    } else if (themeArg == L"dark" || themeArg == L"DARK") {
                        g_isLightTheme = false;
                    }
                } else if (arg == L"--scale" && i + 1 < argc) {
                    const int scalePct = _wtoi(argv[++i]);
                    if (scalePct >= 80 && scalePct <= 175) {
                        g_uiScalePercent = scalePct;
                    }
                }
            }
            LocalFree(argv);
        }
    }
    if ((initialTab == 5 || hasLocalSectionArg) && !hasModeArg) {
        g_isAdvancedMode = true;
    }
    if (!captureSuiteDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(captureSuiteDir, ec);
        g_isAdvancedMode = true;
        g_isLightTheme = false;
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
        LR_DEFAULTCOLOR | LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(
        hInstance,
        MAKEINTRESOURCEW(kAppIconId),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR | LR_SHARED));
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
    g_mainHwnd = hwnd;

    g_dpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    if (g_dpiScale > 0.5f) {
        int initW = static_cast<int>(std::round(1280.0f * g_dpiScale));
        int initH = static_cast<int>(std::round(820.0f * g_dpiScale));
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (mon != nullptr && GetMonitorInfoW(mon, &mi)) {
            const int workW = mi.rcWork.right - mi.rcWork.left;
            const int workH = mi.rcWork.bottom - mi.rcWork.top;
            if (workW > 480) initW = std::min(initW, workW);
            if (workH > 360) initH = std::min(initH, workH);
        }
        SetWindowPos(
            hwnd,
            nullptr,
            0,
            0,
            initW,
            initH,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    SetWindowTitleBarDark(hwnd, !g_isLightTheme);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    CreateAppLogoTexture(hInstance, kAppIconId);

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    g_dpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    const float startupScale = EffectiveUiScale();
    ApplyAppTheme(hwnd, startupScale, g_isLightTheme, true);
    LoadAppFonts(startupScale);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    hdm::CoInitGuard mainComInit(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
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
        int suiteStep = 0;
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

            if (screenshotPath.empty() && captureSuiteDir.empty() && g_swapChainOccluded &&
                g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
                Sleep(10);
                continue;
            }
            g_swapChainOccluded = false;

            RenderSingleFrame(&screenshotPath, &screenshotTaken, &done, &captureSuiteDir, &suiteStep);
        }
    }

    g_mainHwnd = nullptr;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}
