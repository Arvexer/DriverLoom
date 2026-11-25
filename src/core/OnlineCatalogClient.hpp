#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hdm {

[[nodiscard]] std::vector<uint64_t> ParseVersionParts(const std::string& ver);
[[nodiscard]] int ParseDriverDateYmd(std::string_view text);
bool TryExtractTrailingVersionDate(const std::vector<uint64_t>& parts, int& outYmd);
[[nodiscard]] bool IsHostWindows11();
[[nodiscard]] bool IsVersionNewer(const std::string& latest, const std::string& current);
[[nodiscard]] bool IsDriverVersionNewer(const std::string& latest, const std::string& current);
[[nodiscard]] bool EndsWithCaseInsensitive(std::string_view text, std::string_view suffix);
[[nodiscard]] std::string TrimCopy(std::string_view sv);
[[nodiscard]] std::string ExtractVenDevSubsysKey(std::string_view hwId);
[[nodiscard]] std::string UrlEncodeCatalogParam(std::string_view val);

struct CatalogDriverMatch {
    std::string version;
    std::string cabUrl;
    int dateYmd{0};
};

/// Queries Microsoft Update Catalog for a WHQL .cab matching searchKey and host OS/CPU architecture.
CatalogDriverMatch ResolveMsCatalogDriverCab(const std::string& searchKey);

struct WingetManifestInfo {
    bool ok{false};
    std::string version;
    std::string url;
    std::string sha256;
    std::string nestedRelativePath;
    std::string silentArgs;
};

/// Parses a winget .installer.yaml manifest, validating HTTPS URL, nested path, and silent flags.
WingetManifestInfo ParseWingetInstallerYaml(
    const std::string& yaml,
    const std::string& preferredArch);

/// Resolves the latest stable installer manifest from github.com/microsoft/winget-pkgs.
WingetManifestInfo ResolveWingetPkgManifest(
    const std::string& repoPath,
    const std::string& preferredArch);

std::pair<std::string, std::string> ParseLatestVulkanRuntimeFromJson(const std::string& json);
std::pair<std::string, std::string> ParseAmdAdrenalinFromHtml(const std::string& html);
std::pair<std::string, std::string> ParseAmdChipsetFromHtml(const std::string& html);

} // namespace hdm
