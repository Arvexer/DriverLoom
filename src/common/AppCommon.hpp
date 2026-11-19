#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <setupapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hdm {

inline std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
    return wide;
}

inline std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string utf8(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

inline std::string ToLowerAscii(std::string_view input) {
    std::string out(input);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

inline bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    // Use Win32 CompareStringOrdinal / CharLowerBuffW for proper UTF-8/Cyrillic case-insensitive search.
    std::wstring wh = Utf8ToWide(haystack);
    std::wstring wn = Utf8ToWide(needle);
    if (wh.empty() || wn.empty()) {
        return false;
    }
    CharLowerBuffW(wh.data(), static_cast<DWORD>(wh.size()));
    CharLowerBuffW(wn.data(), static_cast<DWORD>(wn.size()));
    return wh.find(wn) != std::wstring::npos;
}

inline std::string FormatWin32Error(DWORD errCode) {
    if (errCode == 0) {
        return "Успешно";
    }
    LPWSTR buffer = nullptr;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    std::string msg;
    if (len > 0 && buffer != nullptr) {
        std::wstring ws(buffer, len);
        while (!ws.empty() && (ws.back() == L'\r' || ws.back() == L'\n' || ws.back() == L' ')) {
            ws.pop_back();
        }
        msg = WideToUtf8(ws);
        LocalFree(buffer);
    }
    char codeBuf[32];
    std::snprintf(codeBuf, sizeof(codeBuf), "0x%08lX", static_cast<unsigned long>(errCode));
    if (msg.empty()) {
        return std::string("Код ошибки ") + codeBuf;
    }
    return msg + " (" + codeBuf + ")";
}

inline std::string FormatHResult(HRESULT hr) {
    return FormatWin32Error(static_cast<DWORD>(hr));
}

inline std::string FormatBytes(uint64_t bytes) {
    if (bytes == 0) {
        return "—";
    }
    char buf[64];
    constexpr double kKB = 1024.0;
    constexpr double kMB = 1024.0 * 1024.0;
    constexpr double kGB = 1024.0 * 1024.0 * 1024.0;
    if (bytes >= static_cast<uint64_t>(kGB)) {
        std::snprintf(buf, sizeof(buf), "%.2f ГБ", static_cast<double>(bytes) / kGB);
    } else if (bytes >= static_cast<uint64_t>(kMB)) {
        std::snprintf(buf, sizeof(buf), "%.1f МБ", static_cast<double>(bytes) / kMB);
    } else if (bytes >= static_cast<uint64_t>(kKB)) {
        std::snprintf(buf, sizeof(buf), "%.0f КБ", static_cast<double>(bytes) / kKB);
    } else {
        std::snprintf(buf, sizeof(buf), "%llu Б", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

// --- RAII Guards ---

class CoInitGuard {
public:
    explicit CoInitGuard(DWORD coInitFlags = COINIT_MULTITHREADED) noexcept
        : hr_(CoInitializeEx(nullptr, coInitFlags)) {}

    ~CoInitGuard() {
        if (hr_ == S_OK || hr_ == S_FALSE) {
            CoUninitialize();
        }
    }

    CoInitGuard(const CoInitGuard&) = delete;
    CoInitGuard& operator=(const CoInitGuard&) = delete;

    [[nodiscard]] bool Succeeded() const noexcept {
        return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE;
    }
    [[nodiscard]] HRESULT Result() const noexcept { return hr_; }

private:
    HRESULT hr_{E_FAIL};
};

template <typename T>
class ComPtr {
public:
    ComPtr() noexcept = default;
    ~ComPtr() { Reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            ptr_ = std::exchange(other.ptr_, nullptr);
        }
        return *this;
    }

    void Reset() noexcept {
        if (ptr_ != nullptr) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

    T* Get() const noexcept { return ptr_; }
    T** Put() noexcept {
        Reset();
        return &ptr_;
    }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    T* Detach() noexcept { return std::exchange(ptr_, nullptr); }

private:
    T* ptr_{nullptr};
};

class BstrGuard {
public:
    BstrGuard() noexcept = default;
    explicit BstrGuard(const wchar_t* str) noexcept
        : bstr_(str ? SysAllocString(str) : nullptr) {}
    ~BstrGuard() { Reset(); }

    BstrGuard(const BstrGuard&) = delete;
    BstrGuard& operator=(const BstrGuard&) = delete;

    void Reset() noexcept {
        if (bstr_ != nullptr) {
            SysFreeString(bstr_);
            bstr_ = nullptr;
        }
    }

    BSTR Get() const noexcept { return bstr_; }
    BSTR* Put() noexcept {
        Reset();
        return &bstr_;
    }

    std::string ToUtf8() const {
        if (bstr_ == nullptr) {
            return {};
        }
        return WideToUtf8(std::wstring_view(bstr_, SysStringLen(bstr_)));
    }

    std::wstring ToWide() const {
        if (bstr_ == nullptr) {
            return {};
        }
        return std::wstring(bstr_, SysStringLen(bstr_));
    }

private:
    BSTR bstr_{nullptr};
};

class DevInfoSetGuard {
public:
    explicit DevInfoSetGuard(HDEVINFO handle = INVALID_HANDLE_VALUE) noexcept
        : handle_(handle) {}
    ~DevInfoSetGuard() { Reset(); }

    DevInfoSetGuard(const DevInfoSetGuard&) = delete;
    DevInfoSetGuard& operator=(const DevInfoSetGuard&) = delete;

    void Reset(HDEVINFO handle = INVALID_HANDLE_VALUE) noexcept {
        if (handle_ != INVALID_HANDLE_VALUE) {
            SetupDiDestroyDeviceInfoList(handle_);
        }
        handle_ = handle;
    }

    [[nodiscard]] bool IsValid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HDEVINFO Get() const noexcept { return handle_; }

private:
    HDEVINFO handle_{INVALID_HANDLE_VALUE};
};

class RegKeyGuard {
public:
    RegKeyGuard() noexcept = default;
    ~RegKeyGuard() { Reset(); }

    RegKeyGuard(const RegKeyGuard&) = delete;
    RegKeyGuard& operator=(const RegKeyGuard&) = delete;

    void Reset() noexcept {
        if (key_ != nullptr) {
            RegCloseKey(key_);
            key_ = nullptr;
        }
    }

    HKEY* Put() noexcept {
        Reset();
        return &key_;
    }
    [[nodiscard]] HKEY Get() const noexcept { return key_; }
    explicit operator bool() const noexcept { return key_ != nullptr; }

private:
    HKEY key_{nullptr};
};

class HandleGuard {
public:
    explicit HandleGuard(HANDLE h = nullptr) noexcept : handle_(h) {}
    ~HandleGuard() { Reset(); }

    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;

    HandleGuard(HandleGuard&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    HandleGuard& operator=(HandleGuard&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    void Reset(HANDLE h = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = h;
    }

    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
    [[nodiscard]] bool IsValid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_{nullptr};
};

template <typename T>
class CoTaskMemGuard {
public:
    CoTaskMemGuard() noexcept = default;
    ~CoTaskMemGuard() { Reset(); }

    CoTaskMemGuard(const CoTaskMemGuard&) = delete;
    CoTaskMemGuard& operator=(const CoTaskMemGuard&) = delete;

    void Reset() noexcept {
        if (ptr_ != nullptr) {
            CoTaskMemFree(ptr_);
            ptr_ = nullptr;
        }
    }

    T** Put() noexcept {
        Reset();
        return &ptr_;
    }
    T* Get() const noexcept { return ptr_; }

private:
    T* ptr_{nullptr};
};

// --- Thread-safe Logging ---

enum class LogLevel {
    Info,
    Success,
    Warning,
    Error
};

struct LogEntry {
    std::string timestamp;
    LogLevel level{LogLevel::Info};
    std::string message;
};

using LogCallback = std::function<void(LogLevel, const std::string&)>;

class LogBuffer {
public:
    void Push(LogLevel level, const std::string& message) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char timeBuf[16];
        std::snprintf(timeBuf, sizeof(timeBuf), "%02u:%02u:%02u",
                      st.wHour, st.wMinute, st.wSecond);

        std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back(LogEntry{timeBuf, level, message});
        if (entries_.size() > 2000) {
            entries_.erase(entries_.begin(), entries_.begin() + 500);
        }
        dirty_ = true;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        dirty_ = false;
    }

    std::vector<LogEntry> Snapshot(bool* outWasDirty = nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (outWasDirty != nullptr) {
            *outWasDirty = dirty_;
        }
        dirty_ = false;
        return entries_;
    }

private:
    std::mutex mutex_;
    std::vector<LogEntry> entries_;
    bool dirty_{false};
};

// --- Elevation Helpers ---

inline bool IsProcessElevated() {
    HANDLE tokenRaw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tokenRaw)) {
        return false;
    }
    HandleGuard token(tokenRaw);
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    if (!GetTokenInformation(token.Get(), TokenElevation, &elevation, sizeof(elevation), &size)) {
        return false;
    }
    return elevation.TokenIsElevated != 0;
}

inline std::wstring GetCurrentExePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (len == 0) {
            return {};
        }
        if (len < path.size() - 1) {
            path.resize(len);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

struct ElevatedRunResult {
    bool launched{false};
    bool cancelled{false};
    bool timedOutWaiting{false};
    DWORD exitCode{1};
    DWORD launchError{0};
};

// Launches the current executable with UAC ("runas") using ShellExecuteExW.
// When waitForCompletion is true, waits for the elevated process while tailing utf8LogPath in real time.
inline ElevatedRunResult ExecuteElevatedSelf(
    const std::wstring& arguments,
    bool waitForCompletion,
    const std::filesystem::path* utf8LogPath = nullptr,
    const LogCallback& onLog = nullptr,
    const std::function<void(const std::string&)>& onControlLine = nullptr,
    const std::atomic<bool>* cancelFlag = nullptr,
    const std::filesystem::path* cancelMarkerPath = nullptr) {
    ElevatedRunResult result;
    const std::wstring exePath = GetCurrentExePath();
    if (exePath.empty()) {
        result.launchError = GetLastError();
        return result;
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = waitForCompletion ? SEE_MASK_NOCLOSEPROCESS : 0;
    sei.lpVerb = L"runas";
    sei.lpFile = exePath.c_str();
    sei.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
    sei.nShow = SW_HIDE;
    if (!waitForCompletion) {
        sei.nShow = SW_SHOWNORMAL;
    }

    if (!ShellExecuteExW(&sei)) {
        result.launchError = GetLastError();
        return result;
    }

    result.launched = true;
    if (!waitForCompletion) {
        result.exitCode = 0;
        return result;
    }

    HandleGuard proc(sei.hProcess);
    size_t readOffset = 0;
    std::string pendingLine;

    auto flushLogFile = [&]() {
        if (utf8LogPath == nullptr || (!onLog && !onControlLine)) {
            return;
        }
        std::ifstream ifs(*utf8LogPath, std::ios::binary);
        if (!ifs.is_open()) {
            return;
        }
        ifs.seekg(static_cast<std::streamoff>(readOffset));
        std::string chunk((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        if (chunk.empty()) {
            return;
        }
        readOffset += chunk.size();
        pendingLine += chunk;
        size_t pos = 0;
        while ((pos = pendingLine.find('\n')) != std::string::npos) {
            std::string line = pendingLine.substr(0, pos);
            pendingLine.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            if (line.rfind("[META] ", 0) == 0) {
                if (onControlLine) {
                    onControlLine(line.substr(7));
                }
                continue;
            }
            if (!onLog) {
                continue;
            }
            LogLevel lvl = LogLevel::Info;
            if (line.rfind("[OK] ", 0) == 0) {
                lvl = LogLevel::Success;
                line.erase(0, 5);
            } else if (line.rfind("[WARN] ", 0) == 0) {
                lvl = LogLevel::Warning;
                line.erase(0, 7);
            } else if (line.rfind("[ERR] ", 0) == 0) {
                lvl = LogLevel::Error;
                line.erase(0, 6);
            } else if (line.rfind("[INFO] ", 0) == 0) {
                lvl = LogLevel::Info;
                line.erase(0, 7);
            }
            onLog(lvl, line);
        }
    };

    if (proc.IsValid()) {
        bool abortSignaled = false;
        std::chrono::steady_clock::time_point abortDeadline;
        while (WaitForSingleObject(proc.Get(), 150) == WAIT_TIMEOUT) {
            flushLogFile();
            if (cancelFlag != nullptr && cancelFlag->load()) {
                if (!abortSignaled) {
                    abortSignaled = true;
                    result.cancelled = true;
                    if (cancelMarkerPath != nullptr) {
                        std::ofstream marker(*cancelMarkerPath, std::ios::binary | std::ios::trunc);
                    }
                    abortDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                } else if (std::chrono::steady_clock::now() >= abortDeadline) {
                    result.timedOutWaiting = true;
                    if (onLog) {
                        onLog(
                            LogLevel::Warning,
                            "Фоновый процесс установки не завершился вовремя после отмены — разблокировка интерфейса.");
                    }
                    break;
                }
            }
        }
        flushLogFile();
        if (!pendingLine.empty()) {
            if (pendingLine.rfind("[META] ", 0) == 0) {
                if (onControlLine) {
                    onControlLine(pendingLine.substr(7));
                }
            } else if (onLog) {
                onLog(LogLevel::Info, pendingLine);
            }
        }
        DWORD code = 1;
        if (!result.timedOutWaiting && GetExitCodeProcess(proc.Get(), &code)) {
            result.exitCode = code;
        }
    }
    return result;
}

inline std::filesystem::path CreateTempLogPath(const wchar_t* prefix) {
    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    wchar_t tempFile[MAX_PATH]{};
    if (GetTempFileNameW(tempDir, prefix, 0, tempFile) != 0) {
        return std::filesystem::path(tempFile);
    }
    return std::filesystem::path(tempDir) / L"hdm_worker.log";
}

} // namespace hdm
