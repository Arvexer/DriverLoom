#pragma once

#include "common/AppCommon.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hdm {

inline constexpr uint64_t kMaxCabPackageBytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr uint64_t kMaxInstallerPackageBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;

/// Resolves and verifies LocalAppData\Temp without reparse points (fail-closed, no %TEMP% fallback).
[[nodiscard]] std::wstring ResolveTrustedLocalTempDirFromBase(
    const std::filesystem::path& localAppDataBase);
[[nodiscard]] std::wstring ResolveTrustedLocalTempDir();

/// Validates an elevated worker temp file path against a canonical non-reparse temp directory.
[[nodiscard]] bool ValidateWorkerTempPathWithBase(
    const std::wstring& rawPath,
    const std::wstring& requiredSuffix,
    const std::wstring& canonicalTempDir,
    std::wstring& outFileName);
[[nodiscard]] bool ValidateWorkerTempPath(
    const std::wstring& rawPath,
    const std::wstring& requiredSuffix,
    std::wstring& outCanonicalTempDir,
    std::wstring& outFileName);

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

/// Returns the static canonical allowlist of trusted vendor publishers permitted for staged packages.
[[nodiscard]] const std::vector<std::string_view>& KnownAllowedPackagePublishers();

/// Checks whether a publisher token exactly matches the canonical elevated-side allowlist.
[[nodiscard]] bool IsKnownAllowedPublisher(std::string_view token);

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

/// Reads PE FixedFileInfo version string (UTF-8), or empty string if unavailable.
[[nodiscard]] std::string GetFileVersionUtf8(const std::filesystem::path& filePath);

/// Validates lowercase/uppercase 64-char SHA-256 hex digest format.
[[nodiscard]] bool IsValidSha256Hex(std::string_view hex);

/// Parses pipe-separated publisher tokens ("Pub1|Pub2") with strict character/length validation.
[[nodiscard]] bool ParsePipeSeparatedPublishers(
    std::string_view raw,
    std::vector<std::string>& outPublishers);

/// Copies a regular non-reparse source file into a newly created destination file up to maxBytes.
bool CopyFileBoundedNoReparse(
    const std::filesystem::path& srcPath,
    const std::filesystem::path& dstPath,
    uint64_t maxBytes,
    std::string* outError = nullptr);

struct StagedPackageOptions {
    std::filesystem::path sourcePackagePath;
    std::string expectedSha256;
    std::vector<std::string> allowedPublishers;
    std::string installerArgs;
    std::string nestedInstallerHint;
    bool isDirectXLegacy{false};
    uint64_t maxPackageBytes{kMaxInstallerPackageBytes};
};

struct StagedPackageResult {
    bool launched{false};
    bool cancelled{false};
    bool processStillRunning{false};
    DWORD exitCode{1};
    DWORD launchError{0};
    std::string fileVersion;
    std::string errorMessage;
};

/// Stages, hashes, extracts (if ZIP), and Authenticode-locks a package inside an isolated directory.
bool StageAndVerifyPackageIntoDir(
    const std::filesystem::path& workDir,
    const StagedPackageOptions& opts,
    std::filesystem::path& outTargetExe,
    std::vector<HandleGuard>& outLocks,
    std::string& outFileVersion,
    std::string& outError,
    const std::atomic<bool>* cancelFlag = nullptr);

/// Stages, verifies, and executes an installer package inside an Admin-only %SystemRoot%\Temp directory.
StagedPackageResult RunStagedPackageInAdminDir(
    const StagedPackageOptions& opts,
    const LogCallback& log,
    const std::atomic<bool>* cancelFlag = nullptr,
    const std::atomic<bool>* stopFlag = nullptr);

/// Launches a verified installer via CreateProcessW when elevated or ShellExecuteExW ("runas") when unelevated,
/// waiting with cancellation support.
bool LaunchAndWaitProcess(
    const std::filesystem::path& exePath,
    const std::wstring& args,
    DWORD* outExitCode,
    DWORD* outLaunchErr,
    const std::atomic<bool>* cancelFlag = nullptr,
    const std::atomic<bool>* stopFlag = nullptr,
    bool* outProcessStillRunning = nullptr);

} // namespace hdm
