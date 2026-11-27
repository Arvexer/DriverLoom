#include "core/PackageSecurity.hpp"
#include "core/OnlineCatalogClient.hpp"
#include "core/SafeZipExtractor.hpp"

#include <bcrypt.h>
#include <knownfolders.h>
#include <objbase.h>
#include <sddl.h>
#include <setupapi.h>
#include <shlobj.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <winver.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace hdm {

namespace {

bool EqualsIgnoreCaseWide(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    if (a.empty()) {
        return true;
    }
    return CompareStringOrdinal(
               a.data(),
               static_cast<int>(a.size()),
               b.data(),
               static_cast<int>(b.size()),
               TRUE) == CSTR_EQUAL;
}

std::wstring StripTrailingSeparators(std::wstring s) {
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) {
        s.pop_back();
    }
    return s;
}

std::wstring GetFinalPathString(HANDLE handle) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return {};
    }
    const DWORD needed = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED);
    if (needed == 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(needed) + 1, L'\0');
    const DWORD written =
        GetFinalPathNameByHandleW(handle, out.data(), needed + 1, FILE_NAME_NORMALIZED);
    if (written == 0 || written >= out.size()) {
        return {};
    }
    out.resize(written);
    return out;
}

std::wstring OpenVerifiedDirCanonical(const std::filesystem::path& dirPath) {
    if (dirPath.empty()) {
        return {};
    }
    HandleGuard dirHandle(CreateFileW(
        dirPath.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    BY_HANDLE_FILE_INFORMATION fi{};
    if (!dirHandle.IsValid() || !GetFileInformationByHandle(dirHandle.Get(), &fi) ||
        (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return {};
    }
    return StripTrailingSeparators(GetFinalPathString(dirHandle.Get()));
}

} // namespace

std::wstring ResolveTrustedLocalTempDirFromBase(const std::filesystem::path& localAppDataBase) {
    const std::wstring localAppCanon = OpenVerifiedDirCanonical(localAppDataBase);
    if (localAppCanon.empty()) {
        return {};
    }
    return OpenVerifiedDirCanonical(std::filesystem::path(localAppCanon) / L"Temp");
}

std::wstring ResolveTrustedLocalTempDir() {
    CoTaskMemGuard<wchar_t> localAppRaw;
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_LocalAppData, KF_FLAG_DEFAULT_PATH, nullptr, localAppRaw.Put())) ||
        localAppRaw.Get() == nullptr) {
        return {};
    }
    return ResolveTrustedLocalTempDirFromBase(std::filesystem::path(localAppRaw.Get()));
}

bool ValidateWorkerTempPathWithBase(
    const std::wstring& rawPath,
    const std::wstring& requiredSuffix,
    const std::wstring& canonicalTempDir,
    std::wstring& outFileName) {
    outFileName.clear();
    if (rawPath.empty() || canonicalTempDir.empty() ||
        rawPath.rfind(L"\\\\", 0) == 0 || rawPath.rfind(L"//", 0) == 0) {
        return false;
    }

    const std::filesystem::path candidate(rawPath);
    if (!candidate.is_absolute()) {
        return false;
    }
    for (const auto& part : candidate) {
        if (part == L"..") {
            return false;
        }
    }

    const std::wstring fileName = candidate.filename().wstring();
    const std::wstring kRequiredPrefix = L"DriverLoom_";
    if (fileName.size() <= kRequiredPrefix.size() + requiredSuffix.size() ||
        fileName.rfind(kRequiredPrefix, 0) != 0 ||
        fileName.compare(
            fileName.size() - requiredSuffix.size(), requiredSuffix.size(), requiredSuffix) != 0) {
        return false;
    }
    for (wchar_t ch : fileName) {
        const bool ok = (ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'Z') ||
                        (ch >= L'a' && ch <= L'z') || ch == L'_' || ch == L'-' || ch == L'.';
        if (!ok) {
            return false;
        }
    }

    const std::filesystem::path parentPath = candidate.parent_path();
    if (parentPath.empty()) {
        return false;
    }
    HandleGuard parentHandle(CreateFileW(
        parentPath.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!parentHandle.IsValid()) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION parentInfo{};
    if (!GetFileInformationByHandle(parentHandle.Get(), &parentInfo) ||
        (parentInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (parentInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return false;
    }
    const std::wstring parentCanon = StripTrailingSeparators(GetFinalPathString(parentHandle.Get()));
    const std::wstring expectedBase = StripTrailingSeparators(canonicalTempDir);
    if (parentCanon.empty() || !EqualsIgnoreCaseWide(parentCanon, expectedBase)) {
        return false;
    }

    outFileName = fileName;
    return true;
}

bool ValidateWorkerTempPath(
    const std::wstring& rawPath,
    const std::wstring& requiredSuffix,
    std::wstring& outCanonicalTempDir,
    std::wstring& outFileName) {
    outCanonicalTempDir = ResolveTrustedLocalTempDir();
    if (outCanonicalTempDir.empty()) {
        outFileName.clear();
        return false;
    }
    return ValidateWorkerTempPathWithBase(
        rawPath, requiredSuffix, outCanonicalTempDir, outFileName);
}

std::filesystem::path CreateTempLogPath(const wchar_t* prefix) {
    const std::wstring canonicalTemp = ResolveTrustedLocalTempDir();
    if (canonicalTemp.empty()) {
        return {};
    }
    std::wstring rootStr = canonicalTemp;
    if (rootStr.rfind(L"\\\\?\\", 0) == 0 && rootStr.size() >= 6 && rootStr[5] == L':') {
        rootStr.erase(0, 4);
    }
    const std::filesystem::path tempRoot(rootStr);

    std::wstring cleanPrefix = (prefix != nullptr && prefix[0] != L'\0') ? prefix : L"log";
    for (wchar_t& ch : cleanPrefix) {
        const bool ok = (ch >= L'0' && ch <= L'9') ||
                        (ch >= L'A' && ch <= L'Z') ||
                        (ch >= L'a' && ch <= L'z') ||
                        ch == L'_' || ch == L'-';
        if (!ok) {
            ch = L'_';
        }
    }

    for (int attempt = 0; attempt < 8; ++attempt) {
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid))) {
            continue;
        }
        wchar_t suffix[48]{};
        std::swprintf(
            suffix,
            std::size(suffix),
            L"_%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x.log",
            static_cast<unsigned long>(guid.Data1),
            guid.Data2,
            guid.Data3,
            guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
            guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
        const std::filesystem::path candidate = tempRoot / (L"DriverLoom_" + cleanPrefix + suffix);
        HandleGuard file(CreateFileW(
            candidate.c_str(),
            GENERIC_WRITE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (file.IsValid()) {
            BY_HANDLE_FILE_INFORMATION fi{};
            const bool safeFile =
                GetFileInformationByHandle(file.Get(), &fi) &&
                (fi.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0 &&
                fi.nNumberOfLinks == 1;
            file.Reset();
            if (safeFile) {
                return candidate;
            }
            std::error_code rmEc;
            std::filesystem::remove(candidate, rmEc);
            return {};
        }
    }
    return {};
}

void TempWorkDirGuard::Set(std::filesystem::path dir) {
    Reset();
    dir_ = std::move(dir);
}

void TempWorkDirGuard::Reset() noexcept {
    if (!dir_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        dir_.clear();
    }
}

bool CreateUniqueTempWorkDir(std::filesystem::path& outDir, std::string* outError) {
    outDir.clear();
    std::error_code tempEc;
    std::filesystem::path tempRoot;
    wchar_t tempBuf[MAX_PATH]{};
    const DWORD tempLen = GetTempPathW(MAX_PATH, tempBuf);
    if (tempLen > 0 && tempLen < MAX_PATH) {
        tempRoot = std::filesystem::path(tempBuf);
    } else {
        tempRoot = std::filesystem::temp_directory_path(tempEc);
    }
    if (tempRoot.empty()) {
        if (outError) *outError = "Не удалось определить временную папку Windows.";
        return false;
    }

    for (int attempt = 0; attempt < 8; ++attempt) {
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid))) {
            continue;
        }
        wchar_t suffix[64]{};
        const int suffixLen = std::swprintf(
            suffix,
            std::size(suffix),
            L"DriverLoom_%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x",
            static_cast<unsigned long>(guid.Data1),
            guid.Data2,
            guid.Data3,
            guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
            guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
        if (suffixLen <= 0 || suffixLen >= static_cast<int>(std::size(suffix))) {
            continue;
        }
        const std::filesystem::path candidate = tempRoot / suffix;
        if (CreateDirectoryW(candidate.c_str(), nullptr)) {
            HandleGuard dirHandle(CreateFileW(
                candidate.c_str(),
                FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr));
            BY_HANDLE_FILE_INFORMATION fi{};
            if (dirHandle.IsValid() && GetFileInformationByHandle(dirHandle.Get(), &fi) &&
                (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                outDir = candidate;
                return true;
            }
            dirHandle.Reset();
            RemoveDirectoryW(candidate.c_str());
            if (outError) *outError = "Временный каталог является точкой повторного анализа.";
            return false;
        }
    }

    if (outError) {
        *outError = "Не удалось создать уникальный временный каталог: " +
                    FormatWin32Error(GetLastError());
    }
    return false;
}

bool CreateAdminOnlyTempWorkDir(std::filesystem::path& outDir, std::string* outError) {
    outDir.clear();
    wchar_t winDir[MAX_PATH]{};
    const UINT winLen = GetSystemWindowsDirectoryW(winDir, MAX_PATH);
    if (winLen == 0 || winLen >= MAX_PATH) {
        if (outError) *outError = "Не удалось определить системный каталог Windows.";
        return false;
    }

    const std::filesystem::path sysTempRoot = std::filesystem::path(winDir) / L"Temp";
    std::error_code ec;
    std::filesystem::create_directories(sysTempRoot, ec);
    {
        HandleGuard rootHandle(CreateFileW(
            sysTempRoot.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        BY_HANDLE_FILE_INFORMATION fi{};
        if (!rootHandle.IsValid() || !GetFileInformationByHandle(rootHandle.Get(), &fi) ||
            (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            if (outError) *outError = "Системный каталог Temp недоступен или является точкой повторного анализа.";
            return false;
        }
    }

    PSECURITY_DESCRIPTOR rawSd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)",
            SDDL_REVISION_1,
            &rawSd,
            nullptr) ||
        rawSd == nullptr) {
        if (outError) {
            *outError = "Не удалось подготовить ACL защищённого каталога: " +
                        FormatWin32Error(GetLastError());
        }
        return false;
    }
    LocalFreeGuard sdGuard(rawSd);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = rawSd;
    sa.bInheritHandle = FALSE;

    for (int attempt = 0; attempt < 8; ++attempt) {
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid))) {
            continue;
        }
        wchar_t suffix[64]{};
        const int suffixLen = std::swprintf(
            suffix,
            std::size(suffix),
            L"DriverLoom_Admin_%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x",
            static_cast<unsigned long>(guid.Data1),
            guid.Data2,
            guid.Data3,
            guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
            guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
        if (suffixLen <= 0 || suffixLen >= static_cast<int>(std::size(suffix))) {
            continue;
        }
        const std::filesystem::path candidate = sysTempRoot / suffix;
        if (CreateDirectoryW(candidate.c_str(), &sa)) {
            HandleGuard dirHandle(CreateFileW(
                candidate.c_str(),
                FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr));
            BY_HANDLE_FILE_INFORMATION fi{};
            if (dirHandle.IsValid() && GetFileInformationByHandle(dirHandle.Get(), &fi) &&
                (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                outDir = candidate;
                return true;
            }
            dirHandle.Reset();
            RemoveDirectoryW(candidate.c_str());
            if (outError) *outError = "Защищённый каталог является точкой повторного анализа.";
            return false;
        }
    }

    if (outError) {
        *outError = "Не удалось создать защищённый временный каталог: " +
                    FormatWin32Error(GetLastError());
    }
    return false;
}

bool IsSafeRelativeInstallerPath(std::string_view rawRel) {
    const std::string trimmed = TrimCopy(rawRel);
    if (trimmed.empty() || trimmed.size() > 260) {
        return false;
    }
    for (unsigned char ch : trimmed) {
        if (ch < 0x20 || ch == ':' || ch == '*' || ch == '?' || ch == '"' ||
            ch == '<' || ch == '>' || ch == '|') {
            return false;
        }
    }
    std::wstring relW = Utf8ToWide(trimmed);
    std::replace(relW.begin(), relW.end(), L'/', L'\\');
    if (relW.empty() || relW.front() == L'\\') {
        return false;
    }
    const std::filesystem::path rel = std::filesystem::path(relW).lexically_normal();
    if (rel.empty() || rel == L"." || rel.is_absolute() ||
        rel.has_root_name() || rel.has_root_directory()) {
        return false;
    }
    for (const auto& part : rel) {
        if (part.empty() || part == L"." || part == L"..") {
            return false;
        }
    }
    const std::string ext = ToLowerAscii(WideToUtf8(rel.extension().wstring()));
    return ext == ".exe" || ext == ".msi";
}

std::filesystem::path ResolveContainedInstallerPath(
    const std::filesystem::path& baseDir,
    std::string_view rawRel) {
    if (!IsSafeRelativeInstallerPath(rawRel)) {
        return {};
    }
    std::wstring relW = Utf8ToWide(TrimCopy(rawRel));
    std::replace(relW.begin(), relW.end(), L'/', L'\\');
    const std::filesystem::path rel = std::filesystem::path(relW).lexically_normal();

    std::error_code ec;
    const std::filesystem::path baseCanon = std::filesystem::weakly_canonical(baseDir, ec);
    if (ec || baseCanon.empty()) {
        return {};
    }
    const std::filesystem::path candCanon = std::filesystem::weakly_canonical(baseCanon / rel, ec);
    if (ec || candCanon.empty() || !std::filesystem::is_regular_file(candCanon, ec) || ec) {
        return {};
    }
    const DWORD attrs = GetFileAttributesW(candCanon.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return {};
    }

    std::wstring baseW = baseCanon.wstring();
    if (!baseW.empty() && baseW.back() != L'\\') {
        baseW.push_back(L'\\');
    }
    const std::wstring candW = candCanon.wstring();
    if (candW.size() <= baseW.size() ||
        CompareStringOrdinal(
            candW.c_str(), static_cast<int>(baseW.size()),
            baseW.c_str(), static_cast<int>(baseW.size()), TRUE) != CSTR_EQUAL) {
        return {};
    }
    return candCanon;
}

std::string SanitizeWingetSilentArgs(std::string_view rawArgs) {
    const std::string trimmed = TrimCopy(rawArgs);
    if (trimmed.empty() || trimmed.size() > 160) {
        return {};
    }
    constexpr std::string_view kForbidden = "&|;<>%$\\:*?()\"'`{}[]";
    for (unsigned char ch : trimmed) {
        if (ch < 0x20 || ch >= 0x7F || kForbidden.find(static_cast<char>(ch)) != std::string_view::npos) {
            return {};
        }
    }

    auto isAllowedSilentToken = [](std::string_view token) -> bool {
        const std::string lower = ToLowerAscii(token);
        if (lower.find("http") != std::string::npos) {
            return false;
        }
        static constexpr std::string_view kAllowedFlags[] = {
            "/s", "/silent", "/verysilent", "/q", "/qn", "/qb", "/quiet",
            "/passive", "/norestart", "/noreboot", "/install",
            "/suppressmsgboxes", "/sp-", "/forcerestart", "/exenoui",
            "--silent", "--quiet", "-s", "-q",
            "reboot=reallysuppress", "agree=yes"
        };
        for (std::string_view allowed : kAllowedFlags) {
            if (lower == allowed) {
                return true;
            }
        }
        return false;
    };

    std::istringstream iss(trimmed);
    std::string tok;
    std::string sanitized;
    while (iss >> tok) {
        if (tok.find("..") != std::string::npos || tok.find("//") != std::string::npos) {
            return {};
        }
        bool validChars = true;
        for (unsigned char ch : tok) {
            const bool ok = std::isalnum(ch) || ch == '/' || ch == '-' ||
                            ch == '.' || ch == '_' || ch == '=';
            if (!ok) {
                validChars = false;
                break;
            }
        }
        if (!validChars || !isAllowedSilentToken(tok)) {
            return {};
        }
        if (!sanitized.empty()) {
            sanitized.push_back(' ');
        }
        sanitized += tok;
    }
    return sanitized;
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

bool HasMsiMagicHeader(const std::filesystem::path& filePath) {
    std::ifstream ifs(filePath, std::ios::binary);
    if (!ifs.is_open()) {
        return false;
    }
    unsigned char hdr[8]{};
    ifs.read(reinterpret_cast<char*>(hdr), 8);
    return ifs.gcount() == 8 &&
           hdr[0] == 0xD0 && hdr[1] == 0xCF && hdr[2] == 0x11 && hdr[3] == 0xE0 &&
           hdr[4] == 0xA1 && hdr[5] == 0xB1 && hdr[6] == 0x1A && hdr[7] == 0xE1;
}

UINT CALLBACK CabExtractCallback(PVOID ctx, UINT notification, UINT_PTR param1, UINT_PTR param2) {
    (void)param2;
    if (notification == SPFILENOTIFY_NEEDNEWCABINET) {
        return FILEOP_ABORT;
    }
    if (notification == SPFILENOTIFY_FILEINCABINET) {
        auto* info = reinterpret_cast<FILE_IN_CABINET_INFO_W*>(param1);
        auto* cabCtx = static_cast<CabExtractContext*>(ctx);
        if (info == nullptr || info->NameInCabinet == nullptr || cabCtx == nullptr) {
            return FILEOP_ABORT;
        }
        const std::wstring_view rawName(info->NameInCabinet);
        if (rawName.empty() || rawName.find(L':') != std::wstring_view::npos) {
            SetLastError(ERROR_ACCESS_DENIED);
            return FILEOP_ABORT;
        }
        const std::filesystem::path rel = std::filesystem::path(rawName).lexically_normal();
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
        if (cabCtx->fileCount >= cabCtx->maxFiles ||
            static_cast<uint64_t>(info->FileSize) > cabCtx->maxTotalBytes - cabCtx->totalBytes) {
            SetLastError(ERROR_DISK_FULL);
            return FILEOP_ABORT;
        }

        const std::filesystem::path dest = (cabCtx->baseDir / rel).lexically_normal();
        const std::wstring destW = dest.wstring();
        if (destW.size() >= MAX_PATH) {
            SetLastError(ERROR_FILENAME_EXCED_RANGE);
            return FILEOP_ABORT;
        }

        auto getCanonDir = [](const std::filesystem::path& p) -> std::wstring {
            HandleGuard h(CreateFileW(
                p.c_str(),
                FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr));
            BY_HANDLE_FILE_INFORMATION fi{};
            if (!h.IsValid() || !GetFileInformationByHandle(h.Get(), &fi) ||
                (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                return {};
            }
            const DWORD needed = GetFinalPathNameByHandleW(h.Get(), nullptr, 0, FILE_NAME_NORMALIZED);
            if (needed == 0) return {};
            std::wstring buf(static_cast<size_t>(needed) + 1, L'\0');
            const DWORD len = GetFinalPathNameByHandleW(h.Get(), buf.data(), needed + 1, FILE_NAME_NORMALIZED);
            if (len == 0 || len >= buf.size()) return {};
            buf.resize(len);
            while (buf.size() > 3 && (buf.back() == L'\\' || buf.back() == L'/')) {
                buf.pop_back();
            }
            return buf;
        };
        const std::wstring baseCanon = getCanonDir(cabCtx->baseDir);
        if (baseCanon.empty()) {
            SetLastError(ERROR_ACCESS_DENIED);
            return FILEOP_ABORT;
        }
        auto isWithinBase = [&](const std::wstring& dirCanon) -> bool {
            if (dirCanon.empty() || dirCanon.size() < baseCanon.size() ||
                _wcsnicmp(dirCanon.c_str(), baseCanon.c_str(), baseCanon.size()) != 0) {
                return false;
            }
            return dirCanon.size() == baseCanon.size() || dirCanon[baseCanon.size()] == L'\\';
        };
        std::filesystem::path curDir = cabCtx->baseDir;
        for (const auto& part : rel.parent_path()) {
            if (part.empty() || part == L".") {
                continue;
            }
            curDir /= part;
            if (!CreateDirectoryW(curDir.c_str(), nullptr)) {
                const DWORD dirErr = GetLastError();
                if (dirErr != ERROR_ALREADY_EXISTS) {
                    return FILEOP_ABORT;
                }
            }
            if (!isWithinBase(getCanonDir(curDir))) {
                SetLastError(ERROR_ACCESS_DENIED);
                return FILEOP_ABORT;
            }
        }

        ++cabCtx->fileCount;
        cabCtx->totalBytes += static_cast<uint64_t>(info->FileSize);
        wcsncpy_s(info->FullTargetName, MAX_PATH, destW.c_str(), _TRUNCATE);
        return FILEOP_DOIT;
    }
    return NO_ERROR;
}

namespace {

std::string ReadCertFieldUtf8(PCCERT_CONTEXT cert, DWORD type, void* typePara) {
    if (cert == nullptr) {
        return {};
    }
    const DWORD needed = CertGetNameStringW(cert, type, 0, typePara, nullptr, 0);
    if (needed <= 1) {
        return {};
    }
    std::wstring buf(static_cast<size_t>(needed), L'\0');
    const DWORD written = CertGetNameStringW(cert, type, 0, typePara, buf.data(), needed);
    if (written <= 1) {
        return {};
    }
    return WideToUtf8(buf.c_str());
}

bool ContainsWholeTokenSequence(std::string_view haystack, std::string_view token) {
    if (token.empty() || haystack.size() < token.size()) {
        return false;
    }
    auto isWordChar = [](unsigned char c) noexcept {
        return std::isalnum(c) != 0;
    };
    size_t pos = 0;
    while ((pos = haystack.find(token, pos)) != std::string_view::npos) {
        const bool leftOk = (pos == 0) || !isWordChar(static_cast<unsigned char>(haystack[pos - 1]));
        const size_t endPos = pos + token.size();
        const bool rightOk =
            (endPos == haystack.size()) || !isWordChar(static_cast<unsigned char>(haystack[endPos]));
        if (leftOk && rightOk) {
            return true;
        }
        ++pos;
    }
    return false;
}

bool MatchesAnyPublisher(
    const std::string& subjectLower,
    const std::string& cnLower,
    const std::string& orgLower,
    const std::vector<std::string_view>& allowedPublishers) {
    if (allowedPublishers.empty()) {
        return true;
    }
    for (std::string_view rawToken : allowedPublishers) {
        const std::string token = ToLowerAscii(rawToken);
        if (token.empty()) {
            continue;
        }
        if (ContainsWholeTokenSequence(cnLower, token) ||
            ContainsWholeTokenSequence(orgLower, token) ||
            ContainsWholeTokenSequence(subjectLower, token)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool MatchesAllowedPublisher(
    std::string_view subject,
    std::string_view cn,
    std::string_view org,
    const std::vector<std::string_view>& allowedPublishers) {
    return MatchesAnyPublisher(
        ToLowerAscii(subject),
        ToLowerAscii(cn),
        ToLowerAscii(org),
        allowedPublishers);
}

bool ComputeFileSha256Hex(
    const std::filesystem::path& filePath,
    std::string& outHex,
    std::string* outReason) {
    outHex.clear();
    const std::wstring wPath = filePath.wstring();
    HandleGuard file(CreateFileW(
        wPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!file.IsValid()) {
        if (outReason != nullptr) *outReason = FormatWin32Error(GetLastError());
        return false;
    }
    BY_HANDLE_FILE_INFORMATION fi{};
    if (!GetFileInformationByHandle(file.Get(), &fi) ||
        (fi.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        if (outReason != nullptr) *outReason = "Файл является точкой повторного анализа или каталогом.";
        return false;
    }

    struct BcryptAlgGuard {
        BCRYPT_ALG_HANDLE h{nullptr};
        ~BcryptAlgGuard() {
            if (h != nullptr) BCryptCloseAlgorithmProvider(h, 0);
        }
    } alg;
    if (BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        if (outReason != nullptr) *outReason = "Не удалось инициализировать CNG SHA-256.";
        return false;
    }

    DWORD hashObjSize = 0;
    DWORD cbData = 0;
    if (BCryptGetProperty(
            alg.h, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&hashObjSize),
            sizeof(hashObjSize), &cbData, 0) < 0 ||
        hashObjSize == 0) {
        if (outReason != nullptr) *outReason = "Ошибка получения размера хэш-объекта SHA-256.";
        return false;
    }

    std::vector<UCHAR> hashObj(hashObjSize);
    struct BcryptHashGuard {
        BCRYPT_HASH_HANDLE h{nullptr};
        ~BcryptHashGuard() {
            if (h != nullptr) BCryptDestroyHash(h);
        }
    } hash;
    if (BCryptCreateHash(alg.h, &hash.h, hashObj.data(), hashObjSize, nullptr, 0, 0) < 0) {
        if (outReason != nullptr) *outReason = "Не удалось создать контекст SHA-256.";
        return false;
    }

    std::vector<UCHAR> readBuf(65536);
    for (;;) {
        DWORD bytesRead = 0;
        if (!ReadFile(file.Get(), readBuf.data(), static_cast<DWORD>(readBuf.size()), &bytesRead, nullptr)) {
            if (outReason != nullptr) *outReason = FormatWin32Error(GetLastError());
            return false;
        }
        if (bytesRead == 0) {
            break;
        }
        if (BCryptHashData(hash.h, readBuf.data(), bytesRead, 0) < 0) {
            if (outReason != nullptr) *outReason = "Ошибка вычисления SHA-256.";
            return false;
        }
    }

    UCHAR digest[32]{};
    if (BCryptFinishHash(hash.h, digest, sizeof(digest), 0) < 0) {
        if (outReason != nullptr) *outReason = "Не удалось завершить вычисление SHA-256.";
        return false;
    }

    static constexpr char kHex[] = "0123456789abcdef";
    outHex.reserve(64);
    for (UCHAR b : digest) {
        outHex.push_back(kHex[(b >> 4) & 0x0Fu]);
        outHex.push_back(kHex[b & 0x0Fu]);
    }
    return true;
}

bool VerifyAndLockAuthenticodeFile(
    const std::filesystem::path& filePath,
    HandleGuard* outReadLock,
    std::string* outReason,
    const std::vector<std::string_view>& allowedPublishers) {
    if (outReadLock != nullptr) {
        outReadLock->Reset();
    }
    const std::wstring wPath = filePath.wstring();
    const DWORD attrs = GetFileAttributesW(wPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        if (outReason != nullptr) {
            *outReason = (attrs == INVALID_FILE_ATTRIBUTES)
                             ? FormatWin32Error(GetLastError())
                             : "Файл является точкой повторного анализа (reparse point).";
        }
        return false;
    }

    // Hold FILE_SHARE_READ without FILE_SHARE_WRITE/FILE_SHARE_DELETE across WinVerifyTrust
    // and installer launch to close the TOCTOU file-swap window.
    HandleGuard fileLock(CreateFileW(
        wPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!fileLock.IsValid()) {
        if (outReason != nullptr) {
            *outReason = FormatWin32Error(GetLastError());
        }
        return false;
    }
    BY_HANDLE_FILE_INFORMATION bhfi{};
    if (!GetFileInformationByHandle(fileLock.Get(), &bhfi) ||
        (bhfi.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        if (outReason != nullptr) {
            *outReason = "Файл является точкой повторного анализа или каталогом.";
        }
        return false;
    }

    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = wPath.c_str();
    fileInfo.hFile = fileLock.Get();

    WINTRUST_DATA trustData{};
    trustData.cbStruct = sizeof(trustData);
    trustData.dwUIChoice = WTD_UI_NONE;
    trustData.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    trustData.dwUnionChoice = WTD_CHOICE_FILE;
    trustData.pFile = &fileInfo;
    trustData.dwStateAction = WTD_STATEACTION_VERIFY;
    trustData.dwProvFlags = WTD_SAFER_FLAG | WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;

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
        if (!allowedPublishers.empty()) {
            bool signerMatched = false;
            std::string actualSigner;
            CRYPT_PROVIDER_DATA* provData = WTHelperProvDataFromStateData(trustData.hWVTStateData);
            if (provData != nullptr) {
                CRYPT_PROVIDER_SGNR* signer = WTHelperGetProvSignerFromChain(provData, 0, FALSE, 0);
                if (signer != nullptr) {
                    CRYPT_PROVIDER_CERT* leafCert = WTHelperGetProvCertFromChain(signer, 0);
                    if (leafCert != nullptr && leafCert->pCert != nullptr) {
                        const std::string simple = ReadCertFieldUtf8(
                            leafCert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, nullptr);
                        const std::string cn = ReadCertFieldUtf8(
                            leafCert->pCert, CERT_NAME_ATTR_TYPE,
                            const_cast<LPSTR>(szOID_COMMON_NAME));
                        const std::string org = ReadCertFieldUtf8(
                            leafCert->pCert, CERT_NAME_ATTR_TYPE,
                            const_cast<LPSTR>(szOID_ORGANIZATION_NAME));
                        actualSigner = !org.empty() ? org : (!cn.empty() ? cn : simple);
                        signerMatched = MatchesAllowedPublisher(
                            simple,
                            cn,
                            org,
                            allowedPublishers);
                    }
                }
            }
            if (!signerMatched) {
                if (outReason != nullptr) {
                    *outReason = actualSigner.empty()
                                     ? "Не удалось определить издателя сертификата."
                                     : ("Неожиданный издатель подписи: " + actualSigner);
                }
                return false;
            }
        }
        if (outReadLock != nullptr) {
            *outReadLock = std::move(fileLock);
        }
        return true;
    }
    if (outReason != nullptr) {
        *outReason = FormatWin32Error(static_cast<DWORD>(status));
    }
    return false;
}

const std::vector<std::string_view>& DefaultCabPublishers() {
    static const std::vector<std::string_view> kCabPublishers{
        "Microsoft Corporation",
        "Microsoft Windows",
        "Intel Corporation",
        "Realtek Semiconductor",
        "Advanced Micro Devices",
        "NVIDIA Corporation",
        "Broadcom",
        "Qualcomm",
        "MediaTek",
        "ASUSTeK Computer",
    };
    return kCabPublishers;
}

const std::vector<std::string_view>& KnownAllowedPackagePublishers() {
    static const std::vector<std::string_view> kKnownPublishers{
        "Microsoft Corporation",
        "Microsoft Windows",
        "Intel Corporation",
        "Realtek Semiconductor",
        "Advanced Micro Devices",
        "NVIDIA Corporation",
        "Broadcom",
        "Qualcomm",
        "MediaTek",
        "ASUSTeK Computer",
        "ASUS",
        "LunarG",
        "Khronos",
        "Creative",
        "OpenAL",
        "Razer",
        "Logitech",
        "SteelSeries",
        "Corsair",
        "EPOS",
        "Sennheiser",
        "DSEA",
        "Elgato",
        "Dell Inc",
        "Dell Technologies",
        "Micro-Star",
        "Samsung Electronics",
        "Philips",
        "MMD",
        "TPV",
        "Portrait Displays",
        "ViewSonic",
        "LG Electronics",
        "BenQ",
        "AOC",
        "Gigabyte",
        "Lenovo",
        "Acer",
        "HP Inc",
        "Hewlett-Packard",
        "Hewlett Packard",
    };
    return kKnownPublishers;
}

bool IsKnownAllowedPublisher(std::string_view token) {
    for (std::string_view allowed : KnownAllowedPackagePublishers()) {
        if (token == allowed) {
            return true;
        }
    }
    return false;
}

namespace {

bool InspectPeBinaryHeader(const std::filesystem::path& filePath, bool& outIsPe) {
    outIsPe = false;
    HandleGuard file(CreateFileW(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!file.IsValid()) {
        return false;
    }
    BY_HANDLE_FILE_INFORMATION fi{};
    if (!GetFileInformationByHandle(file.Get(), &fi) ||
        (fi.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        return false;
    }
    const uint64_t fileSize =
        (static_cast<uint64_t>(fi.nFileSizeHigh) << 32) |
        static_cast<uint64_t>(fi.nFileSizeLow);
    if (fileSize < 64ULL) {
        return true;
    }
    BYTE dosHdr[64]{};
    DWORD readBytes = 0;
    if (!ReadFile(file.Get(), dosHdr, sizeof(dosHdr), &readBytes, nullptr) || readBytes != sizeof(dosHdr)) {
        return false;
    }
    if (dosHdr[0] != 'M' || dosHdr[1] != 'Z') {
        return true;
    }
    uint32_t eLfanew = 0;
    std::memcpy(&eLfanew, dosHdr + 0x3C, sizeof(eLfanew));
    if (eLfanew < 4ULL || static_cast<uint64_t>(eLfanew) > fileSize - 4ULL) {
        return true;
    }
    LARGE_INTEGER offset{};
    offset.QuadPart = static_cast<LONGLONG>(eLfanew);
    if (!SetFilePointerEx(file.Get(), offset, nullptr, FILE_BEGIN)) {
        return false;
    }
    BYTE peSig[4]{};
    readBytes = 0;
    if (!ReadFile(file.Get(), peSig, sizeof(peSig), &readBytes, nullptr) || readBytes != sizeof(peSig)) {
        return false;
    }
    outIsPe = (peSig[0] == 'P' && peSig[1] == 'E' && peSig[2] == 0 && peSig[3] == 0);
    return true;
}

bool IsForbiddenScriptOrRedirectExtension(std::string_view ext) {
    static constexpr std::string_view kBlockedExts[] = {
        ".bat", ".cmd", ".ps1", ".vbs", ".vbe", ".js", ".jse",
        ".wsf", ".wsh", ".hta", ".lnk", ".reg", ".local", ".config",
    };
    for (std::string_view b : kBlockedExts) {
        if (ext == b) {
            return true;
        }
    }
    return false;
}

bool IsBinaryVerificationExtension(std::string_view ext) {
    static constexpr std::string_view kBinaryExts[] = {
        ".exe", ".dll", ".sys", ".msi", ".cab", ".ocx", ".cpl",
        ".msp", ".mst", ".scr", ".drv", ".ax", ".com", ".pyd", ".mui",
    };
    for (std::string_view b : kBinaryExts) {
        if (ext == b) {
            return true;
        }
    }
    return false;
}

} // namespace

bool VerifyExtractedDirectoryBinaries(
    const std::filesystem::path& dir,
    const std::vector<std::string_view>& allowedPublishers,
    std::string* outReason,
    std::vector<HandleGuard>* outLocks) {
    if (outLocks != nullptr) {
        outLocks->clear();
    }
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(
             dir,
             std::filesystem::directory_options::none,
             ec);
         !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        const DWORD attrs = GetFileAttributesW(it->path().c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            if (outReason != nullptr) {
                *outReason = "Обнаружена точка повторного анализа (reparse point) в распакованном архиве.";
            }
            return false;
        }
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        const std::string ext = ToLowerAscii(WideToUtf8(it->path().extension().wstring()));
        if (IsForbiddenScriptOrRedirectExtension(ext)) {
            if (outReason != nullptr) {
                *outReason = "Обнаружен запрещённый файл сценария или перенаправления загрузчика: " +
                             WideToUtf8(it->path().filename().wstring());
            }
            return false;
        }
        bool needsAuthVerify = IsBinaryVerificationExtension(ext);
        if (!needsAuthVerify) {
            bool isPe = false;
            if (!InspectPeBinaryHeader(it->path(), isPe)) {
                if (outReason != nullptr) {
                    *outReason = WideToUtf8(it->path().filename().wstring()) +
                                 ": не удалось открыть файл для проверки заголовка.";
                }
                return false;
            }
            needsAuthVerify = isPe;
        }
        if (needsAuthVerify) {
            std::string reason;
            HandleGuard fileLock;
            if (!VerifyAndLockAuthenticodeFile(
                    it->path(),
                    outLocks != nullptr ? &fileLock : nullptr,
                    &reason,
                    allowedPublishers)) {
                if (outReason != nullptr) {
                    *outReason = WideToUtf8(it->path().filename().wstring()) + ": " + reason;
                }
                return false;
            }
            if (outLocks != nullptr && fileLock.IsValid()) {
                outLocks->push_back(std::move(fileLock));
            }
        }
    }
    if (ec && outReason != nullptr && outReason->empty()) {
        *outReason = "Ошибка обхода распакованного каталога.";
    }
    return !ec;
}

bool LaunchAndWaitProcess(
    const std::filesystem::path& exePath,
    const std::wstring& args,
    DWORD* outExitCode,
    DWORD* outLaunchErr,
    const std::atomic<bool>* cancelFlag,
    const std::atomic<bool>* stopFlag,
    bool* outProcessStillRunning) {
    if (outProcessStillRunning != nullptr) {
        *outProcessStillRunning = false;
    }
    const std::wstring wFile = exePath.wstring();
    const std::wstring wDir = exePath.parent_path().wstring();
    HandleGuard proc;

    if (IsProcessElevated()) {
        std::wstring cmdLine = QuoteWin32Arg(wFile);
        if (!args.empty()) {
            cmdLine += L" ";
            cmdLine += args;
        }
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWNORMAL;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(
                wFile.c_str(),
                cmdLine.data(),
                nullptr,
                nullptr,
                FALSE,
                0,
                nullptr,
                wDir.empty() ? nullptr : wDir.c_str(),
                &si,
                &pi)) {
            if (outLaunchErr) *outLaunchErr = GetLastError();
            return false;
        }
        HandleGuard threadGuard(pi.hThread);
        proc.Reset(pi.hProcess);
    } else {
        CoInitGuard comInit(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.hwnd = GetMainWindowHandle();
        sei.lpVerb = L"runas";
        sei.lpFile = wFile.c_str();
        sei.lpParameters = args.empty() ? nullptr : args.c_str();
        sei.lpDirectory = wDir.empty() ? nullptr : wDir.c_str();
        sei.nShow = SW_SHOWNORMAL;

        if (!ShellExecuteExW(&sei)) {
            if (outLaunchErr) *outLaunchErr = GetLastError();
            return false;
        }
        if (sei.hProcess != nullptr) {
            proc.Reset(sei.hProcess);
        }
    }

    if (proc.IsValid()) {
        for (;;) {
            const DWORD wait = WaitForSingleObject(proc.Get(), 200);
            if (wait == WAIT_OBJECT_0) {
                break;
            }
            if ((stopFlag != nullptr && stopFlag->load()) ||
                (cancelFlag != nullptr && cancelFlag->load())) {
                DWORD waitTerm = WAIT_TIMEOUT;
                if (TerminateProcess(proc.Get(), ERROR_CANCELLED)) {
                    waitTerm = WaitForSingleObject(proc.Get(), 2000);
                } else {
                    const DWORD termErr = GetLastError();
                    waitTerm = WaitForSingleObject(proc.Get(), termErr == ERROR_ACCESS_DENIED ? 0 : 500);
                }
                if (outProcessStillRunning != nullptr && waitTerm != WAIT_OBJECT_0) {
                    *outProcessStillRunning = true;
                }
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

bool IsValidSha256Hex(std::string_view hex) {
    if (hex.size() != 64) {
        return false;
    }
    for (unsigned char ch : hex) {
        if (std::isxdigit(ch) == 0) {
            return false;
        }
    }
    return true;
}

bool ParsePipeSeparatedPublishers(
    std::string_view raw,
    std::vector<std::string>& outPublishers) {
    outPublishers.clear();
    if (raw.empty()) {
        return false;
    }
    size_t start = 0;
    while (start <= raw.size()) {
        const size_t sep = raw.find('|', start);
        const std::string_view part =
            (sep == std::string_view::npos) ? raw.substr(start) : raw.substr(start, sep - start);
        const std::string token = TrimCopy(part);
        if (token.size() < 2 || token.size() > 128) {
            outPublishers.clear();
            return false;
        }
        for (unsigned char ch : token) {
            if (ch < 0x20 || ch >= 0x7F) {
                outPublishers.clear();
                return false;
            }
        }
        if (!IsKnownAllowedPublisher(token)) {
            outPublishers.clear();
            return false;
        }
        outPublishers.push_back(token);
        if (sep == std::string_view::npos) {
            break;
        }
        start = sep + 1;
    }
    return !outPublishers.empty();
}

bool CopyFileBoundedNoReparse(
    const std::filesystem::path& srcPath,
    const std::filesystem::path& dstPath,
    uint64_t maxBytes,
    std::string* outError) {
    if (maxBytes == 0 || srcPath.empty() || dstPath.empty()) {
        if (outError) *outError = "Некорректные параметры копирования пакета.";
        return false;
    }

    HandleGuard srcHandle(CreateFileW(
        srcPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!srcHandle.IsValid()) {
        if (outError) *outError = "Не удалось открыть исходный пакет: " + FormatWin32Error(GetLastError());
        return false;
    }

    BY_HANDLE_FILE_INFORMATION srcInfo{};
    if (!GetFileInformationByHandle(srcHandle.Get(), &srcInfo) ||
        (srcInfo.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        if (outError) *outError = "Исходный файл пакета является каталогом или точкой повторного анализа.";
        return false;
    }

    LARGE_INTEGER srcSize{};
    if (!GetFileSizeEx(srcHandle.Get(), &srcSize) || srcSize.QuadPart <= 0 ||
        static_cast<uint64_t>(srcSize.QuadPart) > maxBytes) {
        if (outError) *outError = "Размер пакета недопустим или превышает допустимый лимит.";
        return false;
    }

    HandleGuard dstHandle(CreateFileW(
        dstPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!dstHandle.IsValid()) {
        if (outError) *outError = "Не удалось создать файл пакета в защищённом каталоге: " + FormatWin32Error(GetLastError());
        return false;
    }

    std::vector<char> copyBuf(65536);
    uint64_t totalCopied = 0;
    bool copyOk = true;
    std::string errMsg;
    for (;;) {
        DWORD readBytes = 0;
        if (!ReadFile(srcHandle.Get(), copyBuf.data(), static_cast<DWORD>(copyBuf.size()), &readBytes, nullptr)) {
            copyOk = false;
            errMsg = "Ошибка чтения исходного пакета: " + FormatWin32Error(GetLastError());
            break;
        }
        if (readBytes == 0) {
            break;
        }
        if (static_cast<uint64_t>(readBytes) > maxBytes - totalCopied) {
            copyOk = false;
            errMsg = "Размер пакета превысил допустимый лимит при копировании.";
            break;
        }
        DWORD offset = 0;
        while (offset < readBytes) {
            DWORD written = 0;
            if (!WriteFile(dstHandle.Get(), copyBuf.data() + offset, readBytes - offset, &written, nullptr) ||
                written == 0) {
                copyOk = false;
                errMsg = "Ошибка записи пакета в защищённый каталог: " + FormatWin32Error(GetLastError());
                break;
            }
            offset += written;
        }
        if (!copyOk) {
            break;
        }
        totalCopied += readBytes;
    }
    if (copyOk && totalCopied == 0) {
        copyOk = false;
        errMsg = "Исходный файл пакета пуст.";
    }
    if (copyOk && !FlushFileBuffers(dstHandle.Get())) {
        copyOk = false;
        errMsg = "Ошибка завершения записи пакета: " + FormatWin32Error(GetLastError());
    }
    dstHandle.Reset();
    srcHandle.Reset();

    if (!copyOk) {
        std::error_code rmEc;
        std::filesystem::remove(dstPath, rmEc);
        if (outError) *outError = errMsg;
        return false;
    }
    return true;
}

bool StageAndVerifyPackageIntoDir(
    const std::filesystem::path& workDir,
    const StagedPackageOptions& opts,
    std::filesystem::path& outTargetExe,
    std::vector<HandleGuard>& outLocks,
    std::string& outFileVersion,
    std::string& outError,
    const std::atomic<bool>* cancelFlag) {
    outTargetExe.clear();
    outLocks.clear();
    outFileVersion.clear();
    outError.clear();

    if (cancelFlag != nullptr && cancelFlag->load()) {
        outError = "Установка отменена.";
        return false;
    }
    if (workDir.empty() || opts.sourcePackagePath.empty() || opts.allowedPublishers.empty()) {
        outError = "Некорректные параметры или отсутствует список доверенных издателей.";
        return false;
    }
    if (!opts.expectedSha256.empty() && !IsValidSha256Hex(opts.expectedSha256)) {
        outError = "Некорректный формат SHA-256.";
        return false;
    }
    if (!opts.installerArgs.empty() && SanitizeWingetSilentArgs(opts.installerArgs).empty()) {
        outError = "Недопустимые аргументы запуска установщика.";
        return false;
    }

    const std::string srcExt = ToLowerAscii(WideToUtf8(opts.sourcePackagePath.extension().wstring()));
    const wchar_t* initialExt =
        (srcExt == ".zip") ? L".zip" : ((srcExt == ".msi") ? L".msi" : L".exe");
    std::filesystem::path stagedFile = workDir / (std::wstring(L"package") + initialExt);

    if (!CopyFileBoundedNoReparse(
            opts.sourcePackagePath, stagedFile, opts.maxPackageBytes, &outError)) {
        return false;
    }
    if (cancelFlag != nullptr && cancelFlag->load()) {
        outError = "Установка отменена.";
        return false;
    }

    if (!opts.expectedSha256.empty()) {
        std::string actualSha;
        std::string shaErr;
        if (!ComputeFileSha256Hex(stagedFile, actualSha, &shaErr)) {
            outError = "Не удалось проверить SHA-256 пакета: " + shaErr;
            return false;
        }
        if (ToLowerAscii(actualSha) != ToLowerAscii(opts.expectedSha256)) {
            outError = "Контрольная сумма SHA-256 пакета не совпадает с ожидаемой.";
            return false;
        }
    }

    bool isZipPackage = (srcExt == ".zip");
    if (!isZipPackage && HasZipMagicHeader(stagedFile)) {
        std::filesystem::path zipRenamed = stagedFile;
        zipRenamed.replace_extension(L".zip");
        std::error_code rec;
        std::filesystem::rename(stagedFile, zipRenamed, rec);
        if (!rec) {
            stagedFile = zipRenamed;
            isZipPackage = true;
        }
    } else if (!isZipPackage && srcExt != ".msi" && HasMsiMagicHeader(stagedFile)) {
        std::filesystem::path msiRenamed = stagedFile;
        msiRenamed.replace_extension(L".msi");
        std::error_code rec;
        std::filesystem::rename(stagedFile, msiRenamed, rec);
        if (!rec) {
            stagedFile = msiRenamed;
        }
    }

    std::vector<std::string_view> pubViews;
    pubViews.reserve(opts.allowedPublishers.size());
    for (const auto& p : opts.allowedPublishers) {
        if (!p.empty()) {
            if (!IsKnownAllowedPublisher(p)) {
                outError = "Указан недоверенный издатель установки.";
                return false;
            }
            pubViews.push_back(p);
        }
    }
    if (pubViews.empty()) {
        outError = "Отсутствует список доверенных издателей.";
        return false;
    }

    if (isZipPackage) {
        const std::filesystem::path extractDir = workDir / L"pkg_extract";
        std::string zipErr;
        if (!ExtractZipSafely(stagedFile, extractDir, cancelFlag, &zipErr)) {
            outError = zipErr.empty()
                           ? "Не удалось безопасно распаковать ZIP-архив."
                           : ("Не удалось безопасно распаковать ZIP-архив: " + zipErr);
            return false;
        }

        std::filesystem::path candidateExe;
        if (!opts.nestedInstallerHint.empty()) {
            candidateExe = ResolveContainedInstallerPath(extractDir, opts.nestedInstallerHint);
        }
        if (candidateExe.empty()) {
            std::filesystem::path firstMsi;
            std::filesystem::path setupExe;
            std::filesystem::path firstExe;
            std::error_code ec;
            for (std::filesystem::recursive_directory_iterator it(
                     extractDir,
                     std::filesystem::directory_options::none,
                     ec);
                 !ec && it != std::filesystem::recursive_directory_iterator();
                 it.increment(ec)) {
                std::error_code entryEc;
                if (!it->is_regular_file(entryEc)) {
                    continue;
                }
                const DWORD attrs = GetFileAttributesW(it->path().c_str());
                if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
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
            if (!firstMsi.empty()) candidateExe = firstMsi;
            else if (!setupExe.empty()) candidateExe = setupExe;
            else candidateExe = firstExe;
        }
        if (candidateExe.empty()) {
            outError = "В архиве не найден исполняемый файл установщика.";
            return false;
        }

        std::string dirSigErr;
        if (!VerifyExtractedDirectoryBinaries(extractDir, pubViews, &dirSigErr, &outLocks)) {
            outError = "Содержимое распакованного архива не прошло проверку подписи (" + dirSigErr + ").";
            outLocks.clear();
            return false;
        }
        outTargetExe = std::move(candidateExe);
    } else {
        outTargetExe = stagedFile;
    }

    if (cancelFlag != nullptr && cancelFlag->load()) {
        outError = "Установка отменена.";
        outLocks.clear();
        return false;
    }

    HandleGuard targetLock;
    std::string sigErr;
    if (!VerifyAndLockAuthenticodeFile(outTargetExe, &targetLock, &sigErr, pubViews)) {
        outError = "Цифровая подпись установщика не прошла проверку (" + sigErr + ").";
        outLocks.clear();
        return false;
    }
    if (targetLock.IsValid()) {
        outLocks.push_back(std::move(targetLock));
    }
    outFileVersion = GetFileVersionUtf8(outTargetExe);
    return true;
}

StagedPackageResult RunStagedPackageInAdminDir(
    const StagedPackageOptions& opts,
    const LogCallback& log,
    const std::atomic<bool>* cancelFlag,
    const std::atomic<bool>* stopFlag) {
    StagedPackageResult res;
    if ((cancelFlag != nullptr && cancelFlag->load()) || (stopFlag != nullptr && stopFlag->load())) {
        res.cancelled = true;
        res.launchError = ERROR_CANCELLED;
        res.errorMessage = "Установка отменена.";
        return res;
    }

    std::filesystem::path adminRoot;
    std::string dirErr;
    if (!CreateAdminOnlyTempWorkDir(adminRoot, &dirErr)) {
        res.launchError = GetLastError();
        res.errorMessage = dirErr.empty() ? "Не удалось создать защищённый каталог установки." : dirErr;
        if ((stopFlag == nullptr || !stopFlag->load()) && log) {
            log(LogLevel::Error, res.errorMessage);
        }
        return res;
    }
    TempWorkDirGuard adminDir;
    adminDir.Set(adminRoot);

    std::filesystem::path targetExe;
    std::vector<HandleGuard> locks;
    std::string stageErr;
    if (!StageAndVerifyPackageIntoDir(
            adminDir.Path(), opts, targetExe, locks, res.fileVersion, stageErr, cancelFlag)) {
        const bool cancelledNow = (cancelFlag != nullptr && cancelFlag->load());
        if (cancelledNow) {
            res.cancelled = true;
            res.launchError = ERROR_CANCELLED;
        }
        res.errorMessage = stageErr;
        if ((stopFlag == nullptr || !stopFlag->load()) && log && !stageErr.empty()) {
            log(cancelledNow ? LogLevel::Warning : LogLevel::Error, stageErr);
        }
        return res;
    }

    if ((stopFlag == nullptr || !stopFlag->load()) && log) {
        log(LogLevel::Success,
            "Подпись подтверждена" +
                (res.fileVersion.empty() ? std::string() : " (версия файла: " + res.fileVersion + ")") +
                ". Запуск установки...");
    }

    if (opts.isDirectXLegacy) {
        const std::filesystem::path dxExtractDir = adminDir.Path() / L"dx_extract";
        if (!CreateDirectoryW(dxExtractDir.c_str(), nullptr)) {
            res.launchError = GetLastError();
            res.errorMessage = "Не удалось создать защищённый каталог распаковки DirectX.";
            if ((stopFlag == nullptr || !stopFlag->load()) && log) {
                log(LogLevel::Error, res.errorMessage);
            }
            return res;
        }
        const std::wstring extractArgs = L"/Q /T:" + QuoteWin32Arg(dxExtractDir.wstring());
        const bool extracted = LaunchAndWaitProcess(
            targetExe,
            extractArgs,
            &res.exitCode,
            &res.launchError,
            cancelFlag,
            stopFlag,
            &res.processStillRunning);
        locks.clear();
        if (extracted) {
            std::error_code ec;
            const std::filesystem::path dxSetup = dxExtractDir / L"DXSETUP.exe";
            const std::filesystem::path dsetupDll = dxExtractDir / L"DSETUP.dll";
            if (res.exitCode != 0) {
                res.errorMessage =
                    "Ошибка распаковки DirectX End-User Runtime (код " + std::to_string(res.exitCode) + ").";
                if ((stopFlag == nullptr || !stopFlag->load()) && log) {
                    log(LogLevel::Error, res.errorMessage);
                }
            } else if (std::filesystem::is_regular_file(dxSetup, ec) &&
                       std::filesystem::is_regular_file(dsetupDll, ec)) {
                const std::vector<std::string_view> msPub{
                    "Microsoft Corporation", "Microsoft Windows"};
                std::vector<HandleGuard> dxLocks;
                std::string dxSigErr;
                if (!VerifyExtractedDirectoryBinaries(dxExtractDir, msPub, &dxSigErr, &dxLocks)) {
                    res.launchError = ERROR_INVALID_DATA;
                    res.errorMessage =
                        "Цифровая подпись компонентов DirectX не прошла проверку (" + dxSigErr +
                        "). Запуск отменён в целях безопасности.";
                    if ((stopFlag == nullptr || !stopFlag->load()) && log) {
                        log(LogLevel::Error, res.errorMessage);
                    }
                } else {
                    res.launched = LaunchAndWaitProcess(
                        dxSetup,
                        L"/silent",
                        &res.exitCode,
                        &res.launchError,
                        cancelFlag,
                        stopFlag,
                        &res.processStillRunning);
                    dxLocks.clear();
                }
            } else {
                res.launchError = ERROR_FILE_NOT_FOUND;
                res.errorMessage = "В распакованном пакете DirectX не найдены DXSETUP.exe или DSETUP.dll.";
                if ((stopFlag == nullptr || !stopFlag->load()) && log) {
                    log(LogLevel::Error, res.errorMessage);
                }
            }
        }
    } else if (ToLowerAscii(WideToUtf8(targetExe.extension().wstring())) == ".msi") {
        wchar_t sysBuf[MAX_PATH]{};
        const UINT sysLen = GetSystemDirectoryW(sysBuf, MAX_PATH);
        if (sysLen > 0 && sysLen < MAX_PATH) {
            const std::filesystem::path msiexec = std::filesystem::path(sysBuf) / L"msiexec.exe";
            const std::wstring msiArgs =
                L"/i " + QuoteWin32Arg(targetExe.wstring()) + L" /passive /norestart";
            res.launched = LaunchAndWaitProcess(
                msiexec,
                msiArgs,
                &res.exitCode,
                &res.launchError,
                cancelFlag,
                stopFlag,
                &res.processStillRunning);
        } else {
            res.launchError = ERROR_PATH_NOT_FOUND;
        }
        locks.clear();
    } else {
        const std::wstring safeArgs = Utf8ToWide(SanitizeWingetSilentArgs(opts.installerArgs));
        res.launched = LaunchAndWaitProcess(
            targetExe,
            safeArgs,
            &res.exitCode,
            &res.launchError,
            cancelFlag,
            stopFlag,
            &res.processStillRunning);
        locks.clear();
    }

    if (res.processStillRunning) {
        adminDir.Release();
        if ((stopFlag == nullptr || !stopFlag->load()) && log) {
            log(LogLevel::Warning,
                "Повышенный установщик ещё выполняется в фоне; временная папка не удалена.");
        }
    }
    if (!res.launched && res.launchError == ERROR_CANCELLED) {
        res.cancelled = true;
    }
    return res;
}

} // namespace hdm
