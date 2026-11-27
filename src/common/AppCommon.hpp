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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/Localization.hpp"

namespace hdm {

inline constexpr const char* kAppVersion = "1.2.1";

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

// Safely quotes a Win32 command-line argument according to CommandLineToArgvW rules
// (properly escapes embedded quotes and trailing backslashes such as "D:\").
inline std::wstring QuoteWin32Arg(std::wstring_view arg) {
    std::wstring out;
    out.reserve(arg.size() + 8);
    out.push_back(L'"');
    size_t backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            if (backslashes > 0) {
                out.append(backslashes, L'\\');
                backslashes = 0;
            }
            out.push_back(ch);
        }
    }
    if (backslashes > 0) {
        out.append(backslashes * 2, L'\\');
    }
    out.push_back(L'"');
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
        switch (errCode) {
        case 0xE000023Du: // ERROR_INF_IN_USE_BY_DEVICES
            msg = Tr(
                "Пакет используется устройством или подсистемой Windows (пропущен для безопасности)",
                "Пакет використовується пристроєм або підсистемою Windows (пропущено для безпеки)",
                "Package is in use by a device or Windows subsystem (skipped for safety)");
            break;
        case 0xE000022Fu: // ERROR_NO_CATALOG_FOR_OEM_INF
            msg = Tr(
                "Отсутствует цифровой каталог подписи (.cat) для INF-пакета",
                "Відсутній цифровий каталог підпису (.cat) для INF-пакета",
                "Missing digital signature catalog (.cat) for INF package");
            break;
        case 0xE0000247u: // ERROR_AUTHENTICODE_TRUST_NOT_ESTABLISHED
            msg = Tr(
                "Не удалось подтвердить цифровую подпись драйвера",
                "Не вдалося підтвердити цифровий підпис драйвера",
                "Failed to verify driver digital signature");
            break;
        case 0xE0000243u: // ERROR_SIGNATURE_OSATTRIBUTE_MISMATCH
            msg = Tr(
                "Цифровая подпись драйвера не соответствует версии Windows",
                "Цифровий підпис драйвера не відповідає версії Windows",
                "Driver signature does not match this Windows version");
            break;
        case 0xE0000235u: // ERROR_IN_WOW64
            msg = Tr(
                "Несовпадение архитектуры (требуется 64-битный процесс)",
                "Невідповідність архітектури (потрібен 64-бітний процес)",
                "Architecture mismatch (64-bit process required)");
            break;
        default:
            return std::string("Код ошибки ") + codeBuf;
        }
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
    const bool en = (GetLang() == Lang::En);
    if (bytes >= static_cast<uint64_t>(kGB)) {
        std::snprintf(buf, sizeof(buf), en ? "%.2f GB" : "%.2f ГБ", static_cast<double>(bytes) / kGB);
    } else if (bytes >= static_cast<uint64_t>(kMB)) {
        std::snprintf(buf, sizeof(buf), en ? "%.1f MB" : "%.1f МБ", static_cast<double>(bytes) / kMB);
    } else if (bytes >= static_cast<uint64_t>(kKB)) {
        std::snprintf(buf, sizeof(buf), en ? "%.0f KB" : "%.0f КБ", static_cast<double>(bytes) / kKB);
    } else {
        std::snprintf(
            buf,
            sizeof(buf),
            en ? "%llu B" : "%llu Б",
            static_cast<unsigned long long>(bytes));
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
        return SUCCEEDED(hr_);
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

class ModuleGuard {
public:
    explicit ModuleGuard(HMODULE mod = nullptr) noexcept : mod_(mod) {}
    ~ModuleGuard() { Reset(); }

    ModuleGuard(const ModuleGuard&) = delete;
    ModuleGuard& operator=(const ModuleGuard&) = delete;

    void Reset(HMODULE mod = nullptr) noexcept {
        if (mod_ != nullptr) {
            FreeLibrary(mod_);
        }
        mod_ = mod;
    }

    [[nodiscard]] HMODULE Get() const noexcept { return mod_; }
    explicit operator bool() const noexcept { return mod_ != nullptr; }

private:
    HMODULE mod_{nullptr};
};

class LocalFreeGuard {
public:
    explicit LocalFreeGuard(HLOCAL ptr = nullptr) noexcept : ptr_(ptr) {}
    ~LocalFreeGuard() { Reset(); }

    LocalFreeGuard(const LocalFreeGuard&) = delete;
    LocalFreeGuard& operator=(const LocalFreeGuard&) = delete;

    void Reset(HLOCAL ptr = nullptr) noexcept {
        if (ptr_ != nullptr) {
            LocalFree(ptr_);
        }
        ptr_ = ptr;
    }

    [[nodiscard]] void* Get() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
    HLOCAL ptr_{nullptr};
};

class InfFileGuard {
public:
    explicit InfFileGuard(HINF hInf = INVALID_HANDLE_VALUE) noexcept : hInf_(hInf) {}
    ~InfFileGuard() { Reset(); }

    InfFileGuard(const InfFileGuard&) = delete;
    InfFileGuard& operator=(const InfFileGuard&) = delete;

    void Reset(HINF hInf = INVALID_HANDLE_VALUE) noexcept {
        if (hInf_ != INVALID_HANDLE_VALUE) {
            SetupCloseInfFile(hInf_);
        }
        hInf_ = hInf;
    }

    [[nodiscard]] bool IsValid() const noexcept { return hInf_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HINF Get() const noexcept { return hInf_; }

private:
    HINF hInf_{INVALID_HANDLE_VALUE};
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

inline std::atomic<HWND>& MainWindowHandleStorage() noexcept {
    static std::atomic<HWND> g_hwnd{nullptr};
    return g_hwnd;
}

inline void SetMainWindowHandle(HWND hwnd) noexcept {
    MainWindowHandleStorage().store(hwnd, std::memory_order_release);
}

inline HWND GetMainWindowHandle() noexcept {
    return MainWindowHandleStorage().load(std::memory_order_acquire);
}

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
    const std::filesystem::path* cancelMarkerPath = nullptr,
    HWND ownerHwnd = nullptr) {
    ElevatedRunResult result;
    const std::wstring exePath = GetCurrentExePath();
    if (exePath.empty()) {
        result.launchError = GetLastError();
        return result;
    }

    CoInitGuard comInit(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = waitForCompletion ? SEE_MASK_NOCLOSEPROCESS : 0;
    const HWND mainHwnd = GetMainWindowHandle();
    sei.hwnd = (ownerHwnd != nullptr) ? ownerHwnd : (mainHwnd != nullptr ? mainHwnd : GetActiveWindow());
    sei.lpVerb = L"runas";
    const std::wstring exeDir = std::filesystem::path(exePath).parent_path().wstring();
    sei.lpFile = exePath.c_str();
    sei.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
    sei.lpDirectory = exeDir.empty() ? nullptr : exeDir.c_str();
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
        if (utf8LogPath == nullptr || utf8LogPath->empty() || (!onLog && !onControlLine)) {
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
                    if (cancelMarkerPath != nullptr && !cancelMarkerPath->empty()) {
                        HandleGuard marker(CreateFileW(
                            cancelMarkerPath->c_str(),
                            GENERIC_WRITE,
                            0,
                            nullptr,
                            CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                            nullptr));
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

inline std::vector<std::vector<std::string>> ChunkStrings(
    const std::vector<std::string>& items,
    size_t maxPerChunk) {
    std::vector<std::vector<std::string>> chunks;
    if (items.empty() || maxPerChunk == 0) {
        return chunks;
    }
    for (size_t i = 0; i < items.size(); i += maxPerChunk) {
        const size_t end = (std::min)(items.size(), i + maxPerChunk);
        chunks.emplace_back(
            items.begin() + static_cast<std::ptrdiff_t>(i),
            items.begin() + static_cast<std::ptrdiff_t>(end));
    }
    return chunks;
}

inline bool IsSafeIdToken(std::string_view tok) noexcept {
    if (tok.empty() || tok.size() > 128) {
        return false;
    }
    for (unsigned char ch : tok) {
        const bool ok = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= 'a' && ch <= 'z') || ch == '_' || ch == '-' ||
                        ch == '.' || ch == '&' || ch == '\\';
        if (!ok) {
            return false;
        }
    }
    return true;
}

inline std::string SerializeIdSet(const std::unordered_set<std::string>& ids) {
    std::vector<std::string> sorted;
    sorted.reserve((std::min)(ids.size(), size_t{256}));
    for (const auto& id : ids) {
        if (IsSafeIdToken(id)) {
            sorted.push_back(id);
            if (sorted.size() >= 256) {
                break;
            }
        }
    }
    std::sort(sorted.begin(), sorted.end());
    std::string out;
    constexpr size_t kMaxSerializedChars = 16380;
    for (size_t i = 0; i < sorted.size(); ++i) {
        const size_t addLen = (out.empty() ? 0u : 1u) + sorted[i].size();
        if (out.size() + addLen > kMaxSerializedChars) {
            break;
        }
        if (!out.empty()) {
            out.push_back('|');
        }
        out += sorted[i];
    }
    return out;
}

inline std::unordered_set<std::string> ParseIdSet(std::string_view raw) {
    std::unordered_set<std::string> out;
    if (raw.empty()) {
        return out;
    }
    size_t start = 0;
    while (start <= raw.size()) {
        const size_t sep = raw.find('|', start);
        std::string_view part =
            (sep == std::string_view::npos) ? raw.substr(start) : raw.substr(start, sep - start);
        while (!part.empty() &&
               (part.front() == ' ' || part.front() == '\t' ||
                part.front() == '\r' || part.front() == '\n')) {
            part.remove_prefix(1);
        }
        while (!part.empty() &&
               (part.back() == ' ' || part.back() == '\t' ||
                part.back() == '\r' || part.back() == '\n')) {
            part.remove_suffix(1);
        }
        if (IsSafeIdToken(part)) {
            out.emplace(part);
            if (out.size() >= 256) {
                break;
            }
        }
        if (sep == std::string_view::npos) {
            break;
        }
        start = sep + 1;
    }
    return out;
}

std::filesystem::path CreateTempLogPath(const wchar_t* prefix);

} // namespace hdm
