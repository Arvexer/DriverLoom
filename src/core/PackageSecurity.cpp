#include "core/PackageSecurity.hpp"
#include "core/OnlineCatalogClient.hpp"

#include <bcrypt.h>
#include <objbase.h>
#include <oleauto.h>
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
        if (!validChars) {
            return {};
        }
        const bool isSwitch = (tok.size() >= 2 && (tok[0] == '/' || tok[0] == '-'));
        const size_t eqPos = tok.find('=');
        const bool isKeyVal = (eqPos != std::string::npos && eqPos > 0 && eqPos + 1 < tok.size() &&
                               std::isalnum(static_cast<unsigned char>(tok[0])));
        if (!isSwitch && !isKeyVal) {
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

    ComPtr<FolderItems> items;
    if (FAILED(zipFolder->Items(items.Put())) || !items) {
        return false;
    }
    long itemCount = 0;
    if (FAILED(items->get_Count(&itemCount)) || itemCount <= 0 ||
        static_cast<size_t>(itemCount) > kMaxArchiveFiles) {
        return false;
    }
    for (long idx = 0; idx < itemCount; ++idx) {
        VARIANT vIdx;
        VariantInit(&vIdx);
        vIdx.vt = VT_I4;
        vIdx.lVal = idx;
        ComPtr<FolderItem> itemObj;
        if (FAILED(items->Item(vIdx, itemObj.Put())) || !itemObj) {
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
            entryName.find(L"..") != std::wstring::npos) {
            return false;
        }
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

    // Folder::CopyHere can finalize asynchronously; poll until file tree settles and stays within limits.
    size_t prevFiles = 0;
    uint64_t prevBytes = 0;
    int stableTicks = 0;
    for (int poll = 0; poll < 100; ++poll) {
        if (cancelFlag != nullptr && cancelFlag->load()) {
            return false;
        }
        size_t curFiles = 0;
        uint64_t curBytes = 0;
        std::error_code scanEc;
        for (std::filesystem::recursive_directory_iterator it(
                 destDir,
                 std::filesystem::directory_options::skip_permission_denied,
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
            std::error_code szEc;
            const uint64_t fz = it->file_size(szEc);
            if (!szEc) {
                if (curBytes > kMaxExtractedBytes - fz) {
                    return false;
                }
                curBytes += fz;
            }
            if (curFiles > kMaxArchiveFiles) {
                return false;
            }
        }
        if (scanEc) {
            return false;
        }
        if (curFiles > 0 && curFiles == prevFiles && curBytes == prevBytes) {
            ++stableTicks;
            if (stableTicks >= 2) {
                return true;
            }
        } else {
            stableTicks = 0;
            prevFiles = curFiles;
            prevBytes = curBytes;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    return prevFiles > 0;
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
        std::error_code ec;
        std::filesystem::create_directories(dest.parent_path(), ec);
        if (ec) {
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
        const std::wstring parentCanon = getCanonDir(dest.parent_path());
        if (baseCanon.empty() || parentCanon.empty() ||
            parentCanon.size() < baseCanon.size() ||
            _wcsnicmp(parentCanon.c_str(), baseCanon.c_str(), baseCanon.size()) != 0 ||
            (parentCanon.size() > baseCanon.size() && parentCanon[baseCanon.size()] != L'\\')) {
            SetLastError(ERROR_ACCESS_DENIED);
            return FILEOP_ABORT;
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
        if (cnLower.find(token) != std::string::npos ||
            orgLower.find(token) != std::string::npos ||
            subjectLower.find(token) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

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

    struct BcryptHashGuard {
        BCRYPT_HASH_HANDLE h{nullptr};
        ~BcryptHashGuard() {
            if (h != nullptr) BCryptDestroyHash(h);
        }
    } hash;
    std::vector<UCHAR> hashObj(hashObjSize);
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
                        signerMatched = MatchesAnyPublisher(
                            ToLowerAscii(simple),
                            ToLowerAscii(cn),
                            ToLowerAscii(org),
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

bool VerifyExtractedDirectoryBinaries(
    const std::filesystem::path& dir,
    const std::vector<std::string_view>& allowedPublishers,
    std::string* outReason) {
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(
             dir,
             std::filesystem::directory_options::skip_permission_denied,
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
        if (ext == ".exe" || ext == ".dll" || ext == ".sys" || ext == ".msi" || ext == ".cab") {
            std::string reason;
            if (!VerifyAndLockAuthenticodeFile(it->path(), nullptr, &reason, allowedPublishers)) {
                if (outReason != nullptr) {
                    *outReason = WideToUtf8(it->path().filename().wstring()) + ": " + reason;
                }
                return false;
            }
        }
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
                TerminateProcess(proc.Get(), ERROR_CANCELLED);
                const DWORD waitTerm = WaitForSingleObject(proc.Get(), 2000);
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
