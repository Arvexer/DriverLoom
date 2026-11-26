#pragma once

#include "common/AppCommon.hpp"

#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hdm {

/// RAII wrapper that deletes an isolated temporary working directory upon destruction.
class TempWorkDirGuard {
public:
    TempWorkDirGuard() = default;
    ~TempWorkDirGuard() { Reset(); }

    TempWorkDirGuard(const TempWorkDirGuard&) = delete;
    TempWorkDirGuard& operator=(const TempWorkDirGuard&) = delete;

    void Set(std::filesystem::path dir);
    void Reset() noexcept;
    void Release() noexcept { dir_.clear(); }
    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return dir_; }

private:
    std::filesystem::path dir_;
};

/// Creates a unique UUID-named directory inside %TEMP% and rejects reparse points.
bool CreateUniqueTempWorkDir(std::filesystem::path& outDir, std::string* outError);

/// Creates a unique UUID-named directory inside %SystemRoot%\Temp with Admin/SYSTEM-only ACL.
bool CreateAdminOnlyTempWorkDir(std::filesystem::path& outDir, std::string* outError);

[[nodiscard]] bool IsSafeRelativeInstallerPath(std::string_view rawRel);
[[nodiscard]] std::filesystem::path ResolveContainedInstallerPath(
    const std::filesystem::path& baseDir,
    std::string_view rawRel);
[[nodiscard]] std::string SanitizeWingetSilentArgs(std::string_view rawArgs);

[[nodiscard]] bool HasZipMagicHeader(const std::filesystem::path& filePath);
[[nodiscard]] bool HasMsiMagicHeader(const std::filesystem::path& filePath);
bool ExtractZipViaShellCom(
    const std::filesystem::path& zipPath,
    const std::filesystem::path& destDir,
    const std::atomic<bool>* cancelFlag = nullptr);

struct CabExtractContext {
    std::filesystem::path baseDir;
    size_t fileCount{0};
    uint64_t totalBytes{0};
    size_t maxFiles{1024};
    uint64_t maxTotalBytes{512ULL * 1024ULL * 1024ULL};
};
UINT CALLBACK CabExtractCallback(PVOID ctx, UINT notification, UINT_PTR param1, UINT_PTR param2);

/// Computes lowercase SHA-256 hex digest of a file using Windows CNG.
bool ComputeFileSha256Hex(
    const std::filesystem::path& filePath,
    std::string& outHex,
    std::string* outReason);

/// Verifies Authenticode chain with revocation checking (and optional signer publisher pinning)
/// while holding a FILE_SHARE_READ handle.
bool VerifyAndLockAuthenticodeFile(
    const std::filesystem::path& filePath,
    HandleGuard* outReadLock,
    std::string* outReason,
    const std::vector<std::string_view>& allowedPublishers = {});

/// Returns the canonical Authenticode publisher allowlist for WHQL CAB driver packages.
[[nodiscard]] const std::vector<std::string_view>& DefaultCabPublishers();

/// Checks Authenticode Subject/CN/Organization fields against an allowlist using whole-token boundaries.
[[nodiscard]] bool MatchesAllowedPublisher(
    std::string_view subject,
    std::string_view cn,
    std::string_view org,
    const std::vector<std::string_view>& allowedPublishers);

/// Verifies every executable/library binary extracted into a temporary folder.
bool VerifyExtractedDirectoryBinaries(
    const std::filesystem::path& dir,
    const std::vector<std::string_view>& allowedPublishers,
    std::string* outReason,
    std::vector<HandleGuard>* outLocks = nullptr);

/// Launches a verified installer via ShellExecuteExW and waits with cancellation support.
bool LaunchAndWaitProcess(
    const std::filesystem::path& exePath,
    const std::wstring& args,
    DWORD* outExitCode,
    DWORD* outLaunchErr,
    const std::atomic<bool>* cancelFlag = nullptr,
    const std::atomic<bool>* stopFlag = nullptr,
    bool* outProcessStillRunning = nullptr);

} // namespace hdm
