#include "core/SystemComponentsScanner.hpp"

#include <oleauto.h>
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
    std::wstring buf(bytes / sizeof(wchar_t) + 1, L'\0');
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

const InstalledAppEntry* FindInstalledApp(
    const std::vector<InstalledAppEntry>& apps,
    std::initializer_list<const char*> keywords) {
    for (const auto& app : apps) {
        for (const char* kw : keywords) {
            if (kw != nullptr && ContainsCaseInsensitive(app.displayName, kw)) {
                return &app;
            }
        }
    }
    return nullptr;
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

std::vector<int> ParseVersionParts(const std::string& ver) {
    std::vector<int> parts;
    int current = 0;
    bool inNum = false;
    for (char ch : ver) {
        if (ch >= '0' && ch <= '9') {
            current = current * 10 + (ch - '0');
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

bool IsVersionNewer(const std::string& latest, const std::string& current) {
    if (latest.empty() || current.empty()) {
        return false;
    }
    const auto a = ParseVersionParts(latest);
    const auto b = ParseVersionParts(current);
    if (a.empty() || b.empty()) {
        return false;
    }
    const size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const int va = i < a.size() ? a[i] : 0;
        const int vb = i < b.size() ? b[i] : 0;
        if (va != vb) {
            return va > vb;
        }
    }
    return false;
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
    GetEnvironmentVariableW(L"ProgramFiles", progFiles, MAX_PATH);
    const std::filesystem::path dir =
        std::filesystem::path(progFiles) / L"dotnet" / L"shared" / L"Microsoft.WindowsDesktop.App";
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        return {};
    }
    std::string best;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const std::string ver = WideToUtf8(entry.path().filename().wstring());
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
    if (wow64) {
        GetSystemWow64DirectoryW(buf, MAX_PATH);
    } else {
        GetSystemDirectoryW(buf, MAX_PATH);
    }
    return std::filesystem::path(buf);
}

std::filesystem::path GetEnvPath(const wchar_t* varName) {
    wchar_t buf[MAX_PATH]{};
    if (GetEnvironmentVariableW(varName, buf, MAX_PATH) == 0) {
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
    const wchar_t* extraHeaders = nullptr) {
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

    if (!WinHttpSendRequest(request.Get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
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

bool DownloadFileViaWinHttp(
    const std::wstring& url,
    const std::filesystem::path& destPath,
    std::atomic<float>* progress,
    std::string* outError) {
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

    ULONGLONG downloaded = 0;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(request.Get(), &avail) && avail > 0) {
        std::vector<char> buf(std::min<DWORD>(avail, 65536));
        DWORD readBytes = 0;
        if (!WinHttpReadData(request.Get(), buf.data(), static_cast<DWORD>(buf.size()), &readBytes) ||
            readBytes == 0) {
            break;
        }
        ofs.write(buf.data(), static_cast<std::streamsize>(readBytes));
        downloaded += readBytes;
        if (progress != nullptr && totalSize > 0) {
            progress->store(std::clamp(static_cast<float>(downloaded) / static_cast<float>(totalSize),
                                       0.0f, 0.94f));
        }
    }
    ofs.close();
    return downloaded > 0;
}

bool ExtractZipViaShellCom(
    const std::filesystem::path& zipPath,
    const std::filesystem::path& destDir) {
    std::error_code ec;
    std::filesystem::create_directories(destDir, ec);

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
    trustData.dwProvFlags = WTD_SAFER_FLAG;

    GUID policyGuid = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &policyGuid, &trustData);

    trustData.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policyGuid, &trustData);

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
    DWORD* outLaunchErr) {
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
        WaitForSingleObject(proc.Get(), INFINITE);
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
                if (!oldItem.directInstallerUrl.empty()) {
                    newItem.directInstallerUrl = oldItem.directInstallerUrl;
                }
                if (!oldItem.installerArgs.empty()) {
                    newItem.installerArgs = oldItem.installerArgs;
                }
            }
            if (!newItem.installed) {
                newItem.updateAvailable = true;
                if (!newItem.latestVersion.empty() &&
                    std::isdigit(static_cast<unsigned char>(newItem.latestVersion[0]))) {
                    newItem.statusText = "Не установлен (" + newItem.latestVersion + ")";
                }
            } else if (!newItem.latestVersion.empty() &&
                       std::isdigit(static_cast<unsigned char>(newItem.latestVersion[0])) &&
                       !newItem.installedVersion.empty()) {
                if (IsVersionNewer(newItem.latestVersion, newItem.installedVersion)) {
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
    const char* defaultLatestLabel;
    const char* uninstallKeywords[3];
};

constexpr PeripheralVendorRule kPeripheralRules[] = {
    {"sw_razer", "Razer", "Razer Synapse (Официальное ПО)",
     "Настройка мышей, клавиатур, гарнитур, веб-камер и микрофонов Razer: DPI, частота опроса, макросы, подсветка Chroma и обновление прошивки.",
     "https://www.razer.com/synapse-4",
     "https://rzr.to/synapse-4-pc-download", "Synapse 4 (онлайн)",
     {"Razer Synapse", "Razer Central", "Razer App Engine"}},
    {"sw_logitech", "Logitech", "Logitech G HUB",
     "Управление периферией Logitech G: настройка сенсора HERO, переназначение кнопок, игровые профили, эквалайзер гарнитур и обновление прошивки.",
     "https://www.logitechg.com/en-us/innovation/g-hub.html",
     "https://download01.logi.com/web/ftp/pub/techsupport/gaming/lghub_installer.exe",
     "Онлайн-установщик",
     {"Logitech G HUB", "Logitech Gaming Software", "LGHUB"}},
    {"sw_steelseries", "SteelSeries", "SteelSeries GG",
     "Официальный пакет SteelSeries: пространственный звук Sonar (игры/чат/микрофон), настройка клавиатур/мышей, OLED-дисплеев и прошивки.",
     "https://steelseries.com/gg",
     "https://steelseries.com/gg/downloads/gg/latest/windows", "Поиск...",
     {"SteelSeries GG", "SteelSeries Engine", nullptr}},
    {"sw_hyperx", "HyperX", "HyperX NGENUITY",
     "Официальное ПО для периферии HyperX: настройка DPI мышей, эквалайзера/микрофона гарнитур, макросов и подсветки.",
     "https://hyperx.com/pages/ngenuity",
     nullptr, "Microsoft Store / сайт",
     {"HyperX NGENUITY", "NGENUITY", nullptr}},
    {"sw_corsair", "Corsair", "Corsair iCUE",
     "Центр управления устройствами Corsair: профили клавиатур и мышей, синхронизация подсветки, мониторинг и обновление прошивки.",
     "https://www.corsair.com/us/en/s/icue",
     nullptr, "Сайт Corsair",
     {"CORSAIR iCUE", "Corsair Utility Engine", nullptr}},
    {"sw_asus", "ASUS ROG", "ASUS Armoury Crate",
     "Фирменный софт ASUS ROG: настройка клавиатур, мышей, гарнитур, синхронизация Aura Sync и обновление драйверов/прошивок.",
     "https://www.asus.com/supportonly/armoury%20crate/helpdesk_download/",
     nullptr, "Сайт ASUS",
     {"Armoury Crate", "ASUS Framework", nullptr}},
    {"sw_wooting", "Wooting", "Wootility (клавиатуры Wooting)",
     "Конфигуратор аналоговых клавиатур Wooting: настройка Rapid Trigger, точки срабатывания клавиш, профилей и прошивки.",
     "https://wooting.io/wootility",
     nullptr, "Веб / Сайт Wooting",
     {"Wootility", nullptr, nullptr}},
    {"sw_keychron", "Keychron", "Keychron Launcher (веб-конфигуратор)",
     "Веб-конфигуратор клавиатур Keychron (QMK/VIA): переназначение клавиш, макросы, подсветка и обновление прошивки прямо из браузера.",
     "https://launcher.keychron.com/",
     nullptr, "Веб-конфигуратор",
     {"Keychron", nullptr, nullptr}},
    {"sw_glorious", "Glorious", "Glorious CORE",
     "Официальное ПО Glorious: настройка DPI, времени отклика (debounce), подсветки, макросов и прошивки мышей/клавиатур.",
     "https://www.gloriousgaming.com/pages/glorious-core",
     nullptr, "Сайт Glorious",
     {"Glorious CORE", nullptr, nullptr}},
    {"sw_pulsar", "Pulsar", "Pulsar Gaming Gear Software",
     "Утилита настройки мышей и клавиатур Pulsar: частота опроса до 4K/8K, DPI, LoD и обновление прошивки.",
     "https://www.pulsar.gg/pages/downloads",
     nullptr, "Сайт Pulsar",
     {"Pulsar", nullptr, nullptr}},
    {"sw_lamzu", "Lamzu", "Lamzu Software",
     "Официальная утилита Lamzu для настройки параметров сенсора, частоты опроса, макросов и обновления прошивки.",
     "https://lamzu.com/pages/download",
     nullptr, "Сайт Lamzu",
     {"Lamzu", nullptr, nullptr}},
    {"sw_endgame", "Endgame Gear", "Endgame Gear Software",
     "Конфигуратор периферии Endgame Gear: точная настройка DPI, дебаунса, Motion Sync и прошивки.",
     "https://www.endgamegear.com/en-us/downloads",
     nullptr, "Сайт Endgame Gear",
     {"Endgame Gear", nullptr, nullptr}},
    {"sw_creative", "Creative", "Creative App / Sound Blaster",
     "Управление звуковыми картами и гарнитурами Creative: эквалайзер, Acoustic Engine, Scout Mode и драйверы.",
     "https://support.creative.com/",
     nullptr, "Сайт Creative",
     {"Creative App", "Sound Blaster", nullptr}},
    {"sw_epos", "EPOS / Sennheiser", "EPOS Gaming Suite",
     "Настройка игровых гарнитур и ЦАП EPOS / Sennheiser: объемный звук 7.1, шумоподавление микрофона и прошивка.",
     "https://www.eposaudio.com/en/us/gaming/downloads",
     nullptr, "Сайт EPOS",
     {"EPOS Gaming Suite", "Sennheiser Gaming", nullptr}},
    {"sw_bloody", "A4Tech / Bloody", "Bloody Official Software",
     "Официальное ПО для мышей и клавиатур Bloody: профили чувствительности, макросы и настройка отклика.",
     "https://www.bloody.com/en/download.php",
     nullptr, "Сайт Bloody",
     {"Bloody", nullptr, nullptr}},
    {"sw_roccat", "Turtle Beach / ROCCAT", "Turtle Beach Swarm II",
     "Единый центр управления гарнитурами, мышами и клавиатурами Turtle Beach / ROCCAT.",
     "https://www.turtlebeach.com/pages/swarm-ii",
     nullptr, "Сайт Turtle Beach",
     {"Swarm II", "ROCCAT Swarm", nullptr}},
    {"sw_elgato", "Elgato", "Elgato Stream Deck / Wave Link",
     "ПО для микрофонов Wave, контроллеров Stream Deck и карт захвата Elgato: микширование звука и профили сцен.",
     "https://www.elgato.com/us/en/s/downloads",
     nullptr, "Сайт Elgato",
     {"Elgato Stream Deck", "Elgato Wave Link", nullptr}},
};

struct MonitorBrandInfo {
    std::string softwareName;
    std::string url;
    const char* uninstallKeyword;
};

MonitorBrandInfo GetMonitorBrandSupport(const std::string& pnpOrBrand, const std::string& model) {
    if (pnpOrBrand == "MSI" || ContainsCaseInsensitive(model, "MSI")) {
        return {"MSI Gaming Intelligence / Драйвер монитора",
                "https://www.msi.com/support/download", "Gaming Intelligence"};
    }
    if (pnpOrBrand == "GSM" || pnpOrBrand == "LGD" || pnpOrBrand == "LG" ||
        ContainsCaseInsensitive(model, "LG")) {
        return {"LG OnScreen Control / Драйвер монитора",
                "https://www.lg.com/global/support", "OnScreen Control"};
    }
    if (pnpOrBrand == "SAM" || pnpOrBrand == "Samsung" ||
        ContainsCaseInsensitive(model, "Samsung") || ContainsCaseInsensitive(model, "SMS")) {
        return {"Samsung Easy Setting Box / Драйвер монитора",
                "https://www.samsung.com/us/support/downloads/", "Easy Setting Box"};
    }
    if (pnpOrBrand == "ACI" || pnpOrBrand == "AUS" || pnpOrBrand == "ASUS" ||
        ContainsCaseInsensitive(model, "ASUS") || ContainsCaseInsensitive(model, "ROG")) {
        return {"ASUS DisplayWidget Center",
                "https://www.asus.com/support/", "DisplayWidget"};
    }
    if (pnpOrBrand == "DEL" || pnpOrBrand == "Dell" ||
        ContainsCaseInsensitive(model, "DELL") || ContainsCaseInsensitive(model, "Alienware")) {
        return {"Dell Display Manager / Драйвер монитора",
                "https://www.dell.com/support/home", "Dell Display Manager"};
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
                "https://www.philips.com/c-w/support-home", "SmartControl"};
    }
    if (pnpOrBrand == "VSC" || ContainsCaseInsensitive(model, "ViewSonic")) {
        return {"ViewSonic vDisplayManager",
                "https://www.viewsonic.com/us/support", "vDisplayManager"};
    }
    return {"Официальная поддержка монитора",
            "https://www.catalog.update.microsoft.com/Search.aspx?q=" + model, nullptr};
}

} // namespace

SystemComponentsScanner::~SystemComponentsScanner() {
    if (scanThread_.joinable()) scanThread_.join();
    if (onlineThread_.joinable()) onlineThread_.join();
    if (installThread_.joinable()) installThread_.join();
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
    if (scanning_.exchange(true)) {
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
        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            items_ = std::move(list);
        }
        generation_.fetch_add(1);
        SetPhaseMessage("Локальная проверка завершена");
        scanning_.store(false);
        if (log) {
            log(LogLevel::Success,
                "Проверка игровых библиотек, мониторов и ПО периферии завершена.");
        }
        if (autoCheckOnline) {
            StartOnlineVersionCheckAsync(log);
        }
    });
}

void SystemComponentsScanner::RefreshLocalPreserveOnlineAsync(
    std::vector<DeviceInfo> devicesSnapshot) {
    if (scanning_.exchange(true)) {
        return;
    }
    if (scanThread_.joinable()) {
        scanThread_.join();
    }

    scanThread_ = std::thread([this, devs = std::move(devicesSnapshot)]() {
        auto refreshed = ScanLocalSynchronously(devs);
        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            if (!items_.empty()) {
                MergeOnlineMetadataIntoRefreshed(refreshed, items_);
            }
            items_ = std::move(refreshed);
        }
        generation_.fetch_add(1);
        scanning_.store(false);
    });
}

void SystemComponentsScanner::StartOnlineVersionCheckAsync(LogCallback onLog) {
    if (checkingOnline_.exchange(true)) {
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

        // 1. .NET Desktop Runtime 8.0 & 9.0
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
                item.latestVersion = latest;
                item.directInstallerUrl = dlUrl;
                item.installerArgs = "/install /passive /norestart";
                item.onlineChecked = true;
                if (!item.installed) {
                    item.updateAvailable = true;
                    item.statusText = "Не установлен (" + latest + ")";
                } else if (IsVersionNewer(latest, item.installedVersion)) {
                    item.updateAvailable = true;
                    item.statusText = "Доступно обновление (" + latest + ")";
                } else {
                    item.updateAvailable = false;
                    item.statusText = "Актуален (" + item.installedVersion + ")";
                }
            });
        };

        checkDotnet("rt_dotnet8",
                    L"https://dotnetcli.blob.core.windows.net/dotnet/release-metadata/8.0/releases.json");
        checkDotnet("rt_dotnet9",
                    L"https://dotnetcli.blob.core.windows.net/dotnet/release-metadata/9.0/releases.json");

        // 2. Microsoft Edge WebView2 Runtime
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
                item.onlineChecked = true;
                if (!latestWv2.empty()) {
                    item.latestVersion = latestWv2;
                    if (!item.installed) {
                        item.updateAvailable = true;
                        item.statusText = "Не установлен (" + latestWv2 + ")";
                    } else if (IsVersionNewer(latestWv2, item.installedVersion)) {
                        item.updateAvailable = true;
                        item.statusText = "Доступно обновление (" + latestWv2 + ")";
                    } else {
                        item.updateAvailable = false;
                        item.statusText = "Актуален (" + item.installedVersion + ")";
                    }
                } else {
                    item.latestVersion = "Онлайн-установщик";
                }
            });
        }

        // 3. LunarG Vulkan Runtime (official files JSON -> exact Runtime installer URL & version)
        {
            const HttpResponse resp = WinHttpRequest(
                L"https://vulkan.lunarg.com/sdk/files", L"GET", true, 512 * 1024);
            const auto [vkLatest, vkUrl] = ParseLatestVulkanRuntimeFromJson(resp.body);
            updateItem("rt_vulkan", [&](SystemComponentItem& item) {
                item.onlineChecked = true;
                if (!vkLatest.empty() && !vkUrl.empty()) {
                    item.latestVersion = vkLatest;
                    item.directInstallerUrl = vkUrl;
                    item.installerArgs = "";
                    if (!item.installed) {
                        item.updateAvailable = true;
                        item.statusText = "Не найден (" + vkLatest + ")";
                    } else if (IsVersionNewer(vkLatest, item.installedVersion)) {
                        item.updateAvailable = true;
                        item.statusText = "Доступно обновление (" + vkLatest + ")";
                    } else {
                        item.updateAvailable = false;
                        item.statusText = "Актуален (" + item.installedVersion + ")";
                    }
                } else {
                    item.latestVersion = "Сайт LunarG";
                }
            });
        }

        // 4. SteelSeries GG latest version & CDN URL via redirect Location header
        {
            const HttpResponse resp = WinHttpRequest(
                L"https://steelseries.com/gg/downloads/gg/latest/windows", L"HEAD", false);
            if (!resp.locationHeader.empty()) {
                std::string ver;
                const size_t ggPos = resp.locationHeader.find("SteelSeriesGG");
                if (ggPos != std::string::npos) {
                    const size_t numStart = ggPos + 13;
                    const size_t setupPos = resp.locationHeader.find("Setup", numStart);
                    if (setupPos != std::string::npos && setupPos > numStart) {
                        ver = resp.locationHeader.substr(numStart, setupPos - numStart);
                    }
                }
                updateItem("sw_steelseries", [&](SystemComponentItem& item) {
                    item.onlineChecked = true;
                    item.directInstallerUrl = resp.locationHeader;
                    if (!ver.empty()) {
                        item.latestVersion = ver;
                        if (!item.installed) {
                            item.updateAvailable = true;
                            item.statusText = "Не установлено (" + ver + ")";
                        } else if (IsVersionNewer(ver, item.installedVersion)) {
                            item.updateAvailable = true;
                            item.statusText = "Доступно обновление (" + ver + ")";
                        } else {
                            item.updateAvailable = false;
                            item.statusText = "Актуален (" + item.installedVersion + ")";
                        }
                    }
                });
            }
        }

        // 5. NVIDIA GeForce Game Ready WHQL latest driver & direct download URL
        {
            bool hasNvItem = false;
            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                for (const auto& it : items_) {
                    if (it.id == "gpu_nvidia") {
                        hasNvItem = true;
                        break;
                    }
                }
            }
            if (hasNvItem) {
                const HttpResponse resp = WinHttpRequest(
                    L"https://gfwsl.geforce.com/services_toolkit/services/com/nvidia/services/AjaxDriverService.php?func=DriverManualLookup&psid=127&pfid=985&osID=57&languageCode=1033&isWHQL=1&dch=1&upcrd=0&numberOfResults=1",
                    L"GET",
                    true,
                    64 * 1024);
                if (resp.ok && !resp.body.empty()) {
                    const std::string nvVer = ExtractJsonStringField(resp.body, "Version");
                    const std::string nvUrl = ExtractJsonStringField(resp.body, "DownloadURL");
                    updateItem("gpu_nvidia", [&](SystemComponentItem& item) {
                        item.onlineChecked = true;
                        if (!nvVer.empty()) {
                            item.latestVersion = nvVer;
                        }
                        if (!nvUrl.empty()) {
                            item.directInstallerUrl = nvUrl;
                            item.installerArgs = "";
                        }
                        if (!nvVer.empty() && !item.installedVersion.empty()) {
                            if (IsVersionNewer(nvVer, item.installedVersion)) {
                                item.updateAvailable = true;
                                item.statusText = "Доступно обновление (" + nvVer + ")";
                            } else {
                                item.updateAvailable = false;
                                item.statusText = "Актуален (" + item.installedVersion + ")";
                            }
                        }
                    });
                }
            }
        }

        // 6. AMD Software: Adrenalin Edition direct installer check
        {
            bool hasAmdItem = false;
            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                for (const auto& it : items_) {
                    if (it.id == "gpu_amd") {
                        hasAmdItem = true;
                        break;
                    }
                }
            }
            if (hasAmdItem) {
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
                        item.latestVersion = amdVer;
                        if (!item.installed) {
                            item.updateAvailable = true;
                            item.statusText = "Доступен " + amdVer;
                        } else if (IsVersionNewer(amdVer, item.installedVersion)) {
                            item.updateAvailable = true;
                            item.statusText = "Доступно обновление (" + amdVer + ")";
                        } else {
                            item.updateAvailable = false;
                            item.statusText = "Актуален (" + item.installedVersion + ")";
                        }
                    } else {
                        item.latestVersion = "Портал AMD";
                    }
                });
            }
        }

        // 7. Verify VC++ endpoints are reachable
        {
            const HttpResponse r64 =
                WinHttpRequest(L"https://aka.ms/vs/17/release/vc_redist.x64.exe", L"HEAD", false);
            updateItem("rt_vcredist_x64", [&](SystemComponentItem& item) {
                item.onlineChecked = true;
                if (item.latestVersion == "Поиск...") {
                    item.latestVersion = r64.ok ? "Онлайн (aka.ms)" : "Сайт Microsoft";
                }
            });
            const HttpResponse r86 =
                WinHttpRequest(L"https://aka.ms/vs/17/release/vc_redist.x86.exe", L"HEAD", false);
            updateItem("rt_vcredist_x86", [&](SystemComponentItem& item) {
                item.onlineChecked = true;
                if (item.latestVersion == "Поиск...") {
                    item.latestVersion = r86.ok ? "Онлайн (aka.ms)" : "Сайт Microsoft";
                }
            });
        }

        generation_.fetch_add(1);
        SetPhaseMessage("Актуальные версии проверены");
        checkingOnline_.store(false);
        if (log) {
            log(LogLevel::Success, "Онлайн-проверка актуальных версий завершена.");
        }
    });
}

void SystemComponentsScanner::DownloadAndInstallAsync(
    const SystemComponentItem& item,
    std::vector<DeviceInfo> devicesSnapshot,
    LogCallback onLog) {
    if (installing_.exchange(true)) {
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

    progress_.store(0.0f);
    SetPhaseMessage("Скачивание: " + item.name);

    installThread_ = std::thread([this, item, devs = std::move(devicesSnapshot), log = std::move(onLog)]() {
        wchar_t tempDir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempDir);
        const bool isZipPackage = (item.id == "rt_openal");
        const std::filesystem::path destFile =
            std::filesystem::path(tempDir) /
            ("DriverLoom_" + item.id + (isZipPackage ? ".zip" : ".exe"));

        if (log) {
            log(LogLevel::Info,
                "Загрузка официального установщика \"" + item.name + "\"...");
        }

        std::string errMsg;
        if (!DownloadFileViaWinHttp(
                Utf8ToWide(item.directInstallerUrl), destFile, &progress_, &errMsg)) {
            if (log) {
                log(LogLevel::Error, "Ошибка скачивания " + item.name + ": " + errMsg);
            }
            SetPhaseMessage("Ошибка загрузки");
            installing_.store(false);
            return;
        }

        std::filesystem::path targetExe = destFile;
        std::filesystem::path extractDir;

        if (isZipPackage) {
            SetPhaseMessage("Распаковка архива...");
            extractDir = std::filesystem::path(tempDir) / L"DriverLoom_OpenAL";
            std::error_code ec;
            std::filesystem::remove_all(extractDir, ec);
            if (!ExtractZipViaShellCom(destFile, extractDir)) {
                std::filesystem::remove(destFile, ec);
                if (log) {
                    log(LogLevel::Error, "Не удалось распаковать архив " + item.name + ".");
                }
                SetPhaseMessage("Ошибка распаковки");
                installing_.store(false);
                return;
            }
            targetExe.clear();
            for (const auto& entry : std::filesystem::recursive_directory_iterator(extractDir, ec)) {
                if (entry.is_regular_file(ec) && entry.path().extension() == L".exe") {
                    targetExe = entry.path();
                    break;
                }
            }
            if (targetExe.empty()) {
                std::filesystem::remove(destFile, ec);
                std::filesystem::remove_all(extractDir, ec);
                if (log) {
                    log(LogLevel::Error, "В архиве не найден исполняемый файл установщика.");
                }
                SetPhaseMessage("Установщик не найден");
                installing_.store(false);
                return;
            }
        }

        SetPhaseMessage("Проверка цифровой подписи Authenticode...");
        std::string sigErr;
        if (!VerifyAuthenticodeSignature(targetExe, &sigErr)) {
            std::error_code ec;
            std::filesystem::remove(destFile, ec);
            if (!extractDir.empty()) std::filesystem::remove_all(extractDir, ec);
            if (log) {
                log(LogLevel::Error,
                    "Цифровая подпись установщика не прошла проверку (" + sigErr +
                        "). Запуск отменён в целях безопасности.");
            }
            SetPhaseMessage("Подпись не подтверждена");
            installing_.store(false);
            return;
        }

        const std::string dlVer = GetFileVersionUtf8(targetExe);
        if (log) {
            log(LogLevel::Success,
                "Подпись подтверждена" +
                    (dlVer.empty() ? std::string() : " (версия файла: " + dlVer + ")") +
                    ". Запуск установки...");
        }

        SetPhaseMessage("Установка " + item.name + "...");
        progress_.store(0.96f);

        DWORD exitCode = 0;
        DWORD launchErr = 0;
        bool launched = false;

        if (item.id == "rt_dx_legacy") {
            const std::filesystem::path dxExtractDir =
                std::filesystem::path(tempDir) / L"DriverLoom_DX2010";
            std::error_code ec;
            std::filesystem::create_directories(dxExtractDir, ec);
            const std::wstring extractArgs = L"/Q /T:\"" + dxExtractDir.wstring() + L"\"";
            if (LaunchAndWaitProcess(targetExe, extractArgs, &exitCode, &launchErr)) {
                const std::filesystem::path dxSetup = dxExtractDir / L"DXSETUP.exe";
                if (std::filesystem::exists(dxSetup, ec)) {
                    launched = LaunchAndWaitProcess(dxSetup, L"/silent", &exitCode, &launchErr);
                }
            }
            std::filesystem::remove_all(dxExtractDir, ec);
        } else {
            launched = LaunchAndWaitProcess(
                targetExe, Utf8ToWide(item.installerArgs), &exitCode, &launchErr);
        }

        if (!launched) {
            if (log) {
                log(launchErr == ERROR_CANCELLED ? LogLevel::Warning : LogLevel::Error,
                    launchErr == ERROR_CANCELLED
                        ? "Установка отменена пользователем (UAC)."
                        : "Не удалось запустить установщик: " + FormatWin32Error(launchErr));
            }
        } else {
            if (exitCode == 0 || exitCode == 3010) {
                if (log) {
                    log(LogLevel::Success,
                        item.name + " успешно установлен" +
                            (exitCode == 3010 ? " (требуется перезагрузка)." : "."));
                }
            } else {
                if (log) {
                    log(LogLevel::Warning,
                        item.name + " завершился с кодом " + std::to_string(exitCode) + ".");
                }
            }
        }

        std::error_code ec;
        std::filesystem::remove(destFile, ec);
        if (!extractDir.empty()) {
            std::filesystem::remove_all(extractDir, ec);
        }

        auto refreshAndCompare = [&]() {
            auto refreshed = ScanLocalSynchronously(devs);
            std::lock_guard<std::mutex> lock(dataMutex_);
            // If we downloaded a VC++ installer with a known file version, record it.
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
                std::this_thread::sleep_for(std::chrono::seconds(3));
                refreshAndCompare();
            }
        }

        progress_.store(1.0f);
        SetPhaseMessage("Установка завершена");
        installing_.store(false);
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
        netFx.latestVersion = "4.8.1 (Windows)";
        {
            RegKeyGuard key;
            if (RegOpenKeyExW(
                    HKEY_LOCAL_MACHINE,
                    L"SOFTWARE\\Microsoft\\NET Framework Setup\\NDP\\v4\\Full",
                    0,
                    KEY_READ,
                    key.Put()) == ERROR_SUCCESS) {
                DWORD release = 0;
                ReadRegDword(key.Get(), L"Release", release);
                netFx.installedVersion = WideToUtf8(ReadRegStr(key.Get(), L"Version"));
                netFx.installed = (release >= 528040);
            }
        }
        netFx.statusText = netFx.installed
                               ? ("Установлен (" + netFx.installedVersion + ")")
                               : "Требуется обновление";
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
        oal.latestVersion = "Офиц. (OpenAL.org)";
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
            mon.officialPageUrl = brandInfo.url;
            mon.latestVersion = "Портал поддержки";
            mon.hardwareConnected = true;
            mon.installed = !d.isGenericMonitorDriver || (app != nullptr);
            if (!d.isGenericMonitorDriver) {
                mon.installedVersion = d.driverVersion.empty() ? d.infName : d.driverVersion;
                mon.statusText = "Вендор-драйвер (" + mon.installedVersion + ")";
            } else if (app != nullptr) {
                mon.installedVersion = app->displayVersion;
                mon.statusText = "Утилита установлена (" + d.infName + ")";
            } else {
                mon.installedVersion = d.driverVersion + " (" + d.infName + ")";
                mon.updateAvailable = true;
                mon.statusText = "Базовый драйвер (monitor.inf)";
            }
            result.push_back(std::move(mon));
        }

        bool hasNvidia = false, hasAmd = false, hasIntelGpu = false;
        std::string nvName, nvInfVer, amdName, intelName;
        for (const auto& d : devicesSnapshot) {
            if (ToLowerAscii(d.deviceClass) != "display") {
                continue;
            }
            if (d.pciVendorId == "10DE") {
                hasNvidia = true;
                nvName = d.name + " [" + d.driverVersion + "]";
                nvInfVer = FormatNvidiaDriverVersion(d.driverVersion);
            } else if (d.pciVendorId == "1002") {
                hasAmd = true;
                amdName = d.name + " [" + d.driverVersion + "]";
            } else if (d.pciVendorId == "8086") {
                hasIntelGpu = true;
                intelName = d.name + " [" + d.driverVersion + "]";
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
        if (hasIntelGpu) {
            const InstalledAppEntry* intelApp = FindInstalledApp(
                apps, {"Intel Driver & Support", "Intel Graphics"});
            SystemComponentItem gpu;
            gpu.id = "gpu_intel";
            gpu.category = ComponentCategory::MonitorAndGpu;
            gpu.name = "Intel Driver & Support Assistant";
            gpu.vendor = "Intel";
            gpu.purposeDescription =
                "Официальный пакет Intel для обновления драйверов графики Intel Arc / UHD, Wi-Fi, Bluetooth и системных контроллеров.";
            gpu.detectedHardware = intelName;
            gpu.officialPageUrl = "https://www.intel.com/content/www/us/en/support/detect.html";
            gpu.latestVersion = "Портал Intel";
            gpu.hardwareConnected = true;
            gpu.installed = (intelApp != nullptr);
            gpu.installedVersion = intelApp ? intelApp->displayVersion : "";
            gpu.statusText = intelApp ? ("Установлен (" + gpu.installedVersion + ")") : "Официальный портал Intel";
            result.push_back(std::move(gpu));
        }
    }

    return result;
}

} // namespace hdm
