#pragma once

#include "ui/AppWidgets.hpp"
#include "core/DeviceScanner.hpp"
#include "core/LocalDriverInstaller.hpp"
#include "core/SystemComponentsScanner.hpp"
#include "core/SystemDiagnostics.hpp"
#include "core/SystemRestore.hpp"
#include "core/WindowsUpdateManager.hpp"

#include "imgui.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class DriverLoomApp {
public:
    explicit DriverLoomApp(HWND mainHwnd, int initialTab = -1);
    ~DriverLoomApp();

    void RenderFrame();

    bool ShouldExitApp() const noexcept { return exitRequested_; }
    void SetInitialDiagSection(int sec) noexcept { diagSectionIndex_ = sec; }
    void SetInitialLocalSection(int sec) noexcept { localSectionIndex_ = sec; }
    void SetInitialSelectedDeviceRow(int row) noexcept { pendingSelectDeviceRow_ = row; }
    bool IsInitialScanSettled() const noexcept;
    void ConfigureCaptureState(
        int tab,
        int diagSec,
        int localSec,
        bool advancedMode,
        bool lightTheme,
        int selectDevRow = -1);
    void OpenSystemProtectionPreviewModal() noexcept;

private:
    enum NavPage : int {
        PageOverview = 0,
        PageComponents = 1,
        PageWindowsUpdate = 2,
        PageDevices = 3,
        PageDiagnostics = 4,
        PageLocalInstaller = 5,
        PageSettings = 6
    };

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

    hdm::LogCallback MakeLogger();

    static std::string FormatSnapshotEntrySummary(
        const std::string& driverVersion,
        const std::string& driverDate,
        const std::string& infName,
        const std::string& statusText);
    static bool IsUserRelevantDeviceIssue(const hdm::DeviceInfo& d);
    static bool IsOptionalVendorSoftware(const hdm::SystemComponentItem& item);
    static bool IsOptionalLegacyRuntime(const hdm::SystemComponentItem& item);
    static bool IsOptionalComponent(const hdm::SystemComponentItem& item);
    static bool IsComponentNotificationMuted(const std::string& id);
    static bool IsComponentRecommendedAction(const hdm::SystemComponentItem& item);
    void RecomputeComponentCounters();

    void LoadBaselineSnapshot();
    void CaptureCurrentAsBaseline(bool logNotification);
    void RebuildSnapshotDiff();
    void SyncScannerSnapshots();

    void RenderSidebar();
    void RenderRebootBanner();
    void StartAppUpdateCheckAsync(bool manual);
    void RenderAboutModal();
    void RequestProtectedInstall(
        std::string actionTitle,
        bool wantRestorePoint,
        std::function<void(bool createRestorePoint)> startInstall);
    void RenderSystemProtectionModal();
    void ProcessPendingSystemProtectionResult();

    enum class AppUpdateState {
        Idle,
        Checking,
        UpToDate,
        UpdateAvailable,
        Error
    };

    void RebuildDeviceViewCache(ImGuiTableSortSpecs* sortSpecs);
    static void SameLineWrapIfNeeded(float nextItemWidth);
    static float CalcButtonWidth(const char* label);
    const hdm::DeviceInfo* FindSelectedDevice() const;
    static bool IsOemInfName(const std::string& infName);
    void JumpToDriverStorePackage(const std::string& infName);
    void HandleKeyboardShortcuts();
    void NavigateTo(int pageIndex);

    void RenderOverviewTab();
    void RenderDevicesTab();
    void RebuildComponentsViewCache(ImGuiTableSortSpecs* sortSpecs);
    void RenderComponentsTab();
    void RenderWindowsUpdateTab();
    void RebuildDriverStoreViewCache(ImGuiTableSortSpecs* sortSpecs);
    void RenderDriverStoreSection();
    void RenderLocalInstallerTab();
    void RenderDiagnosticsTab();
    void RenderSettingsTab();
    void RenderStatusBarAndLogDrawer();

    HWND hwnd_{nullptr};
    bool isElevated_{false};
    int activeTabIndex_{PageOverview};
    bool exitRequested_{false};
    std::atomic<bool> rebootRequired_{false};
    std::atomic<bool> pendingDeviceRescan_{false};
    std::atomic<bool> pendingDiagRescan_{false};
    bool silentDeviceRescan_{false};
    double lastFocusRefreshTime_{-10.0};

    hdm::LogBuffer logBuffer_;

    // System diagnostics snapshot & tab subview state
    hdm::SystemDiagnosticsSnapshot diagCache_;
    uint64_t lastDiagGen_{0};
    int diagSectionIndex_{0};

    // Driver Baseline ("Before / After") snapshot & diff state
    std::string baselineTimestamp_;
    std::vector<DriverBaselineEntry> baselineEntries_;
    std::vector<DriverSnapshotDiffRow> snapshotDiffRows_;
    size_t snapshotChangedCount_{0};
    bool snapShowAll_{false};
    hdm::Lang lastSnapshotLang_{hdm::Lang::Ru};

    // Cached Devices UI state
    std::vector<hdm::DeviceInfo> devicesCache_;
    std::vector<size_t> visibleDeviceIndices_;
    uint64_t lastDeviceGen_{0};
    size_t totalDevicesCount_{0};
    size_t problemDevicesCount_{0};
    size_t userRelevantIssueCount_{0};
    size_t criticalDeviceErrorCount_{0};
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
    size_t actionableComponentsCount_{0};
    size_t mutedComponentsCount_{0};
    size_t gamingComponentsCount_{0};
    size_t peripheralComponentsCount_{0};
    size_t gpuMonitorComponentsCount_{0};
    ComponentFilterMode compFilterMode_{ComponentFilterMode::All};
    bool compRestoreBefore_{true};
    bool compViewDirty_{true};

    // WU UI state
    std::vector<hdm::DriverUpdateItem> wuCache_;
    uint64_t lastWuGen_{0};
    int selectedWuIndex_{-1};
    bool wuSortDirty_{false};
    bool wuRestoreBefore_{true};
    bool wuIncludeSystemUpdates_{false};
    bool wuShowHidden_{false};
    bool wuHasSearchedOnce_{false};

    // Local INF & DriverStore backup UI state
    int localSectionIndex_{0};
    char localFolderBuf_[1024]{};
    bool localForceInstall_{false};
    bool localRestoreBefore_{true};
    bool backupSkipDuplicates_{true};
    bool storeRestoreBefore_{true};
    char storeFilterBuf_[256]{};
    hdm::DriverStoreSnapshot driverStoreCache_;
    std::vector<size_t> visibleStoreIndices_;
    uint64_t lastDriverStoreGen_{0};
    DriverStoreFilterMode storeFilterMode_{DriverStoreFilterMode::All};
    bool storeViewDirty_{true};
    std::mutex pendingPathMutex_;
    std::string pendingFolderUtf8_;
    bool hasPendingFolder_{false};

    // Status bar & collapsible log drawer state
    bool logPanelExpanded_{false};
    bool logAutoScroll_{true};
    int pendingThemeChange_{-1}; // -1 = none, 0 = dark, 1 = light
    TaskbarOverlayState lastTaskbarOverlay_{TaskbarOverlayState::None};
    hdm::Lang lastTaskbarOverlayLang_{hdm::Lang::Ru};

    // About dialog & GitHub Releases update checker
    bool openAboutPopupNextFrame_{false};
    bool aboutModalOpen_{false};
    std::atomic<bool> checkingAppUpdate_{false};
    mutable std::mutex appUpdateMutex_;
    AppUpdateState appUpdateState_{AppUpdateState::Idle};
    std::string appLatestVersion_{hdm::kAppVersion};
    std::string appReleaseUrl_{"https://github.com/Arvexer/DriverLoom/releases"};
    std::atomic<bool> stopAppUpdate_{false};
    std::mutex appHttpMutex_;
    void* activeAppUpdateRequest_{nullptr};

    // Pre-install System Protection prompt modal & 1-click enable state
    bool openSrPromptNextFrame_{false};
    bool srPromptModalOpen_{false};
    hdm::SystemProtectionState srPromptState_{hdm::SystemRestore::QuerySystemProtectionState()};
    double lastSrPromptPollTime_{-1.0};
    std::string srPromptActionTitle_;
    std::function<void(bool)> srPromptInstallCallback_;
    std::function<void(bool)> srPendingAfterEnableCallback_;
    std::atomic<bool> srEnablePendingResult_{false};
    std::atomic<bool> srEnableSucceeded_{false};
    std::mutex srEnableMsgMutex_;
    std::string srEnableResultMsg_;

    // Background worker managers must be declared after caches, logBuffer_, and flags so they join first on destruction.
    hdm::DeviceScanner scanner_;
    hdm::SystemComponentsScanner componentsScanner_;
    hdm::SystemDiagnostics diagnostics_;
    hdm::WindowsUpdateManager wuManager_;
    hdm::LocalDriverInstaller localInstaller_;
    hdm::SystemRestore systemRestore_;
    std::thread appUpdateThread_;
    std::thread baselineSaveThread_;
};
