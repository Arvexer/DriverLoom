#include "core/PackageSecurity.hpp"
#include "core/OnlineCatalogClient.hpp"

#include <objbase.h>
#include <oleauto.h>
#include <setupapi.h>
#include <shldisp.h>
#include <softpub.h>
#include <wintrust.h>

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <fstream>
#include <sstream>
#include <utility>

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
        wchar_t suffix[40]{};
        std::swprintf(
            suffix,
            std::size(suffix),
            L"DriverLoom_%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x",
            static_cast<unsigned long>(guid.Data1),
            guid.Data2,
            guid.Data3,
            guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
            guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
        const std::filesystem::path candidate = tempRoot / suffix;
        if (CreateDirectoryW(candidate.c_str(), nullptr)) {
            const DWORD attrs = GetFileAttributesW(candidate.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                outDir = candidate;
                return true;
            }
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

bool VerifyAndLockAuthenticodeFile(
    const std::filesystem::path& filePath,
    HandleGuard* outReadLock,
    std::string* outReason) {
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
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!fileLock.IsValid()) {
        if (outReason != nullptr) {
            *outReason = FormatWin32Error(GetLastError());
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

bool LaunchAndWaitProcess(
    const std::filesystem::path& exePath,
    const std::wstring& args,
    DWORD* outExitCode,
    DWORD* outLaunchErr,
    const std::atomic<bool>* cancelFlag,
    const std::atomic<bool>* stopFlag) {
    CoInitGuard comInit(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

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
                TerminateProcess(proc.Get(), ERROR_CANCELLED);
                WaitForSingleObject(proc.Get(), 2000);
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
