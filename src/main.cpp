#include "common/AppCommon.hpp"
#include "core/DeviceScanner.hpp"
#include "core/LocalDriverInstaller.hpp"
#include "core/SystemComponentsScanner.hpp"
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

// --- Refined Nordic Slate Dark Theme ---
void ApplyModernDarkTheme(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();

    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.CellPadding = ImVec2(10.0f, 7.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;

    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;

    style.WindowRounding = 0.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;

    if (scale > 0.5f && scale != 1.0f) {
        style.ScaleAllSizes(scale);
    }

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = ImVec4(0.92f, 0.94f, 0.97f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.54f, 0.60f, 0.69f, 1.00f);
    c[ImGuiCol_WindowBg] = ImVec4(0.067f, 0.078f, 0.106f, 1.00f);
    c[ImGuiCol_ChildBg] = ImVec4(0.086f, 0.102f, 0.137f, 1.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.106f, 0.125f, 0.169f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.149f, 0.180f, 0.243f, 1.00f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    c[ImGuiCol_FrameBg] = ImVec4(0.114f, 0.133f, 0.180f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.149f, 0.176f, 0.239f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.184f, 0.220f, 0.298f, 1.00f);

    c[ImGuiCol_TitleBg] = ImVec4(0.067f, 0.078f, 0.106f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.086f, 0.102f, 0.137f, 1.00f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.082f, 0.098f, 0.133f, 1.00f);

    c[ImGuiCol_ScrollbarBg] = ImVec4(0.067f, 0.078f, 0.106f, 0.45f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.22f, 0.27f, 0.36f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30f, 0.36f, 0.47f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.36f, 0.44f, 0.58f, 1.00f);

    c[ImGuiCol_CheckMark] = ImVec4(0.35f, 0.62f, 0.98f, 1.00f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.35f, 0.62f, 0.98f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.47f, 0.71f, 1.00f, 1.00f);

    c[ImGuiCol_Button] = ImVec4(0.19f, 0.42f, 0.78f, 0.92f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.51f, 0.91f, 1.00f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.16f, 0.36f, 0.69f, 1.00f);

    c[ImGuiCol_Header] = ImVec4(0.22f, 0.46f, 0.84f, 0.30f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.50f, 0.90f, 0.44f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.25f, 0.50f, 0.90f, 0.60f);

    c[ImGuiCol_Separator] = ImVec4(0.157f, 0.188f, 0.255f, 1.00f);
    c[ImGuiCol_Tab] = ImVec4(0.106f, 0.125f, 0.169f, 1.00f);
    c[ImGuiCol_TabHovered] = ImVec4(0.22f, 0.46f, 0.84f, 0.85f);
    c[ImGuiCol_TabSelected] = ImVec4(0.19f, 0.42f, 0.78f, 1.00f);

    c[ImGuiCol_TableHeaderBg] = ImVec4(0.114f, 0.133f, 0.180f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.157f, 0.188f, 0.255f, 1.00f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.129f, 0.157f, 0.216f, 1.00f);
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.022f);

    c[ImGuiCol_PlotHistogram] = ImVec4(0.27f, 0.56f, 0.96f, 1.00f);
}

// Helper for crisp status indicator dot + text inside table cells
void RenderStatusBadge(const ImVec4& color, const char* text) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float lineH = ImGui::GetTextLineHeight();
    const float radius = std::max(3.5f, lineH * 0.22f);
    const ImVec2 center(pos.x + radius + 2.0f, pos.y + lineH * 0.5f);
    dl->AddCircleFilled(center, radius, ImGui::ColorConvertFloat4ToU32(color));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + radius * 2.0f + 8.0f);
    ImGui::TextColored(color, "%s", text);
}

bool RenderPillButton(const char* label, bool active) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.84f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.53f, 0.94f, 1.00f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.125f, 0.149f, 0.200f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.165f, 0.196f, 0.263f, 1.00f));
    }
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(2);
    return clicked;
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

    enum class Mode { None, RestorePoint, InstallInf, WuInstall };
    Mode mode = Mode::None;
    std::wstring targetArg;
    std::wstring logPath;
    std::wstring cancelFilePath;
    bool forceInf = false;
    bool restoreBefore = false;
    bool driversOnly = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--cli-restore-point" && i + 1 < argc) {
            mode = Mode::RestorePoint;
            targetArg = argv[++i];
        } else if (arg == L"--cli-install-inf" && i + 1 < argc) {
            mode = Mode::InstallInf;
            targetArg = argv[++i];
        } else if (arg == L"--cli-wu-install" && i + 1 < argc) {
            mode = Mode::WuInstall;
            targetArg = argv[++i];
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
        std::atomic<bool> workerCancel{false};
        std::atomic<bool> watchStop{false};
        std::thread cancelWatcher;
        if (!cancelFilePath.empty()) {
            const std::filesystem::path marker(cancelFilePath);
            cancelWatcher = std::thread([marker, &workerCancel, &watchStop]() {
                while (!watchStop.load()) {
                    std::error_code ec;
                    if (std::filesystem::exists(marker, ec)) {
                        workerCancel.store(true);
                        break;
                    }
                    Sleep(200);
                }
            });
        }

        const hdm::WuInstallSummary summary =
            hdm::WindowsUpdateManager::DownloadAndInstallSync(
                ids,
                restoreBefore,
                fileLogger,
                &workerProgress,
                &workerCancel,
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

        watchStop.store(true);
        if (cancelWatcher.joinable()) {
            cancelWatcher.join();
        }

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
    explicit DriverLoomApp(HWND mainHwnd)
        : hwnd_(mainHwnd), isElevated_(hdm::IsProcessElevated()) {
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
        const float logPanelHeight = std::clamp(availHeight * 0.24f, 138.0f, 230.0f);
        const float tabAreaHeight = std::max(240.0f, availHeight - logPanelHeight - spacingY);

        ImGui::BeginChild(
            "##TabsRegion",
            ImVec2(0.0f, tabAreaHeight),
            false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        if (ImGui::BeginTabBar("##MainTabs")) {
            if (ImGui::BeginTabItem("  Устройства и мониторы  ")) {
                RenderDevicesTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("  Компоненты и периферия  ")) {
                RenderComponentsTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("  Windows Update  ")) {
                RenderWindowsUpdateTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("  Локальные (.inf)  ")) {
                RenderLocalInstallerTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();

        RenderLogPanel();

        ImGui::End();
    }

    bool ShouldExitApp() const noexcept { return exitRequested_; }

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
        }

        const uint64_t compGen = componentsScanner_.Generation();
        if (compGen != lastComponentsGen_) {
            componentsCache_ = componentsScanner_.GetItemsSnapshot();
            lastComponentsGen_ = compGen;
            compViewDirty_ = true;
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
            }
        }
    }

    void RenderHeaderBar() {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float headerHeight = ImGui::GetFrameHeight() + style.WindowPadding.y * 2.0f;
        ImGui::BeginChild(
            "##HeaderBar",
            ImVec2(0.0f, headerHeight),
            true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.42f, 0.70f, 1.00f, 1.00f), "DriverLoom");
        ImGui::SameLine();
        ImGui::TextDisabled("v1.0.0");

        ImGui::SameLine(0.0f, 16.0f);
        if (isElevated_) {
            ImGui::TextColored(ImVec4(0.20f, 0.83f, 0.60f, 1.00f), "[Администратор]");
        } else {
            ImGui::TextColored(ImVec4(0.98f, 0.75f, 0.14f, 1.00f), "[Обычный режим]");
            ImGui::SameLine();
            if (ImGui::Button("Перезапустить от имени администратора")) {
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
        const float btnWidth = ImGui::CalcTextSize(srLabel).x + style.FramePadding.x * 2.0f + 8.0f;
        const float rightX = ImGui::GetWindowContentRegionMax().x - btnWidth;
        if (rightX > ImGui::GetCursorPosX() + 12.0f) {
            ImGui::SameLine(rightX);
        } else {
            ImGui::SameLine();
        }

        ImGui::BeginDisabled(systemRestore_.IsBusy());
        if (ImGui::Button(srLabel, ImVec2(btnWidth, 0.0f))) {
            systemRestore_.CreateRestorePointAsync(
                "DriverLoom (ручная точка восстановления)",
                MakeLogger());
        }
        ImGui::EndDisabled();

        ImGui::EndChild();
    }

    void RenderRebootBanner() {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float bannerHeight = ImGui::GetFrameHeight() + style.WindowPadding.y * 2.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.26f, 0.18f, 0.05f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.96f, 0.68f, 0.16f, 1.00f));
        ImGui::BeginChild(
            "##RebootBanner",
            ImVec2(0.0f, bannerHeight),
            true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(
            ImVec4(1.00f, 0.87f, 0.45f, 1.00f),
            "Требуется перезагрузка Windows для завершения установки драйверов.");
        const char* hideLabel = "Скрыть";
        const float hideW = ImGui::CalcTextSize(hideLabel).x + style.FramePadding.x * 2.0f;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0f,
                                 ImGui::GetWindowContentRegionMax().x - hideW));
        if (ImGui::Button(hideLabel, ImVec2(hideW, 0.0f))) {
            rebootRequired_.store(false);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
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
        const ImGuiStyle& style = ImGui::GetStyle();

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
                1.05f,
                0);
            ImGui::TableSetupColumn("Устройство / Модель", ImGuiTableColumnFlags_WidthStretch, 1.60f, 1);
            ImGui::TableSetupColumn("Производитель", ImGuiTableColumnFlags_WidthStretch, 0.95f, 2);
            ImGui::TableSetupColumn("Класс", ImGuiTableColumnFlags_WidthStretch, 0.65f, 3);
            ImGui::TableSetupColumn("Версия", ImGuiTableColumnFlags_WidthStretch, 0.75f, 4);
            ImGui::TableSetupColumn("Дата", ImGuiTableColumnFlags_WidthStretch, 0.60f, 5);
            ImGui::TableSetupColumn("Hardware ID", ImGuiTableColumnFlags_WidthStretch, 1.35f, 6);
            ImGui::TableHeadersRow();

            ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
            if (sortSpecs != nullptr && sortSpecs->SpecsDirty) {
                deviceViewDirty_ = true;
                sortSpecs->SpecsDirty = false;
            }
            if (deviceViewDirty_) {
                RebuildDeviceViewCache(sortSpecs);
            }

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(visibleDeviceIndices_.size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const hdm::DeviceInfo& dev =
                        devicesCache_[visibleDeviceIndices_[static_cast<size_t>(row)]];
                    ImGui::TableNextRow();
                    ImGui::PushID(row);

                    // Col 0: Status badge + full-row selectable for context menu
                    ImGui::TableSetColumnIndex(0);
                    ImVec4 statusColor(0.20f, 0.83f, 0.60f, 1.00f); // Emerald
                    if (dev.health == hdm::DeviceHealth::Error) {
                        statusColor = ImVec4(0.97f, 0.44f, 0.44f, 1.00f); // Coral Red
                    } else if (dev.health == hdm::DeviceHealth::Warning || dev.isGenericMonitorDriver) {
                        statusColor = ImVec4(0.98f, 0.75f, 0.14f, 1.00f); // Amber
                    }
                    RenderStatusBadge(statusColor, dev.statusText.c_str());

                    ImGui::SameLine();
                    ImGui::Selectable(
                        "##rowSel",
                        false,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
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

                    // Col 1: Device Name
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(dev.name.c_str());
                    if (GetForegroundWindow() == hwnd_ &&
                        ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        ImGui::SetTooltip(
                            "%s\nПроизводитель: %s\nКласс: %s\nINF: %s\nHardware ID: %s%s",
                            dev.name.c_str(),
                            dev.manufacturer.empty() ? "—" : dev.manufacturer.c_str(),
                            dev.deviceClass.empty() ? "—" : dev.deviceClass.c_str(),
                            dev.infName.empty() ? "—" : dev.infName.c_str(),
                            dev.hardwareId.empty() ? "—" : dev.hardwareId.c_str(),
                            dev.edidModel.empty() ? "" : ("\nМодель (EDID): " + dev.edidModel).c_str());
                    }

                    // Col 2: Manufacturer
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextUnformatted(
                        dev.manufacturer.empty() ? "—" : dev.manufacturer.c_str());

                    // Col 3: Class
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(
                        dev.deviceClass.empty() ? "—" : dev.deviceClass.c_str());

                    // Col 4: Version
                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextUnformatted(
                        dev.driverVersion.empty() ? "—" : dev.driverVersion.c_str());

                    // Col 5: Date
                    ImGui::TableSetColumnIndex(5);
                    ImGui::TextUnformatted(
                        dev.driverDate.empty() ? "—" : dev.driverDate.c_str());

                    // Col 6: Hardware ID
                    ImGui::TableSetColumnIndex(6);
                    ImGui::TextUnformatted(
                        dev.hardwareId.empty() ? "—" : dev.hardwareId.c_str());

                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        (void)style;
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
        const bool busy =
            componentsScanner_.IsScanning() ||
            componentsScanner_.IsCheckingOnline() ||
            componentsScanner_.IsInstalling();

        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Обновить проверку")) {
            componentsScanner_.StartLocalScanAsync(devicesCache_, MakeLogger());
        }
        SameLineWrapIfNeeded(CalcButtonWidth("Проверить онлайн"));
        if (ImGui::Button("Проверить онлайн")) {
            componentsScanner_.StartOnlineVersionCheckAsync(MakeLogger());
        }
        ImGui::EndDisabled();

        SameLineWrapIfNeeded(CalcButtonWidth("Все"));
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

        const std::string phase = componentsScanner_.PhaseMessage();
        if (busy || !phase.empty()) {
            const float phaseW = ImGui::CalcTextSize(phase.c_str()).x;
            const float winRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            if (ImGui::GetItemRectMax().x + 16.0f + phaseW <= winRight) {
                ImGui::SameLine(0.0f, 16.0f);
                ImGui::AlignTextToFramePadding();
            }
            ImGui::TextDisabled("%s", phase.c_str());
        }

        if (componentsScanner_.IsInstalling()) {
            const float prog = componentsScanner_.Progress();
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "%.0f%%", prog * 100.0f);
            ImGui::ProgressBar(prog, ImVec2(-1.0f, 18.0f), overlay);
        }

        ImGui::Spacing();

        constexpr ImGuiTableFlags kTableFlags =
            ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
            ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY;

        const ImGuiStyle& compStyle = ImGui::GetStyle();
        const float twoBtnsW =
            CalcButtonWidth("Установить") + compStyle.ItemSpacing.x + CalcButtonWidth("Сайт");
        const float oneBtnW = CalcButtonWidth("Открыть сайт");
        const float actionColW =
            std::max(twoBtnsW, oneBtnW) + compStyle.CellPadding.x * 2.0f + 6.0f;

        if (ImGui::BeginTable("##ComponentsTable", 6, kTableFlags, ImGui::GetContentRegionAvail())) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(
                "Статус",
                ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch,
                1.10f,
                0);
            ImGui::TableSetupColumn(
                "Компонент / Официальное ПО", ImGuiTableColumnFlags_WidthStretch, 1.55f, 1);
            ImGui::TableSetupColumn(
                "Устройство / Назначение", ImGuiTableColumnFlags_WidthStretch, 1.20f, 2);
            ImGui::TableSetupColumn(
                "Установлено", ImGuiTableColumnFlags_WidthStretch, 0.72f, 3);
            ImGui::TableSetupColumn(
                "Последняя офиц.", ImGuiTableColumnFlags_WidthStretch, 0.78f, 4);
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
                RenderStatusBadge(statusCol, item.statusText.c_str());

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(item.name.c_str());
                if (isAppForeground && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(420.0f * uiScale);
                    ImGui::TextColored(ImVec4(0.42f, 0.70f, 1.00f, 1.00f), "%s", item.name.c_str());
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
                ImGui::TextUnformatted(
                    item.detectedHardware.empty() ? "Игры и системные библиотеки Windows"
                                                  : item.detectedHardware.c_str());

                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(
                    item.installedVersion.empty() ? "—" : item.installedVersion.c_str());

                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(
                    item.latestVersion.empty() ? "—" : item.latestVersion.c_str());

                ImGui::TableSetColumnIndex(5);
                if (!item.directInstallerUrl.empty()) {
                    ImGui::BeginDisabled(busy);
                    const char* btnLbl = !item.installed
                                             ? "Установить"
                                             : (item.updateAvailable ? "Обновить" : "Скачать");
                    if (ImGui::SmallButton(btnLbl)) {
                        componentsScanner_.DownloadAndInstallAsync(
                            item, devicesCache_, MakeLogger());
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Сайт")) {
                        OpenUrlInBrowser(item.officialPageUrl);
                    }
                } else if (!item.officialPageUrl.empty()) {
                    if (ImGui::SmallButton("Открыть сайт")) {
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
        if (ImGui::Button("Выбрать все")) {
            wuManager_.SelectAll(true);
            for (auto& u : wuCache_) u.selected = true;
        }
        SameLineWrapIfNeeded(CalcButtonWidth("Снять все"));
        if (ImGui::Button("Снять все")) {
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

        SameLineWrapIfNeeded(CalcButtonWidth("Скачать и установить выбранные"));
        ImGui::BeginDisabled(busy || selectedIds.empty());
        if (ImGui::Button("Скачать и установить выбранные")) {
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
            if (ImGui::Button("Отмена")) {
                wuManager_.RequestCancel();
            }
        }

        ImGui::Checkbox("Включать системные обновления и библиотеки (не только драйверы)", &wuIncludeSystemUpdates_);
        SameLineWrapIfNeeded(CalcButtonWidth("Точка восстановления перед установкой") + 28.0f);
        ImGui::Checkbox("Точка восстановления перед установкой", &wuRestoreBefore_);

        const std::string phase = wuManager_.PhaseMessage();
        if (busy || !phase.empty()) {
            ImGui::TextDisabled("%s", phase.c_str());
        }

        if (wuManager_.IsInstalling() || wuManager_.IsSearching()) {
            if (wuManager_.IsSearching()) {
                const float anim = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
                ImGui::ProgressBar(anim, ImVec2(-1.0f, 18.0f), "Поиск в Windows Update...");
            } else {
                const float prog = wuManager_.Progress();
                char overlay[64];
                std::snprintf(overlay, sizeof(overlay), "%.0f%%", prog * 100.0f);
                ImGui::ProgressBar(prog, ImVec2(-1.0f, 18.0f), overlay);
            }
        }

        ImGui::Spacing();

        const float availY = ImGui::GetContentRegionAvail().y;
        const float detailsH = (selectedWuIndex_ >= 0 && selectedWuIndex_ < static_cast<int>(wuCache_.size()))
                                   ? 88.0f * uiScale
                                   : 0.0f;
        const float tableH = std::max(120.0f, availY - (detailsH > 0.0f ? detailsH + 8.0f : 0.0f));

        constexpr ImGuiTableFlags kTableFlags =
            ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY;

        if (ImGui::BeginTable("##WuTable", 5, kTableFlags, ImVec2(0.0f, tableH))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(
                "Выбор",
                ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed,
                52.0f * uiScale,
                0);
            ImGui::TableSetupColumn("Обновление", ImGuiTableColumnFlags_WidthStretch, 1.80f, 1);
            ImGui::TableSetupColumn(
                "Производитель / Модель", ImGuiTableColumnFlags_WidthStretch, 1.10f, 2);
            ImGui::TableSetupColumn(
                "Размер", ImGuiTableColumnFlags_WidthStretch, 0.50f, 3);
            ImGui::TableSetupColumn(
                "Статус", ImGuiTableColumnFlags_WidthStretch, 0.95f, 4);
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

            for (int i = 0; i < static_cast<int>(wuCache_.size()); ++i) {
                auto& item = wuCache_[static_cast<size_t>(i)];
                ImGui::TableNextRow();
                ImGui::PushID(i);

                ImGui::TableSetColumnIndex(0);
                bool checked = item.selected;
                ImGui::BeginDisabled(busy || item.installedOk);
                if (ImGui::Checkbox("##sel", &checked)) {
                    wuManager_.SetUpdateSelected(item.updateId, checked);
                    item.selected = checked;
                }
                ImGui::EndDisabled();

                ImGui::TableSetColumnIndex(1);
                const bool isSelectedRow = (selectedWuIndex_ == i);
                if (ImGui::Selectable(
                        item.title.c_str(),
                        isSelectedRow,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                    selectedWuIndex_ = i;
                }

                ImGui::TableSetColumnIndex(2);
                std::string mfgModel = item.driverManufacturer;
                if (!item.driverModel.empty()) {
                    if (!mfgModel.empty()) mfgModel += " — ";
                    mfgModel += item.driverModel;
                }
                ImGui::TextUnformatted(mfgModel.empty() ? "—" : mfgModel.c_str());

                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(item.sizeFormatted.c_str());

                ImGui::TableSetColumnIndex(4);
                const ImVec4 col = item.installedOk
                                       ? ImVec4(0.20f, 0.83f, 0.60f, 1.00f)
                                       : ImVec4(0.60f, 0.72f, 0.90f, 1.00f);
                RenderStatusBadge(col, item.statusText.c_str());

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        if (detailsH > 0.0f) {
            const auto& sel = wuCache_[static_cast<size_t>(selectedWuIndex_)];
            ImGui::Spacing();
            ImGui::BeginChild("##WuDetails", ImVec2(0.0f, detailsH), true);
            ImGui::TextWrapped("%s", sel.description.empty() ? sel.title.c_str() : sel.description.c_str());
            ImGui::TextDisabled(
                "Hardware ID: %s   |   Дата: %s   |   Update ID: %s",
                sel.driverHardwareId.empty() ? "—" : sel.driverHardwareId.c_str(),
                sel.driverVerDate.empty() ? "—" : sel.driverVerDate.c_str(),
                sel.updateId.c_str());
            ImGui::EndChild();
        }
    }

    void RenderLocalInstallerTab() {
        const ImGuiStyle& style = ImGui::GetStyle();
        const bool busy =
            localInstaller_.IsPickingFolder() ||
            localInstaller_.IsScanningInf() ||
            localInstaller_.IsInstalling();

        const char* pickLbl = "Обзор папки...";
        const char* scanLbl = "Найти .inf";
        const float buttonsW =
            ImGui::CalcTextSize(pickLbl).x + ImGui::CalcTextSize(scanLbl).x +
            style.FramePadding.x * 4.0f + style.ItemSpacing.x * 3.0f;

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
        if (ImGui::Button(pickLbl)) {
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
        if (ImGui::Button(scanLbl)) {
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
        SameLineWrapIfNeeded(CalcButtonWidth("Точка восстановления перед установкой") + 28.0f);
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

        if (localInstaller_.IsInstalling()) {
            const float prog = localInstaller_.Progress();
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "%.0f%%", prog * 100.0f);
            ImGui::ProgressBar(prog, ImVec2(-1.0f, 18.0f), overlay);
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Найдено .inf файлов: %zu", infFiles.size());

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
                ImGui::TextUnformatted(entry.fileName.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(entry.relativePath.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(hdm::FormatBytes(entry.sizeBytes).c_str());
            }
            ImGui::EndTable();
        }
    }

    void RenderLogPanel() {
        bool wasDirty = false;
        const auto entries = logBuffer_.Snapshot(&wasDirty);

        ImGui::BeginChild(
            "##LogPanel",
            ImVec2(0.0f, 0.0f),
            true,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.62f, 0.72f, 0.88f, 1.00f), "Журнал событий");
        ImGui::SameLine();
        ImGui::Checkbox("Автопрокрутка", &logAutoScroll_);
        ImGui::SameLine();
        if (ImGui::SmallButton("Копировать")) {
            std::string all;
            for (const auto& e : entries) {
                all += "[" + e.timestamp + "] " + e.message + "\n";
            }
            ImGui::SetClipboardText(all.c_str());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Очистить")) {
            logBuffer_.Clear();
        }
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
    bool exitRequested_{false};
    std::atomic<bool> rebootRequired_{false};
    std::atomic<bool> pendingDeviceRescan_{false};
    double lastFocusRefreshTime_{-10.0};

    hdm::LogBuffer logBuffer_;
    hdm::DeviceScanner scanner_;
    hdm::SystemComponentsScanner componentsScanner_;
    hdm::WindowsUpdateManager wuManager_;
    hdm::LocalDriverInstaller localInstaller_;
    hdm::SystemRestore systemRestore_;

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

    // Local INF UI state
    char localFolderBuf_[1024]{};
    bool localForceInstall_{false};
    bool localRestoreBefore_{true};
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

    {
        DriverLoomApp app(hwnd);
        const ImVec4 clearColor(0.067f, 0.078f, 0.106f, 1.00f);

        bool done = false;
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

            if (g_swapChainOccluded &&
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
