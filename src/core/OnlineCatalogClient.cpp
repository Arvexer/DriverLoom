#include "core/OnlineCatalogClient.hpp"
#include "common/AppCommon.hpp"
#include "core/HttpClient.hpp"
#include "core/PackageSecurity.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <utility>

namespace hdm {

namespace {

std::wstring ReadCatalogRegStr(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes == 0 || bytes > 65536u || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return {};
    }
    std::wstring buf(bytes / sizeof(wchar_t) + 2, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(buf.data()), &bytes) != ERROR_SUCCESS) {
        return {};
    }
    return std::wstring(buf.c_str());
}

std::string EncodeWingetPath(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.back() == '/') {
        return {};
    }
    std::string encoded;
    encoded.reserve(path.size() + 8);
    size_t segStart = 0;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            const std::string_view seg = path.substr(segStart, i - segStart);
            if (seg.empty() || seg == "." || seg == "..") {
                return {};
            }
            if (!encoded.empty()) {
                encoded.push_back('/');
            }
            for (unsigned char ch : seg) {
                if (ch < 0x20 || ch >= 0x7F || ch == '\\' || ch == '?' || ch == '#' || ch == '%') {
                    return {};
                }
                if (ch == '+') {
                    encoded += "%2B";
                } else if (ch == ' ') {
                    encoded += "%20";
                } else {
                    encoded.push_back(static_cast<char>(ch));
                }
            }
            segStart = i + 1;
        }
    }
    return encoded;
}

bool IsValidCatalogGuid(std::string_view s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < 36; ++i) {
        const char ch = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (ch != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

bool IsValidCatalogVersion(std::string_view ver) {
    if (ver.empty() || std::isdigit(static_cast<unsigned char>(ver.front())) == 0 ||
        std::isdigit(static_cast<unsigned char>(ver.back())) == 0 ||
        ver.find('.') == std::string_view::npos) {
        return false;
    }
    bool prevDot = false;
    for (char ch : ver) {
        if (ch == '.') {
            if (prevDot) return false;
            prevDot = true;
        } else if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
            prevDot = false;
        } else {
            return false;
        }
    }
    return true;
}

std::string ExtractVersionFromCatalogRowChunk(std::string_view rowChunk) {
    const size_t linkPos = rowChunk.rfind("_link'");
    const size_t linkAltPos = rowChunk.rfind("_link\"");
    size_t anchor = std::string_view::npos;
    if (linkPos != std::string_view::npos && linkAltPos != std::string_view::npos) {
        anchor = std::max(linkPos, linkAltPos);
    } else {
        anchor = (linkPos != std::string_view::npos) ? linkPos : linkAltPos;
    }
    if (anchor == std::string_view::npos) {
        return {};
    }
    const size_t gt = rowChunk.find('>', anchor);
    const size_t endA = (gt != std::string_view::npos) ? rowChunk.find("</a>", gt + 1) : std::string_view::npos;
    if (gt == std::string_view::npos || endA == std::string_view::npos) {
        return {};
    }
    const std::string_view title = rowChunk.substr(gt + 1, endA - gt - 1);

    std::string lastVer;
    size_t i = 0;
    while (i < title.size()) {
        if (std::isdigit(static_cast<unsigned char>(title[i])) == 0) {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < title.size() &&
               (std::isdigit(static_cast<unsigned char>(title[i])) != 0 || title[i] == '.')) {
            ++i;
        }
        size_t end = i;
        while (end > start && title[end - 1] == '.') {
            --end;
        }
        const std::string_view cand = title.substr(start, end - start);
        if (IsValidCatalogVersion(cand)) {
            lastVer.assign(cand);
        }
    }
    return lastVer;
}

int ExtractCatalogDateFromRowChunk(std::string_view rowChunk) {
    const size_t c4 = rowChunk.rfind("_C4_R");
    if (c4 == std::string_view::npos) {
        return 0;
    }
    const size_t gt = rowChunk.find('>', c4 + 5);
    if (gt == std::string_view::npos) {
        return 0;
    }
    const size_t endTd = rowChunk.find("</td>", gt + 1);
    if (endTd == std::string_view::npos || endTd - gt > 64) {
        return 0;
    }
    return ParseDriverDateYmd(TrimCopy(rowChunk.substr(gt + 1, endTd - gt - 1)));
}

// Parses GitHub's non-API tree JSON ({"payload":{"codeViewTreeRoute":{"tree":{"items":[...]}}}})
// so version lookup does not consume GitHub REST API's 60 req/hr quota.
std::vector<std::string> ExtractGithubTreeNames(
    const std::string& json,
    const std::string& wantedContentType) {
    std::vector<std::string> names;
    const size_t treePos = json.find("\"tree\":{\"items\":[");
    if (treePos == std::string::npos) {
        return names;
    }
    const size_t arrStart = json.find('[', treePos);
    if (arrStart == std::string::npos) {
        return names;
    }

    int depth = 0;
    bool inString = false;
    bool escape = false;
    size_t arrEnd = json.size();
    for (size_t i = arrStart; i < json.size(); ++i) {
        const char ch = json[i];
        if (escape) {
            escape = false;
            continue;
        }
        if (inString) {
            if (ch == '\\') escape = true;
            else if (ch == '"') inString = false;
            continue;
        }
        if (ch == '"') {
            inString = true;
        } else if (ch == '[') {
            ++depth;
        } else if (ch == ']') {
            --depth;
            if (depth == 0) {
                arrEnd = i;
                break;
            }
        }
    }

    size_t pos = arrStart;
    while (pos < arrEnd) {
        const size_t namePos = json.find("\"name\":\"", pos);
        if (namePos == std::string::npos || namePos >= arrEnd) {
            break;
        }
        const std::string name = ExtractJsonStringField(json, "name", namePos);
        const size_t objEnd = json.find('}', namePos);
        if (objEnd == std::string::npos || objEnd > arrEnd) {
            break;
        }
        const size_t ctPos = json.find("\"contentType\"", namePos);
        std::string contentType;
        if (ctPos != std::string::npos && ctPos < objEnd) {
            contentType = ExtractJsonStringField(json, "contentType", ctPos);
        }
        if (!name.empty() && contentType == wantedContentType) {
            names.push_back(name);
        }
        pos = objEnd + 1;
    }
    return names;
}

std::string StripYamlInlineComment(std::string_view rawVal) {
    for (size_t i = 0; i < rawVal.size(); ++i) {
        if (rawVal[i] == '#' && i > 0 && (rawVal[i - 1] == ' ' || rawVal[i - 1] == '\t')) {
            return TrimCopy(rawVal.substr(0, i));
        }
    }
    return TrimCopy(rawVal);
}

bool IsPrereleaseVersionLabel(std::string_view ver) {
    const std::string lower = ToLowerAscii(ver);
    return lower.find("-alpha") != std::string::npos ||
           lower.find("-beta") != std::string::npos ||
           lower.find("-rc") != std::string::npos ||
           lower.find("-preview") != std::string::npos ||
           lower.find("-dev") != std::string::npos ||
           lower.find("-canary") != std::string::npos ||
           lower.find("-nightly") != std::string::npos;
}

} // namespace

std::string UrlEncodeCatalogParam(std::string_view val) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(val.size() * 3);
    for (unsigned char c : val) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0F]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::vector<uint64_t> ParseVersionParts(const std::string& ver) {
    std::vector<uint64_t> parts;
    uint64_t current = 0;
    bool inNum = false;
    for (char ch : ver) {
        if (ch >= '0' && ch <= '9') {
            const uint64_t digit = static_cast<uint64_t>(ch - '0');
            if (current <= (std::numeric_limits<uint64_t>::max() - digit) / 10) {
                current = current * 10 + digit;
            } else {
                current = std::numeric_limits<uint64_t>::max();
            }
            inNum = true;
        } else if (inNum) {
            parts.push_back(current);
            current = 0;
            inNum = false;
        }
    }
    if (inNum) {
        parts.push_back(current);
    }
    return parts;
}

int ParseDriverDateYmd(std::string_view text) {
    uint64_t parts[3]{};
    size_t count = 0;
    uint64_t cur = 0;
    bool inNum = false;
    for (char ch : text) {
        if (ch >= '0' && ch <= '9') {
            cur = cur * 10 + static_cast<uint64_t>(ch - '0');
            if (cur > 99999) {
                return 0;
            }
            inNum = true;
        } else if ((ch == '/' || ch == '-' || ch == '.') && inNum) {
            if (count >= 3) {
                return 0;
            }
            parts[count++] = cur;
            cur = 0;
            inNum = false;
        } else if (ch != ' ' && ch != '\t') {
            return 0;
        }
    }
    if (inNum) {
        if (count >= 3) {
            return 0;
        }
        parts[count++] = cur;
    }
    if (count != 3) {
        return 0;
    }
    uint64_t y = 0;
    uint64_t m = 0;
    uint64_t d = 0;
    if (parts[0] >= 1990 && parts[0] <= 2099) {
        y = parts[0];
        m = parts[1];
        d = parts[2];
    } else if (parts[2] >= 1990 && parts[2] <= 2099) {
        m = parts[0];
        d = parts[1];
        y = parts[2];
    } else {
        return 0;
    }
    if (m < 1 || m > 12 || d < 1 || d > 31) {
        return 0;
    }
    return static_cast<int>(y * 10000 + m * 100 + d);
}

bool TryExtractTrailingVersionDate(const std::vector<uint64_t>& parts, int& outYmd) {
    if (parts.size() != 4 || parts[3] < 2015 || parts[3] > 2035) {
        return false;
    }
    const uint64_t mmdd = parts[2];
    if (mmdd < 101 || mmdd > 1231) {
        return false;
    }
    // Reject Intel-style 4-part builds like 32.0.101.2024 where parts[2]==101 is not a MMDD date.
    if (parts[0] >= 25 && mmdd < 200) {
        return false;
    }
    const uint64_t m = mmdd / 100;
    const uint64_t d = mmdd % 100;
    if (m < 1 || m > 12 || d < 1) {
        return false;
    }
    static constexpr uint64_t kDaysInMonth[13] = {0, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (d > kDaysInMonth[m]) {
        return false;
    }
    outYmd = static_cast<int>(parts[3] * 10000 + m * 100 + d);
    return true;
}

bool IsHostWindows11() {
    static const bool sIsWin11 = []() {
        RegKeyGuard key;
        if (RegOpenKeyExW(
                HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                0,
                KEY_READ,
                key.Put()) != ERROR_SUCCESS) {
            return true;
        }
        const std::wstring buildStr = ReadCatalogRegStr(key.Get(), L"CurrentBuildNumber");
        if (buildStr.empty()) {
            return true;
        }
        const unsigned long build = wcstoul(buildStr.c_str(), nullptr, 10);
        return build >= 22000;
    }();
    return sIsWin11;
}

bool IsDriverVersionNewer(const std::string& latest, const std::string& current) {
    if (latest.empty() || current.empty()) {
        return false;
    }
    const auto a = ParseVersionParts(latest);
    const auto b = ParseVersionParts(current);
    if (a.empty() || b.empty()) {
        return false;
    }
    int dateA = 0;
    int dateB = 0;
    if (TryExtractTrailingVersionDate(a, dateA) &&
        TryExtractTrailingVersionDate(b, dateB) &&
        dateA != dateB) {
        return dateA > dateB;
    }
    const size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const uint64_t va = i < a.size() ? a[i] : 0;
        const uint64_t vb = i < b.size() ? b[i] : 0;
        if (va != vb) {
            return va > vb;
        }
    }
    return false;
}

bool IsVersionNewer(const std::string& latest, const std::string& current) {
    if (latest.empty() || current.empty()) {
        return false;
    }
    auto splitPre = [](const std::string& s) {
        const size_t dash = s.find('-');
        if (dash == std::string::npos) {
            return std::pair<std::string, std::string>{s, {}};
        }
        return std::pair<std::string, std::string>{s.substr(0, dash), s.substr(dash + 1)};
    };
    const auto [lCore, lPre] = splitPre(latest);
    const auto [cCore, cPre] = splitPre(current);
    const auto a = ParseVersionParts(lCore);
    const auto b = ParseVersionParts(cCore);
    if (a.empty() || b.empty()) {
        return false;
    }
    const size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const uint64_t va = i < a.size() ? a[i] : 0;
        const uint64_t vb = i < b.size() ? b[i] : 0;
        if (va != vb) {
            return va > vb;
        }
    }
    if (lPre.empty() != cPre.empty()) {
        return lPre.empty();
    }
    if (!lPre.empty() && !cPre.empty()) {
        return ToLowerAscii(lPre) > ToLowerAscii(cPre);
    }
    return false;
}

bool EndsWithCaseInsensitive(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size()) {
        return false;
    }
    const size_t offset = text.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[offset + i])) !=
            std::tolower(static_cast<unsigned char>(suffix[i]))) {
            return false;
        }
    }
    return true;
}

std::string TrimCopy(std::string_view sv) {
    size_t start = 0;
    while (start < sv.size() &&
           (sv[start] == ' ' || sv[start] == '\t' || sv[start] == '\r' || sv[start] == '\n')) {
        ++start;
    }
    size_t end = sv.size();
    while (end > start &&
           (sv[end - 1] == ' ' || sv[end - 1] == '\t' || sv[end - 1] == '\r' || sv[end - 1] == '\n')) {
        --end;
    }
    std::string out(sv.substr(start, end - start));
    if (out.size() >= 2 &&
        ((out.front() == '\'' && out.back() == '\'') ||
         (out.front() == '"' && out.back() == '"'))) {
        out = out.substr(1, out.size() - 2);
    }
    return out;
}

std::string ExtractVenDevSubsysKey(std::string_view hwId) {
    if (hwId.empty()) {
        return {};
    }
    std::string upper(hwId);
    for (char& ch : upper) {
        if (ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
    }
    auto extractHex = [&upper](std::string_view tag, size_t len) -> std::string {
        const size_t p = upper.find(tag);
        if (p == std::string::npos || p + tag.size() + len > upper.size()) {
            return {};
        }
        for (size_t i = 0; i < len; ++i) {
            if (std::isxdigit(static_cast<unsigned char>(upper[p + tag.size() + i])) == 0) {
                return {};
            }
        }
        return upper.substr(p + tag.size(), len);
    };
    const std::string ven = extractHex("VEN_", 4);
    const std::string dev = extractHex("DEV_", 4);
    const std::string subsys = extractHex("SUBSYS_", 8);
    if (ven.empty() || dev.empty() || subsys.empty()) {
        return {};
    }
    return "VEN_" + ven + "&DEV_" + dev + "&SUBSYS_" + subsys;
}

std::pair<std::string, std::string> ParseLatestVulkanRuntimeFromJson(const std::string& json) {
    const size_t winPos = json.find("\"windows\"");
    if (winPos == std::string::npos) {
        return {};
    }
    std::string bestVer;
    std::string bestFile;

    size_t searchPos = winPos;
    while (true) {
        const size_t fnPos = json.find("\"file_name\"", searchPos);
        if (fnPos == std::string::npos) {
            break;
        }
        const std::string fileName = ExtractJsonStringField(json, "file_name", fnPos);
        searchPos = fnPos + 12;
        if (fileName.rfind("VulkanRT-", 0) != 0 ||
            fileName.find("-Installer.exe") == std::string::npos) {
            continue;
        }
        const size_t instPos = fileName.find("-Installer.exe");
        size_t verStart = fileName.rfind('-', instPos - 1);
        if (verStart == std::string::npos) {
            continue;
        }
        const std::string ver = fileName.substr(verStart + 1, instPos - verStart - 1);
        bool validVer = !ver.empty() && std::isdigit(static_cast<unsigned char>(ver.front())) != 0 &&
                        std::isdigit(static_cast<unsigned char>(ver.back())) != 0 &&
                        ver.find("..") == std::string::npos;
        for (char ch : ver) {
            if (!std::isdigit(static_cast<unsigned char>(ch)) && ch != '.') {
                validVer = false;
                break;
            }
        }
        bool validFile = fileName.find("..") == std::string::npos;
        for (unsigned char ch : fileName) {
            const bool ok = std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-';
            if (!ok) {
                validFile = false;
                break;
            }
        }
        if (!validVer || !validFile) {
            continue;
        }
        if (bestVer.empty() || IsVersionNewer(ver, bestVer)) {
            bestVer = ver;
            bestFile = fileName;
        }
    }

    if (bestVer.empty() || bestFile.empty()) {
        return {};
    }
    const std::string url =
        "https://sdk.lunarg.com/sdk/download/" + bestVer + "/windows/" + bestFile;
    return {bestVer, url};
}

std::pair<std::string, std::string> ParseAmdAdrenalinFromHtml(const std::string& html) {
    size_t pos = 0;
    while ((pos = html.find("https://drivers.amd.com/drivers/installer/", pos)) != std::string::npos) {
        size_t end = pos;
        while (end < html.size() && html[end] != '"' && html[end] != '\'' &&
               html[end] != '<' && html[end] != '>' && html[end] != ' ' &&
               html[end] != '\r' && html[end] != '\n' && html[end] != '\t' &&
               html[end] != ')' && html[end] != ']') {
            ++end;
        }
        const std::string url = html.substr(pos, end - pos);
        if (EndsWithCaseInsensitive(StripUrlQuery(url), ".exe")) {
            std::string ver;
            const std::string marker = "adrenalin-edition-";
            const size_t vPos = url.find(marker);
            if (vPos != std::string::npos) {
                const size_t vStart = vPos + marker.size();
                const size_t vEnd = url.find('-', vStart);
                if (vEnd != std::string::npos && vEnd > vStart) {
                    ver = url.substr(vStart, vEnd - vStart);
                }
            }
            return {ver, url};
        }
        pos = end;
    }
    return {};
}

std::pair<std::string, std::string> ParseAmdChipsetFromHtml(const std::string& html) {
    const std::string prefix = "drivers.amd.com/drivers/amd_chipset_software_";
    std::string bestVer;
    size_t pos = 0;
    while ((pos = html.find(prefix, pos)) != std::string::npos) {
        const size_t vStart = pos + prefix.size();
        const size_t exePos = html.find(".exe", vStart);
        if (exePos == std::string::npos || exePos <= vStart || (exePos - vStart) > 40) {
            pos = vStart;
            continue;
        }
        const std::string ver = html.substr(vStart, exePos - vStart);
        bool valid = !ver.empty() && std::isdigit(static_cast<unsigned char>(ver.front())) != 0;
        for (char ch : ver) {
            if (!std::isdigit(static_cast<unsigned char>(ch)) && ch != '.') {
                valid = false;
                break;
            }
        }
        if (valid && (bestVer.empty() || IsVersionNewer(ver, bestVer))) {
            bestVer = ver;
        }
        pos = exePos + 4;
    }
    if (bestVer.empty()) {
        return {};
    }
    return {bestVer, "https://drivers.amd.com/drivers/amd_chipset_software_" + bestVer + ".exe"};
}

CatalogDriverMatch ResolveMsCatalogDriverCab(const std::string& searchKey) {
    if (searchKey.empty()) {
        return {};
    }
    const std::wstring searchUrl = Utf8ToWide(
        "https://www.catalog.update.microsoft.com/Search.aspx?q=" +
        UrlEncodeCatalogParam(searchKey));
    const HttpResponse searchResp = WinHttpRequest(
        searchUrl, L"GET", true, 1024 * 1024, nullptr, {}, {"catalog.update.microsoft.com"});
    if (!searchResp.ok || searchResp.truncated || searchResp.body.empty()) {
        return {};
    }

    const std::string& html = searchResp.body;
    const bool hostIsWin11 = IsHostWindows11();
    constexpr WORD kProcArchArm64 = 12; // PROCESSOR_ARCHITECTURE_ARM64
    SYSTEM_INFO sysInfo{};
    GetNativeSystemInfo(&sysInfo);
    const bool hostIsArm64 = (sysInfo.wProcessorArchitecture == kProcArchArm64);
    const bool hostIsX64 = (sysInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64);
    std::string bestGuid;
    std::string bestVer;
    int bestDateYmd = 0;
    bool bestHasWin11 = false;
    bool bestHasWin10 = false;

    size_t pos = 0;
    while ((pos = html.find("_C5_R", pos)) != std::string::npos) {
        if (pos < 36) {
            pos += 5;
            continue;
        }
        const std::string_view guidCandidate(html.data() + pos - 36, 36);
        if (!IsValidCatalogGuid(guidCandidate)) {
            pos += 5;
            continue;
        }

        const size_t tdClose = html.find('>', pos + 5);
        const size_t endTd = (tdClose != std::string::npos) ? html.find("</td>", tdClose + 1) : std::string_view::npos;
        if (tdClose == std::string::npos || endTd == std::string::npos || endTd - tdClose > 80) {
            pos += 5;
            continue;
        }

        const size_t winStart = (pos > 32768) ? (pos - 32768) : 0;
        std::string_view rowChunk(html.data() + winStart, pos - winStart);
        if (const size_t trPos = rowChunk.rfind("<tr"); trPos != std::string_view::npos) {
            rowChunk = rowChunk.substr(trPos);
        } else if (const size_t prevEndTr = rowChunk.rfind("</tr>"); prevEndTr != std::string_view::npos) {
            rowChunk = rowChunk.substr(prevEndTr + 5);
        }
        if (rowChunk.find(" - Extension - ") != std::string_view::npos ||
            rowChunk.find(" - SoftwareComponent - ") != std::string_view::npos ||
            rowChunk.find(" - Firmware - ") != std::string_view::npos) {
            pos = endTd + 5;
            continue;
        }

        const std::string rowLower = ToLowerAscii(std::string(rowChunk));
        const bool mentionsArm64 = (rowLower.find("arm64") != std::string::npos ||
                                    rowLower.find("aarch64") != std::string::npos);
        const bool mentionsX64 = (rowLower.find("amd64") != std::string::npos ||
                                  rowLower.find("x64") != std::string::npos ||
                                  rowLower.find("64-bit") != std::string::npos);
        const bool mentionsX86 = (rowLower.find("x86") != std::string::npos ||
                                  rowLower.find("32-bit") != std::string::npos ||
                                  rowLower.find("i386") != std::string::npos);
        if (hostIsX64 && ((mentionsArm64 && !mentionsX64) || (mentionsX86 && !mentionsX64))) {
            pos = endTd + 5;
            continue;
        }
        if (hostIsArm64 && ((mentionsX64 || mentionsX86) && !mentionsArm64)) {
            pos = endTd + 5;
            continue;
        }
        if (!hostIsX64 && !hostIsArm64 && ((mentionsX64 || mentionsArm64) && !mentionsX86)) {
            pos = endTd + 5;
            continue;
        }

        const bool hasWin11 = (rowChunk.find("Windows 11") != std::string_view::npos);
        const bool hasWin10 = (rowChunk.find("Windows 10") != std::string_view::npos);
        if (!hasWin11 && !hasWin10 && rowLower.find("windows") != std::string::npos) {
            pos = endTd + 5;
            continue;
        }
        if (!hostIsWin11 && hasWin11 && !hasWin10) {
            pos = endTd + 5;
            continue;
        }

        std::string ver = TrimCopy(std::string_view(html.data() + tdClose + 1, endTd - tdClose - 1));
        if (!IsValidCatalogVersion(ver)) {
            ver = ExtractVersionFromCatalogRowChunk(rowChunk);
        }

        if (IsValidCatalogVersion(ver)) {
            int dateYmd = ExtractCatalogDateFromRowChunk(rowChunk);
            const auto candParts = ParseVersionParts(ver);
            if (dateYmd == 0) {
                TryExtractTrailingVersionDate(candParts, dateYmd);
            }
            const auto bestParts = ParseVersionParts(bestVer);

            bool isBetter = false;
            if (bestVer.empty()) {
                isBetter = true;
            } else if (!candParts.empty() && !bestParts.empty() &&
                       candParts[0] != bestParts[0] &&
                       dateYmd > 0 && bestDateYmd > 0) {
                if (dateYmd != bestDateYmd) {
                    isBetter = (dateYmd > bestDateYmd);
                } else if (hostIsWin11 ? (hasWin11 != bestHasWin11) : (hasWin10 != bestHasWin10)) {
                    isBetter = hostIsWin11 ? hasWin11 : hasWin10;
                } else {
                    isBetter = IsVersionNewer(ver, bestVer);
                }
            } else {
                const bool strictlyNewer = IsVersionNewer(ver, bestVer);
                const bool sameVer = (!strictlyNewer && !IsVersionNewer(bestVer, ver));
                const bool osBetter = hostIsWin11
                    ? (!bestHasWin11 && hasWin11)
                    : (!bestHasWin10 && hasWin10);
                isBetter = strictlyNewer || (sameVer && osBetter);
            }

            if (isBetter) {
                bestVer = std::move(ver);
                bestGuid.assign(guidCandidate);
                bestDateYmd = dateYmd;
                bestHasWin11 = hasWin11;
                bestHasWin10 = hasWin10;
            }
        }
        pos = endTd + 5;
    }

    if (bestGuid.empty() || bestVer.empty()) {
        return {};
    }

    const std::string postBody =
        "updateIDs=" +
        UrlEncodeCatalogParam("[{\"size\":0,\"languages\":\"\",\"uidInfo\":\"" + bestGuid +
                              "\",\"updateID\":\"" + bestGuid + "\"}]");
    const HttpResponse dlResp = WinHttpRequest(
        L"https://www.catalog.update.microsoft.com/DownloadDialog.aspx",
        L"POST",
        true,
        128 * 1024,
        L"Content-Type: application/x-www-form-urlencoded\r\n",
        postBody,
        {"catalog.update.microsoft.com"});
    if (!dlResp.ok || dlResp.truncated || dlResp.body.empty()) {
        return {bestVer, "", bestDateYmd};
    }

    const std::string dlBodyLower = ToLowerAscii(dlResp.body);
    size_t cabPos = 0;
    while ((cabPos = dlBodyLower.find(".cab", cabPos)) != std::string::npos) {
        size_t urlStart = cabPos;
        while (urlStart > 0 && dlResp.body[urlStart - 1] != '\'' && dlResp.body[urlStart - 1] != '"' &&
               dlResp.body[urlStart - 1] != ' ' && dlResp.body[urlStart - 1] != '>') {
            --urlStart;
        }
        std::string cabUrl = dlResp.body.substr(urlStart, (cabPos + 4) - urlStart);
        if (cabUrl.rfind("http://", 0) == 0) {
            cabUrl.replace(0, 7, "https://");
        }
        if (IsAllowedHttpsHost(cabUrl, {"download.windowsupdate.com", "windowsupdate.com"})) {
            return {bestVer, cabUrl, bestDateYmd};
        }
        cabPos += 4;
    }

    return {bestVer, "", bestDateYmd};
}

WingetManifestInfo ParseWingetInstallerYaml(
    const std::string& yaml,
    const std::string& preferredArch) {
    WingetManifestInfo info;
    struct Entry {
        std::string arch;
        std::string url;
        std::string sha256;
        std::string installerType;
        std::string nestedPath;
        std::string silent;
    };
    std::vector<Entry> entries;
    Entry current;
    bool inInstallers = false;
    bool inNestedFiles = false;
    size_t nestedIndent = std::string::npos;
    bool inIgnoredSubBlock = false;
    size_t ignoredSubBlockIndent = std::string::npos;
    size_t installersItemIndent = std::string::npos;
    std::string topType;
    std::string topSha256;
    std::string topNestedPath;
    std::string topSilent;
    std::string topSilentProgress;

    auto normalizeSha256 = [](std::string_view raw) -> std::string {
        std::string hex = ToLowerAscii(TrimCopy(raw));
        if (hex.size() != 64) {
            return {};
        }
        for (char c : hex) {
            const bool isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            if (!isHex) {
                return {};
            }
        }
        return hex;
    };

    std::istringstream iss(yaml);
    std::string rawLine;
    while (std::getline(iss, rawLine)) {
        const std::string trimmed = TrimCopy(rawLine);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        size_t indent = 0;
        while (indent < rawLine.size() && (rawLine[indent] == ' ' || rawLine[indent] == '\t')) {
            ++indent;
        }

        const bool isListItem = (trimmed.rfind("- ", 0) == 0);
        if (inNestedFiles && (indent < nestedIndent || (indent == nestedIndent && !isListItem))) {
            inNestedFiles = false;
            nestedIndent = std::string::npos;
        }
        if (inIgnoredSubBlock &&
            (indent < ignoredSubBlockIndent || (indent == ignoredSubBlockIndent && !isListItem))) {
            inIgnoredSubBlock = false;
            ignoredSubBlockIndent = std::string::npos;
        }

        if (indent == 0 && !isListItem) {
            if (trimmed == "Installers:") {
                inInstallers = true;
                inNestedFiles = false;
                nestedIndent = std::string::npos;
                inIgnoredSubBlock = false;
                ignoredSubBlockIndent = std::string::npos;
                installersItemIndent = std::string::npos;
                continue;
            }
            if (inInstallers) {
                if (!current.url.empty()) {
                    entries.push_back(current);
                }
                current = Entry{};
                inInstallers = false;
                installersItemIndent = std::string::npos;
            }
        }

        if (trimmed == "NestedInstallerFiles:") {
            inNestedFiles = true;
            nestedIndent = indent;
            inIgnoredSubBlock = false;
            ignoredSubBlockIndent = std::string::npos;
            continue;
        }
        if (trimmed == "AppsAndFeaturesEntries:" || trimmed == "Dependencies:" ||
            trimmed == "Capabilities:" || trimmed == "Markets:" ||
            trimmed == "ReturnCodes:" || trimmed == "ExpectedReturnCodes:") {
            inIgnoredSubBlock = true;
            ignoredSubBlockIndent = indent;
            continue;
        }
        if (inIgnoredSubBlock) {
            continue;
        }

        std::string keyPart = trimmed;
        if (isListItem) {
            keyPart = TrimCopy(keyPart.substr(2));
        }

        const size_t colon = keyPart.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string key = TrimCopy(keyPart.substr(0, colon));
        const std::string val = StripYamlInlineComment(keyPart.substr(colon + 1));

        if (!inInstallers) {
            if (key == "PackageVersion" && !val.empty()) {
                info.version = val;
            } else if (key == "InstallerType" && !val.empty()) {
                topType = ToLowerAscii(val);
            } else if (key == "InstallerSha256" && !val.empty()) {
                topSha256 = normalizeSha256(val);
            } else if (key == "RelativeFilePath" && inNestedFiles && !val.empty() &&
                       topNestedPath.empty()) {
                topNestedPath = val;
            } else if (key == "SilentWithProgress" && !val.empty()) {
                topSilentProgress = val;
            } else if (key == "Silent" && !val.empty()) {
                topSilent = val;
            }
            continue;
        }

        if (isListItem && !inNestedFiles) {
            if (installersItemIndent == std::string::npos) {
                installersItemIndent = indent;
            } else if (indent <= installersItemIndent) {
                if (!current.url.empty()) {
                    entries.push_back(current);
                }
                current = Entry{};
            }
        }

        if (key == "Architecture") {
            current.arch = ToLowerAscii(val);
        } else if (key == "InstallerUrl" && !val.empty()) {
            current.url = val;
        } else if (key == "InstallerSha256" && !val.empty()) {
            current.sha256 = normalizeSha256(val);
        } else if (key == "InstallerType" && !val.empty()) {
            current.installerType = ToLowerAscii(val);
        } else if (key == "RelativeFilePath" && !val.empty() && current.nestedPath.empty()) {
            current.nestedPath = val;
        } else if (key == "SilentWithProgress" && !val.empty()) {
            current.silent = val;
        } else if (key == "Silent" && !val.empty() && current.silent.empty()) {
            current.silent = val;
        }
    }
    if (inInstallers && !current.url.empty()) {
        entries.push_back(current);
    }

    if (entries.empty()) {
        return info;
    }

    constexpr WORD kProcArchArm64 = 12; // PROCESSOR_ARCHITECTURE_ARM64
    SYSTEM_INFO sysInfo{};
    GetNativeSystemInfo(&sysInfo);
    const bool hostIsArm64 = (sysInfo.wProcessorArchitecture == kProcArchArm64);

    auto isSupportedEntry = [&](const Entry& e) {
        const std::string t = e.installerType.empty() ? topType : e.installerType;
        if (t == "msix" || t == "appx" || !IsHttpsUrl(e.url)) {
            return false;
        }
        if (!hostIsArm64 && e.arch == "arm64") {
            return false;
        }
        return true;
    };

    const Entry* chosen = nullptr;
    const std::string pref = ToLowerAscii(preferredArch);
    if (!pref.empty()) {
        for (const auto& e : entries) {
            if (e.arch == pref && isSupportedEntry(e)) {
                chosen = &e;
                break;
            }
        }
    }
    if (chosen == nullptr) {
        for (const char* archOrder : {"x64", "neutral", "x86"}) {
            for (const auto& e : entries) {
                if (e.arch == archOrder && isSupportedEntry(e)) {
                    chosen = &e;
                    break;
                }
            }
            if (chosen != nullptr) break;
        }
    }
    if (chosen == nullptr) {
        for (const auto& e : entries) {
            if (isSupportedEntry(e)) {
                chosen = &e;
                break;
            }
        }
    }
    if (chosen == nullptr) {
        return info;
    }

    info.url = chosen->url;
    info.sha256 = !chosen->sha256.empty() ? chosen->sha256 : topSha256;
    info.ok = !info.version.empty() && !info.url.empty() && !info.sha256.empty();
    const std::string& rawNested = !chosen->nestedPath.empty() ? chosen->nestedPath : topNestedPath;
    if (IsSafeRelativeInstallerPath(rawNested)) {
        info.nestedRelativePath = TrimCopy(rawNested);
    }
    if (!chosen->silent.empty()) {
        info.silentArgs = SanitizeWingetSilentArgs(chosen->silent);
    } else if (!topSilentProgress.empty()) {
        info.silentArgs = SanitizeWingetSilentArgs(topSilentProgress);
    } else {
        info.silentArgs = SanitizeWingetSilentArgs(topSilent);
    }
    return info;
}

WingetManifestInfo ResolveWingetPkgManifest(
    const std::string& repoPath,
    const std::string& preferredArch) {
    WingetManifestInfo result;
    if (repoPath.empty()) {
        return result;
    }
    const std::string encPath = EncodeWingetPath(repoPath);
    if (encPath.empty()) {
        return result;
    }
    const std::wstring treeUrl = Utf8ToWide(
        "https://github.com/microsoft/winget-pkgs/tree/master/manifests/" + encPath);
    const HttpResponse treeResp = WinHttpRequest(
        treeUrl, L"GET", true, 256 * 1024, L"Accept: application/json\r\n", {},
        {"github.com", "githubusercontent.com"});
    if (!treeResp.ok || treeResp.truncated || treeResp.body.empty()) {
        return result;
    }

    const std::vector<std::string> dirs = ExtractGithubTreeNames(treeResp.body, "directory");
    std::string bestStable;
    std::string bestAny;
    for (const auto& d : dirs) {
        if (!d.empty() && std::isdigit(static_cast<unsigned char>(d[0]))) {
            if (!IsPrereleaseVersionLabel(d)) {
                if (bestStable.empty() || IsVersionNewer(d, bestStable)) {
                    bestStable = d;
                }
            }
            if (bestAny.empty() || IsVersionNewer(d, bestAny)) {
                bestAny = d;
            }
        }
    }
    const std::string& bestVer = !bestStable.empty() ? bestStable : bestAny;
    auto isSafeSingleSegment = [](std::string_view seg) {
        return !seg.empty() && seg != "." && seg != ".." &&
               seg.find('/') == std::string_view::npos &&
               seg.find('\\') == std::string_view::npos &&
               seg.find(':') == std::string_view::npos;
    };
    if (!isSafeSingleSegment(bestVer)) {
        return result;
    }

    const std::string encVer = EncodeWingetPath(bestVer);
    if (encVer.empty()) {
        return result;
    }
    const std::wstring verTreeUrl = Utf8ToWide(
        "https://github.com/microsoft/winget-pkgs/tree/master/manifests/" +
        encPath + "/" + encVer);
    const HttpResponse verResp = WinHttpRequest(
        verTreeUrl, L"GET", true, 128 * 1024, L"Accept: application/json\r\n", {},
        {"github.com", "githubusercontent.com"});
    if (!verResp.ok || verResp.truncated || verResp.body.empty()) {
        return result;
    }

    const std::vector<std::string> files = ExtractGithubTreeNames(verResp.body, "file");
    std::string installerYaml;
    for (const auto& f : files) {
        if (EndsWithCaseInsensitive(f, ".installer.yaml")) {
            installerYaml = f;
            break;
        }
    }
    if (!isSafeSingleSegment(installerYaml)) {
        return result;
    }

    const std::string encYaml = EncodeWingetPath(installerYaml);
    if (encYaml.empty()) {
        return result;
    }
    const std::wstring rawUrl = Utf8ToWide(
        "https://raw.githubusercontent.com/microsoft/winget-pkgs/master/manifests/" +
        encPath + "/" + encVer + "/" + encYaml);
    const HttpResponse yamlResp = WinHttpRequest(
        rawUrl, L"GET", true, 128 * 1024, nullptr, {}, {"github.com", "githubusercontent.com"});
    if (!yamlResp.ok || yamlResp.truncated || yamlResp.body.empty()) {
        return result;
    }

    result = ParseWingetInstallerYaml(yamlResp.body, preferredArch);
    if (result.version.empty()) {
        result.version = bestVer;
    }
    return result;
}

} // namespace hdm
