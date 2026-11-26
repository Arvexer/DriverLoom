#include "common/AppCommon.hpp"
#include "common/Localization.hpp"
#include "core/HttpClient.hpp"
#include "core/OnlineCatalogClient.hpp"
#include "core/PackageSecurity.hpp"
#include "core/SafeZipExtractor.hpp"

#include <setupapi.h>
#include <winioctl.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failed = 0;
int g_passed = 0;

void Check(bool cond, const char* name, const std::string& detail = {}) {
    if (cond) {
        ++g_passed;
        std::cout << "[PASS] " << name << "\n";
    } else {
        ++g_failed;
        std::cerr << "[FAIL] " << name;
        if (!detail.empty()) {
            std::cerr << " -> " << detail;
        }
        std::cerr << "\n";
    }
}

void TestSha256() {
    std::filesystem::path dir;
    std::string err;
    Check(hdm::CreateUniqueTempWorkDir(dir, &err), "Sha256: CreateUniqueTempWorkDir", err);
    if (dir.empty()) {
        return;
    }
    hdm::TempWorkDirGuard guard;
    guard.Set(dir);

    const std::filesystem::path fAbc = dir / L"abc.bin";
    {
        std::ofstream ofs(fAbc, std::ios::binary);
        ofs.write("abc", 3);
    }
    const std::filesystem::path fEmpty = dir / L"empty.bin";
    {
        std::ofstream ofs(fEmpty, std::ios::binary);
    }

    std::string hex;
    std::string reason;
    bool allOk = true;
    for (int i = 0; i < 10; ++i) {
        if (!hdm::ComputeFileSha256Hex(fAbc, hex, &reason) ||
            hex != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
            allOk = false;
            break;
        }
    }
    Check(allOk, "Sha256: NIST 'abc' x10 (R3-C1 UAF check)", hex + " " + reason);

    Check(
        hdm::ComputeFileSha256Hex(fEmpty, hex, &reason) &&
            hex == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "Sha256: empty file digest",
        hex);
}

void TestAuthenticodeAndPublisher() {
    Check(
        !hdm::MatchesAllowedPublisher("DreamData Inc.", "DreamData Inc.", "DreamData Inc.", {"AMD"}),
        "Publisher: 'AMD' does not spoof 'DreamData Inc.'");
    Check(
        hdm::MatchesAllowedPublisher("Advanced Micro Devices, Inc.", "AMD Inc.", "AMD", {"AMD"}),
        "Publisher: 'AMD' matches 'AMD Inc.'");
    Check(
        !hdm::MatchesAllowedPublisher("Algorithm Corp", "Algorithm Corp", "Algorithm Corp", {"LG Electronics"}),
        "Publisher: 'LG Electronics' does not spoof 'Algorithm Corp'");
    Check(
        !hdm::MatchesAllowedPublisher("Intelligent Apps LLC", "Intelligent Apps LLC", "Intelligent Apps LLC", {"Intel Corporation"}),
        "Publisher: 'Intel Corporation' does not spoof 'Intelligent Apps LLC'");
    Check(
        hdm::MatchesAllowedPublisher(
            "Microsoft Corporation", "Microsoft Corporation", "Microsoft Corporation", {"Microsoft Corporation"}),
        "Publisher: exact 'Microsoft Corporation' matches");
    Check(
        !hdm::DefaultCabPublishers().empty(),
        "Publisher: DefaultCabPublishers is non-empty");

    const wchar_t* kSysCandidates[] = {
        L"C:\\Windows\\explorer.exe",
        L"C:\\Windows\\System32\\msiexec.exe",
        L"C:\\Windows\\System32\\cmd.exe",
        L"C:\\Windows\\System32\\notepad.exe",
        L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
    };
    std::filesystem::path signedSysFile;
    std::string sigReason;
    for (const wchar_t* cand : kSysCandidates) {
        if (std::filesystem::is_regular_file(cand) &&
            hdm::VerifyAndLockAuthenticodeFile(cand, nullptr, &sigReason, {})) {
            signedSysFile = cand;
            break;
        }
    }
    Check(!signedSysFile.empty(), "Authenticode: found embedded-signed Microsoft system binary", sigReason);
    if (!signedSysFile.empty()) {
        hdm::HandleGuard lock;
        Check(
            hdm::VerifyAndLockAuthenticodeFile(
                signedSysFile,
                &lock,
                &sigReason,
                {"Microsoft Corporation", "Microsoft Windows"}) &&
                lock.IsValid(),
            "Authenticode: Microsoft binary passes publisher check and holds read lock",
            sigReason);
        lock.Reset();
        Check(
            !hdm::VerifyAndLockAuthenticodeFile(
                signedSysFile,
                nullptr,
                &sigReason,
                {"NVIDIA Corporation"}),
            "Authenticode: Microsoft binary rejected for NVIDIA publisher");
    }
}

void TestWingetYamlParser() {
    // 1. Same-indent NestedInstallerFiles + valid SHA256 + AppsAndFeaturesEntries isolation
    const std::string yamlValid =
        "PackageVersion: 1.5.3\n"
        "InstallerType: zip\n"
        "Installers:\n"
        "- Architecture: x64\n"
        "  InstallerUrl: https://dlcdnets.asus.com/pub/ASUS/DisplayWidgetCenter.zip\n"
        "  InstallerSha256: 0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF\n"
        "  NestedInstallerFiles:\n"
        "  - RelativeFilePath: DisplayWidgetCenterSetup.msi\n"
        "  AppsAndFeaturesEntries:\n"
        "  - InstallerType: inno\n"
        "    DisplayName: Test App\n"
        "InstallerSwitches:\n"
        "  Silent: /S\n";

    const hdm::WingetManifestInfo info1 = hdm::ParseWingetInstallerYaml(yamlValid, "x64");
    Check(info1.ok, "WingetYAML: valid manifest parsed");
    Check(info1.version == "1.5.3", "WingetYAML: PackageVersion");
    Check(info1.nestedRelativePath == "DisplayWidgetCenterSetup.msi",
          "WingetYAML: same-indent NestedInstallerFiles RelativeFilePath",
          info1.nestedRelativePath);
    Check(info1.sha256 == "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
          "WingetYAML: SHA256 normalized");

    // 2. Missing SHA256 must fail closed (info.ok == false)
    const std::string yamlNoSha =
        "PackageVersion: 2.0.0\n"
        "Installers:\n"
        "- Architecture: x64\n"
        "  InstallerUrl: https://example.com/setup.exe\n";
    const hdm::WingetManifestInfo info2 = hdm::ParseWingetInstallerYaml(yamlNoSha, "x64");
    Check(!info2.ok, "WingetYAML: missing InstallerSha256 rejected (fail-closed)");

    // 3. Entry missing URL must not bleed fields into next entry
    const std::string yamlBleed =
        "PackageVersion: 3.0.0\n"
        "Installers:\n"
        "- Architecture: arm64\n"
        "  InstallerSha256: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
        "  InstallerSwitches:\n"
        "    Silent: /verysilent\n"
        "- Architecture: x64\n"
        "  InstallerUrl: https://download.msi.com/app.exe\n";
    const hdm::WingetManifestInfo info3 = hdm::ParseWingetInstallerYaml(yamlBleed, "x64");
    Check(!info3.ok, "WingetYAML: entry without own/top SHA256 does not inherit stale entry SHA256");

    // 4. Nested AppsAndFeaturesEntries InstallerType: msix must not poison outer zip entry
    const std::string yamlNestedType =
        "PackageVersion: 4.1.0\n"
        "InstallerType: zip\n"
        "Installers:\n"
        "- Architecture: x64\n"
        "  InstallerUrl: https://download.msi.com/Gaming.zip\n"
        "  InstallerSha256: bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
        "  AppsAndFeaturesEntries:\n"
        "  - InstallerType: msix\n"
        "  NestedInstallerFiles:\n"
        "  - RelativeFilePath: sub/setup.exe\n";
    const hdm::WingetManifestInfo info4 = hdm::ParseWingetInstallerYaml(yamlNestedType, "x64");
    Check(info4.ok && info4.nestedRelativePath == "sub/setup.exe",
          "WingetYAML: AppsAndFeaturesEntries msix does not filter out outer zip entry");
}

void TestVersionComparison() {
    Check(hdm::IsVersionNewer("1.2.0", "1.2.0-rc1"), "Version: 1.2.0 > 1.2.0-rc1");
    Check(!hdm::IsVersionNewer("1.2.0-rc1", "1.2.0"), "Version: !(1.2.0-rc1 > 1.2.0)");
    Check(hdm::IsVersionNewer("10.0.15", "10.0.2"), "Version: 10.0.15 > 10.0.2");
    Check(!hdm::IsVersionNewer("1.2.0", "1.2.0"), "Version: !(1.2.0 > 1.2.0)");
}

void TestJsonParsing() {
    const std::string trickyJson =
        R"({"title": "Version", "other": "bad", "Version": "2.5.0", "label": "A\u0026B"})";
    Check(
        hdm::ExtractJsonStringField(trickyJson, "Version") == "2.5.0",
        "Json: key/value confusion avoided");
    Check(
        hdm::ExtractJsonStringField(trickyJson, "label") == "A&B",
        "Json: \\u0026 decoded to &");

    const std::string dotnetIndexSample =
        R"({"releases-index":[{"channel-version":"10.0","latest-runtime":"10.0.15"},{"channel-version":"8.0","latest-runtime":"8.0.24"}]})";
    const size_t pos8 = dotnetIndexSample.find("\"8.0\"");
    const size_t objStart = dotnetIndexSample.rfind('{', pos8);
    const size_t objEnd = dotnetIndexSample.find('}', pos8);
    const std::string slice8 = dotnetIndexSample.substr(objStart, objEnd - objStart + 1);
    Check(
        hdm::ExtractJsonStringField(slice8, "latest-runtime") == "8.0.24",
        "Json: .NET releases-index channel 8.0 extraction");
}

void TestSilentArgsAndPaths() {
    Check(
        hdm::SanitizeWingetSilentArgs("/S /norestart") == "/S /norestart",
        "SilentArgs: allows /S /norestart");
    Check(
        hdm::SanitizeWingetSilentArgs("/S & calc.exe").empty(),
        "SilentArgs: rejects shell metacharacters");
    Check(
        hdm::SanitizeWingetSilentArgs("https://evil.example/a").empty(),
        "SilentArgs: rejects URLs");
    Check(
        hdm::SanitizeWingetSilentArgs("/unknownflag").empty(),
        "SilentArgs: rejects unallowlisted flag");

    Check(hdm::IsSafeRelativeInstallerPath("sub/setup.exe"), "RelPath: allows sub/setup.exe");
    Check(!hdm::IsSafeRelativeInstallerPath("../evil.exe"), "RelPath: rejects .. traversal");
    Check(!hdm::IsSafeRelativeInstallerPath("C:\\evil.exe"), "RelPath: rejects drive root");
    Check(!hdm::IsSafeRelativeInstallerPath("setup.exe:ads"), "RelPath: rejects NTFS ADS :");
    Check(!hdm::IsSafeRelativeInstallerPath("readme.txt"), "RelPath: rejects non-exe/msi");
    Check(!hdm::IsSafeRelativeInstallerPath("\\\\srv\\share\\a.exe"), "RelPath: rejects UNC");
}

void TestHttpsHostAllowlist() {
    Check(
        hdm::IsAllowedHttpsHost("https://builds.dotnet.microsoft.com/x.exe", {"microsoft.com"}),
        "Host: subdomain allowed");
    Check(
        !hdm::IsAllowedHttpsHost("https://evilmicrosoft.com/x.exe", {"microsoft.com"}),
        "Host: suffix-prefix lookalike rejected");
    Check(
        !hdm::IsAllowedHttpsHost("http://microsoft.com/x.exe", {"microsoft.com"}),
        "Host: plain HTTP rejected");
    Check(
        hdm::IsAllowedHttpsHost(
            "https://dl.razerzone.com/synapse.exe", {"rzr.to", "razer.com", "razerzone.com"}),
        "Host: sw_razer CDN allowed");
}

void TestCabExtractCallback() {
    std::filesystem::path dir;
    std::string err;
    Check(hdm::CreateUniqueTempWorkDir(dir, &err), "CabCallback: CreateUniqueTempWorkDir", err);
    if (dir.empty()) {
        return;
    }
    hdm::TempWorkDirGuard guard;
    guard.Set(dir);

    hdm::CabExtractContext ctx{};
    ctx.baseDir = dir;
    ctx.maxFiles = 2;
    ctx.maxTotalBytes = 1024;

    Check(
        hdm::CabExtractCallback(&ctx, SPFILENOTIFY_NEEDNEWCABINET, 0, 0) == FILEOP_ABORT,
        "CabCallback: aborts multi-cabinet spanning");

    FILE_IN_CABINET_INFO_W info{};
    wchar_t badTraversal[] = L"..\\evil.inf";
    info.NameInCabinet = badTraversal;
    info.FileSize = 10;
    Check(
        hdm::CabExtractCallback(&ctx, SPFILENOTIFY_FILEINCABINET, reinterpret_cast<UINT_PTR>(&info), 0) ==
            FILEOP_ABORT,
        "CabCallback: aborts traversal");

    wchar_t badAds[] = L"drv.inf:stream";
    info.NameInCabinet = badAds;
    Check(
        hdm::CabExtractCallback(&ctx, SPFILENOTIFY_FILEINCABINET, reinterpret_cast<UINT_PTR>(&info), 0) ==
            FILEOP_ABORT,
        "CabCallback: aborts NTFS ADS ':'");

    wchar_t badAbs[] = L"C:\\evil.inf";
    info.NameInCabinet = badAbs;
    Check(
        hdm::CabExtractCallback(&ctx, SPFILENOTIFY_FILEINCABINET, reinterpret_cast<UINT_PTR>(&info), 0) ==
            FILEOP_ABORT,
        "CabCallback: aborts absolute path");

    wchar_t tooLarge[] = L"huge.sys";
    info.NameInCabinet = tooLarge;
    info.FileSize = 2048;
    Check(
        hdm::CabExtractCallback(&ctx, SPFILENOTIFY_FILEINCABINET, reinterpret_cast<UINT_PTR>(&info), 0) ==
            FILEOP_ABORT,
        "CabCallback: aborts oversized file");

    wchar_t validRel[] = L"sub\\drv.inf";
    info.NameInCabinet = validRel;
    info.FileSize = 100;
    Check(
        hdm::CabExtractCallback(&ctx, SPFILENOTIFY_FILEINCABINET, reinterpret_cast<UINT_PTR>(&info), 0) ==
                FILEOP_DOIT &&
            ctx.fileCount == 1 && ctx.totalBytes == 100,
        "CabCallback: allows safe nested path within limits");
}

void TestTempAndMiscHelpers() {
    std::filesystem::path dir;
    std::string err;
    if (hdm::CreateUniqueTempWorkDir(dir, &err)) {
        hdm::TempWorkDirGuard guard;
        guard.Set(dir);
        const std::wstring name = dir.filename().wstring();
        Check(name.size() == 43 && name.rfind(L"DriverLoom_", 0) == 0,
              "TempWorkDir: full 32-hex UUID suffix without truncation");
    }

    const std::filesystem::path logFile = hdm::CreateTempLogPath(L"test");
    Check(!logFile.empty() && logFile.filename().wstring().rfind(L"DriverLoom_test_", 0) == 0,
          "TempLogPath: creates safe temp log file");
    if (!logFile.empty()) {
        std::error_code ec;
        std::filesystem::remove(logFile, ec);
    }

    Check(hdm::QuoteWin32Arg(L"D:\\") == L"\"D:\\\\\"", "QuoteWin32Arg: trailing backslash escaped");
    Check(hdm::UrlEncodeCatalogParam("Dell U2723QE & HDR") == "Dell%20U2723QE%20%26%20HDR",
          "UrlEncodeCatalogParam: encodes space and &");

    hdm::HttpRequestRegistry reg;
    reg.Shutdown();
    reg.Reset();
    Check(!reg.Register(reinterpret_cast<HINTERNET>(1)),
          "HttpRequestRegistry: Reset after Shutdown stays disabled");
}

void TestLocalizationTrDyn() {
    const hdm::Lang prevLang = hdm::GetLang();
    hdm::SetLang(hdm::Lang::En);
    Check(hdm::TrDyn("Проверка системных файлов (SFC /scannow)...") ==
              "Checking system files (SFC /scannow)...",
          "TrDyn(EN): SFC phase");
    Check(hdm::TrDyn("Установка завершена (требуется перезагрузка).") ==
              "Installation complete (Reboot required).",
          "TrDyn(EN): local inf complete with period");
    Check(hdm::TrDyn("Экспорт [1/5]: oem1.inf") == "Exporting [1/5]: oem1.inf",
          "TrDyn(EN): export progress");
    Check(hdm::TrDyn("Установка Vulkan Runtime...") == "Installing Vulkan Runtime...",
          "TrDyn(EN): component install prefix");
    Check(hdm::TrDyn("Установка драйвера Realtek...") == "Installing driver Realtek...",
          "TrDyn(EN): driver install prefix takes priority");
    Check(hdm::TrDyn("Пакетная установка завершена: успешно — 2 из 3") ==
              "Batch installation complete: succeeded — 2 of 3",
          "TrDyn(EN): batch summary");
    Check(hdm::TrDyn("Найдено обновлений в Windows Update: 4") ==
              "Windows Update updates found: 4",
          "TrDyn(EN): WU found count");
    Check(hdm::TrDyn("Ошибка сетевого поиска Windows Update") ==
              "Windows Update online search failed",
          "TrDyn(EN): WU online search failed");

    hdm::SetLang(hdm::Lang::Uk);
    Check(hdm::TrDyn("Восстановление образа Windows (DISM /RestoreHealth)...") ==
              "Відновлення образу Windows (DISM /RestoreHealth)...",
          "TrDyn(UK): DISM RestoreHealth phase");
    Check(hdm::TrDyn("Установка завершена (требуется перезагрузка).") ==
              "Встановлення завершено (потрібне перезавантаження).",
          "TrDyn(UK): local inf complete with period");
    hdm::SetLang(prevLang);
}

uint32_t ComputeCrc32(const std::string& data) {
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char b : data) {
        crc ^= b;
        for (int k = 0; k < 8; ++k) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return ~crc;
}

std::vector<uint8_t> BuildMinimalStoredZip(const std::string& fileName, const std::string& content) {
    auto push16 = [](std::vector<uint8_t>& out, uint16_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    };
    auto push32 = [](std::vector<uint8_t>& out, uint32_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    };

    const uint32_t crc = ComputeCrc32(content);
    const uint32_t sz = static_cast<uint32_t>(content.size());
    const uint16_t nameLen = static_cast<uint16_t>(fileName.size());

    std::vector<uint8_t> zip;
    // Local file header
    push32(zip, 0x04034B50u);
    push16(zip, 20);
    push16(zip, 0);
    push16(zip, 0); // stored
    push16(zip, 0);
    push16(zip, 0);
    push32(zip, crc);
    push32(zip, sz);
    push32(zip, sz);
    push16(zip, nameLen);
    push16(zip, 0);
    zip.insert(zip.end(), fileName.begin(), fileName.end());
    zip.insert(zip.end(), content.begin(), content.end());

    const uint32_t cdOffset = static_cast<uint32_t>(zip.size());
    // Central directory header
    push32(zip, 0x02014B50u);
    push16(zip, 20);
    push16(zip, 20);
    push16(zip, 0);
    push16(zip, 0);
    push16(zip, 0);
    push16(zip, 0);
    push32(zip, crc);
    push32(zip, sz);
    push32(zip, sz);
    push16(zip, nameLen);
    push16(zip, 0);
    push16(zip, 0);
    push16(zip, 0);
    push16(zip, 0);
    push32(zip, 0);
    push32(zip, 0);
    zip.insert(zip.end(), fileName.begin(), fileName.end());

    const uint32_t cdSize = static_cast<uint32_t>(zip.size()) - cdOffset;
    // End of central directory
    push32(zip, 0x06054B50u);
    push16(zip, 0);
    push16(zip, 0);
    push16(zip, 1);
    push16(zip, 1);
    push32(zip, cdSize);
    push32(zip, cdOffset);
    push16(zip, 0);
    return zip;
}

void TestZipAndDirectorySecurity() {
    std::filesystem::path dir;
    std::string err;
    Check(hdm::CreateUniqueTempWorkDir(dir, &err), "ZipSecurity: CreateUniqueTempWorkDir", err);
    if (dir.empty()) {
        return;
    }
    hdm::TempWorkDirGuard guard;
    guard.Set(dir);

    const std::filesystem::path zipFile = dir / L"sample.zip";
    const std::string payload = "DriverLoomZipOk";
    const std::vector<uint8_t> zipBytes = BuildMinimalStoredZip("hello.txt", payload);
    {
        std::ofstream ofs(zipFile, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(zipBytes.data()),
                  static_cast<std::streamsize>(zipBytes.size()));
    }
    Check(hdm::HasZipMagicHeader(zipFile), "ZipSecurity: HasZipMagicHeader true on valid zip");

    const std::filesystem::path notZip = dir / L"plain.txt";
    {
        std::ofstream ofs(notZip, std::ios::binary);
        ofs.write("not a zip", 9);
    }
    Check(!hdm::HasZipMagicHeader(notZip), "ZipSecurity: HasZipMagicHeader false on text file");

    const std::filesystem::path extractDir = dir / L"out";
    Check(hdm::ExtractZipSafely(zipFile, extractDir, nullptr, &err),
          "ZipSecurity: ExtractZipSafely extracts valid stored zip", err);
    {
        std::ifstream ifs(extractDir / L"hello.txt", std::ios::binary);
        std::string extracted((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        Check(extracted == payload, "ZipSecurity: extracted file content matches", extracted);
    }

    const std::filesystem::path unsignedDll = extractDir / L"evil.dll";
    {
        std::ofstream ofs(unsignedDll, std::ios::binary);
        ofs.write("MZ_FAKE_UNSIGNED_DLL", 20);
    }
    std::string sigReason;
    std::vector<hdm::HandleGuard> locks;
    Check(!hdm::VerifyExtractedDirectoryBinaries(
              extractDir, {"Microsoft Corporation"}, &sigReason, &locks),
          "ZipSecurity: unsigned evil.dll rejected by VerifyExtractedDirectoryBinaries",
          sigReason);
}

bool CreateNtfsJunction(
    const std::filesystem::path& junctionPath,
    const std::filesystem::path& targetDir) {
    std::error_code ec;
    if (!std::filesystem::create_directories(junctionPath, ec) &&
        !std::filesystem::exists(junctionPath, ec)) {
        return false;
    }
    std::wstring absTarget = std::filesystem::weakly_canonical(targetDir, ec).wstring();
    if (absTarget.empty()) {
        absTarget = targetDir.wstring();
    }
    if (absTarget.rfind(L"\\\\?\\", 0) == 0 && absTarget.size() >= 6 && absTarget[5] == L':') {
        absTarget.erase(0, 4);
    }
    while (absTarget.size() > 3 && absTarget.back() == L'\\') {
        absTarget.pop_back();
    }
    const std::wstring substitute = L"\\??\\" + absTarget;
    const std::wstring& printName = absTarget;

    const uint16_t subBytes = static_cast<uint16_t>(substitute.size() * sizeof(wchar_t));
    const uint16_t printBytes = static_cast<uint16_t>(printName.size() * sizeof(wchar_t));
    const size_t pathBytes = static_cast<size_t>(subBytes) + sizeof(wchar_t) +
                             static_cast<size_t>(printBytes) + sizeof(wchar_t);
    std::vector<uint8_t> buf(16 + pathBytes, 0);

    auto write16 = [&](size_t off, uint16_t v) {
        buf[off] = static_cast<uint8_t>(v & 0xFFu);
        buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    };
    auto write32 = [&](size_t off, uint32_t v) {
        buf[off] = static_cast<uint8_t>(v & 0xFFu);
        buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
        buf[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
        buf[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    };

    write32(0, IO_REPARSE_TAG_MOUNT_POINT);
    write16(4, static_cast<uint16_t>(8 + pathBytes));
    write16(6, 0);
    write16(8, 0);
    write16(10, subBytes);
    write16(12, static_cast<uint16_t>(subBytes + sizeof(wchar_t)));
    write16(14, printBytes);
    std::memcpy(buf.data() + 16, substitute.data(), subBytes);
    std::memcpy(buf.data() + 16 + subBytes + sizeof(wchar_t), printName.data(), printBytes);

    hdm::HandleGuard hDir(CreateFileW(
        junctionPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!hDir.IsValid()) {
        RemoveDirectoryW(junctionPath.c_str());
        return false;
    }
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(
        hDir.Get(),
        FSCTL_SET_REPARSE_POINT,
        buf.data(),
        static_cast<DWORD>(buf.size()),
        nullptr,
        0,
        &returned,
        nullptr);
    if (!ok) {
        hDir.Reset();
        RemoveDirectoryW(junctionPath.c_str());
        return false;
    }
    return true;
}

void TestTrustedTempAndCliValidation() {
    const std::wstring trustedTemp = hdm::ResolveTrustedLocalTempDir();
    Check(!trustedTemp.empty(), "CliTemp: ResolveTrustedLocalTempDir returns verified LocalAppData\\Temp");

    const std::filesystem::path logPath = hdm::CreateTempLogPath(L"unit_r3");
    Check(!logPath.empty(), "CliTemp: CreateTempLogPath succeeds");
    if (!logPath.empty()) {
        std::wstring canon;
        std::wstring fname;
        Check(hdm::ValidateWorkerTempPath(logPath.wstring(), L".log", canon, fname),
              "CliTemp: CreateTempLogPath output passes ValidateWorkerTempPath");
        std::error_code rmEc;
        std::filesystem::remove(logPath, rmEc);
    }

    std::filesystem::path rootDir;
    std::string err;
    Check(hdm::CreateUniqueTempWorkDir(rootDir, &err), "CliTemp: CreateUniqueTempWorkDir", err);
    if (rootDir.empty()) {
        return;
    }
    hdm::TempWorkDirGuard guard;
    guard.Set(rootDir);

    std::error_code ec;
    const std::filesystem::path validBase = rootDir / L"valid_local";
    const std::filesystem::path validTemp = validBase / L"Temp";
    const std::filesystem::path validSub = validTemp / L"sub";
    std::filesystem::create_directories(validSub, ec);
    const std::wstring validCanon = hdm::ResolveTrustedLocalTempDirFromBase(validBase);
    Check(!validCanon.empty(), "CliTemp: ResolveTrustedLocalTempDirFromBase succeeds on real Temp");

    const std::filesystem::path noTempBase = rootDir / L"missing_temp";
    std::filesystem::create_directories(noTempBase, ec);
    Check(hdm::ResolveTrustedLocalTempDirFromBase(noTempBase).empty(),
          "CliTemp: missing LocalAppData\\Temp fails closed (no GetTempPathW fallback)");

    const std::filesystem::path realTarget = rootDir / L"redirect_target";
    std::filesystem::create_directories(realTarget, ec);
    const std::filesystem::path juncBase = rootDir / L"junc_local";
    const std::filesystem::path juncTemp = juncBase / L"Temp";
    const bool juncCreated = CreateNtfsJunction(juncTemp, realTarget);
    Check(juncCreated, "CliTemp: CreateNtfsJunction helper created mount point");
    if (juncCreated) {
        Check(hdm::ResolveTrustedLocalTempDirFromBase(juncBase).empty(),
              "CliTemp: junction Temp fails closed in ResolveTrustedLocalTempDirFromBase");
        std::wstring dummyName;
        Check(!hdm::ValidateWorkerTempPathWithBase(
                  (juncTemp / L"DriverLoom_evil.log").wstring(), L".log", validCanon, dummyName),
              "CliTemp: ValidateWorkerTempPathWithBase rejects junction parent");
        RemoveDirectoryW(juncTemp.c_str());
    }

    std::wstring fname;
    Check(hdm::ValidateWorkerTempPathWithBase(
              (validTemp / L"DriverLoom_ok123.log").wstring(), L".log", validCanon, fname) &&
              fname == L"DriverLoom_ok123.log",
          "CliTemp: accepts valid DriverLoom_*.log");
    Check(hdm::ValidateWorkerTempPathWithBase(
              (validTemp / L"DriverLoom_ok123.log.cancel").wstring(), L".log.cancel", validCanon, fname),
          "CliTemp: accepts valid DriverLoom_*.log.cancel");
    Check(!hdm::ValidateWorkerTempPathWithBase(
              L"C:\\Windows\\System32\\DriverLoom_ok123.log", L".log", validCanon, fname),
          "CliTemp: rejects path outside canonical Temp (System32)");
    Check(!hdm::ValidateWorkerTempPathWithBase(
              L"\\\\server\\share\\DriverLoom_ok123.log", L".log", validCanon, fname),
          "CliTemp: rejects UNC path");
    Check(!hdm::ValidateWorkerTempPathWithBase(
              (validSub / L".." / L"DriverLoom_ok123.log").wstring(), L".log", validCanon, fname),
          "CliTemp: rejects '..' traversal component");
    Check(!hdm::ValidateWorkerTempPathWithBase(
              (validTemp / L"Other_ok123.log").wstring(), L".log", validCanon, fname),
          "CliTemp: rejects non-DriverLoom prefix");
    Check(!hdm::ValidateWorkerTempPathWithBase(
              (validTemp / L"DriverLoom_ok123.txt").wstring(), L".log", validCanon, fname),
          "CliTemp: rejects wrong suffix");
    Check(!hdm::ValidateWorkerTempPathWithBase(
              (validTemp / L"DriverLoom_bad$1.log").wstring(), L".log", validCanon, fname),
          "CliTemp: rejects disallowed filename character");
    Check(!hdm::ValidateWorkerTempPathWithBase(
              (validTemp / L"DriverLoom_ok123.log").wstring(), L".log", L"", fname),
          "CliTemp: rejects empty canonical temp base");
}

void TestBoundedCopyAndPackageStaging() {
    std::filesystem::path dir;
    std::string err;
    Check(hdm::CreateUniqueTempWorkDir(dir, &err), "Staging: CreateUniqueTempWorkDir", err);
    if (dir.empty()) {
        return;
    }
    hdm::TempWorkDirGuard guard;
    guard.Set(dir);

    const std::string data64(64, 'A');
    const std::filesystem::path src64 = dir / L"src64.exe";
    {
        std::ofstream ofs(src64, std::ios::binary);
        ofs.write(data64.data(), static_cast<std::streamsize>(data64.size()));
    }
    const std::filesystem::path dst64 = dir / L"dst64.exe";
    Check(hdm::CopyFileBoundedNoReparse(src64, dst64, 128, &err),
          "CopyBounded: copies 64-byte file within 128-byte limit", err);
    {
        std::ifstream ifs(dst64, std::ios::binary);
        std::string copied((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        Check(copied == data64, "CopyBounded: copied content matches source");
    }
    Check(!hdm::CopyFileBoundedNoReparse(src64, dst64, 128, &err),
          "CopyBounded: rejects existing destination file (CREATE_NEW)");

    const std::filesystem::path dstOver = dir / L"dst_over.exe";
    Check(!hdm::CopyFileBoundedNoReparse(src64, dstOver, 16, &err),
          "CopyBounded: rejects 64-byte file when limit is 16 bytes");
    std::error_code ec;
    Check(!std::filesystem::exists(dstOver, ec),
          "CopyBounded: oversized failure leaves no partial destination file");

    const std::filesystem::path srcEmpty = dir / L"empty.exe";
    {
        std::ofstream ofs(srcEmpty, std::ios::binary);
    }
    Check(!hdm::CopyFileBoundedNoReparse(srcEmpty, dir / L"dst_empty.exe", 128, &err),
          "CopyBounded: rejects 0-byte source file");
    Check(!hdm::CopyFileBoundedNoReparse(dir, dir / L"dst_dir.exe", 128, &err),
          "CopyBounded: rejects directory source");

    Check(hdm::IsValidSha256Hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "Sha256Format: valid lowercase 64 hex");
    Check(hdm::IsValidSha256Hex("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"),
          "Sha256Format: valid uppercase 64 hex");
    Check(!hdm::IsValidSha256Hex("ba7816bf"), "Sha256Format: rejects short digest");
    Check(!hdm::IsValidSha256Hex(std::string(65, 'a')), "Sha256Format: rejects 65-char digest");
    Check(!hdm::IsValidSha256Hex("ga7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "Sha256Format: rejects non-hex character");
    Check(!hdm::IsValidSha256Hex(""), "Sha256Format: rejects empty digest");

    std::vector<std::string> parsedPubs;
    Check(hdm::ParsePipeSeparatedPublishers("Microsoft Corporation|Microsoft Windows", parsedPubs) &&
              parsedPubs.size() == 2 && parsedPubs[0] == "Microsoft Corporation" &&
              parsedPubs[1] == "Microsoft Windows",
          "Publishers: parses valid pipe-separated allowlist");
    Check(!hdm::ParsePipeSeparatedPublishers("", parsedPubs), "Publishers: rejects empty string");
    Check(!hdm::ParsePipeSeparatedPublishers("Microsoft|", parsedPubs), "Publishers: rejects trailing pipe");
    Check(!hdm::ParsePipeSeparatedPublishers("A", parsedPubs), "Publishers: rejects 1-char token");
    Check(!hdm::ParsePipeSeparatedPublishers("Bad\nPub", parsedPubs), "Publishers: rejects control char");

    std::string src64Sha;
    Check(hdm::ComputeFileSha256Hex(src64, src64Sha, &err), "Staging: hash src64", err);

    auto runStage = [&](const std::wstring& subName, const hdm::StagedPackageOptions& opts, std::string& outErr) {
        const std::filesystem::path workSub = dir / subName;
        std::filesystem::create_directories(workSub, ec);
        std::filesystem::path targetExe;
        std::vector<hdm::HandleGuard> locks;
        std::string ver;
        return hdm::StageAndVerifyPackageIntoDir(workSub, opts, targetExe, locks, ver, outErr, nullptr);
    };

    hdm::StagedPackageOptions optNoPub;
    optNoPub.sourcePackagePath = src64;
    Check(!runStage(L"s_nopub", optNoPub, err), "Staging: rejects empty allowedPublishers");

    hdm::StagedPackageOptions optBadSha = optNoPub;
    optBadSha.allowedPublishers = {"Microsoft Corporation"};
    optBadSha.expectedSha256 = "not_a_sha";
    Check(!runStage(L"s_badsha", optBadSha, err), "Staging: rejects malformed SHA-256");

    hdm::StagedPackageOptions optBadArgs = optBadSha;
    optBadArgs.expectedSha256 = src64Sha;
    optBadArgs.installerArgs = "/S & calc.exe";
    Check(!runStage(L"s_badargs", optBadArgs, err), "Staging: rejects shell-injection installerArgs");

    hdm::StagedPackageOptions optOver = optBadSha;
    optOver.expectedSha256 = src64Sha;
    optOver.maxPackageBytes = 16;
    Check(!runStage(L"s_over", optOver, err), "Staging: rejects oversized package copy");

    hdm::StagedPackageOptions optMismatch = optBadSha;
    optMismatch.expectedSha256 = std::string(64, '0');
    Check(!runStage(L"s_mismatch", optMismatch, err), "Staging: rejects SHA-256 mismatch");

    hdm::StagedPackageOptions optUnsignedExe = optBadSha;
    optUnsignedExe.expectedSha256 = src64Sha;
    Check(!runStage(L"s_unsigned_exe", optUnsignedExe, err),
          "Staging: rejects unsigned .exe even when SHA-256 matches", err);

    const std::filesystem::path zipNoInst = dir / L"no_inst.zip";
    {
        const std::vector<uint8_t> zb = BuildMinimalStoredZip("readme.txt", "hello");
        std::ofstream ofs(zipNoInst, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(zb.data()), static_cast<std::streamsize>(zb.size()));
    }
    hdm::StagedPackageOptions optZipNoInst;
    optZipNoInst.sourcePackagePath = zipNoInst;
    optZipNoInst.allowedPublishers = {"Microsoft Corporation"};
    Check(!runStage(L"s_zip_noinst", optZipNoInst, err),
          "Staging: rejects ZIP without installer executable", err);

    const std::filesystem::path zipUnsigned = dir / L"unsigned_inst.zip";
    {
        const std::vector<uint8_t> zb = BuildMinimalStoredZip("setup.exe", "MZ_FAKE_UNSIGNED_SETUP");
        std::ofstream ofs(zipUnsigned, std::ios::binary);
        ofs.write(reinterpret_cast<const char*>(zb.data()), static_cast<std::streamsize>(zb.size()));
    }
    std::string zipSha;
    Check(hdm::ComputeFileSha256Hex(zipUnsigned, zipSha, &err), "Staging: hash zipUnsigned", err);
    hdm::StagedPackageOptions optZipUnsigned;
    optZipUnsigned.sourcePackagePath = zipUnsigned;
    optZipUnsigned.expectedSha256 = zipSha;
    optZipUnsigned.allowedPublishers = {"Microsoft Corporation"};
    Check(!runStage(L"s_zip_unsigned", optZipUnsigned, err),
          "Staging: rejects ZIP with unsigned setup.exe even when ZIP SHA-256 matches", err);
}

struct ZipTestEntry {
    std::string name;
    uint16_t method{0};
    uint16_t gpFlag{0};
    uint16_t versionMadeBy{20};
    uint32_t externalAttrs{0};
    std::vector<uint8_t> compBytes;
    uint32_t crc32{0};
    uint32_t uncompSize{0};
    bool zeroLocalSizes{false};
};

std::vector<uint8_t> BuildCustomZip(const std::vector<ZipTestEntry>& entries) {
    auto push16 = [](std::vector<uint8_t>& out, uint16_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    };
    auto push32 = [](std::vector<uint8_t>& out, uint32_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
    };

    std::vector<uint8_t> zip;
    std::vector<uint32_t> localOffsets;
    localOffsets.reserve(entries.size());

    for (const auto& e : entries) {
        const uint32_t lfhOff = static_cast<uint32_t>(zip.size());
        localOffsets.push_back(lfhOff);
        const uint16_t nameLen = static_cast<uint16_t>(e.name.size());
        const uint32_t compSz = static_cast<uint32_t>(e.compBytes.size());

        push32(zip, 0x04034B50u);
        push16(zip, 20);
        push16(zip, e.gpFlag);
        push16(zip, e.method);
        push16(zip, 0);
        push16(zip, 0);
        push32(zip, e.zeroLocalSizes ? 0u : e.crc32);
        push32(zip, e.zeroLocalSizes ? 0u : compSz);
        push32(zip, e.zeroLocalSizes ? 0u : e.uncompSize);
        push16(zip, nameLen);
        push16(zip, 0);
        zip.insert(zip.end(), e.name.begin(), e.name.end());
        zip.insert(zip.end(), e.compBytes.begin(), e.compBytes.end());
    }

    const uint32_t cdOffset = static_cast<uint32_t>(zip.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        const uint16_t nameLen = static_cast<uint16_t>(e.name.size());
        const uint32_t compSz = static_cast<uint32_t>(e.compBytes.size());

        push32(zip, 0x02014B50u);
        push16(zip, e.versionMadeBy);
        push16(zip, 20);
        push16(zip, e.gpFlag);
        push16(zip, e.method);
        push16(zip, 0);
        push16(zip, 0);
        push32(zip, e.crc32);
        push32(zip, compSz);
        push32(zip, e.uncompSize);
        push16(zip, nameLen);
        push16(zip, 0);
        push16(zip, 0);
        push16(zip, 0);
        push16(zip, 0);
        push32(zip, e.externalAttrs);
        push32(zip, localOffsets[i]);
        zip.insert(zip.end(), e.name.begin(), e.name.end());
    }

    const uint32_t cdSize = static_cast<uint32_t>(zip.size()) - cdOffset;
    const uint16_t count16 = static_cast<uint16_t>(entries.size());
    push32(zip, 0x06054B50u);
    push16(zip, 0);
    push16(zip, 0);
    push16(zip, count16);
    push16(zip, count16);
    push32(zip, cdSize);
    push32(zip, cdOffset);
    push16(zip, 0);
    return zip;
}

void WriteBytesFile(const std::filesystem::path& p, const std::vector<uint8_t>& bytes) {
    std::ofstream ofs(p, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::string ReadBytesAsString(const std::filesystem::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

ZipTestEntry MakeStoredEntry(const std::string& name, const std::string& content) {
    ZipTestEntry e;
    e.name = name;
    e.method = 0;
    e.compBytes.assign(content.begin(), content.end());
    e.crc32 = ComputeCrc32(content);
    e.uncompSize = static_cast<uint32_t>(content.size());
    return e;
}

void TestSafeZipExtractor() {
    std::filesystem::path dir;
    std::string err;
    Check(hdm::CreateUniqueTempWorkDir(dir, &err), "SafeZip: CreateUniqueTempWorkDir", err);
    if (dir.empty()) {
        return;
    }
    hdm::TempWorkDirGuard guard;
    guard.Set(dir);

    auto countFilesIn = [](const std::filesystem::path& p) -> size_t {
        size_t n = 0;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(p, ec);
             !ec && it != std::filesystem::recursive_directory_iterator();
             it.increment(ec)) {
            if (!it->is_directory(ec)) {
                ++n;
            }
        }
        return n;
    };

    // 1. Multi-file nested stored + dir entry + zero-byte file
    {
        ZipTestEntry dirEntry;
        dirEntry.name = "sub/";
        const auto zipBytes = BuildCustomZip({
            dirEntry,
            MakeStoredEntry("sub/a.txt", "AlphaPayload"),
            MakeStoredEntry("sub/deep/b.txt", "BetaPayload"),
            MakeStoredEntry("empty.txt", ""),
        });
        const auto zPath = dir / L"stored_multi.zip";
        const auto out = dir / L"out_stored";
        WriteBytesFile(zPath, zipBytes);
        Check(hdm::ExtractZipSafely(zPath, out, nullptr, &err), "SafeZip: stored multi+nested", err);
        Check(ReadBytesAsString(out / L"sub" / L"a.txt") == "AlphaPayload", "SafeZip: sub/a.txt matches");
        Check(ReadBytesAsString(out / L"sub" / L"deep" / L"b.txt") == "BetaPayload",
              "SafeZip: sub/deep/b.txt matches");
        Check(ReadBytesAsString(out / L"empty.txt").empty(), "SafeZip: empty stored file matches");
    }

    // 2. DEFLATE BTYPE=0 (stored block inside method 8)
    {
        ZipTestEntry e;
        e.name = "btype0.txt";
        e.method = 8;
        e.compBytes = {0x01, 0x05, 0x00, 0xFA, 0xFF, 'H', 'e', 'l', 'l', 'o'};
        e.crc32 = ComputeCrc32("Hello");
        e.uncompSize = 5;
        const auto zPath = dir / L"deflate0.zip";
        const auto out = dir / L"out_def0";
        WriteBytesFile(zPath, BuildCustomZip({e}));
        Check(hdm::ExtractZipSafely(zPath, out, nullptr, &err), "SafeZip: DEFLATE BTYPE=0", err);
        Check(ReadBytesAsString(out / L"btype0.txt") == "Hello", "SafeZip: DEFLATE BTYPE=0 content");
    }

    // 3. DEFLATE BTYPE=1 (Fixed Huffman, 760 bytes)
    {
        std::string fixedPayload;
        for (int i = 0; i < 12; ++i) {
            fixedPayload += "DriverLoom SafeZipExtractor RFC1951 Dynamic Huffman Test! ";
        }
        fixedPayload += "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*()_+-=[]{}|;:,.<>?/~";
        ZipTestEntry e;
        e.name = "fixed.txt";
        e.method = 8;
        e.compBytes = {
            0x73, 0x29, 0xCA, 0x2C, 0x4B, 0x2D, 0xF2, 0xC9, 0xCF, 0xCF, 0x55, 0x08, 0x4E, 0x4C, 0x4B, 0x8D,
            0xCA, 0x2C, 0x70, 0xAD, 0x28, 0x29, 0x4A, 0x4C, 0x2E, 0xC9, 0x2F, 0x52, 0x08, 0x72, 0x73, 0x36,
            0xB4, 0x34, 0x35, 0x54, 0x70, 0xA9, 0xCC, 0x4B, 0xCC, 0xCD, 0x4C, 0x56, 0xF0, 0x28, 0x4D, 0x4B,
            0xCB, 0x4D, 0xCC, 0x53, 0x08, 0x49, 0x2D, 0x2E, 0x51, 0x54, 0x70, 0x19, 0xD5, 0x39, 0xAA, 0x73,
            0xC0, 0x74, 0x3A, 0x3A, 0x39, 0xBB, 0xB8, 0xBA, 0xB9, 0x7B, 0x78, 0x7A, 0x79, 0xFB, 0xF8, 0xFA,
            0xF9, 0x07, 0x04, 0x06, 0x05, 0x87, 0x84, 0x86, 0x85, 0x47, 0x44, 0x46, 0x19, 0x18, 0x1A, 0x19,
            0x9B, 0x98, 0x9A, 0x99, 0x5B, 0x58, 0x2A, 0x3A, 0x28, 0xAB, 0xA8, 0xC6, 0xA9, 0x69, 0x69, 0x68,
            0xC6, 0x6B, 0xEB, 0xDA, 0x46, 0xC7, 0x56, 0xD7, 0xD6, 0x58, 0x5B, 0xE9, 0xE8, 0xD9, 0xD8, 0xD9,
            0xEB, 0xD7, 0x01, 0x00
        };
        e.crc32 = ComputeCrc32(fixedPayload);
        e.uncompSize = static_cast<uint32_t>(fixedPayload.size());
        const auto zPath = dir / L"deflate1.zip";
        const auto out = dir / L"out_def1";
        WriteBytesFile(zPath, BuildCustomZip({e}));
        Check(hdm::ExtractZipSafely(zPath, out, nullptr, &err), "SafeZip: DEFLATE BTYPE=1 Fixed Huffman", err);
        Check(ReadBytesAsString(out / L"fixed.txt") == fixedPayload,
              "SafeZip: DEFLATE BTYPE=1 exact 760-byte payload");
    }

    // 4. DEFLATE BTYPE=2 (Dynamic Huffman, 586 bytes) + Data Descriptor flag (gpFlag=0x0008)
    {
        const std::string dynPayload =
            "The quick brown fox jumps over the lazy dog. DriverLoom verifies Authenticode signatures, "
            "SHA-256 digests, and blocks directory traversal, NTFS alternate data streams, reparse points, "
            "and zip bombs! Pack my box with five dozen liquor jugs. Sphinx of black quartz, judge my vow. "
            "0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz "
            "DriverLoom hybrid driver manager streaming deflate dynamic huffman block verification payload 2026! "
            "The quick brown fox jumps over the lazy dog again while testing dynamic huffman trees and LZ77 "
            "sliding window back-references across distances.";
        ZipTestEntry e;
        e.name = "dyn.txt";
        e.method = 8;
        e.gpFlag = 0x0008;
        e.zeroLocalSizes = true;
        e.compBytes = {
            0x8D, 0x51, 0xC1, 0x76, 0xD3, 0x30, 0x10, 0xFC, 0x95, 0xE9, 0x3D, 0xCD, 0x2B, 0x29, 0x4D, 0xE0,
            0x18, 0x68, 0x4B, 0x81, 0x50, 0x5A, 0x12, 0xA0, 0xED, 0x6D, 0x6D, 0xAD, 0xED, 0x6D, 0x64, 0xC9,
            0x91, 0xE4, 0x38, 0xF6, 0xD7, 0xB3, 0x4E, 0xE1, 0x3D, 0x1E, 0x27, 0x4E, 0xFB, 0x34, 0x3B, 0x9A,
            0x9D, 0x9D, 0xDD, 0x54, 0x8C, 0x5D, 0x2B, 0xF9, 0x16, 0x59, 0xF0, 0x9D, 0x43, 0xE1, 0x0F, 0x78,
            0x6E, 0xEB, 0x26, 0xC2, 0xEF, 0x39, 0x20, 0x69, 0xDB, 0xD2, 0xD0, 0xC3, 0xF8, 0x72, 0x8A, 0xCB,
            0x20, 0x0A, 0xAE, 0xBC, 0xAF, 0xA1, 0x55, 0x0A, 0xE1, 0x88, 0x65, 0xAB, 0x1C, 0x97, 0x24, 0xF7,
            0x86, 0x11, 0xA5, 0x74, 0x94, 0xDA, 0xC0, 0x71, 0x82, 0xF5, 0xCD, 0xF2, 0x74, 0x76, 0x31, 0x87,
            0x91, 0x92, 0x63, 0x52, 0x80, 0x9C, 0x41, 0x66, 0x7D, 0xBE, 0x8D, 0x8A, 0x05, 0xCE, 0x93, 0x0F,
            0x3D, 0x52, 0x20, 0x95, 0x8A, 0x64, 0x27, 0xB8, 0xDD, 0x5C, 0xAF, 0x41, 0x36, 0x71, 0x50, 0x0D,
            0x86, 0xA1, 0x44, 0x88, 0x29, 0x30, 0xD5, 0xFA, 0x39, 0x70, 0x43, 0x21, 0x32, 0x1A, 0x2F, 0xEE,
            0x8F, 0xD8, 0x20, 0x0D, 0x32, 0x5F, 0x67, 0xF1, 0x04, 0x77, 0xA4, 0x1B, 0xD4, 0xBD, 0x3E, 0x0F,
            0xE8, 0x24, 0x55, 0x28, 0xD4, 0xA9, 0x9A, 0x1E, 0xD8, 0xC1, 0xCA, 0xAE, 0xF5, 0x41, 0xB7, 0x2A,
            0xE3, 0x14, 0xEB, 0xA6, 0x12, 0x77, 0x80, 0x2F, 0xD4, 0xCA, 0xF8, 0x67, 0xD7, 0x52, 0x48, 0xC3,
            0x44, 0xBB, 0xA6, 0xE4, 0x51, 0x61, 0xEF, 0xBB, 0x29, 0xCE, 0x5E, 0xCD, 0xCE, 0x5F, 0x5F, 0xCC,
            0x17, 0x6F, 0xDE, 0x62, 0xF9, 0xEE, 0xFD, 0xE5, 0xD5, 0xF5, 0x87, 0x9B, 0x8F, 0x9F, 0x3E, 0xAF,
            0xBE, 0xDC, 0x7E, 0xBD, 0xBB, 0xFF, 0xB6, 0xDE, 0x7C, 0xFF, 0xF1, 0xF3, 0xE1, 0xF1, 0x09, 0x94,
            0xE5, 0x86, 0x8B, 0xB2, 0x92, 0xE7, 0xAD, 0xAD, 0x9D, 0x6F, 0x76, 0x21, 0xA6, 0x76, 0xDF, 0x1D,
            0xFA, 0xE1, 0xEF, 0xA4, 0xAA, 0x3E, 0x0B, 0x62, 0x60, 0x8E, 0x08, 0x6A, 0x72, 0x54, 0x6A, 0x7D,
            0x59, 0x4C, 0x5C, 0x09, 0x95, 0xB0, 0xC7, 0x7D, 0x7B, 0xA7, 0x40, 0x8E, 0xAA, 0x2D, 0x0A, 0x65,
            0xBD, 0x44, 0xF5, 0x3B, 0xE7, 0x9C, 0x92, 0x78, 0x87, 0x86, 0x7A, 0xEB, 0xC9, 0x60, 0x76, 0x36,
            0x9B, 0x9F, 0x60, 0xF3, 0xFF, 0xA7, 0x03, 0x95, 0x24, 0x0E, 0x5D, 0x25, 0x96, 0x91, 0xF4, 0x1C,
            0xC7, 0xC1, 0xFF, 0x0C, 0x54, 0x47, 0x7A, 0xCF, 0x31, 0xD8, 0xD5, 0xD3, 0x62, 0x81, 0x68, 0xC5,
            0x8C, 0xB4, 0x4E, 0x9C, 0xF1, 0x1D, 0x32, 0x4D, 0xEB, 0x34, 0x70, 0xC1, 0x81, 0x5D, 0x3E, 0xF2,
            0xF2, 0xE0, 0xE3, 0x78, 0xC9, 0x98, 0x68, 0x04, 0xA6, 0xBF, 0x00
        };
        e.crc32 = ComputeCrc32(dynPayload);
        e.uncompSize = static_cast<uint32_t>(dynPayload.size());
        const auto zPath = dir / L"deflate2.zip";
        const auto out = dir / L"out_def2";
        WriteBytesFile(zPath, BuildCustomZip({e}));
        Check(hdm::ExtractZipSafely(zPath, out, nullptr, &err),
              "SafeZip: DEFLATE BTYPE=2 Dynamic Huffman + data descriptor", err);
        Check(ReadBytesAsString(out / L"dyn.txt") == dynPayload,
              "SafeZip: DEFLATE BTYPE=2 exact 586-byte payload");
    }

    // 5. Zip-bomb (spoofed header + honest oversized header), both aborted & cleaned up
    {
        const std::vector<uint8_t> bombDeflate = {
            0xED, 0xC1, 0x31, 0x01, 0x00, 0x00, 0x00, 0xC2, 0xA0, 0x6C, 0xEB, 0x5F, 0xCA, 0x18, 0x3E, 0x40,
            0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x6F, 0x03
        };
        const uint32_t bombCrc = ComputeCrc32(std::string(16384, 'A'));
        hdm::ZipExtractLimits smallLim;
        smallLim.maxTotalBytes = 4096;

        ZipTestEntry spoofed;
        spoofed.name = "bomb_spoof.txt";
        spoofed.method = 8;
        spoofed.compBytes = bombDeflate;
        spoofed.crc32 = bombCrc;
        spoofed.uncompSize = 16;
        const auto zSpoof = dir / L"bomb_spoof.zip";
        const auto outSpoof = dir / L"out_bomb_spoof";
        WriteBytesFile(zSpoof, BuildCustomZip({spoofed}));
        Check(!hdm::ExtractZipSafely(zSpoof, outSpoof, nullptr, &err, smallLim),
              "SafeZip: rejects zip-bomb with spoofed uncompSize", err);
        Check(countFilesIn(outSpoof) == 0, "SafeZip: spoofed zip-bomb cleans partial files");

        ZipTestEntry honest = spoofed;
        honest.name = "bomb_honest.txt";
        honest.uncompSize = 16384;
        const auto zHonest = dir / L"bomb_honest.zip";
        const auto outHonest = dir / L"out_bomb_honest";
        WriteBytesFile(zHonest, BuildCustomZip({honest}));
        Check(!hdm::ExtractZipSafely(zHonest, outHonest, nullptr, &err, smallLim),
              "SafeZip: rejects zip-bomb with oversized declared uncompSize", err);
        Check(countFilesIn(outHonest) == 0, "SafeZip: oversized zip-bomb leaves zero files");
    }

    // 6. CRC-32 mismatch cleans partial file
    {
        auto badCrc = MakeStoredEntry("bad.txt", "CorruptMe");
        badCrc.crc32 ^= 1u;
        const auto zPath = dir / L"bad_crc.zip";
        const auto out = dir / L"out_bad_crc";
        WriteBytesFile(zPath, BuildCustomZip({badCrc}));
        Check(!hdm::ExtractZipSafely(zPath, out, nullptr, &err),
              "SafeZip: rejects CRC-32 mismatch", err);
        Check(countFilesIn(out) == 0, "SafeZip: CRC-32 failure removes partial file");
    }

    // 7. Malicious entry paths & flags
    auto expectReject = [&](const char* label, const std::vector<ZipTestEntry>& entries,
                            const hdm::ZipExtractLimits& lim = {}) {
        static int idxCounter = 0;
        ++idxCounter;
        const auto zp = dir / (L"bad_" + std::to_wstring(idxCounter) + L".zip");
        const auto od = dir / (L"out_bad_" + std::to_wstring(idxCounter));
        WriteBytesFile(zp, BuildCustomZip(entries));
        std::string r;
        Check(!hdm::ExtractZipSafely(zp, od, nullptr, &r, lim), label, r);
    };

    expectReject("SafeZip: rejects ../ traversal", {MakeStoredEntry("../evil.exe", "x")});
    expectReject("SafeZip: rejects sub/../../ traversal", {MakeStoredEntry("sub/../../evil.exe", "x")});
    expectReject("SafeZip: rejects leading slash path", {MakeStoredEntry("/evil.exe", "x")});
    expectReject("SafeZip: rejects drive path", {MakeStoredEntry("C:\\evil.exe", "x")});
    expectReject("SafeZip: rejects NTFS ADS ':'", {MakeStoredEntry("setup.exe:stream", "x")});
    expectReject("SafeZip: rejects Windows reserved CON.txt", {MakeStoredEntry("CON.txt", "x")});
    expectReject("SafeZip: rejects Windows reserved sub/NUL", {MakeStoredEntry("sub/NUL", "x")});
    expectReject("SafeZip: rejects trailing dot segment", {MakeStoredEntry("setup.exe.", "x")});
    expectReject("SafeZip: rejects trailing space dir segment", {MakeStoredEntry("dir /a.txt", "x")});

    {
        auto sym = MakeStoredEntry("sym.txt", "target");
        sym.versionMadeBy = 0x0314;
        sym.externalAttrs = 0xA1FF0000u;
        expectReject("SafeZip: rejects Unix symlink entry", {sym});
    }
    {
        auto enc = MakeStoredEntry("enc.txt", "secret");
        enc.gpFlag = 0x0001u;
        expectReject("SafeZip: rejects encrypted entry", {enc});
    }
    {
        hdm::ZipExtractLimits lim3;
        lim3.maxFiles = 3;
        expectReject("SafeZip: enforces maxFiles limit",
                     {MakeStoredEntry("1.txt", "a"), MakeStoredEntry("2.txt", "b"),
                      MakeStoredEntry("3.txt", "c"), MakeStoredEntry("4.txt", "d")},
                     lim3);
    }
    expectReject("SafeZip: rejects case-insensitive duplicate path",
                 {MakeStoredEntry("a.txt", "1"), MakeStoredEntry("A.TXT", "2")});

    // 8. Immediate pre-cancellation
    {
        std::atomic<bool> cancelled{true};
        const auto zp = dir / L"cancel.zip";
        const auto od = dir / L"out_cancel";
        WriteBytesFile(zp, BuildCustomZip({MakeStoredEntry("ok.txt", "hi")}));
        Check(!hdm::ExtractZipSafely(zp, od, &cancelled, &err),
              "SafeZip: aborts immediately when cancelFlag is set", err);
    }

    // 9. Pre-existing NTFS junction inside destDir blocked before writing outside
    {
        std::error_code ec;
        const auto outsideDir = dir / L"outside_target";
        const auto juncDest = dir / L"out_junc";
        const auto juncSub = juncDest / L"sub";
        std::filesystem::create_directories(outsideDir, ec);
        std::filesystem::create_directories(juncDest, ec);
        if (CreateNtfsJunction(juncSub, outsideDir)) {
            const auto zp = dir / L"junc_attack.zip";
            WriteBytesFile(zp, BuildCustomZip({MakeStoredEntry("sub/pwn.txt", "PWNED")}));
            Check(!hdm::ExtractZipSafely(zp, juncDest, nullptr, &err),
                  "SafeZip: rejects NTFS junction inside destDir", err);
            Check(!std::filesystem::exists(outsideDir / L"pwn.txt", ec),
                  "SafeZip: junction target remains untouched");
            RemoveDirectoryW(juncSub.c_str());
        }
    }

    // 10. ZIP64 multi-disk locator rejected even with 0xFFFF EOCD markers
    {
        std::vector<uint8_t> z64Bad = BuildCustomZip({MakeStoredEntry("ok.txt", "hi")});
        if (z64Bad.size() >= 22) {
            const size_t eocdPos = z64Bad.size() - 22;
            // Set totalEntries (offset +8/+10) to 0xFFFF to trigger ZIP64 locator lookup
            z64Bad[eocdPos + 8] = 0xFFu;
            z64Bad[eocdPos + 9] = 0xFFu;
            z64Bad[eocdPos + 10] = 0xFFu;
            z64Bad[eocdPos + 11] = 0xFFu;
            // Prepend a 20-byte ZIP64 EOCD Locator right before EOCD with diskWithZ64 = 1 (multi-disk)
            std::vector<uint8_t> loc(20, 0);
            loc[0] = 0x50u; loc[1] = 0x4Bu; loc[2] = 0x06u; loc[3] = 0x07u; // 0x07064B50
            loc[4] = 0x01u; // diskWithZip64Eocd = 1 (forbidden)
            loc[16] = 0x01u; // totalDisks = 1
            z64Bad.insert(z64Bad.begin() + static_cast<std::ptrdiff_t>(eocdPos), loc.begin(), loc.end());
            const auto zp = dir / L"z64_multidisk.zip";
            const auto od = dir / L"out_z64_bad";
            WriteBytesFile(zp, z64Bad);
            Check(!hdm::ExtractZipSafely(zp, od, nullptr, &err),
                  "SafeZip: rejects multi-disk ZIP64 locator", err);
        }
    }
}

void TestChunkStrings() {
    Check(hdm::ChunkStrings({}, 100).empty(), "ChunkStrings: empty input gives 0 chunks");
    Check(hdm::ChunkStrings({"a", "b"}, 0).empty(), "ChunkStrings: maxPerChunk=0 returns empty");

    const auto single = hdm::ChunkStrings({"oem1.inf", "oem2.inf"}, 100);
    Check(single.size() == 1 && single[0].size() == 2 && single[0][1] == "oem2.inf",
          "ChunkStrings: <100 keeps 1 chunk");

    std::vector<std::string> many;
    many.reserve(250);
    for (int i = 0; i < 250; ++i) {
        many.push_back("oem" + std::to_string(i) + ".inf");
    }
    const auto split = hdm::ChunkStrings(many, 100);
    Check(split.size() == 3 && split[0].size() == 100 && split[1].size() == 100 && split[2].size() == 50 &&
              split[0][0] == "oem0.inf" && split[1][0] == "oem100.inf" && split[2][49] == "oem249.inf",
          "ChunkStrings: 250 items split into 100+100+50 preserving order");
}

} // namespace

int main() {
    hdm::CoInitGuard comInit(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    TestSha256();
    TestAuthenticodeAndPublisher();
    TestWingetYamlParser();
    TestVersionComparison();
    TestJsonParsing();
    TestSilentArgsAndPaths();
    TestHttpsHostAllowlist();
    TestCabExtractCallback();
    TestTempAndMiscHelpers();
    TestLocalizationTrDyn();
    TestZipAndDirectorySecurity();
    TestTrustedTempAndCliValidation();
    TestBoundedCopyAndPackageStaging();
    TestSafeZipExtractor();
    TestChunkStrings();

    std::cout << "\nSummary: " << g_passed << " passed, " << g_failed << " failed.\n";
    return g_failed == 0 ? 0 : 1;
}
