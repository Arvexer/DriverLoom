#include "core/HttpClient.hpp"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <utility>
#include <vector>

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

namespace hdm {

namespace {

thread_local HttpRequestRegistry* t_activeHttpRegistry = nullptr;

void EnforceTls12Or13(HINTERNET sessionHandle) noexcept {
    if (sessionHandle == nullptr) {
        return;
    }
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(sessionHandle, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(sessionHandle, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
}

bool EqualsIgnoreCaseAscii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) {
            return false;
        }
    }
    return true;
}

bool MatchesAllowedHostSuffix(
    std::string_view hostLower,
    const std::vector<std::string_view>& allowedSuffixes) {
    if (allowedSuffixes.empty()) {
        return false;
    }
    for (std::string_view rawSuffix : allowedSuffixes) {
        const std::string suffix = ToLowerAscii(rawSuffix);
        if (suffix.empty()) {
            continue;
        }
        if (hostLower == suffix) {
            return true;
        }
        if (hostLower.size() > suffix.size() &&
            hostLower[hostLower.size() - suffix.size() - 1] == '.' &&
            EqualsIgnoreCaseAscii(
                hostLower.substr(hostLower.size() - suffix.size()), suffix)) {
            return true;
        }
    }
    return false;
}

bool CrackHttpsUrl(
    const std::wstring& url,
    std::wstring& outHost,
    std::wstring& outPath,
    INTERNET_PORT& outPort) {
    URL_COMPONENTS urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t hostBuf[256]{};
    wchar_t pathBuf[2048]{};
    urlComp.lpszHostName = hostBuf;
    urlComp.dwHostNameLength = static_cast<DWORD>(std::size(hostBuf));
    urlComp.lpszUrlPath = pathBuf;
    urlComp.dwUrlPathLength = static_cast<DWORD>(std::size(pathBuf));
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &urlComp) ||
        urlComp.nScheme != INTERNET_SCHEME_HTTPS || hostBuf[0] == L'\0') {
        return false;
    }
    outHost.assign(hostBuf);
    outPath.assign(pathBuf[0] != L'\0' ? pathBuf : L"/");
    outPort = urlComp.nPort;
    return true;
}

std::wstring ResolveRedirectTarget(
    const std::wstring& currentHost,
    INTERNET_PORT currentPort,
    const std::wstring& rawLocation) {
    if (rawLocation.empty()) {
        return {};
    }
    if (_wcsnicmp(rawLocation.c_str(), L"https://", 8) == 0 ||
        _wcsnicmp(rawLocation.c_str(), L"http://", 7) == 0) {
        return rawLocation;
    }
    if (rawLocation.rfind(L"//", 0) == 0) {
        return L"https:" + rawLocation;
    }
    std::wstring origin = L"https://" + currentHost;
    if (currentPort != 0 && currentPort != INTERNET_DEFAULT_HTTPS_PORT) {
        origin += L":" + std::to_wstring(currentPort);
    }
    if (rawLocation.front() == L'/') {
        return origin + rawLocation;
    }
    return origin + L"/" + rawLocation;
}

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

class TrackedWinHttpRequestGuard {
public:
    explicit TrackedWinHttpRequestGuard(HINTERNET h = nullptr) noexcept
        : registry_(t_activeHttpRegistry), handle_(h) {
        if (handle_ != nullptr && registry_ != nullptr && !registry_->Register(handle_)) {
            WinHttpCloseHandle(handle_);
            handle_ = nullptr;
        }
    }
    ~TrackedWinHttpRequestGuard() {
        Reset();
    }

    void Reset(HINTERNET h = nullptr) noexcept {
        if (handle_ != nullptr) {
            if (registry_ != nullptr) {
                registry_->CloseIfOwned(handle_);
            } else {
                WinHttpCloseHandle(handle_);
            }
            handle_ = nullptr;
        }
        if (h != nullptr) {
            handle_ = h;
            if (registry_ != nullptr && !registry_->Register(handle_)) {
                WinHttpCloseHandle(handle_);
                handle_ = nullptr;
            }
        }
    }

    TrackedWinHttpRequestGuard(const TrackedWinHttpRequestGuard&) = delete;
    TrackedWinHttpRequestGuard& operator=(const TrackedWinHttpRequestGuard&) = delete;

    [[nodiscard]] HINTERNET Get() const noexcept { return handle_; }
    [[nodiscard]] bool IsValid() const noexcept { return handle_ != nullptr; }

private:
    HttpRequestRegistry* registry_{nullptr};
    HINTERNET handle_{nullptr};
};

} // namespace

bool HttpRequestRegistry::Register(HINTERNET h) {
    if (h == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mtx);
    if (aborted || shutdown) {
        return false;
    }
    handles.insert(h);
    return true;
}

void HttpRequestRegistry::CloseIfOwned(HINTERNET h) noexcept {
    if (h == nullptr) {
        return;
    }
    bool owned = false;
    {
        std::lock_guard<std::mutex> lock(mtx);
        owned = (handles.erase(h) > 0);
    }
    if (owned) {
        WinHttpCloseHandle(h);
    }
}

void HttpRequestRegistry::AbortAll() noexcept {
    std::unordered_set<HINTERNET> toClose;
    {
        std::lock_guard<std::mutex> lock(mtx);
        aborted = true;
        toClose = std::move(handles);
        handles.clear();
    }
    for (HINTERNET h : toClose) {
        if (h != nullptr) {
            WinHttpCloseHandle(h);
        }
    }
}

void HttpRequestRegistry::Shutdown() noexcept {
    std::unordered_set<HINTERNET> toClose;
    {
        std::lock_guard<std::mutex> lock(mtx);
        shutdown = true;
        aborted = true;
        toClose = std::move(handles);
        handles.clear();
    }
    for (HINTERNET h : toClose) {
        if (h != nullptr) {
            WinHttpCloseHandle(h);
        }
    }
}

void HttpRequestRegistry::Reset() noexcept {
    std::lock_guard<std::mutex> lock(mtx);
    if (!shutdown) {
        aborted = false;
    }
}

ScopedHttpRegistry::ScopedHttpRegistry(HttpRequestRegistry* reg) noexcept
    : prev_(t_activeHttpRegistry) {
    t_activeHttpRegistry = reg;
}

ScopedHttpRegistry::~ScopedHttpRegistry() {
    t_activeHttpRegistry = prev_;
}

bool IsHttpsUrl(std::string_view url) {
    return url.size() > 8 && ToLowerAscii(url.substr(0, 8)) == "https://";
}

std::string ExtractHttpsHost(std::string_view url) {
    if (!IsHttpsUrl(url)) {
        return {};
    }
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    if (!CrackHttpsUrl(Utf8ToWide(url), host, path, port)) {
        return {};
    }
    return ToLowerAscii(WideToUtf8(host));
}

bool IsAllowedHttpsHost(
    std::string_view url,
    const std::vector<std::string_view>& allowedSuffixes) {
    const std::string host = ExtractHttpsHost(url);
    if (host.empty()) {
        return false;
    }
    return MatchesAllowedHostSuffix(host, allowedSuffixes);
}

std::string StripUrlQuery(std::string_view url) {
    const size_t q = url.find('?');
    return std::string(q == std::string_view::npos ? url : url.substr(0, q));
}

std::string ExtractJsonStringField(const std::string& json, const std::string& key, size_t startPos) {
    const std::string pattern = "\"" + key + "\"";
    size_t searchPos = startPos;
    while (searchPos < json.size()) {
        const size_t pos = json.find(pattern, searchPos);
        if (pos == std::string::npos) {
            return {};
        }
        size_t colon = pos + pattern.size();
        while (colon < json.size() &&
               (json[colon] == ' ' || json[colon] == '\t' || json[colon] == '\r' || json[colon] == '\n')) {
            ++colon;
        }
        if (colon >= json.size() || json[colon] != ':') {
            searchPos = pos + pattern.size();
            continue;
        }
        size_t q1 = colon + 1;
        while (q1 < json.size() &&
               (json[q1] == ' ' || json[q1] == '\t' || json[q1] == '\r' || json[q1] == '\n')) {
            ++q1;
        }
        if (q1 >= json.size() || json[q1] != '"') {
            return {};
        }
        std::string out;
        bool escaped = false;
        for (size_t i = q1 + 1; i < json.size(); ++i) {
            const char ch = json[i];
            if (escaped) {
                if (ch == '"' || ch == '\\' || ch == '/') {
                    out.push_back(ch);
                } else if (ch == 't') {
                    out.push_back('\t');
                } else if (ch == 'u' && i + 4 < json.size()) {
                    unsigned int code = 0;
                    bool hexOk = true;
                    for (size_t k = 1; k <= 4; ++k) {
                        const char hc = json[i + k];
                        code <<= 4;
                        if (hc >= '0' && hc <= '9') code |= static_cast<unsigned int>(hc - '0');
                        else if (hc >= 'a' && hc <= 'f') code |= static_cast<unsigned int>(hc - 'a' + 10);
                        else if (hc >= 'A' && hc <= 'F') code |= static_cast<unsigned int>(hc - 'A' + 10);
                        else {
                            hexOk = false;
                            break;
                        }
                    }
                    if (hexOk && code >= 0x20u && code <= 0x7Eu) {
                        out.push_back(static_cast<char>(code));
                        i += 4;
                    } else {
                        out.push_back('\\');
                        out.push_back(ch);
                    }
                } else {
                    out.push_back('\\');
                    out.push_back(ch);
                }
                escaped = false;
                continue;
            }
            if (ch == '\\') {
                escaped = true;
                continue;
            }
            if (ch == '"') {
                return out;
            }
            out.push_back(ch);
        }
        return {};
    }
    return {};
}

HttpResponse WinHttpRequest(
    const std::wstring& url,
    const wchar_t* method,
    bool followRedirects,
    size_t maxBytes,
    const wchar_t* extraHeaders,
    std::string_view postBody) {
    HttpResponse res;
    URL_COMPONENTS urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t hostBuf[256]{};
    wchar_t pathBuf[2048]{};
    urlComp.lpszHostName = hostBuf;
    urlComp.dwHostNameLength = static_cast<DWORD>(std::size(hostBuf));
    urlComp.lpszUrlPath = pathBuf;
    urlComp.dwUrlPathLength = static_cast<DWORD>(std::size(pathBuf));

    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &urlComp) ||
        urlComp.nScheme != INTERNET_SCHEME_HTTPS) {
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
    EnforceTls12Or13(session.Get());
    WinHttpSetTimeouts(session.Get(), 3000, 3000, 3500, 6000);

    WinHttpGuard connect(WinHttpConnect(session.Get(), hostBuf, urlComp.nPort, 0));
    if (!connect.IsValid()) {
        return res;
    }

    TrackedWinHttpRequestGuard request(WinHttpOpenRequest(
        connect.Get(),
        method,
        pathBuf,
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (!request.IsValid()) {
        return res;
    }

    DWORD redirPolicy = followRedirects
                            ? WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP
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
    res.ok = followRedirects ? (status >= 200 && status < 300) : (status >= 200 && status < 400);

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
        for (;;) {
            if (!WinHttpQueryDataAvailable(request.Get(), &avail)) {
                res.ok = false;
                break;
            }
            if (avail == 0) {
                break;
            }
            if (res.body.size() >= maxBytes) {
                res.truncated = true;
                break;
            }
            const size_t toRead = std::min<size_t>(avail, maxBytes - res.body.size());
            std::string chunk(toRead, '\0');
            DWORD readBytes = 0;
            if (!WinHttpReadData(request.Get(), chunk.data(), static_cast<DWORD>(toRead), &readBytes)) {
                res.ok = false;
                break;
            }
            if (readBytes == 0) {
                break;
            }
            chunk.resize(readBytes);
            res.body += chunk;
        }
    }
    return res;
}

bool DownloadFileViaWinHttp(
    const std::wstring& url,
    const std::filesystem::path& destPath,
    std::atomic<float>* progress,
    std::string* outError,
    float baseProgress,
    float progressSpan,
    const std::atomic<bool>* cancelFlag,
    const std::vector<std::string_view>& allowedHostSuffixes) {
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
    EnforceTls12Or13(session.Get());
    WinHttpSetTimeouts(session.Get(), 5000, 5000, 10000, 60000);

    std::wstring currentUrl = url;
    WinHttpGuard connect;
    TrackedWinHttpRequestGuard request;
    DWORD status = 0;

    constexpr int kMaxRedirects = 8;
    for (int hop = 0; hop <= kMaxRedirects; ++hop) {
        if (cancelFlag != nullptr && cancelFlag->load()) {
            if (outError) *outError = "Отменено пользователем.";
            return false;
        }
        std::wstring host;
        std::wstring path;
        INTERNET_PORT port = 0;
        if (!CrackHttpsUrl(currentUrl, host, path, port)) {
            if (outError) {
                *outError = IsHttpsUrl(WideToUtf8(currentUrl))
                                ? "Некорректный URL загрузки."
                                : "Загрузка разрешена только по защищённому протоколу HTTPS.";
            }
            return false;
        }
        if (!allowedHostSuffixes.empty()) {
            const std::string hostLower = ToLowerAscii(WideToUtf8(host));
            if (!MatchesAllowedHostSuffix(hostLower, allowedHostSuffixes)) {
                if (outError) {
                    *outError = "Хост загрузки не входит в доверенный список (" + hostLower + ").";
                }
                return false;
            }
        }

        request.Reset();
        connect.Reset(WinHttpConnect(session.Get(), host.c_str(), port, 0));
        if (!connect.IsValid()) {
            if (outError) *outError = "Ошибка WinHttpConnect: " + FormatWin32Error(GetLastError());
            return false;
        }

        request.Reset(WinHttpOpenRequest(
            connect.Get(),
            L"GET",
            path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE));
        if (!request.IsValid()) {
            if (outError) *outError = "Ошибка WinHttpOpenRequest: " + FormatWin32Error(GetLastError());
            return false;
        }

        DWORD redirPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request.Get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirPolicy, sizeof(redirPolicy));

        const std::string hostLowerForHeader = ToLowerAscii(WideToUtf8(host));
        if (MatchesAllowedHostSuffix(hostLowerForHeader, {"amd.com"})) {
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

        status = 0;
        DWORD statusLen = sizeof(status);
        WinHttpQueryHeaders(
            request.Get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &statusLen,
            WINHTTP_NO_HEADER_INDEX);

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            if (hop == kMaxRedirects) {
                if (outError) *outError = "Превышено число перенаправлений HTTP.";
                return false;
            }
            wchar_t locBuf[2048]{};
            DWORD locBytes = sizeof(locBuf);
            if (!WinHttpQueryHeaders(
                    request.Get(),
                    WINHTTP_QUERY_LOCATION,
                    WINHTTP_HEADER_NAME_BY_INDEX,
                    locBuf,
                    &locBytes,
                    WINHTTP_NO_HEADER_INDEX) ||
                locBuf[0] == L'\0') {
                if (outError) *outError = "Сервер вернул пустой заголовок перенаправления.";
                return false;
            }
            currentUrl = ResolveRedirectTarget(host, port, locBuf);
            continue;
        }
        break;
    }
    if (status < 200 || status >= 300) {
        if (outError) *outError = "Сервер вернул HTTP статус " + std::to_string(status);
        return false;
    }

    constexpr ULONGLONG kMaxInstallerBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
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
    if (totalSize > kMaxInstallerBytes) {
        if (outError) {
            *outError = "Размер загружаемого файла превышает допустимый лимит (2 ГБ).";
        }
        return false;
    }

    HandleGuard outFile(CreateFileW(
        destPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!outFile.IsValid()) {
        if (outError) {
            *outError = "Не удалось создать временный файл для установки: " +
                        FormatWin32Error(GetLastError());
        }
        return false;
    }

    bool ioError = false;
    std::string ioErrMsg;
    ULONGLONG downloaded = 0;
    DWORD avail = 0;
    std::vector<char> buf(65536);
    for (;;) {
        if (cancelFlag != nullptr && cancelFlag->load()) {
            outFile.Reset();
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
        const DWORD chunkCap = std::min<DWORD>(avail, static_cast<DWORD>(buf.size()));
        DWORD readBytes = 0;
        if (!WinHttpReadData(request.Get(), buf.data(), chunkCap, &readBytes)) {
            ioError = true;
            ioErrMsg = "Ошибка загрузки данных: " + FormatWin32Error(GetLastError());
            break;
        }
        if (readBytes == 0) {
            break;
        }
        if (downloaded + readBytes > kMaxInstallerBytes) {
            ioError = true;
            ioErrMsg = "Размер загружаемого файла превысил допустимый лимит (2 ГБ).";
            break;
        }
        DWORD offset = 0;
        while (offset < readBytes) {
            DWORD written = 0;
            if (!WriteFile(outFile.Get(), buf.data() + offset, readBytes - offset, &written, nullptr) ||
                written == 0) {
                ioError = true;
                ioErrMsg = "Ошибка записи временного файла на диск: " +
                           FormatWin32Error(GetLastError());
                break;
            }
            offset += written;
        }
        if (ioError) {
            break;
        }
        downloaded += readBytes;
        if (progress != nullptr && totalSize > 0) {
            const float localFrac = std::clamp(
                static_cast<float>(downloaded) / static_cast<float>(totalSize), 0.0f, 0.90f);
            progress->store(std::clamp(baseProgress + localFrac * progressSpan, 0.0f, 0.99f));
        }
    }
    if (!ioError && !FlushFileBuffers(outFile.Get())) {
        ioError = true;
        ioErrMsg = "Ошибка завершения записи временного файла: " +
                   FormatWin32Error(GetLastError());
    }
    outFile.Reset();

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

} // namespace hdm
