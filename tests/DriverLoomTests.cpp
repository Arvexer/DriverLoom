#include "common/AppCommon.hpp"
#include "common/Localization.hpp"
#include "core/HttpClient.hpp"
#include "core/OnlineCatalogClient.hpp"
#include "core/PackageSecurity.hpp"

#include <setupapi.h>

#include <cstdint>
#include <cstdio>
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
    Check(hdm::ExtractZipViaShellCom(zipFile, extractDir, nullptr),
          "ZipSecurity: ExtractZipViaShellCom extracts valid stored zip");
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

    std::cout << "\nSummary: " << g_passed << " passed, " << g_failed << " failed.\n";
    return g_failed == 0 ? 0 : 1;
}
