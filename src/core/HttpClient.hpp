#pragma once

#include "common/AppCommon.hpp"

#include <winhttp.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace hdm {

/// Thread-safe registry of active WinHTTP request handles that can be aborted on cancel/shutdown.
struct HttpRequestRegistry {
    std::mutex mtx;
    bool aborted{false};
    std::unordered_set<HINTERNET> handles;

    bool Register(HINTERNET h);
    void CloseIfOwned(HINTERNET h) noexcept;
    void AbortAll() noexcept;
    void Reset() noexcept;
};

/// Binds an HttpRequestRegistry to the current worker thread so WinHTTP requests become cancellable.
class ScopedHttpRegistry {
public:
    explicit ScopedHttpRegistry(HttpRequestRegistry* reg) noexcept;
    ~ScopedHttpRegistry();

    ScopedHttpRegistry(const ScopedHttpRegistry&) = delete;
    ScopedHttpRegistry& operator=(const ScopedHttpRegistry&) = delete;

private:
    HttpRequestRegistry* prev_{nullptr};
};

struct HttpResponse {
    bool ok{false};
    bool truncated{false};
    DWORD statusCode{0};
    std::string body;
    std::string locationHeader;
};

[[nodiscard]] bool IsHttpsUrl(std::string_view url);
[[nodiscard]] std::string StripUrlQuery(std::string_view url);
[[nodiscard]] std::string ExtractJsonStringField(
    const std::string& json,
    const std::string& key,
    size_t startPos = 0);

/// Performs an HTTPS-only WinHTTP request (GET/POST/HEAD) disallowing HTTPS->HTTP downgrades.
HttpResponse WinHttpRequest(
    const std::wstring& url,
    const wchar_t* method,
    bool followRedirects = true,
    size_t maxBytes = 256 * 1024,
    const wchar_t* extraHeaders = nullptr,
    std::string_view postBody = {});

/// Streams an HTTPS payload into a newly created destination file (2 GiB cap, cancellable).
bool DownloadFileViaWinHttp(
    const std::wstring& url,
    const std::filesystem::path& destPath,
    std::atomic<float>* progress,
    std::string* outError,
    float baseProgress = 0.0f,
    float progressSpan = 1.0f,
    const std::atomic<bool>* cancelFlag = nullptr);

} // namespace hdm
