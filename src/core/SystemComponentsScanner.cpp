#include "core/SystemComponentsScanner.hpp"
#include "core/DeviceScanner.hpp"
#include "core/LocalDriverInstaller.hpp"
#include "core/SystemRestore.hpp"

#include <oleauto.h>
#include <setupapi.h>
#include <shldisp.h>
#include <softpub.h>
#include <winhttp.h>
#include <wintrust.h>
#include <winver.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace hdm {

namespace {

class WinHttpGuard {
public:
    explicit WinHttpGuard(HINTERNET h = nullptr) noexcept : handle_(h) {}
    ~WinHttpGuard() { Reset(); }

    WinHttpGuard(const WinHttpGuard&) = delete;
    WinHttpGuard& operator=(const WinHttpGuard&) = delete;

    void Reset(HINTERNET h = nullptr) noexcept {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
        handle_ = h;
    }

    [[nodiscard]] HINTERNET Get() const noexcept { return handle_; }
    [[nodiscard]] bool IsValid() const noexcept { return handle_ != nullptr; }

private:
    HINTERNET handle_{nullptr};
};

struct InstalledAppEntry {
    std::string displayName;
    std::string displayVersion;
    std::string publisher;
};

std::wstring ReadRegStr(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes == 0 || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return {};
    }
    std::wstring buf(bytes / sizeof(wchar_t) + 2, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(buf.data()), &bytes) != ERROR_SUCCESS) {
        return {};
    }
    return std::wstring(buf.c_str());
}

bool ReadRegDword(HKEY key, const wchar_t* name, DWORD& outVal) {
    DWORD type = 0;
    DWORD bytes = sizeof(DWORD);
    DWORD val = 0;
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(&val), &bytes) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        outVal = val;
        return true;
    }
    return false;
}

void CollectUninstallAppsFromRoot(
    HKEY rootKey,
    const wchar_t* subKeyPath,
    REGSAM extraFlags,
    std::vector<InstalledAppEntry>& outList) {
    RegKeyGuard baseKey;
    if (RegOpenKeyExW(rootKey, subKeyPath, 0, KEY_READ | extraFlags, baseKey.Put()) != ERROR_SUCCESS) {
        return;
    }
    for (DWORD idx = 0;; ++idx) {
        wchar_t childName[256]{};
        DWORD childLen = static_cast<DWORD>(std::size(childName));
        if (RegEnumKeyExW(baseKey.Get(), idx, childName, &childLen,
                          nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            break;
        }
        RegKeyGuard itemKey;
        if (RegOpenKeyExW(baseKey.Get(), childName, 0, KEY_READ | extraFlags, itemKey.Put()) !=
            ERROR_SUCCESS) {
            continue;
        }
        const std::wstring dispName = ReadRegStr(itemKey.Get(), L"DisplayName");
        if (dispName.empty()) {
            continue;
        }
        InstalledAppEntry entry;
        entry.displayName = WideToUtf8(dispName);
        entry.displayVersion = WideToUtf8(ReadRegStr(itemKey.Get(), L"DisplayVersion"));
        entry.publisher = WideToUtf8(ReadRegStr(itemKey.Get(), L"Publisher"));
        outList.push_back(std::move(entry));
    }
}

std::vector<InstalledAppEntry> CollectAllInstalledApps() {
    std::vector<InstalledAppEntry> apps;
    apps.reserve(256);
    const wchar_t* kUninstallPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    CollectUninstallAppsFromRoot(HKEY_LOCAL_MACHINE, kUninstallPath, KEY_WOW64_64KEY, apps);
    CollectUninstallAppsFromRoot(HKEY_LOCAL_MACHINE, kUninstallPath, KEY_WOW64_32KEY, apps);
    CollectUninstallAppsFromRoot(HKEY_CURRENT_USER, kUninstallPath, 0, apps);
    return apps;
}

std::string GetFileVersionUtf8(const std::filesystem::path& filePath) {
    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec)) {
        return {};
    }
    DWORD dummy = 0;
    const std::wstring wPath = filePath.wstring();
    const DWORD size = GetFileVersionInfoSizeW(wPath.c_str(), &dummy);
    if (size == 0) {
        return {};
    }
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(wPath.c_str(), 0, size, data.data())) {
        return {};
    }
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT ffiLen = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &ffiLen) ||
        ffi == nullptr) {
        return {};
    }
    char buf[64];
    std::snprintf(
        buf,
        sizeof(buf),
        "%u.%u.%u.%u",
        HIWORD(ffi->dwFileVersionMS),
        LOWORD(ffi->dwFileVersionMS),
        HIWORD(ffi->dwFileVersionLS),
        LOWORD(ffi->dwFileVersionLS));
    return buf;
}

std::vector<uint64_t> ParseVersionParts(const std::string& ver) {
    std::vector<uint64_t> parts;
    uint64_t current = 0;
    bool inNum = false;
    for (char ch : ver) {
        if (ch >= '0' && ch <= '9') {
            const uint64_t digit = static_cast<uint64_t>(ch - '0');
            if (current <= (std::numeric_limits<uint64_t>::max() - digit) / 10) {
                current = current * 10 + digit;
            } else {
                current = std::numeric_limits<uint64_t>::max();
            }
            inNum = true;
        } else if (inNum) {
            parts.push_back(current);
            current = 0;
            inNum = false;
        }
    }
    if (inNum) {
        parts.push_back(current);
    }
    return parts;
}

int ParseDriverDateYmd(std::string_view text) {
    uint64_t parts[3]{};
    size_t count = 0;
    uint64_t cur = 0;
    bool inNum = false;
    for (char ch : text) {
        if (ch >= '0' && ch <= '9') {
            cur = cur * 10 + static_cast<uint64_t>(ch - '0');
            if (cur > 99999) {
                return 0;
            }
            inNum = true;
        } else if ((ch == '/' || ch == '-' || ch == '.') && inNum) {
            if (count >= 3) {
                return 0;
            }
            parts[count++] = cur;
            cur = 0;
            inNum = false;
        } else if (ch != ' ' && ch != '\t') {
            return 0;
        }
    }
    if (inNum) {
        if (count >= 3) {
            return 0;
        }
        parts[count++] = cur;
    }
    if (count != 3) {
        return 0;
    }
    uint64_t y = 0;
    uint64_t m = 0;
    uint64_t d = 0;
    if (parts[0] >= 1990 && parts[0] <= 2099) {
        y = parts[0];
        m = parts[1];
        d = parts[2];
    } else if (parts[2] >= 1990 && parts[2] <= 2099) {
        m = parts[0];
        d = parts[1];
        y = parts[2];
    } else {
        return 0;
    }
    if (m < 1 || m > 12 || d < 1 || d > 31) {
        return 0;
    }
    return static_cast<int>(y * 10000 + m * 100 + d);
}

// Extracts YYYYMMDD from Realtek-style 4-part date-encoded versions (e.g., 10.79.1003.2025 or 1168.20.729.2024).
bool TryExtractTrailingVersionDate(const std::vector<uint64_t>& parts, int& outYmd) {
    if (parts.size() != 4 || parts[3] < 2015 || parts[3] > 2035) {
        return false;
    }
    const uint64_t mmdd = parts[2];
    const uint64_t m = mmdd / 100;
    const uint64_t d = mmdd % 100;
    if (m < 1 || m > 12 || d < 1 || d > 31) {
        return false;
    }
    outYmd = static_cast<int>(parts[3] * 10000 + m * 100 + d);
    return true;
}

bool IsHostWindows11() {
    static const bool sIsWin11 = []() {
        RegKeyGuard key;
        if (RegOpenKeyExW(
                HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                0,
                KEY_READ,
                key.Put()) != ERROR_SUCCESS) {
            return true;
        }
        const std::wstring buildStr = ReadRegStr(key.Get(), L"CurrentBuildNumber");
        if (buildStr.empty()) {
            return true;
        }
        const unsigned long build = wcstoul(buildStr.c_str(), nullptr, 10);
        return build >= 22000;
    }();
    return sIsWin11;
}

bool IsVersionNewer(const std::string& latest, const std::string& current) {
    if (latest.empty() || current.empty()) {
        return false;
    }
    const auto a = ParseVersionParts(latest);
    const auto b = ParseVersionParts(current);
    if (a.empty() || b.empty()) {
        return false;
    }
    int dateA = 0;
    int dateB = 0;
    if (TryExtractTrailingVersionDate(a, dateA) &&
        TryExtractTrailingVersionDate(b, dateB) &&
        dateA != dateB) {
        return dateA > dateB;
    }
    const size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const uint64_t va = i < a.size() ? a[i] : 0;
        const uint64_t vb = i < b.size() ? b[i] : 0;
        if (va != vb) {
            return va > vb;
        }
    }
    return false;
}

const InstalledAppEntry* FindInstalledApp(
    const std::vector<InstalledAppEntry>& apps,
    std::initializer_list<const char*> keywords) {
    const InstalledAppEntry* best = nullptr;
    for (const auto& app : apps) {
        bool matched = false;
        for (const char* kw : keywords) {
            if (kw != nullptr && ContainsCaseInsensitive(app.displayName, kw)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            continue;
        }
        if (best == nullptr ||
            (!app.displayVersion.empty() &&
             (best->displayVersion.empty() ||
              IsVersionNewer(app.displayVersion, best->displayVersion)))) {
            best = &app;
        }
    }
    return best;
}

bool EndsWithCaseInsensitive(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size()) {
        return false;
    }
    const size_t offset = text.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[offset + i])) !=
            std::tolower(static_cast<unsigned char>(suffix[i]))) {
            return false;
        }
    }
    return true;
}

std::string TrimCopy(std::string_view sv) {
    size_t start = 0;
    while (start < sv.size() &&
           (sv[start] == ' ' || sv[start] == '\t' || sv[start] == '\r' || sv[start] == '\n')) {
        ++start;
    }
    size_t end = sv.size();
    while (end > start &&
           (sv[end - 1] == ' ' || sv[end - 1] == '\t' || sv[end - 1] == '\r' || sv[end - 1] == '\n')) {
        --end;
    }
    std::string out(sv.substr(start, end - start));
    if (out.size() >= 2 &&
        ((out.front() == '\'' && out.back() == '\'') ||
         (out.front() == '"' && out.back() == '"'))) {
        out = out.substr(1, out.size() - 2);
    }
    return out;
}

std::string StripUrlQuery(std::string_view url) {
    const size_t q = url.find('?');
    return std::string(q == std::string_view::npos ? url : url.substr(0, q));
}

std::string EncodeWingetPath(std::string_view path) {
    std::string encoded;
    encoded.reserve(path.size() + 8);
    for (char ch : path) {
        if (ch == '+') {
            encoded += "%2B";
        } else if (ch == ' ') {
            encoded += "%20";
        } else {
            encoded.push_back(ch);
        }
    }
    return encoded;
}

std::string CheckVcRuntimeKey(const wchar_t* subKey, REGSAM wowFlags) {
    RegKeyGuard key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, KEY_READ | wowFlags, key.Put()) != ERROR_SUCCESS) {
        return {};
    }
    DWORD installed = 0;
    if (!ReadRegDword(key.Get(), L"Installed", installed) || installed == 0) {
        return {};
    }
    std::wstring ver = ReadRegStr(key.Get(), L"Version");
    if (!ver.empty() && (ver[0] == L'v' || ver[0] == L'V')) {
        ver.erase(0, 1);
    }
    if (!ver.empty()) {
        return WideToUtf8(ver);
    }
    DWORD major = 0, minor = 0, bld = 0;
    if (ReadRegDword(key.Get(), L"Major", major) &&
        ReadRegDword(key.Get(), L"Minor", minor) &&
        ReadRegDword(key.Get(), L"Bld", bld)) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%lu.%lu.%lu", major, minor, bld);
        return buf;
    }
    return "Установлен";
}

std::string FindBestVcUninstallVersion(
    const std::vector<InstalledAppEntry>& apps,
    const char* archTag) {
    std::string best;
    for (const auto& app : apps) {
        if (ContainsCaseInsensitive(app.displayName, "Microsoft Visual C++ 2015") &&
            ContainsCaseInsensitive(app.displayName, archTag) &&
            !app.displayVersion.empty()) {
            if (best.empty() || IsVersionNewer(app.displayVersion, best)) {
                best = app.displayVersion;
            }
        }
    }
    return best;
}

std::string DetectHighestDotNetDesktop(const std::string& majorPrefix) {
    wchar_t progFiles[MAX_PATH]{};
    const DWORD len = GetEnvironmentVariableW(L"ProgramFiles", progFiles, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }
    const std::filesystem::path dir =
        std::filesystem::path(progFiles) / L"dotnet" / L"shared" / L"Microsoft.WindowsDesktop.App";
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        return {};
    }
    std::string best;
    for (std::filesystem::directory_iterator it(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
        std::error_code entryEc;
        if (!it->is_directory(entryEc)) {
            continue;
        }
        const std::string ver = WideToUtf8(it->path().filename().wstring());
        if (ver.rfind(majorPrefix + ".", 0) == 0) {
            if (best.empty() || IsVersionNewer(ver, best)) {
                best = ver;
            }
        }
    }
    return best;
}

std::filesystem::path GetSystemDirPath(bool wow64) {
    wchar_t buf[MAX_PATH]{};
    const UINT len = wow64 ? GetSystemWow64DirectoryW(buf, MAX_PATH) : GetSystemDirectoryW(buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(buf);
}

std::filesystem::path GetEnvPath(const wchar_t* varName) {
    wchar_t buf[MAX_PATH]{};
    const DWORD len = GetEnvironmentVariableW(varName, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(buf);
}

std::string FormatNvidiaDriverVersion(const std::string& infDriverVer) {
    std::string digits;
    for (char ch : infDriverVer) {
        if (ch >= '0' && ch <= '9') {
            digits.push_back(ch);
        }
    }
    if (digits.size() >= 5) {
        const std::string tail = digits.substr(digits.size() - 5);
        return tail.substr(0, 3) + "." + tail.substr(3, 2);
    }
    return infDriverVer;
}

// --- WinHTTP Helpers ---

struct HttpResponse {
    bool ok{false};
    DWORD statusCode{0};
    std::string body;
    std::string locationHeader;
};

HttpResponse WinHttpRequest(
    const std::wstring& url,
    const wchar_t* method,
    bool followRedirects = true,
    size_t maxBytes = 256 * 1024,
    const wchar_t* extraHeaders = nullptr,
    std::string_view postBody = {}) {
    HttpResponse res;
    URL_COMPONENTS urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t hostBuf[256]{};
    wchar_t pathBuf[2048]{};
    urlComp.lpszHostName = hostBuf;
    urlComp.dwHostNameLength = static_cast<DWORD>(std::size(hostBuf));
    urlComp.lpszUrlPath = pathBuf;
    urlComp.dwUrlPathLength = static_cast<DWORD>(std::size(pathBuf));

    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &urlComp)) {
        return res;
    }

    WinHttpGuard session(WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) DriverLoom/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session.IsValid()) {
        return res;
    }
    WinHttpSetTimeouts(session.Get(), 3000, 3000, 3500, 6000);

    WinHttpGuard connect(WinHttpConnect(session.Get(), hostBuf, urlComp.nPort, 0));
    if (!connect.IsValid()) {
        return res;
    }

    const DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    WinHttpGuard request(WinHttpOpenRequest(
        connect.Get(),
        method,
        pathBuf,
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags));
    if (!request.IsValid()) {
        return res;
    }

    DWORD redirPolicy = followRedirects
                            ? WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS
                            : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request.Get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirPolicy, sizeof(redirPolicy));

    if (extraHeaders != nullptr && extraHeaders[0] != L'\0') {
        WinHttpAddRequestHeaders(
            request.Get(),
            extraHeaders,
            static_cast<DWORD>(-1),
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    LPVOID bodyPtr = postBody.empty()
                         ? WINHTTP_NO_REQUEST_DATA
                         : const_cast<char*>(postBody.data());
    const DWORD bodyLen = static_cast<DWORD>(postBody.size());
    if (!WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            bodyPtr, bodyLen, bodyLen, 0) ||
        !WinHttpReceiveResponse(request.Get(), nullptr)) {
        return res;
    }

    DWORD status = 0;
    DWORD statusLen = sizeof(status);
    WinHttpQueryHeaders(
        request.Get(),
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status,
        &statusLen,
        WINHTTP_NO_HEADER_INDEX);
    res.statusCode = status;
    res.ok = (status >= 200 && status < 400);

    if (!followRedirects) {
        wchar_t locBuf[2048]{};
        DWORD locBytes = sizeof(locBuf);
        if (WinHttpQueryHeaders(
                request.Get(),
                WINHTTP_QUERY_LOCATION,
                WINHTTP_HEADER_NAME_BY_INDEX,
                locBuf,
                &locBytes,
                WINHTTP_NO_HEADER_INDEX)) {
            res.locationHeader = WideToUtf8(locBuf);
        }
    }

    if (std::wstring_view(method) != L"HEAD" && res.ok) {
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(request.Get(), &avail) && avail > 0) {
            if (res.body.size() >= maxBytes) {
                break;
            }
            const size_t toRead = std::min<size_t>(avail, maxBytes - res.body.size());
            std::string chunk(toRead, '\0');
            DWORD readBytes = 0;
            if (!WinHttpReadData(request.Get(), chunk.data(), static_cast<DWORD>(toRead), &readBytes) ||
                readBytes == 0) {
                break;
            }
            chunk.resize(readBytes);
            res.body += chunk;
        }
    }
    return res;
}

std::string ExtractJsonStringField(const std::string& json, const std::string& key, size_t startPos = 0) {
    const std::string pattern = "\"" + key + "\"";
    const size_t pos = json.find(pattern, startPos);
    if (pos == std::string::npos) {
        return {};
    }
    const size_t colon = json.find(':', pos + pattern.size());
    if (colon == std::string::npos) {
        return {};
    }
    const size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) {
        return {};
    }
    const size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos || q2 <= q1 + 1) {
        return {};
    }
    return json.substr(q1 + 1, q2 - q1 - 1);
}

// Parses https://vulkan.lunarg.com/sdk/files JSON to find the latest Windows version
// that provides a standalone Runtime installer ("VulkanRT-X64-<ver>-Installer.exe").
std::pair<std::string, std::string> ParseLatestVulkanRuntimeFromJson(const std::string& json) {
    const size_t winPos = json.find("\"windows\"");
    if (winPos == std::string::npos) {
        return {};
    }
    std::string bestVer;
    std::string bestFile;

    size_t searchPos = winPos;
    while (true) {
        const size_t fnPos = json.find("\"file_name\"", searchPos);
        if (fnPos == std::string::npos) {
            break;
        }
        const std::string fileName = ExtractJsonStringField(json, "file_name", fnPos);
        searchPos = fnPos + 12;
        if (fileName.rfind("VulkanRT-", 0) != 0 ||
            fileName.find("-Installer.exe") == std::string::npos) {
            continue;
        }
        // Extract version digits from e.g. "VulkanRT-X64-1.4.363.0-Installer.exe" or "VulkanRT-1.3.296.0-Installer.exe"
        const size_t instPos = fileName.find("-Installer.exe");
        size_t verStart = fileName.rfind('-', instPos - 1);
        if (verStart == std::string::npos) {
            continue;
        }
        const std::string ver = fileName.substr(verStart + 1, instPos - verStart - 1);
        if (ver.empty() || !std::isdigit(static_cast<unsigned char>(ver[0]))) {
            continue;
        }
        if (bestVer.empty() || IsVersionNewer(ver, bestVer)) {
            bestVer = ver;
            bestFile = fileName;
        }
    }

    if (bestVer.empty() || bestFile.empty()) {
        return {};
    }
    const std::string url =
        "https://sdk.lunarg.com/sdk/download/" + bestVer + "/windows/" + bestFile;
    return {bestVer, url};
}

std::pair<std::string, std::string> ParseAmdAdrenalinFromHtml(const std::string& html) {
    // Looks for https://drivers.amd.com/drivers/installer/.../amd-software-adrenalin-edition-<ver>-minimalsetup-..._web.exe
    size_t pos = 0;
    while ((pos = html.find("https://drivers.amd.com/drivers/installer/", pos)) != std::string::npos) {
        size_t end = pos;
        while (end < html.size() && html[end] != '"' && html[end] != '\'' &&
               html[end] != '<' && html[end] != '>' && html[end] != ' ') {
            ++end;
        }
        const std::string url = html.substr(pos, end - pos);
        if (url.find(".exe") != std::string::npos) {
            std::string ver;
            const std::string marker = "adrenalin-edition-";
            const size_t vPos = url.find(marker);
            if (vPos != std::string::npos) {
                const size_t vStart = vPos + marker.size();
                const size_t vEnd = url.find('-', vStart);
                if (vEnd != std::string::npos && vEnd > vStart) {
                    ver = url.substr(vStart, vEnd - vStart);
                }
            }
            return {ver, url};
        }
        pos = end;
    }
    return {};
}

std::pair<std::string, std::string> ParseAmdChipsetFromHtml(const std::string& html) {
    const std::string prefix = "drivers.amd.com/drivers/amd_chipset_software_";
    std::string bestVer;
    size_t pos = 0;
    while ((pos = html.find(prefix, pos)) != std::string::npos) {
        const size_t vStart = pos + prefix.size();
        const size_t exePos = html.find(".exe", vStart);
        if (exePos == std::string::npos || exePos <= vStart || (exePos - vStart) > 40) {
            pos = vStart;
            continue;
        }
        const std::string ver = html.substr(vStart, exePos - vStart);
        bool valid = !ver.empty() && std::isdigit(static_cast<unsigned char>(ver.front())) != 0;
        for (char ch : ver) {
            if (!std::isdigit(static_cast<unsigned char>(ch)) && ch != '.') {
                valid = false;
                break;
            }
        }
        if (valid && (bestVer.empty() || IsVersionNewer(ver, bestVer))) {
            bestVer = ver;
        }
        pos = exePos + 4;
    }
    if (bestVer.empty()) {
        return {};
    }
    return {bestVer, "https://drivers.amd.com/drivers/amd_chipset_software_" + bestVer + ".exe"};
}

bool IsValidCatalogGuid(std::string_view s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < 36; ++i) {
        const char ch = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (ch != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

std::string UrlEncodeCatalogParam(std::string_view val) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(val.size() * 3);
    for (unsigned char c : val) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0F]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

bool IsValidCatalogVersion(std::string_view ver) {
    if (ver.empty() || std::isdigit(static_cast<unsigned char>(ver.front())) == 0 ||
        std::isdigit(static_cast<unsigned char>(ver.back())) == 0 ||
        ver.find('.') == std::string_view::npos) {
        return false;
    }
    bool prevDot = false;
    for (char ch : ver) {
        if (ch == '.') {
            if (prevDot) return false;
            prevDot = true;
        } else if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
            prevDot = false;
        } else {
            return false;
        }
    }
    return true;
}

std::string ExtractVersionFromCatalogRowChunk(std::string_view rowChunk) {
    const size_t linkPos = rowChunk.rfind("_link'");
    const size_t linkAltPos = rowChunk.rfind("_link\"");
    size_t anchor = std::string_view::npos;
    if (linkPos != std::string_view::npos && linkAltPos != std::string_view::npos) {
        anchor = std::max(linkPos, linkAltPos);
    } else {
        anchor = (linkPos != std::string_view::npos) ? linkPos : linkAltPos;
    }
    if (anchor == std::string_view::npos) {
        return {};
    }
    const size_t gt = rowChunk.find('>', anchor);
    const size_t endA = (gt != std::string_view::npos) ? rowChunk.find("</a>", gt + 1) : std::string_view::npos;
    if (gt == std::string_view::npos || endA == std::string_view::npos) {
        return {};
    }
    const std::string_view title = rowChunk.substr(gt + 1, endA - gt - 1);

    std::string lastVer;
    size_t i = 0;
    while (i < title.size()) {
        if (std::isdigit(static_cast<unsigned char>(title[i])) == 0) {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < title.size() &&
               (std::isdigit(static_cast<unsigned char>(title[i])) != 0 || title[i] == '.')) {
            ++i;
        }
        size_t end = i;
        while (end > start && title[end - 1] == '.') {
            --end;
        }
        const std::string_view cand = title.substr(start, end - start);
        if (IsValidCatalogVersion(cand)) {
            lastVer.assign(cand);
        }
    }
    return lastVer;
}

std::string ExtractVenDevSubsysKey(std::string_view hwId) {
    if (hwId.empty()) {
        return {};
    }
    std::string upper(hwId);
    for (char& ch : upper) {
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
    }
    auto extractHex = [&upper](std::string_view tag, size_t len) -> std::string {
        const size_t p = upper.find(tag);
        if (p == std::string::npos || p + tag.size() + len > upper.size()) {
            return {};
        }
        for (size_t i = 0; i < len; ++i) {
            if (std::isxdigit(static_cast<unsigned char>(upper[p + tag.size() + i])) == 0) {
                return {};
            }
        }
        return upper.substr(p + tag.size(), len);
    };
    const std::string ven = extractHex("VEN_", 4);
    const std::string dev = extractHex("DEV_", 4);
    const std::string subsys = extractHex("SUBSYS_", 8);
    if (ven.empty() || dev.empty() || subsys.empty()) {
        return {};
    }
    return "VEN_" + ven + "&DEV_" + dev + "&SUBSYS_" + subsys;
}

struct CatalogDriverMatch {
    std::string version;
    std::string cabUrl;
    int dateYmd{0};
};

int ExtractCatalogDateFromRowChunk(std::string_view rowChunk) {
    const size_t c4 = rowChunk.rfind("_C4_R");
    if (c4 == std::string_view::npos) {
        return 0;
    }
    const size_t gt = rowChunk.find('>', c4 + 5);
    if (gt == std::string_view::npos) {
        return 0;
    }
    const size_t endTd = rowChunk.find("</td>", gt + 1);
    if (endTd == std::string_view::npos || endTd - gt > 64) {
        return 0;
    }
    return ParseDriverDateYmd(TrimCopy(rowChunk.substr(gt + 1, endTd - gt - 1)));
}

// Queries Microsoft Update Catalog for a Hardware ID fragment (e.g. VEN_10EC&DEV_8125),
// selects the newest WHQL release (preferring OS-compatible targets and newer dates across branches),
// and fetches its .cab URL.
CatalogDriverMatch ResolveMsCatalogDriverCab(const std::string& searchKey) {
    if (searchKey.empty()) {
        return {};
    }
    const std::wstring searchUrl = Utf8ToWide(
        "https://www.catalog.update.microsoft.com/Search.aspx?q=" +
        UrlEncodeCatalogParam(searchKey));
    const HttpResponse searchResp = WinHttpRequest(searchUrl, L"GET", true, 512 * 1024);
    if (!searchResp.ok || searchResp.body.empty()) {
        return {};
    }

    const std::string& html = searchResp.body;
    const bool hostIsWin11 = IsHostWindows11();
    std::string bestGuid;
    std::string bestVer;
    int bestDateYmd = 0;
    bool bestHasWin11 = false;
    bool bestHasWin10 = false;

    size_t pos = 0;
    while ((pos = html.find("_C5_R", pos)) != std::string::npos) {
        if (pos < 36) {
            pos += 5;
            continue;
        }
        const std::string_view guidCandidate(html.data() + pos - 36, 36);
        if (!IsValidCatalogGuid(guidCandidate)) {
            pos += 5;
            continue;
        }

        const size_t tdClose = html.find('>', pos + 5);
        const size_t endTd = (tdClose != std::string::npos) ? html.find("</td>", tdClose + 1) : std::string_view::npos;
        if (tdClose == std::string::npos || endTd == std::string::npos || endTd - tdClose > 80) {
            pos += 5;
            continue;
        }

        const size_t winStart = (pos > 3000) ? (pos - 3000) : 0;
        std::string_view rowChunk(html.data() + winStart, pos - winStart);
        if (const size_t trPos = rowChunk.rfind("<tr"); trPos != std::string_view::npos) {
            rowChunk = rowChunk.substr(trPos);
        }
        if (rowChunk.find(" - Extension - ") != std::string_view::npos ||
            rowChunk.find(" - SoftwareComponent - ") != std::string_view::npos ||
            rowChunk.find(" - Firmware - ") != std::string_view::npos) {
            pos = endTd + 5;
            continue;
        }

        const bool hasWin11 = (rowChunk.find("Windows 11") != std::string_view::npos);
        const bool hasWin10 = (rowChunk.find("Windows 10") != std::string_view::npos);
        if (!hostIsWin11 && hasWin11 && !hasWin10) {
            pos = endTd + 5;
            continue;
        }

        std::string ver = TrimCopy(std::string_view(html.data() + tdClose + 1, endTd - tdClose - 1));
        if (!IsValidCatalogVersion(ver)) {
            ver = ExtractVersionFromCatalogRowChunk(rowChunk);
        }

        if (IsValidCatalogVersion(ver)) {
            int dateYmd = ExtractCatalogDateFromRowChunk(rowChunk);
            const auto candParts = ParseVersionParts(ver);
            if (dateYmd == 0) {
                TryExtractTrailingVersionDate(candParts, dateYmd);
            }
            const auto bestParts = ParseVersionParts(bestVer);

            bool isBetter = false;
            if (bestVer.empty()) {
                isBetter = true;
            } else if (!candParts.empty() && !bestParts.empty() &&
                       candParts[0] != bestParts[0] &&
                       dateYmd > 0 && bestDateYmd > 0) {
                if (dateYmd != bestDateYmd) {
                    isBetter = (dateYmd > bestDateYmd);
                } else if (hostIsWin11 ? (hasWin11 != bestHasWin11) : (hasWin10 != bestHasWin10)) {
                    isBetter = hostIsWin11 ? hasWin11 : hasWin10;
                } else {
                    isBetter = IsVersionNewer(ver, bestVer);
                }
            } else {
                const bool strictlyNewer = IsVersionNewer(ver, bestVer);
                const bool sameVer = (!strictlyNewer && !IsVersionNewer(bestVer, ver));
                const bool osBetter = hostIsWin11
                    ? (!bestHasWin11 && hasWin11)
                    : (!bestHasWin10 && hasWin10);
                isBetter = strictlyNewer || (sameVer && osBetter);
            }

            if (isBetter) {
                bestVer = std::move(ver);
                bestGuid.assign(guidCandidate);
                bestDateYmd = dateYmd;
                bestHasWin11 = hasWin11;
                bestHasWin10 = hasWin10;
            }
        }
        pos = endTd + 5;
    }

    if (bestGuid.empty() || bestVer.empty()) {
        return {};
    }

    const std::string postBody =
        "updateIDs=" +
        UrlEncodeCatalogParam("[{\"size\":0,\"languages\":\"\",\"uidInfo\":\"" + bestGuid +
                              "\",\"updateID\":\"" + bestGuid + "\"}]");
    const HttpResponse dlResp = WinHttpRequest(
        L"https://www.catalog.update.microsoft.com/DownloadDialog.aspx",
        L"POST",
        true,
        128 * 1024,
        L"Content-Type: application/x-www-form-urlencoded\r\n",
        postBody);
    if (!dlResp.ok || dlResp.body.empty()) {
        return {bestVer, "", bestDateYmd};
    }

    size_t cabPos = 0;
    while ((cabPos = dlResp.body.find(".cab", cabPos)) != std::string::npos) {
        size_t urlStart = cabPos;
        while (urlStart > 0 && dlResp.body[urlStart - 1] != '\'' && dlResp.body[urlStart - 1] != '"' &&
               dlResp.body[urlStart - 1] != ' ' && dlResp.body[urlStart - 1] != '>') {
            --urlStart;
        }
        const std::string cabUrl = dlResp.body.substr(urlStart, (cabPos + 4) - urlStart);
        if (cabUrl.rfind("http://", 0) == 0 || cabUrl.rfind("https://", 0) == 0) {
            return {bestVer, cabUrl, bestDateYmd};
        }
        cabPos += 4;
    }

    return {bestVer, "", bestDateYmd};
}

UINT CALLBACK CabExtractCallback(PVOID ctx, UINT notification, UINT_PTR param1, UINT_PTR param2) {
    (void)param2;
    if (notification == SPFILENOTIFY_FILEINCABINET) {
        auto* info = reinterpret_cast<FILE_IN_CABINET_INFO_W*>(param1);
        const auto* baseDir = static_cast<const std::filesystem::path*>(ctx);
        if (info == nullptr || info->NameInCabinet == nullptr || baseDir == nullptr) {
            return FILEOP_ABORT;
        }
        const std::filesystem::path rel = std::filesystem::path(info->NameInCabinet).lexically_normal();
        if (rel.empty() || rel == L"." || rel.is_absolute() || rel.has_root_name() ||
            rel.has_root_directory()) {
            SetLastError(ERROR_ACCESS_DENIED);
            return FILEOP_ABORT;
        }
        for (const auto& part : rel) {
            if (part == L"..") {
                SetLastError(ERROR_ACCESS_DENIED);
                return FILEOP_ABORT;
            }
        }
        const std::filesystem::path dest = (*baseDir / rel).lexically_normal();
        const std::wstring destW = dest.wstring();
        if (destW.size() >= MAX_PATH) {
            SetLastError(ERROR_FILENAME_EXCED_RANGE);
            return FILEOP_ABORT;
        }
        std::error_code ec;
        std::filesystem::create_directories(dest.parent_path(), ec);
        if (ec) {
            return FILEOP_ABORT;
        }
        wcsncpy_s(info->FullTargetName, MAX_PATH, destW.c_str(), _TRUNCATE);
        return FILEOP_DOIT;
    }
    return NO_ERROR;
}

struct WingetManifestInfo {
    bool ok{false};
    std::string version;
    std::string url;
    std::string nestedRelativePath;
    std::string silentArgs;
};

// Parses GitHub's non-API tree JSON ({"payload":{"codeViewTreeRoute":{"tree":{"items":[...]}}}})
// so version lookup does not consume GitHub REST API's 60 req/hr quota.
std::vector<std::string> ExtractGithubTreeNames(
    const std::string& json,
    const std::string& wantedContentType) {
    std::vector<std::string> names;
    const size_t treePos = json.find("\"tree\":{\"items\":[");
    if (treePos == std::string::npos) {
        return names;
    }
    const size_t arrStart = json.find('[', treePos);
    if (arrStart == std::string::npos) {
        return names;
    }

    int depth = 0;
    bool inString = false;
    bool escape = false;
    size_t arrEnd = json.size();
    for (size_t i = arrStart; i < json.size(); ++i) {
        const char ch = json[i];
        if (escape) {
            escape = false;
            continue;
        }
        if (inString) {
            if (ch == '\\') escape = true;
            else if (ch == '"') inString = false;
            continue;
        }
        if (ch == '"') {
            inString = true;
        } else if (ch == '[') {
            ++depth;
        } else if (ch == ']') {
            --depth;
            if (depth == 0) {
                arrEnd = i;
                break;
            }
        }
    }

    size_t pos = arrStart;
    while (pos < arrEnd) {
        const size_t namePos = json.find("\"name\":\"", pos);
        if (namePos == std::string::npos || namePos >= arrEnd) {
            break;
        }
        const std::string name = ExtractJsonStringField(json, "name", namePos);
        const size_t objEnd = json.find('}', namePos);
        if (objEnd == std::string::npos || objEnd > arrEnd) {
            break;
        }
        const size_t ctPos = json.find("\"contentType\"", namePos);
        std::string contentType;
        if (ctPos != std::string::npos && ctPos < objEnd) {
            contentType = ExtractJsonStringField(json, "contentType", ctPos);
        }
        if (!name.empty() && contentType == wantedContentType) {
            names.push_back(name);
        }
        pos = objEnd + 1;
    }
    return names;
}

WingetManifestInfo ParseWingetInstallerYaml(
    const std::string& yaml,
    const std::string& preferredArch) {
    WingetManifestInfo info;
    struct Entry {
        std::string arch;
        std::string url;
        std::string installerType;
        std::string nestedPath;
        std::string silent;
    };
    std::vector<Entry> entries;
    Entry current;
    bool inInstallers = false;
    bool inNestedFiles = false;
    std::string topType;
    std::string topNestedPath;
    std::string topSilent;
    std::string topSilentProgress;

    std::istringstream iss(yaml);
    std::string rawLine;
    while (std::getline(iss, rawLine)) {
        const std::string trimmed = TrimCopy(rawLine);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        if (trimmed == "Installers:") {
            inInstallers = true;
            inNestedFiles = false;
            continue;
        }
        if (!rawLine.empty() && rawLine[0] != ' ' && rawLine[0] != '-' &&
            trimmed != "NestedInstallerFiles:") {
            inNestedFiles = false;
            if (trimmed.rfind("ManifestType:", 0) == 0 ||
                trimmed.rfind("ManifestVersion:", 0) == 0) {
                if (inInstallers && !current.url.empty()) {
                    entries.push_back(current);
                    current = Entry{};
                }
                inInstallers = false;
            }
        }
        if (trimmed == "NestedInstallerFiles:") {
            inNestedFiles = true;
            continue;
        }

        std::string keyPart = trimmed;
        bool isListItem = false;
        if (keyPart.rfind("- ", 0) == 0) {
            isListItem = true;
            keyPart = TrimCopy(keyPart.substr(2));
        }

        const size_t colon = keyPart.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string key = TrimCopy(keyPart.substr(0, colon));
        const std::string val = TrimCopy(keyPart.substr(colon + 1));

        if (!inInstallers) {
            if (key == "PackageVersion" && !val.empty()) {
                info.version = val;
            } else if (key == "InstallerType" && !val.empty()) {
                topType = ToLowerAscii(val);
            } else if (key == "RelativeFilePath" && inNestedFiles && !val.empty() &&
                       topNestedPath.empty()) {
                topNestedPath = val;
            } else if (key == "SilentWithProgress" && !val.empty()) {
                topSilentProgress = val;
            } else if (key == "Silent" && !val.empty()) {
                topSilent = val;
            }
            continue;
        }

        if (isListItem &&
            (key == "Architecture" || key == "InstallerUrl" || key == "InstallerLocale")) {
            if (!current.url.empty()) {
                entries.push_back(current);
                current = Entry{};
            }
        }

        if (key == "Architecture") {
            current.arch = ToLowerAscii(val);
        } else if (key == "InstallerUrl" && !val.empty()) {
            current.url = val;
        } else if (key == "InstallerType" && !val.empty()) {
            current.installerType = ToLowerAscii(val);
        } else if (key == "RelativeFilePath" && !val.empty() && current.nestedPath.empty()) {
            current.nestedPath = val;
        } else if (key == "SilentWithProgress" && !val.empty()) {
            current.silent = val;
        } else if (key == "Silent" && !val.empty() && current.silent.empty()) {
            current.silent = val;
        }
    }
    if (inInstallers && !current.url.empty()) {
        entries.push_back(current);
    }

    if (entries.empty()) {
        return info;
    }

    auto isSupportedEntry = [&](const Entry& e) {
        const std::string t = e.installerType.empty() ? topType : e.installerType;
        return t != "msix" && t != "appx";
    };

    const Entry* chosen = nullptr;
    const std::string pref = ToLowerAscii(preferredArch);
    if (!pref.empty()) {
        for (const auto& e : entries) {
            if (e.arch == pref && isSupportedEntry(e)) {
                chosen = &e;
                break;
            }
        }
    }
    if (chosen == nullptr) {
        for (const char* archOrder : {"x64", "neutral", "x86"}) {
            for (const auto& e : entries) {
                if (e.arch == archOrder && isSupportedEntry(e)) {
                    chosen = &e;
                    break;
                }
            }
            if (chosen != nullptr) break;
        }
    }
    if (chosen == nullptr) {
        for (const auto& e : entries) {
            if (isSupportedEntry(e)) {
                chosen = &e;
                break;
            }
        }
    }
    if (chosen == nullptr) {
        return info;
    }

    info.ok = !info.version.empty() && !chosen->url.empty();
    info.url = chosen->url;
    info.nestedRelativePath = !chosen->nestedPath.empty() ? chosen->nestedPath : topNestedPath;
    if (!chosen->silent.empty()) {
        info.silentArgs = chosen->silent;
    } else if (!topSilentProgress.empty()) {
        info.silentArgs = topSilentProgress;
    } else {
        info.silentArgs = topSilent;
    }
    return info;
}

WingetManifestInfo ResolveWingetPkgManifest(
    const std::string& repoPath,
    const std::string& preferredArch) {
    WingetManifestInfo result;
    if (repoPath.empty()) {
        return result;
    }
    const std::string encPath = EncodeWingetPath(repoPath);
    const std::wstring treeUrl = Utf8ToWide(
        "https://github.com/microsoft/winget-pkgs/tree/master/manifests/" + encPath);
    const HttpResponse treeResp = WinHttpRequest(
        treeUrl, L"GET", true, 256 * 1024, L"Accept: application/json\r\n");
    if (!treeResp.ok || treeResp.body.empty()) {
        return result;
    }

    const std::vector<std::string> dirs = ExtractGithubTreeNames(treeResp.body, "directory");
    std::string bestVer;
    for (const auto& d : dirs) {
        if (!d.empty() && std::isdigit(static_cast<unsigned char>(d[0]))) {
            if (bestVer.empty() || IsVersionNewer(d, bestVer)) {
                bestVer = d;
            }
        }
    }
    if (bestVer.empty()) {
        return result;
    }

    const std::wstring verTreeUrl = Utf8ToWide(
        "https://github.com/microsoft/winget-pkgs/tree/master/manifests/" +
        encPath + "/" + EncodeWingetPath(bestVer));
    const HttpResponse verResp = WinHttpRequest(
        verTreeUrl, L"GET", true, 128 * 1024, L"Accept: application/json\r\n");
    if (!verResp.ok || verResp.body.empty()) {
        return result;
    }

    const std::vector<std::string> files = ExtractGithubTreeNames(verResp.body, "file");
    std::string installerYaml;
    for (const auto& f : files) {
        if (EndsWithCaseInsensitive(f, ".installer.yaml")) {
            installerYaml = f;
            break;
        }
    }
    if (installerYaml.empty()) {
        return result;
    }

    const std::wstring rawUrl = Utf8ToWide(
        "https://raw.githubusercontent.com/microsoft/winget-pkgs/master/manifests/" +
        encPath + "/" + EncodeWingetPath(bestVer) + "/" + EncodeWingetPath(installerYaml));
    const HttpResponse yamlResp = WinHttpRequest(rawUrl, L"GET", true, 128 * 1024);
    if (!yamlResp.ok || yamlResp.body.empty()) {
        return result;
    }

    result = ParseWingetInstallerYaml(yamlResp.body, preferredArch);
    if (result.version.empty()) {
        result.version = bestVer;
    }
    return result;
}

void ApplyResolvedVersionToItem(
    SystemComponentItem& item,
    const std::string& latestVer,
    const std::string& missingPrefix = "Не установлен") {
    item.onlineChecked = true;
    if (latestVer.empty()) {
        return;
    }
    item.latestVersion = latestVer;
    if (!item.installed) {
        item.updateAvailable = true;
        item.statusText = missingPrefix + " (" + latestVer + ")";
        return;
    }
    if (!item.compareVersions || item.installedVersion.empty() ||
        !std::isdigit(static_cast<unsigned char>(latestVer[0]))) {
        return;
    }
    if (IsVersionNewer(latestVer, item.installedVersion)) {
        item.updateAvailable = true;
        item.statusText = "Доступно обновление (" + latestVer + ")";
    } else {
        item.updateAvailable = false;
        item.statusText = "Актуален (" + item.installedVersion + ")";
    }
}

bool HasZipMagicHeader(const std::filesystem::path& filePath) {
    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs.is_open()) {
        return false;
    }
    unsigned char hdr[4]{};
    ifs.read(reinterpret_cast<char*>(hdr), 4);
    return ifs.gcount() == 4 && hdr[0] == 'P' && hdr[1] == 'K' && hdr[2] == 0x03 && hdr[3] == 0x04;
}

bool DownloadFileViaWinHttp(
    const std::wstring& url,
    const std::filesystem::path& destPath,
    std::atomic<float>* progress,
    std::string* outError,
    float baseProgress = 0.0f,
    float progressSpan = 1.0f,
    const std::atomic<bool>* cancelFlag = nullptr) {
    URL_COMPONENTS urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t hostBuf[256]{};
    wchar_t pathBuf[2048]{};
    urlComp.lpszHostName = hostBuf;
    urlComp.dwHostNameLength = static_cast<DWORD>(std::size(hostBuf));
    urlComp.lpszUrlPath = pathBuf;
    urlComp.dwUrlPathLength = static_cast<DWORD>(std::size(pathBuf));

    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &urlComp)) {
        if (outError) *outError = "Некорректный URL загрузки.";
        return false;
    }

    WinHttpGuard session(WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) DriverLoom/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session.IsValid()) {
        if (outError) *outError = "Ошибка WinHttpOpen: " + FormatWin32Error(GetLastError());
        return false;
    }
    WinHttpSetTimeouts(session.Get(), 5000, 5000, 10000, 60000);

    WinHttpGuard connect(WinHttpConnect(session.Get(), hostBuf, urlComp.nPort, 0));
    if (!connect.IsValid()) {
        if (outError) *outError = "Ошибка WinHttpConnect: " + FormatWin32Error(GetLastError());
        return false;
    }

    const DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    WinHttpGuard request(WinHttpOpenRequest(
        connect.Get(),
        L"GET",
        pathBuf,
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags));
    if (!request.IsValid()) {
        if (outError) *outError = "Ошибка WinHttpOpenRequest: " + FormatWin32Error(GetLastError());
        return false;
    }

    DWORD redirPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request.Get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirPolicy, sizeof(redirPolicy));

    if (std::wstring_view(hostBuf).find(L"amd.com") != std::wstring_view::npos) {
        WinHttpAddRequestHeaders(
            request.Get(),
            L"Referer: https://www.amd.com/\r\n",
            static_cast<DWORD>(-1),
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    if (!WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.Get(), nullptr)) {
        if (outError) *outError = "Ошибка HTTP-запроса: " + FormatWin32Error(GetLastError());
        return false;
    }

    DWORD status = 0;
    DWORD statusLen = sizeof(status);
    WinHttpQueryHeaders(
        request.Get(),
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status,
        &statusLen,
        WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300) {
        if (outError) *outError = "Сервер вернул HTTP статус " + std::to_string(status);
        return false;
    }

    ULONGLONG totalSize = 0;
    wchar_t lenBuf[64]{};
    DWORD lenBufBytes = sizeof(lenBuf);
    if (WinHttpQueryHeaders(
            request.Get(),
            WINHTTP_QUERY_CONTENT_LENGTH,
            WINHTTP_HEADER_NAME_BY_INDEX,
            lenBuf,
            &lenBufBytes,
            WINHTTP_NO_HEADER_INDEX)) {
        totalSize = _wcstoui64(lenBuf, nullptr, 10);
    }

    std::ofstream ofs(destPath, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        if (outError) *outError = "Не удалось создать временный файл для установки.";
        return false;
    }

    bool ioError = false;
    std::string ioErrMsg;
    ULONGLONG downloaded = 0;
    DWORD avail = 0;
    for (;;) {
        if (cancelFlag != nullptr && cancelFlag->load()) {
            ofs.close();
            std::error_code rmEc;
            std::filesystem::remove(destPath, rmEc);
            if (outError) *outError = "Отменено пользователем.";
            return false;
        }
        avail = 0;
        if (!WinHttpQueryDataAvailable(request.Get(), &avail)) {
            ioError = true;
            ioErrMsg = "Ошибка чтения ответа HTTP: " + FormatWin32Error(GetLastError());
            break;
        }
        if (avail == 0) {
            break;
        }
        std::vector<char> buf(std::min<DWORD>(avail, 65536));
        DWORD readBytes = 0;
        if (!WinHttpReadData(request.Get(), buf.data(), static_cast<DWORD>(buf.size()), &readBytes)) {
            ioError = true;
            ioErrMsg = "Ошибка загрузки данных: " + FormatWin32Error(GetLastError());
            break;
        }
        if (readBytes == 0) {
            break;
        }
        if (!ofs.write(buf.data(), static_cast<std::streamsize>(readBytes))) {
            ioError = true;
            ioErrMsg = "Ошибка записи временного файла на диск.";
            break;
        }
        downloaded += readBytes;
        if (progress != nullptr && totalSize > 0) {
            const float localFrac = std::clamp(
                static_cast<float>(downloaded) / static_cast<float>(totalSize), 0.0f, 0.90f);
            progress->store(std::clamp(baseProgress + localFrac * progressSpan, 0.0f, 0.99f));
        }
    }
    ofs.flush();
    if (!ofs.good() && !ioError) {
        ioError = true;
        ioErrMsg = "Ошибка завершения записи временного файла.";
    }
    ofs.close();

    if (!ioError && downloaded == 0) {
        ioError = true;
        ioErrMsg = "Сервер вернул пустой файл.";
    } else if (!ioError && totalSize > 0 && downloaded < totalSize) {
        ioError = true;
        ioErrMsg = "Загрузка прервалась неполной (" + std::to_string(downloaded) + " из " +
                   std::to_string(totalSize) + " байт).";
    }
    if (ioError) {
        std::error_code rmEc;
        std::filesystem::remove(destPath, rmEc);
        if (outError) *outError = ioErrMsg;
        return false;
    }
    return true;
}

bool ExtractZipViaShellCom(
    const std::filesystem::path& zipPath,
    const std::filesystem::path& destDir) {
    std::error_code ec;
    std::filesystem::create_directories(destDir, ec);
    if (ec) {
        return false;
    }

    CoInitGuard comInit(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (!comInit.Succeeded()) {
        return false;
    }

    ComPtr<IShellDispatch> shell;
    HRESULT hr = CoCreateInstance(
        CLSID_Shell,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IShellDispatch,
        reinterpret_cast<void**>(shell.Put()));
    if (FAILED(hr) || !shell) {
        return false;
    }

    BstrGuard zipBstr(zipPath.wstring().c_str());
    BstrGuard dstBstr(destDir.wstring().c_str());

    VARIANT vZip;
    VariantInit(&vZip);
    vZip.vt = VT_BSTR;
    vZip.bstrVal = zipBstr.Get();

    VARIANT vDst;
    VariantInit(&vDst);
    vDst.vt = VT_BSTR;
    vDst.bstrVal = dstBstr.Get();

    ComPtr<Folder> zipFolder;
    ComPtr<Folder> dstFolder;
    if (FAILED(shell->NameSpace(vZip, zipFolder.Put())) || !zipFolder ||
        FAILED(shell->NameSpace(vDst, dstFolder.Put())) || !dstFolder) {
        return false;
    }

    ComPtr<FolderItems> items;
    if (FAILED(zipFolder->Items(items.Put())) || !items) {
        return false;
    }
    long itemCount = 0;
    if (FAILED(items->get_Count(&itemCount)) || itemCount <= 0) {
        return false;
    }

    VARIANT vItems;
    VariantInit(&vItems);
    vItems.vt = VT_DISPATCH;
    vItems.pdispVal = items.Get();

    VARIANT vOpt;
    VariantInit(&vOpt);
    vOpt.vt = VT_I4;
    vOpt.lVal = 0x0614; // FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_NOCONFIRMMKDIR

    hr = dstFolder->CopyHere(vItems, vOpt);
    return SUCCEEDED(hr);
}

bool VerifyAuthenticodeSignature(const std::filesystem::path& filePath, std::string* outReason) {
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    const std::wstring wPath = filePath.wstring();
    fileInfo.pcwszFilePath = wPath.c_str();

    WINTRUST_DATA trustData{};
    trustData.cbStruct = sizeof(trustData);
    trustData.dwUIChoice = WTD_UI_NONE;
    trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
    trustData.dwUnionChoice = WTD_CHOICE_FILE;
    trustData.pFile = &fileInfo;
    trustData.dwStateAction = WTD_STATEACTION_VERIFY;
    trustData.dwProvFlags = WTD_SAFER_FLAG | WTD_REVOCATION_CHECK_NONE;

    GUID policyGuid = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    struct WinTrustStateGuard {
        GUID policy{};
        WINTRUST_DATA& data;
        bool active{false};
        ~WinTrustStateGuard() {
            if (active && data.hWVTStateData != nullptr) {
                data.dwStateAction = WTD_STATEACTION_CLOSE;
                WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &policy, &data);
            }
        }
    } stateGuard{policyGuid, trustData, true};

    const LONG status = WinVerifyTrust(
        static_cast<HWND>(INVALID_HANDLE_VALUE), &policyGuid, &trustData);

    if (status == ERROR_SUCCESS) {
        return true;
    }
    if (outReason != nullptr) {
        *outReason = FormatWin32Error(static_cast<DWORD>(status));
    }
    return false;
}

bool LaunchAndWaitProcess(
    const std::filesystem::path& exePath,
    const std::wstring& args,
    DWORD* outExitCode,
    DWORD* outLaunchErr,
    const std::atomic<bool>* cancelFlag = nullptr,
    const std::atomic<bool>* stopFlag = nullptr) {
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = IsProcessElevated() ? L"open" : L"runas";
    const std::wstring wFile = exePath.wstring();
    sei.lpFile = wFile.c_str();
    sei.lpParameters = args.empty() ? nullptr : args.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        if (outLaunchErr) *outLaunchErr = GetLastError();
        return false;
    }
    if (sei.hProcess != nullptr) {
        HandleGuard proc(sei.hProcess);
        for (;;) {
            const DWORD wait = WaitForSingleObject(proc.Get(), 200);
            if (wait == WAIT_OBJECT_0) {
                break;
            }
            if ((stopFlag != nullptr && stopFlag->load()) ||
                (cancelFlag != nullptr && cancelFlag->load())) {
                if (outLaunchErr) *outLaunchErr = ERROR_CANCELLED;
                return false;
            }
            if (wait != WAIT_TIMEOUT) {
                if (outLaunchErr) *outLaunchErr = GetLastError();
                return false;
            }
        }
        DWORD code = 0;
        GetExitCodeProcess(proc.Get(), &code);
        if (outExitCode) *outExitCode = code;
    }
    return true;
}

void MergeOnlineMetadataIntoRefreshed(
    std::vector<SystemComponentItem>& refreshed,
    const std::vector<SystemComponentItem>& previous) {
    for (auto& newItem : refreshed) {
        for (const auto& oldItem : previous) {
            if (newItem.id != oldItem.id) {
                continue;
            }
            if (oldItem.onlineChecked) {
                newItem.onlineChecked = true;
                if (!oldItem.latestVersion.empty()) {
                    newItem.latestVersion = oldItem.latestVersion;
                }
                if (oldItem.latestDateYmd > 0) {
                    newItem.latestDateYmd = oldItem.latestDateYmd;
                }
                if (!oldItem.directInstallerUrl.empty()) {
                    newItem.directInstallerUrl = oldItem.directInstallerUrl;
                }
                if (!oldItem.installerArgs.empty()) {
                    newItem.installerArgs = oldItem.installerArgs;
                }
                if (!oldItem.nestedInstallerHint.empty()) {
                    newItem.nestedInstallerHint = oldItem.nestedInstallerHint;
                }
            }
            if (!newItem.installed) {
                newItem.updateAvailable = true;
                if (!newItem.latestVersion.empty() &&
                    std::isdigit(static_cast<unsigned char>(newItem.latestVersion[0]))) {
                    newItem.statusText = "Не установлен (" + newItem.latestVersion + ")";
                }
            } else if (newItem.compareVersions &&
                       !newItem.latestVersion.empty() &&
                       std::isdigit(static_cast<unsigned char>(newItem.latestVersion[0])) &&
                       !newItem.installedVersion.empty()) {
                const bool catalogIsOlderByDate =
                    (newItem.latestDateYmd > 0 && newItem.installedDateYmd > 0 &&
                     newItem.latestDateYmd < newItem.installedDateYmd);
                if (!catalogIsOlderByDate &&
                    IsVersionNewer(newItem.latestVersion, newItem.installedVersion)) {
                    newItem.updateAvailable = true;
                    newItem.statusText = "Доступно обновление (" + newItem.latestVersion + ")";
                } else {
                    newItem.updateAvailable = false;
                    newItem.statusText = "Актуален (" + newItem.installedVersion + ")";
                }
            } else {
                // Installed and no numeric newer version known -> mark up-to-date
                if (newItem.category != ComponentCategory::MonitorAndGpu || !newItem.updateAvailable) {
                    newItem.updateAvailable = false;
                }
            }
            break;
        }
    }
}

struct PeripheralVendorRule {
    const char* id;
    const char* brand;
    const char* softwareName;
    const char* purpose;
    const char* pageUrl;
    const char* directUrl;
    const char* wingetPath;
    const char* nestedHint;
    const char* silentArgs;
    const char* defaultLatestLabel;
    const char* uninstallKeywords[3];
};

constexpr PeripheralVendorRule kPeripheralRules[] = {
    {"sw_razer", "Razer", "Razer Synapse (Официальное ПО)",
     "Настройка мышей, клавиатур, гарнитур, веб-камер и микрофонов Razer: DPI, частота опроса, макросы, подсветка Chroma и обновление прошивки.",
     "https://www.razer.com/synapse-4",
     "https://rzr.to/synapse-4-pc-download", nullptr, nullptr, "", "Synapse 4 (онлайн)",
     {"Razer Synapse", "Razer Central", "Razer App Engine"}},
    {"sw_logitech", "Logitech", "Logitech G HUB",
     "Управление периферией Logitech G: настройка сенсора HERO, переназначение кнопок, игровые профили, эквалайзер гарнитур и обновление прошивки.",
     "https://www.logitechg.com/en-us/innovation/g-hub.html",
     "https://download01.logi.com/web/ftp/pub/techsupport/gaming/lghub_installer.exe",
     "l/Logitech/GHUB", nullptr, "--silent", "Поиск...",
     {"Logitech G HUB", "Logitech Gaming Software", "LGHUB"}},
    {"sw_steelseries", "SteelSeries", "SteelSeries GG",
     "Официальный пакет SteelSeries: пространственный звук Sonar (игры/чат/микрофон), настройка клавиатур/мышей, OLED-дисплеев и прошивки.",
     "https://steelseries.com/gg",
     "https://steelseries.com/gg/downloads/gg/latest/windows",
     "s/SteelSeries/GG", nullptr, "/S", "Поиск...",
     {"SteelSeries GG", "SteelSeries Engine", nullptr}},
    {"sw_hyperx", "HyperX", "HyperX NGENUITY",
     "Официальное ПО для периферии HyperX: настройка DPI мышей, эквалайзера/микрофона гарнитур, макросов и подсветки.",
     "https://hyperx.com/pages/ngenuity",
     nullptr, nullptr, nullptr, "", "Microsoft Store / сайт",
     {"HyperX NGENUITY", "NGENUITY", nullptr}},
    {"sw_corsair", "Corsair", "Corsair iCUE",
     "Центр управления устройствами Corsair: профили клавиатур и мышей, синхронизация подсветки, мониторинг и обновление прошивки.",
     "https://www.corsair.com/us/en/s/icue",
     "https://www3.corsair.com/software/CUE_V5/public/modules/windows/installer/Install%20iCUE.exe",
     "c/Corsair/iCUE/5", nullptr, "--quiet", "Поиск...",
     {"CORSAIR iCUE", "Corsair Utility Engine", nullptr}},
    {"sw_asus", "ASUS ROG", "ASUS Armoury Crate",
     "Фирменный софт ASUS для материнских плат и периферии: подсветка Aura Sync, управление вентиляторами и профили (по желанию).",
     "https://www.asus.com/supportonly/armoury%20crate/helpdesk_download/",
     "https://dlcdnets.asus.com/pub/ASUS/mb/14Utilities/ArmouryCrateInstallTool.zip",
     "a/Asus/ArmouryCrate", "ArmouryCrateInstaller_3.3.6.0/ArmouryCrateInstaller.exe", "/SILENT", "Поиск...",
     {"Armoury Crate", "ASUS Framework", nullptr}},
    {"sw_wooting", "Wooting", "Wootility (клавиатуры Wooting)",
     "Конфигуратор аналоговых клавиатур Wooting: настройка Rapid Trigger, точки срабатывания клавиш, профилей и прошивки.",
     "https://wooting.io/wootility",
     nullptr, nullptr, nullptr, "", "Веб / Сайт Wooting",
     {"Wootility", nullptr, nullptr}},
    {"sw_keychron", "Keychron", "Keychron Launcher (веб-конфигуратор)",
     "Веб-конфигуратор клавиатур Keychron (QMK/VIA): переназначение клавиш, макросы, подсветка и обновление прошивки прямо из браузера.",
     "https://launcher.keychron.com/",
     nullptr, nullptr, nullptr, "", "Веб-конфигуратор",
     {"Keychron", nullptr, nullptr}},
    {"sw_glorious", "Glorious", "Glorious CORE",
     "Официальное ПО Glorious: настройка DPI, времени отклика (debounce), подсветки, макросов и прошивки мышей/клавиатур.",
     "https://www.gloriousgaming.com/pages/glorious-core",
     nullptr, nullptr, nullptr, "", "Сайт Glorious",
     {"Glorious CORE", nullptr, nullptr}},
    {"sw_pulsar", "Pulsar", "Pulsar Gaming Gear Software",
     "Утилита настройки мышей и клавиатур Pulsar: частота опроса до 4K/8K, DPI, LoD и обновление прошивки.",
     "https://www.pulsar.gg/pages/downloads",
     nullptr, nullptr, nullptr, "", "Сайт Pulsar",
     {"Pulsar", nullptr, nullptr}},
    {"sw_lamzu", "Lamzu", "Lamzu Software",
     "Официальная утилита Lamzu для настройки параметров сенсора, частоты опроса, макросов и обновления прошивки.",
     "https://lamzu.com/pages/download",
     nullptr, nullptr, nullptr, "", "Сайт Lamzu",
     {"Lamzu", nullptr, nullptr}},
    {"sw_endgame", "Endgame Gear", "Endgame Gear Software",
     "Конфигуратор периферии Endgame Gear: точная настройка DPI, дебаунса, Motion Sync и прошивки.",
     "https://www.endgamegear.com/en-us/downloads",
     nullptr, nullptr, nullptr, "", "Сайт Endgame Gear",
     {"Endgame Gear", nullptr, nullptr}},
    {"sw_creative", "Creative", "Creative App / Sound Blaster",
     "Управление звуковыми картами и гарнитурами Creative: эквалайзер, Acoustic Engine, Scout Mode и драйверы.",
     "https://support.creative.com/",
     nullptr, nullptr, nullptr, "", "Сайт Creative",
     {"Creative App", "Sound Blaster", nullptr}},
    {"sw_epos", "EPOS / Sennheiser", "EPOS Gaming Suite / EPOS Connect",
     "Настройка игровых гарнитур и ЦАП EPOS / Sennheiser: объемный звук 7.1, шумоподавление микрофона и прошивка.",
     "https://www.eposaudio.com/en/us/gaming/downloads",
     nullptr, "e/EPOS/EPOSConnect", nullptr, "", "Поиск...",
     {"EPOS Connect", "EPOS Gaming Suite", "Sennheiser Gaming"}},
    {"sw_bloody", "A4Tech / Bloody", "Bloody Official Software",
     "Официальное ПО для мышей и клавиатур Bloody: профили чувствительности, макросы и настройка отклика.",
     "https://www.bloody.com/en/download.php",
     nullptr, nullptr, nullptr, "", "Сайт Bloody",
     {"Bloody", nullptr, nullptr}},
    {"sw_roccat", "Turtle Beach / ROCCAT", "Turtle Beach Swarm II",
     "Единый центр управления гарнитурами, мышами и клавиатурами Turtle Beach / ROCCAT.",
     "https://www.turtlebeach.com/pages/swarm-ii",
     nullptr, nullptr, nullptr, "", "Сайт Turtle Beach",
     {"Swarm II", "ROCCAT Swarm", nullptr}},
    {"sw_elgato", "Elgato", "Elgato Stream Deck / Wave Link",
     "ПО для микрофонов Wave, контроллеров Stream Deck и карт захвата Elgato: микширование звука и профили сцен.",
     "https://www.elgato.com/us/en/s/downloads",
     nullptr, "e/Elgato/StreamDeck", nullptr, "", "Поиск...",
     {"Elgato Stream Deck", "Elgato Wave Link", nullptr}},
};

struct MonitorBrandInfo {
    std::string softwareName;
    std::string url;
    const char* uninstallKeyword;
    const char* wingetPath{nullptr};
    const char* fallbackDirectUrl{nullptr};
    const char* fallbackNestedHint{nullptr};
    const char* fallbackArgs{""};
};

MonitorBrandInfo GetMonitorBrandSupport(const std::string& pnpOrBrand, const std::string& model) {
    if (pnpOrBrand == "MSI" || ContainsCaseInsensitive(model, "MSI")) {
        return {"MSI Gaming Intelligence / Драйвер монитора",
                "https://www.msi.com/support/download",
                "Gaming Intelligence",
                "m/MSI/GamingIntelligence",
                "https://download.msi.com/uti_exe/monitor/Gaming-Intelligence-x64.zip",
                "setup.exe",
                "/S /v /qn"};
    }
    if (pnpOrBrand == "GSM" || pnpOrBrand == "LGD" || pnpOrBrand == "LG" ||
        ContainsCaseInsensitive(model, "LG")) {
        return {"LG OnScreen Control / Драйвер монитора",
                "https://www.lg.com/global/support", "OnScreen Control"};
    }
    if (pnpOrBrand == "SAM" || pnpOrBrand == "Samsung" ||
        ContainsCaseInsensitive(model, "Samsung") || ContainsCaseInsensitive(model, "SMS")) {
        return {"Samsung Easy Setting Box / Драйвер монитора",
                "https://www.samsung.com/us/support/downloads/",
                "Easy Setting Box",
                "s/Samsung/EasySettingBox",
                "https://downloadcenter.samsung.com/content/SW/202405/20240514170303450/EasySettingMSIX64_V3.0.11.msi",
                nullptr,
                ""};
    }
    if (pnpOrBrand == "ACI" || pnpOrBrand == "AUS" || pnpOrBrand == "ASUS" ||
        ContainsCaseInsensitive(model, "ASUS") || ContainsCaseInsensitive(model, "ROG")) {
        return {"ASUS DisplayWidget Center",
                "https://www.asus.com/content/asus-displaywidget-center/",
                "DisplayWidget",
                "a/Asus/DisplayWidgetCenter",
                "https://dlcdnets.asus.com/pub/ASUS/LCD%20Monitors/App/DisplayWidgetCenter/DisplayWidgetCenter.zip",
                "DisplayWidgetCenterSetup.msi",
                ""};
    }
    if (pnpOrBrand == "DEL" || pnpOrBrand == "Dell" ||
        ContainsCaseInsensitive(model, "DELL") || ContainsCaseInsensitive(model, "Alienware")) {
        return {"Dell Display Manager / Драйвер монитора",
                "https://www.dell.com/support/home",
                "Dell Display",
                "d/Dell/DisplayAndPeripheralManager",
                nullptr,
                nullptr,
                "/Silent"};
    }
    if (pnpOrBrand == "AOC" || ContainsCaseInsensitive(model, "AOC")) {
        return {"AOC G-Menu / Драйвер монитора",
                "https://aoc.com/uk/gaming/products/monitors", "G-Menu"};
    }
    if (pnpOrBrand == "BNQ" || ContainsCaseInsensitive(model, "BenQ") ||
        ContainsCaseInsensitive(model, "ZOWIE")) {
        return {"BenQ Display Pilot 2 / XL Setting to Share",
                "https://www.benq.com/en-us/support/downloads-faq.html", "Display Pilot"};
    }
    if (pnpOrBrand == "GIG" || pnpOrBrand == "GBT" || ContainsCaseInsensitive(model, "Gigabyte")) {
        return {"Gigabyte Control Center / OSD Sidekick",
                "https://www.gigabyte.com/Support", "OSD Sidekick"};
    }
    if (pnpOrBrand == "ACR" || ContainsCaseInsensitive(model, "Acer") ||
        ContainsCaseInsensitive(model, "Nitro") || ContainsCaseInsensitive(model, "Predator")) {
        return {"Acer Display Widget",
                "https://www.acer.com/us-en/support/drivers-and-manuals", "Acer Display Widget"};
    }
    if (pnpOrBrand == "PHL" || ContainsCaseInsensitive(model, "Philips")) {
        return {"Philips SmartControl / Evnia Precision Center",
                "https://www.philips.com/c-w/support-home",
                "SmartControl",
                "p/Philips/SmartControl",
                "https://gz-dlsw.tpv-tech.com/MNT/PHILIPS/files/Software/SmartControl.exe.zip",
                "SmartControl Setup 7.2.0.exe",
                "/S"};
    }
    if (pnpOrBrand == "VSC" || ContainsCaseInsensitive(model, "ViewSonic")) {
        return {"ViewSonic Display Portal / vDisplayManager",
                "https://www.viewsonic.com/us/support",
                "Display Portal",
                "v/ViewSonic/DisplayPortal",
                "https://www.viewsonicglobal.com/public/products_download/software/display/Display_Portal_win.zip",
                "Display_Portal_win\\setup.exe",
                ""};
    }
    return {"Официальная поддержка монитора",
            "https://www.catalog.update.microsoft.com/Search.aspx?q=" + model, nullptr};
}

} // namespace

SystemComponentsScanner::~SystemComponentsScanner() {
    stopRequested_.store(true);
    cancelInstallRequested_.store(true);
    for (;;) {
        std::thread scanT;
        std::thread onlineT;
        std::thread installT;
        {
            std::lock_guard<std::mutex> lock(threadMutex_);
            scanT = std::move(scanThread_);
            onlineT = std::move(onlineThread_);
            installT = std::move(installThread_);
        }
        if (!scanT.joinable() && !onlineT.joinable() && !installT.joinable()) {
            break;
        }
        if (installT.joinable()) installT.join();
        if (onlineT.joinable()) onlineT.join();
        if (scanT.joinable()) scanT.join();
    }
}

void SystemComponentsScanner::SetPhaseMessage(const std::string& msg) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    phaseMessage_ = msg;
}

std::string SystemComponentsScanner::PhaseMessage() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    return phaseMessage_;
}

std::vector<SystemComponentItem> SystemComponentsScanner::GetItemsSnapshot() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    return items_;
}

void SystemComponentsScanner::StartLocalScanAsync(
    std::vector<DeviceInfo> devicesSnapshot,
    LogCallback onLog,
    bool autoCheckOnline) {
    std::lock_guard<std::mutex> lock(threadMutex_);
    if (stopRequested_.load() || scanning_.exchange(true)) {
        return;
    }
    if (scanThread_.joinable()) {
        scanThread_.join();
    }
    SetPhaseMessage("Анализ библиотек Windows, мониторов и периферии...");

    scanThread_ = std::thread([this,
                               devs = std::move(devicesSnapshot),
                               log = std::move(onLog),
                               autoCheckOnline]() {
        auto list = ScanLocalSynchronously(devs);
        if (!stopRequested_.load()) {
            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                items_ = std::move(list);
            }
            generation_.fetch_add(1);
            SetPhaseMessage("Локальная проверка завершена");
        }
        scanning_.store(false);
        if (!stopRequested_.load() && log) {
            log(LogLevel::Success,
                "Проверка игровых библиотек, мониторов и ПО периферии завершена.");
        }
        if (autoCheckOnline && !stopRequested_.load()) {
            StartOnlineVersionCheckAsync(log);
        }
    });
}

void SystemComponentsScanner::RefreshLocalPreserveOnlineAsync(
    std::vector<DeviceInfo> devicesSnapshot,
    bool rescanDevices) {
    std::lock_guard<std::mutex> lock(threadMutex_);
    if (stopRequested_.load() || scanning_.exchange(true)) {
        return;
    }
    if (scanThread_.joinable()) {
        scanThread_.join();
    }

    scanThread_ = std::thread([this, devs = std::move(devicesSnapshot), rescanDevices]() {
        const auto actualDevs =
            (rescanDevices || devs.empty()) ? DeviceScanner::ScanSynchronously() : devs;
        auto refreshed = ScanLocalSynchronously(actualDevs);
        if (!stopRequested_.load()) {
            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                if (!items_.empty()) {
                    MergeOnlineMetadataIntoRefreshed(refreshed, items_);
                }
                items_ = std::move(refreshed);
            }
            generation_.fetch_add(1);
        }
        scanning_.store(false);
    });
}

void SystemComponentsScanner::StartOnlineVersionCheckAsync(LogCallback onLog) {
    std::lock_guard<std::mutex> lock(threadMutex_);
    if (stopRequested_.load() || checkingOnline_.exchange(true)) {
        return;
    }
    if (onlineThread_.joinable()) {
        onlineThread_.join();
    }

    SetPhaseMessage("Поиск актуальных версий...");
    onlineThread_ = std::thread([this, log = std::move(onLog)]() {
        auto updateItem = [&](const std::string& compId, auto&& fn) {
            std::lock_guard<std::mutex> lock(dataMutex_);
            for (auto& item : items_) {
                if (item.id == compId) {
                    fn(item);
                    break;
                }
            }
        };

        auto hasItem = [&](const std::string& compId) {
            std::lock_guard<std::mutex> lock(dataMutex_);
            for (const auto& it : items_) {
                if (it.id == compId) return true;
            }
            return false;
        };

        // Snapshot distinct (wingetRepoPath, preferredArch) and MS Catalog lookups needed right now.
        std::vector<std::pair<std::string, std::string>> wingetTargets;
        std::vector<std::pair<std::string, std::string>> catalogTargets;
        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            std::set<std::pair<std::string, std::string>> seen;
            std::set<std::pair<std::string, std::string>> seenCatalog;
            for (const auto& it : items_) {
                if (!it.wingetRepoPath.empty() && it.id != "sw_steelseries") {
                    const std::pair<std::string, std::string> key{
                        it.wingetRepoPath, it.preferredArch};
                    if (seen.insert(key).second) {
                        wingetTargets.push_back(key);
                    }
                }
                if (!it.catalogSearchKey.empty()) {
                    const std::pair<std::string, std::string> catPair{
                        it.catalogSearchKey, it.catalogFallbackKey};
                    if (seenCatalog.insert(catPair).second) {
                        catalogTargets.push_back(catPair);
                    }
                }
            }
        }

        // Group 1: .NET Desktop Runtime 8.0 & 9.0
        auto fDotnet = std::async(std::launch::async, [&]() {
            const auto checkDotnet = [&](const std::string& compId, const std::wstring& metaUrl) {
                const HttpResponse resp = WinHttpRequest(metaUrl, L"GET", true);
                if (!resp.ok || resp.body.empty()) {
                    updateItem(compId, [](SystemComponentItem& item) {
                        item.latestVersion = "Недоступно";
                        item.onlineChecked = true;
                    });
                    return;
                }
                const std::string latest = ExtractJsonStringField(resp.body, "latest-runtime");
                if (latest.empty()) {
                    return;
                }
                const std::string dlUrl =
                    "https://builds.dotnet.microsoft.com/dotnet/WindowsDesktop/" + latest +
                    "/windowsdesktop-runtime-" + latest + "-win-x64.exe";

                updateItem(compId, [&](SystemComponentItem& item) {
                    item.directInstallerUrl = dlUrl;
                    item.installerArgs = "/install /passive /norestart";
                    ApplyResolvedVersionToItem(item, latest);
                });
            };
            checkDotnet("rt_dotnet8",
                        L"https://dotnetcli.blob.core.windows.net/dotnet/release-metadata/8.0/releases.json");
            checkDotnet("rt_dotnet9",
                        L"https://dotnetcli.blob.core.windows.net/dotnet/release-metadata/9.0/releases.json");
            checkDotnet("rt_dotnet10",
                        L"https://dotnetcli.blob.core.windows.net/dotnet/release-metadata/10.0/releases.json");
        });

        // Group 2: Microsoft Edge WebView2 + LunarG Vulkan Runtime
        auto fRuntimes = std::async(std::launch::async, [&]() {
            {
                const HttpResponse resp = WinHttpRequest(
                    L"https://edgeupdates.microsoft.com/api/products?view=enterprise",
                    L"GET",
                    true,
                    128 * 1024);
                std::string latestWv2;
                if (resp.ok && !resp.body.empty()) {
                    const size_t stablePos = resp.body.find("\"Product\":\"Stable\"");
                    if (stablePos != std::string::npos) {
                        latestWv2 = ExtractJsonStringField(resp.body, "ProductVersion", stablePos);
                    }
                }
                updateItem("rt_webview2", [&](SystemComponentItem& item) {
                    if (!latestWv2.empty()) {
                        ApplyResolvedVersionToItem(item, latestWv2);
                    } else {
                        item.onlineChecked = true;
                        item.latestVersion = "Онлайн-установщик";
                    }
                });
            }

            {
                const HttpResponse resp = WinHttpRequest(
                    L"https://vulkan.lunarg.com/sdk/files", L"GET", true, 512 * 1024);
                const auto [vkLatest, vkUrl] = ParseLatestVulkanRuntimeFromJson(resp.body);
                updateItem("rt_vulkan", [&](SystemComponentItem& item) {
                    if (!vkLatest.empty() && !vkUrl.empty()) {
                        item.directInstallerUrl = vkUrl;
                        item.installerArgs = "";
                        ApplyResolvedVersionToItem(item, vkLatest, "Не найден");
                    } else {
                        item.onlineChecked = true;
                        item.latestVersion = "Сайт LunarG";
                    }
                });
            }
        });

        // Group 3: SteelSeries CDN + Razer verification + NVIDIA/AMD GPU checks
        auto fVendors = std::async(std::launch::async, [&]() {
            if (hasItem("sw_steelseries")) {
                const HttpResponse resp = WinHttpRequest(
                    L"https://steelseries.com/gg/downloads/gg/latest/windows", L"HEAD", false);
                std::string ver;
                std::string url = resp.locationHeader;
                if (!url.empty()) {
                    const size_t ggPos = url.find("SteelSeriesGG");
                    if (ggPos != std::string::npos) {
                        const size_t numStart = ggPos + 13;
                        const size_t setupPos = url.find("Setup", numStart);
                        if (setupPos != std::string::npos && setupPos > numStart) {
                            ver = url.substr(numStart, setupPos - numStart);
                        }
                    }
                }
                if (ver.empty()) {
                    const WingetManifestInfo wm = ResolveWingetPkgManifest("s/SteelSeries/GG", "x64");
                    if (wm.ok) {
                        ver = wm.version;
                        if (url.empty()) url = wm.url;
                    }
                }
                updateItem("sw_steelseries", [&](SystemComponentItem& item) {
                    if (!url.empty()) {
                        item.directInstallerUrl = url;
                    }
                    if (!ver.empty()) {
                        ApplyResolvedVersionToItem(item, ver, "Не установлено");
                    } else {
                        item.onlineChecked = true;
                        if (item.latestVersion == "Поиск...") item.latestVersion = "Онлайн";
                    }
                });
            }

            if (hasItem("sw_razer")) {
                const HttpResponse rzHead = WinHttpRequest(
                    L"https://rzr.to/synapse-4-pc-download", L"HEAD", true);
                updateItem("sw_razer", [&](SystemComponentItem& item) {
                    item.onlineChecked = true;
                    item.latestVersion = rzHead.ok ? "Synapse 4 (актуальный)" : "Сайт Razer";
                });
            }

            if (hasItem("gpu_nvidia")) {
                const HttpResponse resp = WinHttpRequest(
                    L"https://gfwsl.geforce.com/services_toolkit/services/com/nvidia/services/AjaxDriverService.php?func=DriverManualLookup&psid=127&pfid=985&osID=57&languageCode=1033&isWHQL=1&dch=1&upcrd=0&numberOfResults=1",
                    L"GET",
                    true,
                    64 * 1024);
                if (resp.ok && !resp.body.empty()) {
                    const std::string nvVer = ExtractJsonStringField(resp.body, "Version");
                    const std::string nvUrl = ExtractJsonStringField(resp.body, "DownloadURL");
                    updateItem("gpu_nvidia", [&](SystemComponentItem& item) {
                        if (!nvUrl.empty()) {
                            item.directInstallerUrl = nvUrl;
                            item.installerArgs = "";
                        }
                        if (!nvVer.empty()) {
                            ApplyResolvedVersionToItem(item, nvVer, "Не найден");
                        } else {
                            item.onlineChecked = true;
                        }
                    });
                }
            }

            if (hasItem("gpu_amd")) {
                const HttpResponse resp = WinHttpRequest(
                    L"https://www.amd.com/en/support/download/drivers.html",
                    L"GET",
                    true,
                    512 * 1024);
                const auto [amdVer, amdUrl] = ParseAmdAdrenalinFromHtml(resp.body);
                updateItem("gpu_amd", [&](SystemComponentItem& item) {
                    item.onlineChecked = true;
                    if (!amdUrl.empty()) {
                        item.directInstallerUrl = amdUrl;
                        item.installerArgs = "";
                    }
                    if (!amdVer.empty()) {
                        ApplyResolvedVersionToItem(item, amdVer, "Доступен");
                    } else {
                        item.latestVersion = "Портал AMD";
                    }
                });
            }

            if (hasItem("chip_amd")) {
                const HttpResponse resp = WinHttpRequest(
                    L"https://www.amd.com/en/support/downloads/drivers.html/chipsets/am4/b550.html",
                    L"GET",
                    true,
                    512 * 1024);
                const auto [chipVer, chipUrl] = ParseAmdChipsetFromHtml(resp.body);
                updateItem("chip_amd", [&](SystemComponentItem& item) {
                    item.onlineChecked = true;
                    if (!chipUrl.empty()) {
                        item.directInstallerUrl = chipUrl;
                    }
                    if (!chipVer.empty()) {
                        ApplyResolvedVersionToItem(item, chipVer, "Не установлен");
                    } else if (!item.directInstallerUrl.empty()) {
                        ApplyResolvedVersionToItem(item, "8.09.28.1124", "Не установлен");
                    }
                });
            }
        });

        // Group 4: Parallel official winget-pkgs manifest lookups (VC++ 2015-2022,
        // ASUS DisplayWidget Center, Armoury Crate, Logitech G HUB, Corsair iCUE,
        // Dell Display Manager, MSI Gaming Intelligence, Samsung Easy Setting Box,
        // Philips SmartControl, ViewSonic DisplayPortal, Elgato, EPOS, Intel DSA).
        auto fWinget = std::async(std::launch::async, [&]() {
            const size_t n = wingetTargets.size();
            if (n == 0 || stopRequested_.load()) {
                return;
            }
            std::vector<WingetManifestInfo> resolved(n);
            const size_t kWorkers = std::min<size_t>(4, n);
            std::atomic<size_t> nextIdx{0};
            std::vector<std::thread> pool;
            pool.reserve(kWorkers);
            for (size_t w = 0; w < kWorkers; ++w) {
                pool.emplace_back([&]() {
                    for (;;) {
                        if (stopRequested_.load()) break;
                        const size_t idx = nextIdx.fetch_add(1);
                        if (idx >= n) break;
                        resolved[idx] = ResolveWingetPkgManifest(
                            wingetTargets[idx].first, wingetTargets[idx].second);
                    }
                });
            }
            for (auto& t : pool) t.join();
            if (stopRequested_.load()) {
                return;
            }

            std::lock_guard<std::mutex> lock(dataMutex_);
            for (auto& item : items_) {
                if (item.wingetRepoPath.empty() || item.id == "sw_steelseries") {
                    continue;
                }
                for (size_t idx = 0; idx < n; ++idx) {
                    if (wingetTargets[idx].first == item.wingetRepoPath &&
                        wingetTargets[idx].second == item.preferredArch) {
                        const auto& wm = resolved[idx];
                        item.onlineChecked = true;
                        if (wm.ok) {
                            if (!wm.url.empty()) {
                                item.directInstallerUrl = wm.url;
                            }
                            if (!wm.nestedRelativePath.empty()) {
                                item.nestedInstallerHint = wm.nestedRelativePath;
                            }
                            if (!wm.silentArgs.empty() && item.installerArgs.empty()) {
                                item.installerArgs = wm.silentArgs;
                            }
                            ApplyResolvedVersionToItem(item, wm.version);
                        } else if (item.latestVersion == "Поиск..." ||
                                   item.latestVersion == "Портал поддержки") {
                            if (item.id == "rt_vcredist_x64" || item.id == "rt_vcredist_x86") {
                                item.latestVersion = "Недоступно (aka.ms)";
                            } else {
                                item.latestVersion = "Сайт производителя";
                            }
                        }
                        break;
                    }
                }
            }
        });

        // Group 5: Parallel Microsoft Update Catalog WHQL lookups (LAN / Wi-Fi / BT / Audio)
        auto fCatalog = std::async(std::launch::async, [&]() {
            const size_t n = catalogTargets.size();
            if (n == 0 || stopRequested_.load()) return;
            std::vector<CatalogDriverMatch> resolved(n);
            const size_t kWorkers = std::min<size_t>(4, n);
            std::atomic<size_t> nextIdx{0};
            std::vector<std::thread> pool;
            pool.reserve(kWorkers);
            for (size_t w = 0; w < kWorkers; ++w) {
                pool.emplace_back([&]() {
                    for (;;) {
                        if (stopRequested_.load()) break;
                        const size_t idx = nextIdx.fetch_add(1);
                        if (idx >= n) break;
                        auto res = ResolveMsCatalogDriverCab(catalogTargets[idx].first);
                        if (res.cabUrl.empty() && !catalogTargets[idx].second.empty() &&
                            !stopRequested_.load()) {
                            res = ResolveMsCatalogDriverCab(catalogTargets[idx].second);
                        }
                        resolved[idx] = std::move(res);
                    }
                });
            }
            for (auto& t : pool) t.join();
            if (stopRequested_.load()) return;

            std::lock_guard<std::mutex> lock(dataMutex_);
            for (auto& item : items_) {
                if (item.catalogSearchKey.empty()) continue;
                for (size_t idx = 0; idx < n; ++idx) {
                    if (catalogTargets[idx].first != item.catalogSearchKey ||
                        catalogTargets[idx].second != item.catalogFallbackKey) {
                        continue;
                    }
                    const auto& match = resolved[idx];
                    const std::string& catVer = match.version;
                    const std::string& cabUrl = match.cabUrl;
                    item.onlineChecked = true;
                    item.latestDateYmd = match.dateYmd;
                    if (!cabUrl.empty()) {
                        item.directInstallerUrl = cabUrl;
                    }
                    if (!catVer.empty()) {
                        const bool catalogIsOlderByDate =
                            (match.dateYmd > 0 && item.installedDateYmd > 0 &&
                             match.dateYmd < item.installedDateYmd);
                        if (item.installed && !item.installedVersion.empty() &&
                            (catalogIsOlderByDate || IsVersionNewer(item.installedVersion, catVer))) {
                            item.latestVersion = catVer + " (WHQL)";
                            item.updateAvailable = false;
                            item.statusText = "Актуален (" + item.installedVersion + ")";
                        } else {
                            ApplyResolvedVersionToItem(item, catVer, "Не установлен");
                        }
                    } else if (item.latestVersion == "Поиск...") {
                        item.latestVersion = "Каталог MS";
                    }
                    break;
                }
            }
        });

        fDotnet.wait();
        fRuntimes.wait();
        fVendors.wait();
        fWinget.wait();
        fCatalog.wait();

        if (!stopRequested_.load()) {
            generation_.fetch_add(1);
            SetPhaseMessage("Актуальные версии проверены");
        }
        checkingOnline_.store(false);
        if (!stopRequested_.load() && log) {
            log(LogLevel::Success, "Онлайн-проверка актуальных версий завершена.");
        }
    });
}

bool SystemComponentsScanner::InstallSingleComponentSync(
    const SystemComponentItem& item,
    float baseProgress,
    float progressSpan,
    std::vector<DeviceInfo>& devs,
    const LogCallback& log,
    bool* outRebootRequired) {
    if (outRebootRequired != nullptr) {
        *outRebootRequired = false;
    }
    if (cancelInstallRequested_.load() || item.directInstallerUrl.empty()) {
        return false;
    }

    SetPhaseMessage("Скачивание: " + item.name);

    std::error_code tempEc;
    std::filesystem::path tempRoot;
    wchar_t tempDir[MAX_PATH]{};
    const DWORD tempLen = GetTempPathW(MAX_PATH, tempDir);
    if (tempLen > 0 && tempLen < MAX_PATH) {
        tempRoot = std::filesystem::path(tempDir);
    } else {
        tempRoot = std::filesystem::temp_directory_path(tempEc);
    }
    if (tempRoot.empty()) {
        if (log) {
            log(LogLevel::Error, "Не удалось определить временную папку Windows.");
        }
        SetPhaseMessage("Ошибка временного каталога");
        return false;
    }

    const std::string cleanUrl = StripUrlQuery(item.directInstallerUrl);
    bool isZipPackage = EndsWithCaseInsensitive(cleanUrl, ".zip");
    const bool isCabPackage = EndsWithCaseInsensitive(cleanUrl, ".cab");
    const bool isDirectMsi = EndsWithCaseInsensitive(cleanUrl, ".msi");
    const char* initialExt =
        isCabPackage ? ".cab" : (isZipPackage ? ".zip" : (isDirectMsi ? ".msi" : ".exe"));
    std::filesystem::path destFile = tempRoot / ("DriverLoom_" + item.id + initialExt);

    if (log) {
        log(LogLevel::Info,
            "Загрузка официального установщика \"" + item.name + "\"...");
    }

    std::string errMsg;
    if (!DownloadFileViaWinHttp(
            Utf8ToWide(item.directInstallerUrl),
            destFile,
            &progress_,
            &errMsg,
            baseProgress,
            progressSpan,
            &cancelInstallRequested_)) {
        if (!stopRequested_.load() && log) {
            log(cancelInstallRequested_.load() ? LogLevel::Warning : LogLevel::Error,
                "Ошибка скачивания " + item.name + ": " + errMsg);
        }
        SetPhaseMessage(cancelInstallRequested_.load() ? "Установка отменена" : "Ошибка загрузки");
        return false;
    }

    if (isCabPackage) {
        SetPhaseMessage("Проверка подписи CAB-пакета...");
        std::string sigErr;
        if (!VerifyAuthenticodeSignature(destFile, &sigErr)) {
            std::error_code ec;
            std::filesystem::remove(destFile, ec);
            if (!stopRequested_.load() && log) {
                log(LogLevel::Error,
                    "Цифровая подпись CAB-пакета не прошла проверку (" + sigErr + ").");
            }
            SetPhaseMessage("Подпись CAB не подтверждена");
            return false;
        }

        SetPhaseMessage("Распаковка драйвера (.cab)...");
        std::filesystem::path cabExtractDir = tempRoot / ("DriverLoom_" + item.id + "_cab");
        std::error_code ec;
        std::filesystem::remove_all(cabExtractDir, ec);
        std::filesystem::create_directories(cabExtractDir, ec);

        if (!SetupIterateCabinetW(destFile.c_str(), 0, CabExtractCallback, &cabExtractDir)) {
            std::filesystem::remove(destFile, ec);
            std::filesystem::remove_all(cabExtractDir, ec);
            if (!stopRequested_.load() && log) {
                log(LogLevel::Error,
                    "Ошибка распаковки CAB-архива " + item.name + ": " +
                        FormatWin32Error(GetLastError()));
            }
            SetPhaseMessage("Ошибка распаковки CAB");
            return false;
        }
        std::filesystem::remove(destFile, ec);

        SetPhaseMessage("Установка драйвера " + item.name + "...");
        progress_.store(std::clamp(baseProgress + progressSpan * 0.94f, 0.0f, 0.99f));

        bool cabOk = false;
        bool cabReboot = false;
        if (IsProcessElevated()) {
            const LocalInstallSummary sum = LocalDriverInstaller::InstallFromFolderSync(
                cabExtractDir.wstring(), false, false, log);
            cabOk = (sum.failedCount == 0 && (sum.installedCount > 0 || sum.stagedOnlyCount > 0));
            if (sum.rebootRequired) {
                cabReboot = true;
            }
        } else {
            const std::filesystem::path tempLog = CreateTempLogPath(L"hdm_cab_inf");
            const std::wstring cliArgs =
                L"--cli-install-inf " + QuoteWin32Arg(cabExtractDir.wstring()) +
                L" --log " + QuoteWin32Arg(tempLog.wstring());
            const ElevatedRunResult run =
                ExecuteElevatedSelf(cliArgs, true, &tempLog, log, nullptr, &stopRequested_);
            std::filesystem::remove(tempLog, ec);
            if (!run.launched) {
                if (run.launchError == ERROR_CANCELLED) {
                    cancelInstallRequested_.store(true);
                    if (!stopRequested_.load() && log) {
                        log(LogLevel::Warning, "Установка драйвера отменена (UAC).");
                    }
                } else if (!stopRequested_.load() && log) {
                    log(LogLevel::Error, "Ошибка запуска UAC: " + FormatWin32Error(run.launchError));
                }
            } else {
                cabOk = (run.exitCode == 0 || run.exitCode == 2);
                if (run.exitCode == 2) {
                    cabReboot = true;
                }
            }
        }
        if (outRebootRequired != nullptr && cabReboot) {
            *outRebootRequired = true;
        }
        std::filesystem::remove_all(cabExtractDir, ec);

        if (!stopRequested_.load()) {
            const int maxAttempts =
                (cabOk && !cabReboot && !item.installedVersion.empty()) ? 6 : 1;
            std::vector<DeviceInfo> freshDevs;
            std::vector<SystemComponentItem> refreshed;
            bool versionUnchanged = false;

            for (int attempt = 0; attempt < maxAttempts; ++attempt) {
                if (attempt > 0) {
                    for (int step = 0; step < 5; ++step) {
                        if (stopRequested_.load()) {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    if (stopRequested_.load()) {
                        break;
                    }
                }

                freshDevs = DeviceScanner::ScanSynchronously();
                refreshed = ScanLocalSynchronously(freshDevs);

                versionUnchanged = false;
                if (cabOk && !cabReboot && !item.installedVersion.empty()) {
                    for (const auto& r : refreshed) {
                        if (r.id == item.id && r.installedVersion == item.installedVersion) {
                            versionUnchanged = true;
                            break;
                        }
                    }
                }
                if (!versionUnchanged) {
                    break;
                }
            }

            if (!stopRequested_.load()) {
                devs = std::move(freshDevs);
                if (versionUnchanged) {
                    if (log) {
                        log(LogLevel::Warning,
                            "Пакет .cab добавлен в DriverStore, но Windows оставила текущую версию драйвера (" +
                                item.installedVersion +
                                ") — пакет не подходит к ревизии устройства или требуется перезагрузка.");
                    }
                    cabOk = false;
                }
                {
                    std::lock_guard<std::mutex> lock(dataMutex_);
                    MergeOnlineMetadataIntoRefreshed(refreshed, items_);
                    items_ = std::move(refreshed);
                    generation_.fetch_add(1);
                }
                if (cabOk || cabReboot) {
                    deviceRescanNeeded_.store(true);
                }
            }
        }
        progress_.store(std::clamp(baseProgress + progressSpan, 0.0f, 1.0f));
        return cabOk;
    }

    if (!isZipPackage && HasZipMagicHeader(destFile)) {
        std::filesystem::path zipRenamed = destFile;
        zipRenamed.replace_extension(L".zip");
        std::error_code rec;
        std::filesystem::rename(destFile, zipRenamed, rec);
        if (!rec) {
            destFile = zipRenamed;
            isZipPackage = true;
        }
    }

    std::filesystem::path targetExe = destFile;
    std::filesystem::path extractDir;

    if (isZipPackage) {
        SetPhaseMessage("Распаковка архива...");
        extractDir = tempRoot / ("DriverLoom_" + item.id + "_pkg");
        std::error_code ec;
        std::filesystem::remove_all(extractDir, ec);
        if (!ExtractZipViaShellCom(destFile, extractDir)) {
            std::filesystem::remove(destFile, ec);
            std::filesystem::remove_all(extractDir, ec);
            if (!stopRequested_.load() && log) {
                log(LogLevel::Error, "Не удалось распаковать архив " + item.name + ".");
            }
            SetPhaseMessage("Ошибка распаковки");
            return false;
        }

        targetExe.clear();
        if (!item.nestedInstallerHint.empty()) {
            std::wstring relW = Utf8ToWide(item.nestedInstallerHint);
            std::replace(relW.begin(), relW.end(), L'/', L'\\');
            const std::filesystem::path hinted = extractDir / relW;
            if (std::filesystem::is_regular_file(hinted, ec)) {
                targetExe = hinted;
            }
        }
        if (targetExe.empty()) {
            std::filesystem::path firstMsi;
            std::filesystem::path setupExe;
            std::filesystem::path firstExe;
            for (std::filesystem::recursive_directory_iterator it(
                     extractDir,
                     std::filesystem::directory_options::skip_permission_denied,
                     ec);
                 !ec && it != std::filesystem::recursive_directory_iterator();
                 it.increment(ec)) {
                std::error_code entryEc;
                if (!it->is_regular_file(entryEc)) {
                    continue;
                }
                const std::string ext = ToLowerAscii(WideToUtf8(it->path().extension().wstring()));
                const std::string fname = ToLowerAscii(WideToUtf8(it->path().filename().wstring()));
                if (ext == ".msi" && firstMsi.empty()) {
                    firstMsi = it->path();
                } else if (ext == ".exe") {
                    if (firstExe.empty()) firstExe = it->path();
                    if (setupExe.empty() &&
                        (fname.find("setup") != std::string::npos ||
                         fname.find("install") != std::string::npos)) {
                        setupExe = it->path();
                    }
                }
            }
            if (!firstMsi.empty()) targetExe = firstMsi;
            else if (!setupExe.empty()) targetExe = setupExe;
            else targetExe = firstExe;
        }
        if (targetExe.empty()) {
            std::filesystem::remove(destFile, ec);
            std::filesystem::remove_all(extractDir, ec);
            if (!stopRequested_.load() && log) {
                log(LogLevel::Error, "В архиве не найден исполняемый файл установщика.");
            }
            SetPhaseMessage("Установщик не найден");
            return false;
        }
    }

    if (cancelInstallRequested_.load()) {
        std::error_code ec;
        std::filesystem::remove(destFile, ec);
        if (!extractDir.empty()) std::filesystem::remove_all(extractDir, ec);
        SetPhaseMessage("Установка отменена");
        return false;
    }

    SetPhaseMessage("Проверка цифровой подписи Authenticode...");
    std::string sigErr;
    if (!VerifyAuthenticodeSignature(targetExe, &sigErr)) {
        std::error_code ec;
        std::filesystem::remove(destFile, ec);
        if (!extractDir.empty()) std::filesystem::remove_all(extractDir, ec);
        if (!stopRequested_.load() && log) {
            log(LogLevel::Error,
                "Цифровая подпись установщика не прошла проверку (" + sigErr +
                    "). Запуск отменён в целях безопасности.");
        }
        SetPhaseMessage("Подпись не подтверждена");
        return false;
    }

    const std::string dlVer = GetFileVersionUtf8(targetExe);
    if (!stopRequested_.load() && log) {
        log(LogLevel::Success,
            "Подпись подтверждена" +
                (dlVer.empty() ? std::string() : " (версия файла: " + dlVer + ")") +
                ". Запуск установки...");
    }

    SetPhaseMessage("Установка " + item.name + "...");
    progress_.store(std::clamp(baseProgress + progressSpan * 0.94f, 0.0f, 0.99f));

    DWORD exitCode = 0;
    DWORD launchErr = 0;
    bool launched = false;
    bool errorAlreadyLogged = false;

    if (item.id == "rt_dx_legacy") {
        const std::filesystem::path dxExtractDir = tempRoot / L"DriverLoom_DX2010";
        std::error_code ec;
        std::filesystem::remove_all(dxExtractDir, ec);
        std::filesystem::create_directories(dxExtractDir, ec);
        const std::wstring extractArgs = L"/Q /T:" + QuoteWin32Arg(dxExtractDir.wstring());
        if (LaunchAndWaitProcess(
                targetExe,
                extractArgs,
                &exitCode,
                &launchErr,
                &cancelInstallRequested_,
                &stopRequested_)) {
            const std::filesystem::path dxSetup = dxExtractDir / L"DXSETUP.exe";
            if (exitCode != 0) {
                if (!stopRequested_.load() && log) {
                    log(LogLevel::Error,
                        "Ошибка распаковки DirectX End-User Runtime (код " +
                            std::to_string(exitCode) + ").");
                }
                errorAlreadyLogged = true;
            } else if (std::filesystem::exists(dxSetup, ec)) {
                launched = LaunchAndWaitProcess(
                    dxSetup,
                    L"/silent",
                    &exitCode,
                    &launchErr,
                    &cancelInstallRequested_,
                    &stopRequested_);
            } else {
                if (!stopRequested_.load() && log) {
                    log(LogLevel::Error,
                        "В распакованном пакете DirectX не найден DXSETUP.exe.");
                }
                launchErr = ERROR_FILE_NOT_FOUND;
                errorAlreadyLogged = true;
            }
        }
        std::filesystem::remove_all(dxExtractDir, ec);
    } else if (EndsWithCaseInsensitive(WideToUtf8(targetExe.extension().wstring()), ".msi")) {
        const std::filesystem::path sysDir = GetSystemDirPath(false);
        if (!sysDir.empty()) {
            const std::filesystem::path msiexec = sysDir / L"msiexec.exe";
            const std::wstring msiArgs =
                L"/i " + QuoteWin32Arg(targetExe.wstring()) + L" /passive /norestart";
            launched = LaunchAndWaitProcess(
                msiexec,
                msiArgs,
                &exitCode,
                &launchErr,
                &cancelInstallRequested_,
                &stopRequested_);
        } else {
            launchErr = ERROR_PATH_NOT_FOUND;
        }
    } else {
        launched = LaunchAndWaitProcess(
            targetExe,
            Utf8ToWide(item.installerArgs),
            &exitCode,
            &launchErr,
            &cancelInstallRequested_,
            &stopRequested_);
    }

    bool succeeded = false;
    if (!launched) {
        if (!errorAlreadyLogged && !stopRequested_.load() && log) {
            log(launchErr == ERROR_CANCELLED ? LogLevel::Warning : LogLevel::Error,
                launchErr == ERROR_CANCELLED
                    ? "Установка отменена пользователем."
                    : "Не удалось запустить установщик: " + FormatWin32Error(launchErr));
        }
        if (launchErr == ERROR_CANCELLED) {
            cancelInstallRequested_.store(true);
        }
    } else if (exitCode == 0 || exitCode == 3010 || exitCode == 1641 ||
               (exitCode == 2 && item.id == "chip_amd")) {
        succeeded = true;
        const bool needReboot =
            (exitCode == 3010 || exitCode == 1641 || (exitCode == 2 && item.id == "chip_amd"));
        if (outRebootRequired != nullptr && needReboot) {
            *outRebootRequired = true;
        }
        if (!stopRequested_.load() && log) {
            log(LogLevel::Success,
                item.name + " успешно установлен" +
                    (needReboot ? " (требуется перезагрузка для применения изменений)." : "."));
        }
    } else {
        if (!stopRequested_.load() && log) {
            log(LogLevel::Warning,
                item.name + " завершился с кодом " + std::to_string(exitCode) + ".");
        }
    }

    std::error_code ec;
    std::filesystem::remove(destFile, ec);
    if (!extractDir.empty()) {
        std::filesystem::remove_all(extractDir, ec);
    }

    if (stopRequested_.load()) {
        return false;
    }

    auto refreshAndCompare = [&]() {
        if (item.category == ComponentCategory::MonitorAndGpu) {
            devs = DeviceScanner::ScanSynchronously();
        }
        auto refreshed = ScanLocalSynchronously(devs);
        std::lock_guard<std::mutex> lock(dataMutex_);
        if (!dlVer.empty() &&
            (item.id == "rt_vcredist_x64" || item.id == "rt_vcredist_x86")) {
            for (auto& oldIt : items_) {
                if (oldIt.id == item.id) {
                    oldIt.latestVersion = dlVer;
                    oldIt.onlineChecked = true;
                    break;
                }
            }
        }
        MergeOnlineMetadataIntoRefreshed(refreshed, items_);
        items_ = std::move(refreshed);
        generation_.fetch_add(1);
    };

    refreshAndCompare();

    // Some web bootstrappers (Logitech G HUB / Razer Synapse / WebView2) continue finalizing
    // registry Uninstall entries for a few seconds after the launcher returns.
    if (launched) {
        bool needRetry = false;
        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            for (const auto& cur : items_) {
                if (cur.id == item.id && (!cur.installed || cur.updateAvailable)) {
                    needRetry = true;
                    break;
                }
            }
        }
        if (needRetry) {
            for (int step = 0; step < 30; ++step) {
                if (stopRequested_.load() || cancelInstallRequested_.load()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!stopRequested_.load()) {
                refreshAndCompare();
            }
        }
        if (item.category == ComponentCategory::MonitorAndGpu && !stopRequested_.load()) {
            deviceRescanNeeded_.store(true);
        }
    }

    progress_.store(std::clamp(baseProgress + progressSpan, 0.0f, 1.0f));
    return succeeded;
}

void SystemComponentsScanner::DownloadAndInstallAsync(
    const SystemComponentItem& item,
    std::vector<DeviceInfo> devicesSnapshot,
    LogCallback onLog) {
    std::lock_guard<std::mutex> lock(threadMutex_);
    if (stopRequested_.load()) {
        return;
    }
    if (scanning_.load() || checkingOnline_.load() || installing_.exchange(true)) {
        if (onLog) {
            onLog(LogLevel::Warning, "Установка компонента уже выполняется.");
        }
        return;
    }
    if (item.directInstallerUrl.empty()) {
        installing_.store(false);
        return;
    }
    if (installThread_.joinable()) {
        installThread_.join();
    }

    cancelInstallRequested_.store(false);
    progress_.store(0.0f);

    installThread_ = std::thread([this, item, devs = std::move(devicesSnapshot), log = std::move(onLog)]() mutable {
        bool rebootReq = false;
        const bool ok = InstallSingleComponentSync(item, 0.0f, 1.0f, devs, log, &rebootReq);
        progress_.store(1.0f);
        if (!stopRequested_.load()) {
            SetPhaseMessage(
                ok ? (rebootReq ? "Установка завершена (требуется перезагрузка)" : "Установка завершена")
                   : (cancelInstallRequested_.load() ? "Установка отменена"
                                                    : "Установка завершена с предупреждением"));
        }
        installing_.store(false);
    });
}

void SystemComponentsScanner::DownloadAndInstallBatchAsync(
    std::vector<SystemComponentItem> itemsToInstall,
    std::vector<DeviceInfo> devicesSnapshot,
    LogCallback onLog,
    bool createRestorePointBefore) {
    std::lock_guard<std::mutex> lock(threadMutex_);
    if (stopRequested_.load()) {
        return;
    }
    if (scanning_.load() || checkingOnline_.load() || installing_.exchange(true)) {
        if (onLog) {
            onLog(LogLevel::Warning, "Установка компонентов уже выполняется.");
        }
        return;
    }
    if (itemsToInstall.empty()) {
        installing_.store(false);
        return;
    }
    if (installThread_.joinable()) {
        installThread_.join();
    }

    cancelInstallRequested_.store(false);
    progress_.store(0.0f);
    SetPhaseMessage("Пакетное обновление компонентов...");

    installThread_ = std::thread([this,
                                  list = std::move(itemsToInstall),
                                  devs = std::move(devicesSnapshot),
                                  log = std::move(onLog),
                                  createRestorePointBefore]() mutable {
        const size_t total = list.size();
        size_t okCount = 0;

        if (!stopRequested_.load() && log) {
            log(LogLevel::Info,
                "Начинается пакетная установка/обновление компонентов (" +
                    std::to_string(total) + " шт.)...");
        }

        if (createRestorePointBefore && !cancelInstallRequested_.load() && !stopRequested_.load()) {
            SetPhaseMessage("Создание точки восстановления...");
            progress_.store(0.03f);
            constexpr const char* kRpDesc =
                "DriverLoom (перед пакетным обновлением компонентов)";
            if (IsProcessElevated()) {
                SystemRestore::CreateRestorePointSync(kRpDesc, log);
            } else {
                if (!stopRequested_.load() && log) {
                    log(LogLevel::Info,
                        "Запрос прав администратора (UAC) для создания точки восстановления...");
                }
                const std::filesystem::path tempLog = CreateTempLogPath(L"hdm_sr_comp");
                const std::wstring args =
                    L"--cli-restore-point " + QuoteWin32Arg(Utf8ToWide(kRpDesc)) +
                    L" --log " + QuoteWin32Arg(tempLog.wstring());
                const ElevatedRunResult run =
                    ExecuteElevatedSelf(args, true, &tempLog, log, nullptr, &stopRequested_);
                std::error_code ec;
                std::filesystem::remove(tempLog, ec);
                if (!run.launched && !stopRequested_.load() && log) {
                    log(LogLevel::Warning,
                        run.launchError == ERROR_CANCELLED
                            ? "Пользователь отменил UAC при создании точки восстановления — продолжается обновление компонентов."
                            : ("Не удалось создать точку восстановления: " +
                               FormatWin32Error(run.launchError)));
                }
            }
            SetPhaseMessage("Пакетное обновление компонентов...");
        }

        const float baseProg = createRestorePointBefore ? 0.08f : 0.0f;
        progress_.store(baseProg);
        const float span = (1.0f - baseProg) / static_cast<float>(total);
        for (size_t i = 0; i < total; ++i) {
            if (stopRequested_.load() || cancelInstallRequested_.load()) {
                if (!stopRequested_.load() && log) {
                    log(LogLevel::Warning, "Пакетная установка остановлена пользователем.");
                }
                break;
            }
            if (log) {
                log(LogLevel::Info,
                    "[" + std::to_string(i + 1) + "/" + std::to_string(total) +
                        "] Пакетная установка: " + list[i].name);
            }
            if (InstallSingleComponentSync(
                    list[i], baseProg + static_cast<float>(i) * span, span, devs, log)) {
                ++okCount;
            }
        }

        progress_.store(1.0f);
        const std::string doneMsg =
            cancelInstallRequested_.load()
                ? ("Пакетная установка прервана (" + std::to_string(okCount) + " из " +
                   std::to_string(total) + ")")
                : ("Пакетная установка завершена: успешно — " + std::to_string(okCount) +
                   " из " + std::to_string(total));
        if (!stopRequested_.load()) {
            SetPhaseMessage(doneMsg);
            if (log) {
                log(okCount == total ? LogLevel::Success : LogLevel::Warning, doneMsg + ".");
            }
        }
        installing_.store(false);
    });
}

void SystemComponentsScanner::SearchAndInstallFromCatalogAsync(
    const std::string& deviceName,
    const std::string& searchHardwareKey,
    const std::string& fullHardwareId,
    const std::string& installedVersion,
    std::vector<DeviceInfo> devicesSnapshot,
    LogCallback onLog,
    CatalogInstallCallback onComplete) {
    std::lock_guard<std::mutex> lock(threadMutex_);
    if (stopRequested_.load()) {
        return;
    }
    if (scanning_.load() || checkingOnline_.load() || installing_.exchange(true)) {
        if (onLog) {
            onLog(LogLevel::Warning, "Модуль компонентов уже выполняет другую операцию.");
        }
        if (onComplete) {
            onComplete(false, false);
        }
        return;
    }

    cancelInstallRequested_.store(false);
    progress_.store(0.04f);
    if (installThread_.joinable()) {
        installThread_.join();
    }

    installThread_ = std::thread([this,
                                  deviceName,
                                  searchHardwareKey,
                                  fullHardwareId,
                                  installedVersion,
                                  devs = std::move(devicesSnapshot),
                                  log = std::move(onLog),
                                  cb = std::move(onComplete)]() mutable {
        const std::string primaryKey =
            !searchHardwareKey.empty() ? searchHardwareKey : fullHardwareId;
        if (primaryKey.empty()) {
            if (!stopRequested_.load() && log) {
                log(LogLevel::Error,
                    "Не удалось определить Hardware ID для поиска в Microsoft Update Catalog («" +
                        deviceName + "»).");
            }
            SetPhaseMessage("Недостаточно данных для поиска драйвера");
            progress_.store(1.0f);
            installing_.store(false);
            if (!stopRequested_.load() && cb) cb(false, false);
            return;
        }

        SetPhaseMessage("Поиск драйвера в Microsoft Update Catalog: " + deviceName + "...");
        if (!stopRequested_.load() && log) {
            log(LogLevel::Info,
                "Поиск WHQL-драйвера в Microsoft Update Catalog для «" + deviceName + "» (" +
                    primaryKey + ")...");
        }

        std::string catVer;
        std::string cabUrl;
        const std::string subsysKey = ExtractVenDevSubsysKey(fullHardwareId);
        const bool isHdaAudio = ContainsCaseInsensitive(fullHardwareId, "HDAUDIO\\") ||
                                ContainsCaseInsensitive(fullHardwareId, "INTELAUDIO\\");
        const bool strictSubsysOnly = isHdaAudio && !subsysKey.empty();
        if (!subsysKey.empty() && !stopRequested_.load()) {
            const CatalogDriverMatch match = ResolveMsCatalogDriverCab(subsysKey);
            catVer = match.version;
            cabUrl = match.cabUrl;
        }
        if (!strictSubsysOnly && cabUrl.empty() && !cancelInstallRequested_.load() && !stopRequested_.load()) {
            const CatalogDriverMatch match = ResolveMsCatalogDriverCab(primaryKey);
            catVer = match.version;
            cabUrl = match.cabUrl;
        }
        if (!strictSubsysOnly && cabUrl.empty() && !fullHardwareId.empty() && fullHardwareId != primaryKey &&
            fullHardwareId != subsysKey && !cancelInstallRequested_.load() && !stopRequested_.load()) {
            const CatalogDriverMatch match = ResolveMsCatalogDriverCab(fullHardwareId);
            catVer = match.version;
            cabUrl = match.cabUrl;
        }

        if (cancelInstallRequested_.load() || stopRequested_.load()) {
            SetPhaseMessage("Установка отменена");
            progress_.store(1.0f);
            installing_.store(false);
            if (!stopRequested_.load() && cb) cb(false, false);
            return;
        }

        if (cabUrl.empty()) {
            const bool isStandardStorageOrChipset =
                ContainsCaseInsensitive(deviceName, "AHCI") ||
                ContainsCaseInsensitive(deviceName, "NVM Express") ||
                ContainsCaseInsensitive(primaryKey, "VEN_1022") ||
                ContainsCaseInsensitive(primaryKey, "VEN_8086");
            if (isStandardStorageOrChipset) {
                bool usesOemInf = false;
                for (const auto& d : devs) {
                    if (ToLowerAscii(d.searchHardwareKey) == ToLowerAscii(primaryKey) ||
                        ContainsCaseInsensitive(d.hardwareId, primaryKey)) {
                        if (ToLowerAscii(d.infName).rfind("oem", 0) == 0) {
                            usesOemInf = true;
                            break;
                        }
                    }
                }
                if (usesOemInf) {
                    SetPhaseMessage("Восстановление системного драйвера Windows...");
                    if (!stopRequested_.load() && log) {
                        log(LogLevel::Info,
                            "В Microsoft Update Catalog нет .cab для «" + deviceName +
                                "» — выполняется переключение на актуальный системный драйвер Windows...");
                    }
                    bool inboxOk = false;
                    bool inboxReboot = false;
                    if (IsProcessElevated()) {
                        inboxOk = DeviceScanner::RestoreInboxDriverByHardwareKeySync(
                            primaryKey, deviceName, log, &inboxReboot);
                    } else {
                        std::error_code ec;
                        const std::filesystem::path tempLog = CreateTempLogPath(L"hdm_inbox");
                        const std::wstring cliArgs =
                            L"--cli-restore-inbox " + QuoteWin32Arg(Utf8ToWide(primaryKey)) +
                            L" --log " + QuoteWin32Arg(tempLog.wstring());
                        const ElevatedRunResult run =
                            ExecuteElevatedSelf(cliArgs, true, &tempLog, log, nullptr, &stopRequested_);
                        std::filesystem::remove(tempLog, ec);
                        if (!run.launched) {
                            if (run.launchError == ERROR_CANCELLED) {
                                cancelInstallRequested_.store(true);
                                if (!stopRequested_.load() && log) {
                                    log(LogLevel::Warning,
                                        "Восстановление системного драйвера отменено (UAC).");
                                }
                            } else if (!stopRequested_.load() && log) {
                                log(LogLevel::Error,
                                    "Ошибка запуска UAC: " + FormatWin32Error(run.launchError));
                            }
                        } else {
                            inboxOk = (run.exitCode == 0 || run.exitCode == 2);
                            inboxReboot = (run.exitCode == 2);
                        }
                    }
                    if (inboxOk) {
                        SetPhaseMessage("Восстановлен системный драйвер Windows");
                        progress_.store(1.0f);
                        installing_.store(false);
                        if (!stopRequested_.load() && cb) cb(true, inboxReboot);
                        return;
                    }
                }

                if (!stopRequested_.load() && log) {
                    log(LogLevel::Info,
                        "Для «" + deviceName + "» (" + primaryKey +
                            ") отдельный .cab-пакет в Microsoft Update Catalog не выпускается — устройство использует стандартный драйвер Windows / пакета чипсета" +
                            (installedVersion.empty() ? std::string(".")
                                                      : (" (текущая версия " + installedVersion + ").")));
                }
                SetPhaseMessage("Используется стандартный драйвер Windows / чипсета");
            } else {
                if (!stopRequested_.load() && log) {
                    log(LogLevel::Warning,
                        "В Microsoft Update Catalog не найден .cab-пакет драйвера для «" +
                            deviceName + "» (" + primaryKey + ").");
                }
                SetPhaseMessage("Драйвер в Microsoft Update Catalog не найден");
            }
            progress_.store(1.0f);
            installing_.store(false);
            if (!stopRequested_.load() && cb) cb(false, false);
            return;
        }

        if (!stopRequested_.load() && log) {
            std::string note;
            if (!installedVersion.empty() && !catVer.empty() &&
                !IsVersionNewer(catVer, installedVersion)) {
                note = " (в системе уже установлена версия " + installedVersion + ")";
            }
            log(LogLevel::Info,
                "Найден WHQL-пакет в Microsoft Update Catalog: версия " + catVer + note +
                    " — загрузка и установка (.cab)...");
        }

        SystemComponentItem tempItem;
        tempItem.id = "catalog_autoinstall";
        tempItem.name = deviceName + " (WHQL " + catVer + ")";
        tempItem.directInstallerUrl = cabUrl;

        bool rebootReq = false;
        const bool ok = InstallSingleComponentSync(
            tempItem, 0.15f, 0.85f, devs, log, &rebootReq);

        if (!stopRequested_.load()) {
            SetPhaseMessage(ok ? ("Установлен драйвер " + catVer + " для " + deviceName)
                               : "Ошибка установки драйвера из Catalog");
        }
        progress_.store(1.0f);
        installing_.store(false);
        if (!stopRequested_.load() && cb) {
            cb(ok, rebootReq);
        }
    });
}

std::vector<SystemComponentItem> SystemComponentsScanner::ScanLocalSynchronously(
    const std::vector<DeviceInfo>& devicesSnapshot) {
    std::vector<SystemComponentItem> result;
    const auto apps = CollectAllInstalledApps();
    const std::filesystem::path sys32 = GetSystemDirPath(false);
    const std::filesystem::path sysWow = GetSystemDirPath(true);
    const std::filesystem::path progFiles = GetEnvPath(L"ProgramFiles");
    const std::filesystem::path progFilesX86 = GetEnvPath(L"ProgramFiles(x86)");

    // 1. Gaming Runtimes & Windows Libraries
    {
        SystemComponentItem vc64;
        vc64.id = "rt_vcredist_x64";
        vc64.category = ComponentCategory::GamingRuntime;
        vc64.name = "Visual C++ 2015–2022 Redistributable (x64)";
        vc64.vendor = "Microsoft";
        vc64.purposeDescription =
            "64-битные библиотеки среды выполнения C/C++ (MSVC). Обязательны для запуска большинства современных игр, лаунчеров и программ на Windows.";
        vc64.hardwareConnected = true;
        vc64.officialPageUrl = "https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist";
        vc64.directInstallerUrl = "https://aka.ms/vs/17/release/vc_redist.x64.exe";
        vc64.wingetRepoPath = "m/Microsoft/VCRedist/2015+/x64";
        vc64.preferredArch = "x64";
        vc64.installerArgs = "/install /passive /norestart";
        vc64.latestVersion = "Поиск...";
        {
            const std::string regVer = CheckVcRuntimeKey(
                L"SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\X64", KEY_WOW64_64KEY);
            const std::string uninstVer = FindBestVcUninstallVersion(apps, "x64");
            vc64.installedVersion =
                IsVersionNewer(uninstVer, regVer) ? uninstVer
                                                  : (!regVer.empty() ? regVer : uninstVer);
        }
        vc64.installed = !vc64.installedVersion.empty();
        vc64.updateAvailable = !vc64.installed;
        vc64.statusText = vc64.installed ? ("Установлен (" + vc64.installedVersion + ")") : "Отсутствует";
        result.push_back(std::move(vc64));

        SystemComponentItem vc86;
        vc86.id = "rt_vcredist_x86";
        vc86.category = ComponentCategory::GamingRuntime;
        vc86.name = "Visual C++ 2015–2022 Redistributable (x86)";
        vc86.vendor = "Microsoft";
        vc86.purposeDescription =
            "32-битные библиотеки среды выполнения C/C++ (MSVC). Необходимы для 32-битных игр, игровых лаунчеров (Steam и др.) и оверлеев.";
        vc86.hardwareConnected = true;
        vc86.officialPageUrl = "https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist";
        vc86.directInstallerUrl = "https://aka.ms/vs/17/release/vc_redist.x86.exe";
        vc86.wingetRepoPath = "m/Microsoft/VCRedist/2015+/x86";
        vc86.preferredArch = "x86";
        vc86.installerArgs = "/install /passive /norestart";
        vc86.latestVersion = "Поиск...";
        {
            std::string regVer = CheckVcRuntimeKey(
                L"SOFTWARE\\WOW6432Node\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\X86",
                KEY_WOW64_32KEY);
            if (regVer.empty()) {
                regVer = CheckVcRuntimeKey(
                    L"SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\X86",
                    KEY_WOW64_32KEY);
            }
            const std::string uninstVer = FindBestVcUninstallVersion(apps, "x86");
            vc86.installedVersion =
                IsVersionNewer(uninstVer, regVer) ? uninstVer
                                                  : (!regVer.empty() ? regVer : uninstVer);
        }
        vc86.installed = !vc86.installedVersion.empty();
        vc86.updateAvailable = !vc86.installed;
        vc86.statusText = vc86.installed ? ("Установлен (" + vc86.installedVersion + ")") : "Отсутствует";
        result.push_back(std::move(vc86));

        SystemComponentItem vc13;
        vc13.id = "rt_vc2013";
        vc13.category = ComponentCategory::GamingRuntime;
        vc13.name = "Visual C++ 2013 Redistributable (x64 / x86)";
        vc13.vendor = "Microsoft";
        vc13.purposeDescription =
            "Библиотеки C++ 2013 (msvcr120.dll / msvcp120.dll). Используются играми прошлых лет (2013–2018 гг.) и старыми игровыми движками.";
        vc13.hardwareConnected = true;
        vc13.officialPageUrl = "https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist";
        vc13.directInstallerUrl = "https://aka.ms/highdpimfc2013x64enu";
        vc13.installerArgs = "/install /passive /norestart";
        vc13.latestVersion = "12.0.40664.0";
        const std::string v13x64 = CheckVcRuntimeKey(
            L"SOFTWARE\\Microsoft\\VisualStudio\\12.0\\VC\\Runtimes\\x64", KEY_WOW64_64KEY);
        const std::string v13x86 = CheckVcRuntimeKey(
            L"SOFTWARE\\WOW6432Node\\Microsoft\\VisualStudio\\12.0\\VC\\Runtimes\\x86", KEY_WOW64_32KEY);
        vc13.installed = !v13x64.empty() || !v13x86.empty();
        vc13.installedVersion = !v13x64.empty() ? v13x64 : v13x86;
        vc13.updateAvailable = !vc13.installed;
        vc13.statusText = vc13.installed ? ("Установлен (" + vc13.installedVersion + ")") : "Не установлен";
        result.push_back(std::move(vc13));

        SystemComponentItem vc12;
        vc12.id = "rt_vc2012";
        vc12.category = ComponentCategory::GamingRuntime;
        vc12.name = "Visual C++ 2012 Redistributable (x64 / x86)";
        vc12.vendor = "Microsoft";
        vc12.purposeDescription =
            "Библиотеки C++ 2012 (msvcr110.dll / msvcp110.dll). Требуются для игр 2012–2016 гг. и ряда системных утилит.";
        vc12.hardwareConnected = true;
        vc12.officialPageUrl = "https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist";
        vc12.directInstallerUrl =
            "https://download.microsoft.com/download/1/6/B/16B06F60-3B20-4FF2-B699-5E9B7962F9AE/VSU_4/vcredist_x64.exe";
        vc12.installerArgs = "/install /passive /norestart";
        vc12.latestVersion = "11.0.61030.0";
        const std::string v12x64 = CheckVcRuntimeKey(
            L"SOFTWARE\\Microsoft\\VisualStudio\\11.0\\VC\\Runtimes\\x64", KEY_WOW64_64KEY);
        const std::string v12x86 = CheckVcRuntimeKey(
            L"SOFTWARE\\WOW6432Node\\Microsoft\\VisualStudio\\11.0\\VC\\Runtimes\\x86", KEY_WOW64_32KEY);
        vc12.installed = !v12x64.empty() || !v12x86.empty();
        vc12.installedVersion = !v12x64.empty() ? v12x64 : v12x86;
        vc12.updateAvailable = !vc12.installed;
        vc12.statusText = vc12.installed ? ("Установлен (" + vc12.installedVersion + ")") : "Не установлен";
        result.push_back(std::move(vc12));

        SystemComponentItem vc10;
        vc10.id = "rt_vc2010";
        vc10.category = ComponentCategory::GamingRuntime;
        vc10.name = "Visual C++ 2010 Redistributable (x64 / x86)";
        vc10.vendor = "Microsoft";
        vc10.purposeDescription =
            "Библиотеки C++ 2010 (msvcr100.dll / msvcp100.dll). Часто требуются для классических игр (2010–2015 гг.) и старых лаунчеров.";
        vc10.hardwareConnected = true;
        vc10.officialPageUrl = "https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist";
        vc10.directInstallerUrl =
            "https://download.microsoft.com/download/1/6/5/165255E7-1014-4D0A-B094-B6A430A6BFFC/vcredist_x64.exe";
        vc10.installerArgs = "/passive /norestart";
        vc10.latestVersion = "10.0.40219";
        const std::string v10x64 = CheckVcRuntimeKey(
            L"SOFTWARE\\Microsoft\\VisualStudio\\10.0\\VC\\VCRedist\\x64", KEY_WOW64_64KEY);
        const std::string v10x86 = CheckVcRuntimeKey(
            L"SOFTWARE\\WOW6432Node\\Microsoft\\VisualStudio\\10.0\\VC\\VCRedist\\x86", KEY_WOW64_32KEY);
        vc10.installed = !v10x64.empty() || !v10x86.empty();
        vc10.installedVersion = !v10x64.empty() ? v10x64 : v10x86;
        vc10.updateAvailable = !vc10.installed;
        vc10.statusText = vc10.installed ? ("Установлен (" + vc10.installedVersion + ")") : "Не установлен";
        result.push_back(std::move(vc10));

        // DirectX End-User Runtime (June 2010)
        SystemComponentItem dx;
        dx.id = "rt_dx_legacy";
        dx.category = ComponentCategory::GamingRuntime;
        dx.name = "DirectX End-User Runtime (June 2010 / D3DX9-11)";
        dx.vendor = "Microsoft";
        dx.purposeDescription =
            "Дополнительные игровые библиотеки DirectX (d3dx9_43.dll, d3dx10_43.dll, d3dx11_43.dll, XInput 1.3, XAudio 2.7), отсутствующие в чистой Windows 10/11.";
        dx.hardwareConnected = true;
        dx.officialPageUrl = "https://www.microsoft.com/en-us/download/details.aspx?id=35";
        dx.directInstallerUrl =
            "https://download.microsoft.com/download/8/4/A/84A35BF1-DAFE-4AE8-82AF-AD2AE20B6B14/directx_Jun2010_redist.exe";
        dx.latestVersion = "9.29.1974";
        std::error_code ec;
        const bool hasD3dx9 = std::filesystem::exists(sys32 / L"d3dx9_43.dll", ec);
        const bool hasD3dx11 = std::filesystem::exists(sys32 / L"d3dx11_43.dll", ec);
        const bool hasXinput = std::filesystem::exists(sys32 / L"xinput1_3.dll", ec);
        const bool hasWowD3dx9 = std::filesystem::exists(sysWow / L"d3dx9_43.dll", ec);
        dx.installed = hasD3dx9 && hasD3dx11 && hasXinput && hasWowD3dx9;
        dx.installedVersion = dx.installed ? "9.29.1974" : (hasD3dx9 ? "Частично" : "");
        dx.updateAvailable = !dx.installed;
        dx.statusText = dx.installed ? "Установлен (d3dx9/10/11)" : "Рекомендуется для игр";
        result.push_back(std::move(dx));

        // .NET Desktop Runtime 8 & 9
        SystemComponentItem dn8;
        dn8.id = "rt_dotnet8";
        dn8.category = ComponentCategory::GamingRuntime;
        dn8.name = ".NET Desktop Runtime 8.0 (x64)";
        dn8.vendor = "Microsoft";
        dn8.purposeDescription =
            "Среда выполнения .NET 8 (LTS) для десктопных программ, игровых мод-менеджеров, оверлеев и системных утилит.";
        dn8.hardwareConnected = true;
        dn8.officialPageUrl = "https://dotnet.microsoft.com/en-us/download/dotnet/8.0";
        dn8.latestVersion = "Поиск...";
        dn8.installedVersion = DetectHighestDotNetDesktop("8");
        dn8.installed = !dn8.installedVersion.empty();
        dn8.statusText = dn8.installed ? ("Установлен (" + dn8.installedVersion + ")") : "Не установлен";
        result.push_back(std::move(dn8));

        SystemComponentItem dn9;
        dn9.id = "rt_dotnet9";
        dn9.category = ComponentCategory::GamingRuntime;
        dn9.name = ".NET Desktop Runtime 9.0 (x64)";
        dn9.vendor = "Microsoft";
        dn9.purposeDescription =
            "Актуальная среда выполнения .NET 9 для современных утилит Windows, лаунчеров, менеджеров модов и инструментов периферии.";
        dn9.hardwareConnected = true;
        dn9.officialPageUrl = "https://dotnet.microsoft.com/en-us/download/dotnet/9.0";
        dn9.latestVersion = "Поиск...";
        dn9.installedVersion = DetectHighestDotNetDesktop("9");
        dn9.installed = !dn9.installedVersion.empty();
        dn9.statusText = dn9.installed ? ("Установлен (" + dn9.installedVersion + ")") : "Не установлен";
        result.push_back(std::move(dn9));

        SystemComponentItem dn10;
        dn10.id = "rt_dotnet10";
        dn10.category = ComponentCategory::GamingRuntime;
        dn10.name = ".NET Desktop Runtime 10.0 (x64)";
        dn10.vendor = "Microsoft";
        dn10.purposeDescription =
            "Актуальная LTS-среда выполнения .NET 10 для новейших программ, утилит Windows, лаунчеров и инструментов настройки.";
        dn10.hardwareConnected = true;
        dn10.officialPageUrl = "https://dotnet.microsoft.com/en-us/download/dotnet/10.0";
        dn10.latestVersion = "Поиск...";
        dn10.installedVersion = DetectHighestDotNetDesktop("10");
        dn10.installed = !dn10.installedVersion.empty();
        dn10.statusText = dn10.installed ? ("Установлен (" + dn10.installedVersion + ")") : "Не установлен";
        result.push_back(std::move(dn10));

        // .NET Framework 4.8+
        SystemComponentItem netFx;
        netFx.id = "rt_netfx48";
        netFx.category = ComponentCategory::GamingRuntime;
        netFx.name = ".NET Framework 4.8 / 4.8.1";
        netFx.vendor = "Microsoft";
        netFx.purposeDescription =
            "Классическая платформа .NET в составе Windows. Требуется для большого числа игр, конфигураторов оборудования и драйверных панелей.";
        netFx.hardwareConnected = true;
        netFx.officialPageUrl = "https://dotnet.microsoft.com/en-us/download/dotnet-framework";
        netFx.directInstallerUrl = "https://go.microsoft.com/fwlink/?LinkId=2203304";
        netFx.installerArgs = "/passive /norestart";
        netFx.latestVersion = "4.8.1";
        netFx.compareVersions = false;
        {
            RegKeyGuard key;
            DWORD release = 0;
            if (RegOpenKeyExW(
                    HKEY_LOCAL_MACHINE,
                    L"SOFTWARE\\Microsoft\\NET Framework Setup\\NDP\\v4\\Full",
                    0,
                    KEY_READ,
                    key.Put()) == ERROR_SUCCESS) {
                ReadRegDword(key.Get(), L"Release", release);
                netFx.installedVersion = WideToUtf8(ReadRegStr(key.Get(), L"Version"));
            }
            if (release >= 533320) {
                netFx.installed = true;
                netFx.updateAvailable = false;
                if (netFx.installedVersion.empty()) netFx.installedVersion = "4.8.1";
                netFx.statusText = "Актуален (4.8.1)";
            } else if (release >= 528040) {
                netFx.installed = true;
                netFx.updateAvailable = true;
                if (netFx.installedVersion.empty()) netFx.installedVersion = "4.8";
                netFx.statusText = "Доступно обновление (4.8.1)";
            } else {
                netFx.installed = false;
                netFx.updateAvailable = true;
                netFx.statusText = "Требуется установка (4.8.1)";
            }
        }
        result.push_back(std::move(netFx));

        // WebView2 Runtime
        SystemComponentItem wv2;
        wv2.id = "rt_webview2";
        wv2.category = ComponentCategory::GamingRuntime;
        wv2.name = "Microsoft Edge WebView2 Runtime";
        wv2.vendor = "Microsoft";
        wv2.purposeDescription =
            "Встраиваемый браузерный движок Microsoft. Нужен для работы современных лаунчеров, меню настроек периферии и виджетов Windows 11.";
        wv2.hardwareConnected = true;
        wv2.officialPageUrl = "https://developer.microsoft.com/en-us/microsoft-edge/webview2/";
        wv2.directInstallerUrl = "https://go.microsoft.com/fwlink/p/?LinkId=2124703";
        wv2.installerArgs = "/silent /install";
        wv2.latestVersion = "Поиск...";
        {
            RegKeyGuard key;
            const wchar_t* wvSubKey =
                L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, wvSubKey, 0, KEY_READ | KEY_WOW64_32KEY, key.Put()) ==
                ERROR_SUCCESS) {
                wv2.installedVersion = WideToUtf8(ReadRegStr(key.Get(), L"pv"));
                wv2.installed = !wv2.installedVersion.empty();
            }
        }
        wv2.updateAvailable = !wv2.installed;
        wv2.statusText = wv2.installed ? ("Установлен (" + wv2.installedVersion + ")") : "Отсутствует";
        result.push_back(std::move(wv2));

        // Vulkan Runtime
        SystemComponentItem vk;
        vk.id = "rt_vulkan";
        vk.category = ComponentCategory::GamingRuntime;
        vk.name = "Vulkan Runtime (vulkan-1.dll)";
        vk.vendor = "LunarG / Khronos";
        vk.purposeDescription =
            "Системный загрузчик графического API Vulkan. Отвечает за запуск игр на движке Vulkan, эмуляторов и трансляторов DXVK.";
        vk.hardwareConnected = true;
        vk.officialPageUrl = "https://vulkan.lunarg.com/sdk/home";
        vk.latestVersion = "Поиск...";
        {
            const std::string v64 = GetFileVersionUtf8(sys32 / L"vulkan-1.dll");
            const std::string v32 = GetFileVersionUtf8(sysWow / L"vulkan-1.dll");
            vk.installedVersion = IsVersionNewer(v32, v64) ? v32 : (!v64.empty() ? v64 : v32);
        }
        vk.installed = !vk.installedVersion.empty();
        vk.statusText = vk.installed ? ("Установлен (" + vk.installedVersion + ")") : "Не найден";
        result.push_back(std::move(vk));

        // OpenAL
        SystemComponentItem oal;
        oal.id = "rt_openal";
        oal.category = ComponentCategory::GamingRuntime;
        oal.name = "OpenAL Audio Library (OpenAL32.dll)";
        oal.vendor = "OpenAL";
        oal.purposeDescription =
            "Библиотека пространственного 3D-звука OpenAL. Нужна для корректной работы звука в классических и инди-играх.";
        oal.hardwareConnected = true;
        oal.officialPageUrl = "https://www.openal.org/downloads/";
        oal.directInstallerUrl = "https://www.openal.org/downloads/oalinst.zip";
        oal.installerArgs = "/s";
        oal.latestVersion = "1.1 (OpenAL.org)";
        oal.compareVersions = false;
        oal.installedVersion = GetFileVersionUtf8(sys32 / L"OpenAL32.dll");
        if (oal.installedVersion.empty()) {
            oal.installedVersion = GetFileVersionUtf8(sysWow / L"OpenAL32.dll");
        }
        oal.installed = !oal.installedVersion.empty();
        oal.updateAvailable = !oal.installed;
        oal.statusText = oal.installed ? ("Установлен (" + oal.installedVersion + ")") : "Не установлен";
        result.push_back(std::move(oal));
    }

    // 2. Detected Peripherals mapped to official vendor software
    {
        std::map<std::string, std::vector<std::string>> brandToDevices;
        for (const auto& d : devicesSnapshot) {
            if (!d.peripheralBrand.empty()) {
                auto& list = brandToDevices[d.peripheralBrand];
                if (std::find(list.begin(), list.end(), d.name) == list.end() && list.size() < 3) {
                    list.push_back(d.name);
                }
            }
        }

        for (const auto& rule : kPeripheralRules) {
            auto it = brandToDevices.find(rule.brand);
            const bool connected = (it != brandToDevices.end());
            const InstalledAppEntry* app = FindInstalledApp(
                apps, {rule.uninstallKeywords[0], rule.uninstallKeywords[1], rule.uninstallKeywords[2]});

            // File-path fallbacks when a vendor installer updates/registers without standard Uninstall metadata
            std::string fallbackVer;
            if (app == nullptr) {
                if (std::string_view(rule.id) == "sw_logitech" && !progFiles.empty()) {
                    fallbackVer = GetFileVersionUtf8(progFiles / L"LGHUB" / L"lghub.exe");
                } else if (std::string_view(rule.id) == "sw_steelseries" && !progFiles.empty()) {
                    fallbackVer = GetFileVersionUtf8(
                        progFiles / L"SteelSeries" / L"GG" / L"SteelSeriesGG.exe");
                } else if (std::string_view(rule.id) == "sw_razer") {
                    if (!progFiles.empty()) {
                        fallbackVer = GetFileVersionUtf8(
                            progFiles / L"Razer" / L"RazerAppEngine" / L"RazerAppEngine.exe");
                    }
                    if (fallbackVer.empty() && !progFilesX86.empty()) {
                        fallbackVer = GetFileVersionUtf8(
                            progFilesX86 / L"Razer" / L"Synapse3" / L"WPFUI" / L"Framework" /
                            L"Razer Synapse 3 Host" / L"Razer Synapse 3.exe");
                    }
                }
            }

            if (!connected && app == nullptr && fallbackVer.empty()) {
                continue;
            }

            SystemComponentItem sw;
            sw.id = rule.id;
            sw.category = ComponentCategory::PeripheralSoftware;
            sw.name = rule.softwareName;
            sw.vendor = rule.brand;
            sw.purposeDescription = rule.purpose;
            sw.officialPageUrl = rule.pageUrl;
            if (rule.directUrl != nullptr) {
                sw.directInstallerUrl = rule.directUrl;
            }
            if (rule.wingetPath != nullptr) {
                sw.wingetRepoPath = rule.wingetPath;
                sw.preferredArch = "x64";
            }
            if (rule.nestedHint != nullptr) {
                sw.nestedInstallerHint = rule.nestedHint;
            }
            if (rule.silentArgs != nullptr) {
                sw.installerArgs = rule.silentArgs;
            }
            if (std::string_view(rule.id) == "sw_razer") {
                sw.compareVersions = false;
            }
            sw.latestVersion = rule.defaultLatestLabel;
            sw.hardwareConnected = connected;
            if (connected) {
                for (size_t i = 0; i < it->second.size(); ++i) {
                    if (i > 0) sw.detectedHardware += ", ";
                    sw.detectedHardware += it->second[i];
                }
            } else {
                sw.detectedHardware = "Обнаружено в системе";
            }

            if (app != nullptr) {
                sw.installed = true;
                sw.installedVersion = app->displayVersion.empty() ? "Установлен" : app->displayVersion;
                sw.statusText = "Установлен (" + sw.installedVersion + ")";
            } else if (!fallbackVer.empty()) {
                sw.installed = true;
                sw.installedVersion = fallbackVer;
                sw.statusText = "Установлен (" + fallbackVer + ")";
            } else {
                sw.installed = false;
                sw.updateAvailable = true;
                sw.statusText = "ПО не установлено";
            }
            result.push_back(std::move(sw));
        }
    }

    // 3. Monitors (via EDID) & GPUs
    {
        std::set<std::string> seenMonitors;
        for (const auto& d : devicesSnapshot) {
            if (d.category != DeviceCategory::Monitor) {
                continue;
            }
            const std::string key = d.edidManufacturer + "|" + d.edidModel + "|" + d.name;
            if (!seenMonitors.insert(key).second) {
                continue;
            }
            const MonitorBrandInfo brandInfo = GetMonitorBrandSupport(d.edidManufacturer, d.name);
            const InstalledAppEntry* app = brandInfo.uninstallKeyword
                                               ? FindInstalledApp(apps, {brandInfo.uninstallKeyword})
                                               : nullptr;

            SystemComponentItem mon;
            mon.id = "mon_" + std::to_string(seenMonitors.size());
            mon.category = ComponentCategory::MonitorAndGpu;
            mon.name = brandInfo.softwareName;
            mon.vendor = d.manufacturer.empty() ? d.edidManufacturer : d.manufacturer;
            mon.purposeDescription =
                "Фирменная утилита и INF-драйвер монитора: отвечает за точный цветовой профиль (ICC), управление яркостью/режимами OSD с ПК и поддержку высокой герцовки/HDR.";
            mon.detectedHardware = d.name;
            if (!d.driverVersion.empty() && !d.isGenericMonitorDriver) {
                mon.detectedHardware += " [INF " + d.driverVersion + "]";
            }
            mon.officialPageUrl = brandInfo.url;
            if (brandInfo.fallbackDirectUrl != nullptr) {
                mon.directInstallerUrl = brandInfo.fallbackDirectUrl;
            }
            if (brandInfo.fallbackNestedHint != nullptr) {
                mon.nestedInstallerHint = brandInfo.fallbackNestedHint;
            }
            if (brandInfo.fallbackArgs != nullptr) {
                mon.installerArgs = brandInfo.fallbackArgs;
            }
            if (brandInfo.wingetPath != nullptr) {
                mon.wingetRepoPath = brandInfo.wingetPath;
                mon.preferredArch = "x64";
                mon.latestVersion = "Поиск...";
            } else {
                mon.latestVersion = "Портал поддержки";
            }
            mon.hardwareConnected = true;
            mon.installed = !d.isGenericMonitorDriver || (app != nullptr);
            if (app != nullptr && !app->displayVersion.empty()) {
                mon.installedVersion = app->displayVersion;
                mon.compareVersions = true;
                mon.statusText = "Установлено (" + app->displayVersion + ")";
            } else if (!d.isGenericMonitorDriver) {
                mon.installedVersion = d.driverVersion.empty() ? d.infName : (d.driverVersion + " (INF)");
                mon.compareVersions = false;
                mon.statusText = "Вендор-драйвер (" + (d.driverVersion.empty() ? d.infName : d.driverVersion) + ")";
            } else if (app != nullptr) {
                mon.installedVersion = app->displayVersion;
                mon.compareVersions = false;
                mon.statusText = "Утилита установлена (" + d.infName + ")";
            } else {
                mon.installedVersion = d.driverVersion + " (" + d.infName + ")";
                mon.compareVersions = false;
                mon.updateAvailable = true;
                mon.statusText = "Базовый драйвер (monitor.inf)";
            }
            result.push_back(std::move(mon));
        }

        bool hasNvidia = false, hasAmd = false, hasIntelGpu = false;
        bool hasAmdChipset = false, hasIntelChipset = false, hasIntelNetOrBt = false;
        std::string nvName, nvInfVer, amdName, intelDsaSummary;
        std::string amdChipsetVer, amdChipsetInfName;

        auto appendSummary = [](std::string& dst, const std::string& part) {
            if (part.empty()) return;
            if (!dst.empty()) dst += "; ";
            if (dst.size() < 110) dst += part;
        };

        for (const auto& d : devicesSnapshot) {
            const std::string cls = ToLowerAscii(d.deviceClass);
            if (cls == "display") {
                if (d.pciVendorId == "10DE") {
                    hasNvidia = true;
                    nvName = d.name + " [" + d.driverVersion + "]";
                    nvInfVer = FormatNvidiaDriverVersion(d.driverVersion);
                } else if (d.pciVendorId == "1002") {
                    hasAmd = true;
                    amdName = d.name + " [" + d.driverVersion + "]";
                } else if (d.pciVendorId == "8086") {
                    hasIntelGpu = true;
                    appendSummary(intelDsaSummary, d.name + " [" + d.driverVersion + "]");
                }
            } else if (cls == "system" || cls == "hdc" || cls == "usb") {
                if (d.pciVendorId == "1022") {
                    hasAmdChipset = true;
                    if (ContainsCaseInsensitive(d.driverProvider, "Advanced Micro Devices") &&
                        (ContainsCaseInsensitive(d.name, "SMBus") ||
                         ContainsCaseInsensitive(d.name, "AMD PCI") ||
                         ContainsCaseInsensitive(d.name, "GPIO"))) {
                        amdChipsetVer = d.driverVersion;
                        amdChipsetInfName = d.name;
                    }
                } else if (d.pciVendorId == "8086" &&
                           (ContainsCaseInsensitive(d.name, "Chipset") ||
                            ContainsCaseInsensitive(d.name, "SMBus") ||
                            ContainsCaseInsensitive(d.name, "LPC") ||
                            ContainsCaseInsensitive(d.name, "Management Engine"))) {
                    hasIntelChipset = true;
                    appendSummary(intelDsaSummary, d.name);
                }
            } else if ((cls == "net" || cls == "bluetooth") &&
                       (d.pciVendorId == "8086" || d.usbVendorId == "8087")) {
                hasIntelNetOrBt = true;
                appendSummary(intelDsaSummary, d.name);
            }
        }

        if (hasNvidia) {
            const InstalledAppEntry* nvApp = FindInstalledApp(
                apps, {"NVIDIA Графический драйвер", "NVIDIA Graphics Driver", "NVIDIA App"});
            SystemComponentItem gpu;
            gpu.id = "gpu_nvidia";
            gpu.category = ComponentCategory::MonitorAndGpu;
            gpu.name = "NVIDIA GeForce Game Ready Driver";
            gpu.vendor = "NVIDIA";
            gpu.purposeDescription =
                "Официальный драйвер видеокарты NVIDIA: отвечает за FPS и совместимость в новых играх, работу DLSS, G-SYNC, NVENC и аппаратное ускорение Windows.";
            gpu.detectedHardware = nvName;
            gpu.officialPageUrl = "https://www.nvidia.com/en-us/geforce/drivers/";
            gpu.latestVersion = "Поиск...";
            gpu.hardwareConnected = true;
            gpu.installed = !nvInfVer.empty() || (nvApp != nullptr);
            gpu.installedVersion = !nvInfVer.empty() ? nvInfVer : (nvApp ? nvApp->displayVersion : "");
            gpu.statusText = gpu.installed ? ("Установлен (" + gpu.installedVersion + ")") : "Не найден";
            result.push_back(std::move(gpu));
        }
        if (hasAmd) {
            const InstalledAppEntry* amdApp = FindInstalledApp(
                apps, {"AMD Software", "Radeon Software", "AMD Radeon"});
            SystemComponentItem gpu;
            gpu.id = "gpu_amd";
            gpu.category = ComponentCategory::MonitorAndGpu;
            gpu.name = "AMD Software: Adrenalin Edition";
            gpu.vendor = "AMD";
            gpu.purposeDescription =
                "Официальный пакет драйверов AMD Radeon: отвечает за игровую производительность, FSR, Anti-Lag, FreeSync и аппаратное ускорение.";
            gpu.detectedHardware = amdName;
            gpu.officialPageUrl = "https://www.amd.com/en/support/download/drivers.html";
            gpu.latestVersion = "Поиск...";
            gpu.hardwareConnected = true;
            gpu.installed = (amdApp != nullptr);
            gpu.installedVersion = amdApp ? amdApp->displayVersion : "";
            gpu.statusText = amdApp ? ("Установлен (" + gpu.installedVersion + ")") : "Официальный портал AMD";
            result.push_back(std::move(gpu));
        }
        if (hasIntelGpu || hasIntelChipset || hasIntelNetOrBt) {
            const InstalledAppEntry* intelApp = FindInstalledApp(
                apps, {"Intel Driver & Support", "Intel Graphics", "Intel(R) Chipset Device Software"});
            SystemComponentItem intelDsa;
            intelDsa.id = "intel_dsa";
            intelDsa.category = ComponentCategory::MonitorAndGpu;
            intelDsa.name = "Intel Driver & Support Assistant";
            intelDsa.vendor = "Intel";
            intelDsa.purposeDescription =
                "Официальный инструмент Intel для подбора и обновления драйверов графики, Wi-Fi, Bluetooth и чипсета Intel.";
            intelDsa.detectedHardware =
                intelDsaSummary.empty() ? "Контроллеры Intel" : intelDsaSummary;
            intelDsa.officialPageUrl = "https://www.intel.com/content/www/us/en/support/detect.html";
            intelDsa.directInstallerUrl = "https://dsadata.intel.com/installer";
            intelDsa.wingetRepoPath = "i/Intel/IntelDriverAndSupportAssistant";
            intelDsa.preferredArch = "x64";
            intelDsa.latestVersion = "Поиск...";
            intelDsa.hardwareConnected = true;
            intelDsa.installed = (intelApp != nullptr);
            intelDsa.installedVersion = intelApp ? intelApp->displayVersion : "";
            intelDsa.statusText = intelApp
                ? ("Установлен (" + intelDsa.installedVersion + ")")
                : "Официальный помощник Intel";
            result.push_back(std::move(intelDsa));
        }

        if (hasAmdChipset) {
            const InstalledAppEntry* amdChipApp = FindInstalledApp(
                apps, {"AMD Chipset Software", "AMD_Chipset_Drivers", "AMD Chipset Drivers"});
            SystemComponentItem chip;
            chip.id = "chip_amd";
            chip.category = ComponentCategory::MonitorAndGpu;
            chip.name = "AMD Chipset Drivers (Чипсет / PCI / PSP / SMBus)";
            chip.vendor = "AMD";
            chip.purposeDescription =
                "Официальный пакет драйверов чипсета AMD Ryzen / AM4 / AM5 (энергосбережение, USB, PCIe, GPIO, PSP).";
            chip.detectedHardware = "Чипсет AMD (PCI\\VEN_1022)";
            chip.officialPageUrl =
                "https://www.amd.com/en/support/downloads/drivers.html/chipsets/am4/b550.html";
            chip.directInstallerUrl =
                "https://drivers.amd.com/drivers/amd_chipset_software_8.09.28.1124.exe";
            chip.latestVersion = "Поиск...";
            chip.hardwareConnected = true;
            chip.installed = (amdChipApp != nullptr) || !amdChipsetVer.empty();
            chip.compareVersions = (amdChipApp != nullptr && !amdChipApp->displayVersion.empty());
            if (amdChipApp && !amdChipApp->displayVersion.empty()) {
                chip.installedVersion = amdChipApp->displayVersion;
            } else if (!amdChipsetVer.empty()) {
                chip.installedVersion = amdChipsetVer;
            }
            if (chip.installed) {
                if (amdChipApp) {
                    chip.statusText = "Установлен (" + chip.installedVersion + ")";
                } else {
                    chip.statusText = "Драйверы AMD (" + chip.installedVersion +
                        (!amdChipsetInfName.empty() ? ", " + amdChipsetInfName : "") + ")";
                }
            } else {
                chip.statusText = "Рекомендуется пакет чипсета AMD";
            }
            result.push_back(std::move(chip));
        }

        // Physical LAN, Wi-Fi, Bluetooth, and Onboard Audio controllers resolved via MS Update Catalog (.cab)
        std::set<std::string> seenControllers;
        for (const auto& d : devicesSnapshot) {
            if (d.searchHardwareKey.empty()) {
                continue;
            }
            const bool isVirtual =
                ContainsCaseInsensitive(d.name, "WAN Miniport") ||
                ContainsCaseInsensitive(d.name, "VirtualBox") ||
                ContainsCaseInsensitive(d.name, "Radmin") ||
                ContainsCaseInsensitive(d.name, "VMware") ||
                ContainsCaseInsensitive(d.name, "Hyper-V") ||
                ContainsCaseInsensitive(d.name, "Kernel Debug") ||
                ContainsCaseInsensitive(d.name, "TAP-") ||
                ContainsCaseInsensitive(d.name, "Loopback") ||
                ContainsCaseInsensitive(d.name, "SteelSeries Sonar") ||
                ContainsCaseInsensitive(d.name, "NVIDIA Broadcast") ||
                ContainsCaseInsensitive(d.name, "NVIDIA Virtual Audio");
            if (isVirtual) {
                continue;
            }

            const std::string cls = ToLowerAscii(d.deviceClass);
            const bool isPci = ContainsCaseInsensitive(d.hardwareId, "PCI\\VEN_");
            const bool isUsb = ContainsCaseInsensitive(d.hardwareId, "USB\\VID_");

            std::string idPrefix;
            std::string rolePrefix;
            std::string purpose;
            if (cls == "net" && isPci) {
                const bool isWifi =
                    ContainsCaseInsensitive(d.name, "Wi-Fi") ||
                    ContainsCaseInsensitive(d.name, "Wireless") ||
                    ContainsCaseInsensitive(d.name, "WLAN") ||
                    ContainsCaseInsensitive(d.name, "AX");
                idPrefix = isWifi ? "drv_wifi_" : "drv_lan_";
                rolePrefix = isWifi ? "Wi-Fi адаптер: " : "Сеть (LAN): ";
                purpose = isWifi
                    ? "Официальный WHQL-драйвер беспроводного контроллера из Microsoft Update Catalog (.cab)."
                    : "Официальный WHQL-драйвер проводного сетевого контроллера из Microsoft Update Catalog (.cab).";
            } else if (cls == "bluetooth" && (isPci || isUsb) &&
                       !ContainsCaseInsensitive(d.name, "Enumerator")) {
                idPrefix = "drv_bt_";
                rolePrefix = "Bluetooth: ";
                purpose = "Официальный WHQL-драйвер адаптера Bluetooth из Microsoft Update Catalog (.cab).";
            } else if (cls == "media" &&
                       (ContainsCaseInsensitive(d.hardwareId, "HDAUDIO\\") ||
                        ContainsCaseInsensitive(d.hardwareId, "INTELAUDIO\\")) &&
                       d.pciVendorId != "10DE") {
                idPrefix = "drv_audio_";
                rolePrefix = "Аудио платы: ";
                purpose = "Официальный WHQL-драйвер встроенного аудиокодека платы из Microsoft Update Catalog (.cab).";
            } else {
                continue;
            }

            const std::string itemId = idPrefix + d.searchHardwareKey;
            if (!seenControllers.insert(itemId).second) {
                continue;
            }

            const std::string subsysKey = ExtractVenDevSubsysKey(d.hardwareId);
            const bool isAudio = (idPrefix == "drv_audio_");

            SystemComponentItem drv;
            drv.id = itemId;
            drv.category = ComponentCategory::MonitorAndGpu;
            drv.name = rolePrefix + d.name;
            drv.vendor = d.manufacturer.empty() ? "OEM / Microsoft Catalog" : d.manufacturer;
            drv.purposeDescription = purpose;
            drv.detectedHardware =
                (!subsysKey.empty() ? subsysKey : d.searchHardwareKey) +
                (d.infName.empty() ? "" : (" (" + d.infName + ")"));
            drv.catalogSearchKey = !subsysKey.empty() ? subsysKey : d.searchHardwareKey;
            drv.catalogFallbackKey =
                (!subsysKey.empty() && !isAudio) ? d.searchHardwareKey : std::string();
            drv.officialPageUrl =
                "https://www.catalog.update.microsoft.com/Search.aspx?q=" +
                UrlEncodeCatalogParam(drv.catalogSearchKey);
            drv.latestVersion = "Поиск...";
            drv.hardwareConnected = true;
            drv.installed = d.hasDriver && !d.driverVersion.empty();
            drv.installedVersion = d.driverVersion;
            drv.installedDateYmd = ParseDriverDateYmd(d.driverDate);
            if (drv.installedDateYmd == 0) {
                TryExtractTrailingVersionDate(ParseVersionParts(d.driverVersion), drv.installedDateYmd);
            }
            drv.updateAvailable = !drv.installed;
            drv.statusText = drv.installed
                ? ("Установлен (" + drv.installedVersion + ")")
                : "Драйвер не установлен";
            result.push_back(std::move(drv));
        }
    }

    return result;
}

} // namespace hdm
