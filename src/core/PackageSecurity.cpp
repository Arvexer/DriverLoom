#include "core/PackageSecurity.hpp"
#include "core/OnlineCatalogClient.hpp"

#include <bcrypt.h>
#include <objbase.h>
#include <oleauto.h>
#include <sddl.h>
#include <setupapi.h>
#include <shldisp.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <fstream>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace hdm {

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

bool ExtractZipViaShellCom(
    const std::filesystem::path& zipPath,
    const std::filesystem::path& destDir,
    const std::atomic<bool>* cancelFlag) {
    constexpr size_t kMaxArchiveFiles = 1024;
    constexpr uint64_t kMaxExtractedBytes = 512ULL * 1024ULL * 1024ULL;

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

    auto validateZipFolderEntries = [&](auto& self,
                                      Folder* folder,
                                      size_t& totalEntries,
                                      size_t maxEntries,
                                      int depth) -> bool {
        if (folder == nullptr || depth > 16) {
            return false;
        }
        ComPtr<FolderItems> subItems;
        if (FAILED(folder->Items(subItems.Put())) || !subItems) {
            return false;
        }
        long count = 0;
        if (FAILED(subItems->get_Count(&count)) || count < 0) {
            return false;
        }
        if (depth == 0 && count == 0) {
            return false;
        }
        for (long idx = 0; idx < count; ++idx) {
            if (totalEntries >= maxEntries) {
                return false;
            }
            ++totalEntries;
            VARIANT vIdx;
            VariantInit(&vIdx);
            vIdx.vt = VT_I4;
            vIdx.lVal = idx;
            ComPtr<FolderItem> itemObj;
            if (FAILED(subItems->Item(vIdx, itemObj.Put())) || !itemObj) {
                return false;
            }
            BSTR rawName = nullptr;
            if (FAILED(itemObj->get_Name(&rawName)) || rawName == nullptr) {
                if (rawName != nullptr) SysFreeString(rawName);
                return false;
            }
            const std::wstring entryName(rawName);
            SysFreeString(rawName);
            if (entryName.empty() || entryName == L"." || entryName == L".." ||
                entryName.find(L':') != std::wstring::npos ||
                entryName.find(L"..") != std::wstring::npos ||
                entryName.find(L'/') != std::wstring::npos ||
                entryName.find(L'\\') != std::wstring::npos) {
                return false;
            }
            VARIANT_BOOL isFolder = VARIANT_FALSE;
            if (SUCCEEDED(itemObj->get_IsFolder(&isFolder)) && isFolder == VARIANT_TRUE) {
                ComPtr<IDispatch> subDisp;
                if (FAILED(itemObj->get_GetFolder(subDisp.Put())) || !subDisp) {
                    return false;
                }
                ComPtr<Folder> subFolder;
                if (FAILED(subDisp->QueryInterface(
                        IID_Folder, reinterpret_cast<void**>(subFolder.Put()))) ||
                    !subFolder) {
                    return false;
                }
                if (!self(self, subFolder.Get(), totalEntries, maxEntries, depth + 1)) {
                    return false;
                }
            }
        }
        return true;
    };

    ComPtr<FolderItems> items;
    if (FAILED(zipFolder->Items(items.Put())) || !items) {
        return false;
    }
    size_t totalPrecheckedEntries = 0;
    if (!validateZipFolderEntries(
            validateZipFolderEntries, zipFolder.Get(), totalPrecheckedEntries, kMaxArchiveFiles, 0) ||
        totalPrecheckedEntries == 0) {
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
    if (FAILED(hr)) {
        return false;
    }

    // Folder::CopyHere can finalize asynchronously; pump STA messages and poll until settled.
    size_t prevFiles = 0;
    uint64_t prevBytes = 0;
    int stableTicks = 0;
    for (int poll = 0; poll < 100; ++poll) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (cancelFlag != nullptr && cancelFlag->load()) {
            return false;
        }
        size_t curFiles = 0;
        uint64_t curBytes = 0;
        bool allReadable = true;
        std::error_code scanEc;
        for (std::filesystem::recursive_directory_iterator it(
                 destDir,
                 std::filesystem::directory_options::none,
                 scanEc);
             !scanEc && it != std::filesystem::recursive_directory_iterator();
             it.increment(scanEc)) {
            const DWORD attrs = GetFileAttributesW(it->path().c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                return false;
            }
            if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                continue;
            }
            ++curFiles;
            if (curFiles > kMaxArchiveFiles) {
                return false;
            }
            std::error_code szEc;
            const uint64_t fz = it->file_size(szEc);
            if (!szEc) {
                if (fz > kMaxExtractedBytes || curBytes > kMaxExtractedBytes - fz) {
                    return false;
                }
                curBytes += fz;
            }
            HandleGuard probeRead(CreateFileW(
                it->path().c_str(),
                GENERIC_READ,
                FILE_SHARE_READ,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr));
            if (!probeRead.IsValid()) {
                allReadable = false;
            }
        }
        if (scanEc) {
            return false;
        }
        if (allReadable && curFiles > 0 && curFiles == prevFiles && curBytes == prevBytes) {
            ++stableTicks;
            if (stableTicks >= 4) {
                return true;
            }
        } else {
            stableTicks = 0;
            prevFiles = curFiles;
            prevBytes = curBytes;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    return false;
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
        if (ext == ".exe" || ext == ".dll" || ext == ".sys" || ext == ".msi" ||
            ext == ".cab" || ext == ".ocx" || ext == ".cpl" || ext == ".msp" ||
            ext == ".mst" || ext == ".scr") {
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
    CoInitGuard comInit(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = GetMainWindowHandle();
    sei.lpVerb = IsProcessElevated() ? L"open" : L"runas";
    const std::wstring wFile = exePath.wstring();
    const std::wstring wDir = exePath.parent_path().wstring();
    sei.lpFile = wFile.c_str();
    sei.lpParameters = args.empty() ? nullptr : args.c_str();
    sei.lpDirectory = wDir.empty() ? nullptr : wDir.c_str();
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

} // namespace hdm
