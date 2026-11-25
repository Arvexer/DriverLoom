#include "core/HttpClient.hpp"

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

namespace hdm {

namespace {

thread_local HttpRequestRegistry* t_activeHttpRegistry = nullptr;

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
        if (handle_ != nullptr) {
            if (registry_ != nullptr) {
                registry_->CloseIfOwned(handle_);
            } else {
                WinHttpCloseHandle(handle_);
            }
            handle_ = nullptr;
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
    if (aborted) {
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

void HttpRequestRegistry::Reset() noexcept {
    std::lock_guard<std::mutex> lock(mtx);
    aborted = false;
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

std::string StripUrlQuery(std::string_view url) {
    const size_t q = url.find('?');
    return std::string(q == std::string_view::npos ? url : url.substr(0, q));
}

std::string ExtractJsonStringField(const std::string& json, const std::string& key, size_t startPos) {
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
                res.truncated = true;
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

bool DownloadFileViaWinHttp(
    const std::wstring& url,
    const std::filesystem::path& destPath,
    std::atomic<float>* progress,
    std::string* outError,
    float baseProgress,
    float progressSpan,
    const std::atomic<bool>* cancelFlag) {
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
    if (urlComp.nScheme != INTERNET_SCHEME_HTTPS) {
        if (outError) *outError = "Загрузка разрешена только по защищённому протоколу HTTPS.";
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

    TrackedWinHttpRequestGuard request(WinHttpOpenRequest(
        connect.Get(),
        L"GET",
        pathBuf,
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (!request.IsValid()) {
        if (outError) *outError = "Ошибка WinHttpOpenRequest: " + FormatWin32Error(GetLastError());
        return false;
    }

    DWORD redirPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
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
