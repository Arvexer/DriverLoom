#include "core/SafeZipExtractor.hpp"
#include "common/AppCommon.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hdm {

namespace {

constexpr uint32_t kLocalFileHeaderSig = 0x04034B50u;
constexpr uint32_t kCentralDirHeaderSig = 0x02014B50u;
constexpr uint32_t kEocdSig = 0x06054B50u;
constexpr uint32_t kZip64LocatorSig = 0x07064B50u;
constexpr uint32_t kZip64EocdSig = 0x06064B50u;
constexpr uint64_t kMaxCentralDirBytes = 32ULL * 1024ULL * 1024ULL;

uint16_t ReadU16LE(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadU32LE(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t ReadU64LE(const uint8_t* p) noexcept {
    return static_cast<uint64_t>(ReadU32LE(p)) |
           (static_cast<uint64_t>(ReadU32LE(p + 4)) << 32);
}

const std::array<uint32_t, 256>& GetCrc32Table() noexcept {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

uint32_t UpdateCrc32(uint32_t crc, const uint8_t* data, size_t len) noexcept {
    const auto& table = GetCrc32Table();
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

bool EqualsIgnoreCaseAsciiRange(std::wstring_view a, std::wstring_view b) noexcept {
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
    return StripTrailingSeparators(std::move(out));
}

bool ReadExactAt(
    HANDLE hFile,
    uint64_t offset,
    uint8_t* dst,
    size_t len,
    const std::atomic<bool>* cancelFlag,
    std::string* outError) {
    if (len == 0) {
        return true;
    }
    if (offset > static_cast<uint64_t>((std::numeric_limits<LONGLONG>::max)())) {
        if (outError) *outError = "Смещение в ZIP-архиве выходит за пределы допустимого диапазона.";
        return false;
    }
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(hFile, li, nullptr, FILE_BEGIN)) {
        if (outError) *outError = "Ошибка позиционирования в ZIP-архиве: " + FormatWin32Error(GetLastError());
        return false;
    }
    size_t totalRead = 0;
    while (totalRead < len) {
        if (cancelFlag != nullptr && cancelFlag->load()) {
            if (outError) *outError = "Распаковка ZIP-архива отменена.";
            return false;
        }
        const DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(len - totalRead, 1024u * 1024u));
        DWORD got = 0;
        if (!ReadFile(hFile, dst + totalRead, chunk, &got, nullptr) || got == 0) {
            if (outError) *outError = "Неожиданный конец или ошибка чтения ZIP-архива.";
            return false;
        }
        totalRead += got;
    }
    return true;
}

bool DecodeZipEntryName(const uint8_t* raw, size_t len, bool utf8Flag, std::wstring& out) {
    out.clear();
    if (len == 0 || len > 4096) {
        return false;
    }
    auto tryCodePage = [&](UINT cp, DWORD flags) -> bool {
        const int needed = MultiByteToWideChar(
            cp, flags, reinterpret_cast<const char*>(raw), static_cast<int>(len), nullptr, 0);
        if (needed <= 0) {
            return false;
        }
        std::wstring w(static_cast<size_t>(needed), L'\0');
        const int written = MultiByteToWideChar(
            cp, flags, reinterpret_cast<const char*>(raw), static_cast<int>(len), w.data(), needed);
        if (written != needed) {
            return false;
        }
        out = std::move(w);
        return true;
    };

    if (utf8Flag) {
        return tryCodePage(CP_UTF8, MB_ERR_INVALID_CHARS);
    }
    if (tryCodePage(CP_UTF8, MB_ERR_INVALID_CHARS)) {
        return true;
    }
    return tryCodePage(CP_OEMCP, 0);
}

bool IsReservedWindowsName(std::wstring_view segment) noexcept {
    const size_t dot = segment.find(L'.');
    const std::wstring_view stem = (dot == std::wstring_view::npos) ? segment : segment.substr(0, dot);
    static constexpr std::wstring_view kReserved[] = {
        L"CON", L"PRN", L"AUX", L"NUL",
        L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
        L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"
    };
    for (std::wstring_view r : kReserved) {
        if (EqualsIgnoreCaseAsciiRange(stem, r)) {
            return true;
        }
    }
    return false;
}

bool ParseSafeZipPath(
    std::wstring rawName,
    int maxDepth,
    std::vector<std::wstring>& outSegments,
    bool& outIsDir,
    std::wstring& outNormalizedLowerKey) {
    outSegments.clear();
    outIsDir = false;
    outNormalizedLowerKey.clear();

    if (rawName.empty()) {
        return false;
    }
    for (wchar_t& ch : rawName) {
        if (ch == L'/') {
            ch = L'\\';
        }
        if (ch == L'\0' || static_cast<unsigned int>(ch) < 0x20u ||
            ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' ||
            ch == L'<' || ch == L'>' || ch == L'|') {
            return false;
        }
    }
    if (rawName.front() == L'\\') {
        return false;
    }
    if (rawName.back() == L'\\') {
        outIsDir = true;
        rawName.pop_back();
        if (rawName.empty() || rawName.back() == L'\\') {
            return false;
        }
    }

    size_t start = 0;
    while (start <= rawName.size()) {
        const size_t sep = rawName.find(L'\\', start);
        const std::wstring seg = (sep == std::wstring::npos)
                                     ? rawName.substr(start)
                                     : rawName.substr(start, sep - start);
        if (seg.empty() || seg == L"." || seg == L"..") {
            return false;
        }
        if (seg.back() == L' ' || seg.back() == L'.') {
            return false;
        }
        if (IsReservedWindowsName(seg)) {
            return false;
        }
        outSegments.push_back(seg);
        if (outSegments.size() > static_cast<size_t>(maxDepth)) {
            return false;
        }
        if (sep == std::wstring::npos) {
            break;
        }
        start = sep + 1;
    }
    if (outSegments.empty()) {
        return false;
    }

    std::wstring key = rawName;
    const int mapped = LCMapStringEx(
        LOCALE_NAME_INVARIANT,
        LCMAP_LOWERCASE,
        rawName.data(),
        static_cast<int>(rawName.size()),
        key.data(),
        static_cast<int>(key.size()),
        nullptr,
        nullptr,
        0);
    if (mapped <= 0) {
        return false;
    }
    outNormalizedLowerKey = std::move(key);
    return true;
}

struct ExtractCleanupGuard {
    std::vector<std::filesystem::path> createdFiles;
    std::vector<std::filesystem::path> createdDirs;
    bool armed{true};

    ~ExtractCleanupGuard() {
        if (!armed) {
            return;
        }
        std::error_code ec;
        for (auto it = createdFiles.rbegin(); it != createdFiles.rend(); ++it) {
            std::filesystem::remove(*it, ec);
        }
        for (auto it = createdDirs.rbegin(); it != createdDirs.rend(); ++it) {
            RemoveDirectoryW(it->c_str());
        }
    }
};

struct ZipEntryMeta {
    std::vector<std::wstring> segments;
    bool isDir{false};
    uint16_t method{0};
    uint32_t crc32{0};
    uint64_t compSize{0};
    uint64_t uncompSize{0};
    uint64_t localHeaderOffset{0};
};

class BitReader {
public:
    BitReader(HANDLE hFile, uint64_t offset, uint64_t compBytes, const std::atomic<bool>* cancelFlag)
        : hFile_(hFile), nextOffset_(offset), remainingBytes_(compBytes), cancelFlag_(cancelFlag) {}

    [[nodiscard]] bool ReadBits(int n, uint32_t& out) {
        while (bitCount_ < n) {
            if (!EnsureByte()) {
                return false;
            }
            bitBuf_ |= static_cast<uint64_t>(buf_[pos_++]) << bitCount_;
            bitCount_ += 8;
        }
        out = static_cast<uint32_t>(bitBuf_ & ((1ULL << n) - 1ULL));
        bitBuf_ >>= n;
        bitCount_ -= n;
        return true;
    }

    [[nodiscard]] bool ReadBit(int& outBit) {
        if (bitCount_ > 0) {
            outBit = static_cast<int>(bitBuf_ & 1ULL);
            bitBuf_ >>= 1;
            --bitCount_;
            return true;
        }
        uint32_t v = 0;
        if (!ReadBits(1, v)) {
            return false;
        }
        outBit = static_cast<int>(v);
        return true;
    }

    void AlignToByte() noexcept {
        const int drop = bitCount_ & 7;
        bitBuf_ >>= drop;
        bitCount_ -= drop;
    }

    [[nodiscard]] bool ReadAlignedBytes(uint8_t* dst, size_t len) {
        while (len > 0 && bitCount_ >= 8) {
            *dst++ = static_cast<uint8_t>(bitBuf_ & 0xFFu);
            bitBuf_ >>= 8;
            bitCount_ -= 8;
            --len;
        }
        while (len > 0) {
            if (!EnsureByte()) {
                return false;
            }
            const size_t chunk = std::min(len, avail_ - pos_);
            std::memcpy(dst, buf_.data() + pos_, chunk);
            pos_ += chunk;
            dst += chunk;
            len -= chunk;
        }
        return true;
    }

private:
    [[nodiscard]] bool EnsureByte() {
        if (pos_ < avail_) {
            return true;
        }
        if (remainingBytes_ == 0) {
            return false;
        }
        if (cancelFlag_ != nullptr && cancelFlag_->load()) {
            return false;
        }
        const size_t want = static_cast<size_t>(
            std::min<uint64_t>(remainingBytes_, buf_.size()));
        if (!ReadExactAt(hFile_, nextOffset_, buf_.data(), want, cancelFlag_, nullptr)) {
            return false;
        }
        nextOffset_ += want;
        remainingBytes_ -= want;
        pos_ = 0;
        avail_ = want;
        return true;
    }

    HANDLE hFile_;
    uint64_t nextOffset_;
    uint64_t remainingBytes_;
    const std::atomic<bool>* cancelFlag_;
    std::array<uint8_t, 65536> buf_{};
    size_t pos_{0};
    size_t avail_{0};
    uint64_t bitBuf_{0};
    int bitCount_{0};
};

class HuffmanTree {
public:
    static constexpr int kMaxBits = 15;
    static constexpr int kMaxSyms = 288;

    bool Build(const uint8_t* lengths, int numSyms) noexcept {
        if (numSyms <= 0 || numSyms > kMaxSyms) {
            return false;
        }
        count_.fill(0);
        for (int sym = 0; sym < numSyms; ++sym) {
            if (lengths[sym] > kMaxBits) {
                return false;
            }
            ++count_[lengths[sym]];
        }
        if (count_[0] == static_cast<uint16_t>(numSyms)) {
            empty_ = true;
            return true;
        }
        int left = 1;
        for (int len = 1; len <= kMaxBits; ++len) {
            left <<= 1;
            left -= static_cast<int>(count_[len]);
            if (left < 0) {
                return false;
            }
        }
        std::array<uint16_t, kMaxBits + 1> offs{};
        for (int len = 1; len < kMaxBits; ++len) {
            offs[len + 1] = static_cast<uint16_t>(offs[len] + count_[len]);
        }
        for (int sym = 0; sym < numSyms; ++sym) {
            if (lengths[sym] != 0) {
                symbol_[offs[lengths[sym]]++] = static_cast<uint16_t>(sym);
            }
        }
        empty_ = false;
        return true;
    }

    bool Decode(BitReader& br, int& outSym) const {
        if (empty_) {
            return false;
        }
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            int bit = 0;
            if (!br.ReadBit(bit)) {
                return false;
            }
            code |= bit;
            const int c = static_cast<int>(count_[len]);
            if (code - c < first) {
                outSym = static_cast<int>(symbol_[index + (code - first)]);
                return true;
            }
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        return false;
    }

private:
    std::array<uint16_t, kMaxBits + 1> count_{};
    std::array<uint16_t, kMaxSyms> symbol_{};
    bool empty_{true};
};

struct OutputSink {
    HANDLE outFile{INVALID_HANDLE_VALUE};
    uint64_t expectedEntryBytes{0};
    uint64_t maxTotalBytes{0};
    uint64_t& totalExtractedBytes;
    const std::atomic<bool>* cancelFlag{nullptr};
    std::string* outError{nullptr};

    uint64_t entryBytes{0};
    uint32_t crc{0xFFFFFFFFu};
    std::array<uint8_t, 32768> window{};
    size_t winPos{0};
    std::array<uint8_t, 32768> outBuf{};
    size_t outUsed{0};

    [[nodiscard]] bool EmitByte(uint8_t b) {
        if (entryBytes >= expectedEntryBytes) {
            if (outError) *outError = "Размер распаковываемого файла превышает заголовок ZIP (обнаружена zip-бомба).";
            return false;
        }
        if (totalExtractedBytes >= maxTotalBytes) {
            if (outError) *outError = "Превышен общий лимит распакованных данных ZIP-архива.";
            return false;
        }
        ++entryBytes;
        ++totalExtractedBytes;
        window[winPos & 0x7FFFu] = b;
        ++winPos;
        outBuf[outUsed++] = b;
        if (outUsed == outBuf.size()) {
            return Flush();
        }
        return true;
    }

    [[nodiscard]] bool Flush() {
        if (outUsed == 0) {
            return true;
        }
        if (cancelFlag != nullptr && cancelFlag->load()) {
            if (outError) *outError = "Распаковка ZIP-архива отменена.";
            return false;
        }
        crc = UpdateCrc32(crc, outBuf.data(), outUsed);
        DWORD offset = 0;
        const DWORD total = static_cast<DWORD>(outUsed);
        while (offset < total) {
            DWORD written = 0;
            if (!WriteFile(outFile, outBuf.data() + offset, total - offset, &written, nullptr) ||
                written == 0) {
                if (outError) *outError = "Ошибка записи распакованного файла: " + FormatWin32Error(GetLastError());
                return false;
            }
            offset += written;
        }
        outUsed = 0;
        return true;
    }
};

const HuffmanTree& GetFixedLitTree() {
    static const HuffmanTree tree = [] {
        std::array<uint8_t, 288> lengths{};
        for (int i = 0; i <= 143; ++i) lengths[i] = 8;
        for (int i = 144; i <= 255; ++i) lengths[i] = 9;
        for (int i = 256; i <= 279; ++i) lengths[i] = 7;
        for (int i = 280; i <= 287; ++i) lengths[i] = 8;
        HuffmanTree t;
        t.Build(lengths.data(), static_cast<int>(lengths.size()));
        return t;
    }();
    return tree;
}

const HuffmanTree& GetFixedDistTree() {
    static const HuffmanTree tree = [] {
        std::array<uint8_t, 30> lengths{};
        lengths.fill(5);
        HuffmanTree t;
        t.Build(lengths.data(), static_cast<int>(lengths.size()));
        return t;
    }();
    return tree;
}

bool InflateStream(BitReader& br, OutputSink& sink, std::string* outError) {
    static constexpr uint16_t kLenBase[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static constexpr uint8_t kLenExtra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static constexpr uint16_t kDistBase[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
    };
    static constexpr uint8_t kDistExtra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };
    static constexpr uint8_t kCodeLenOrder[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    uint32_t bfinal = 0;
    do {
        if (sink.cancelFlag != nullptr && sink.cancelFlag->load()) {
            if (outError) *outError = "Распаковка ZIP-архива отменена.";
            return false;
        }
        uint32_t btype = 0;
        if (!br.ReadBits(1, bfinal) || !br.ReadBits(2, btype)) {
            if (outError) *outError = "Повреждён заголовок блока DEFLATE в ZIP-архиве.";
            return false;
        }
        if (btype == 3) {
            if (outError) *outError = "Недопустимый тип блока DEFLATE (BTYPE=3).";
            return false;
        }
        if (btype == 0) {
            br.AlignToByte();
            uint8_t hdr[4]{};
            if (!br.ReadAlignedBytes(hdr, sizeof(hdr))) {
                if (outError) *outError = "Неожиданный конец несжатого блока DEFLATE.";
                return false;
            }
            const uint16_t len = ReadU16LE(hdr);
            const uint16_t nlen = ReadU16LE(hdr + 2);
            if (static_cast<uint16_t>(len ^ 0xFFFFu) != nlen) {
                if (outError) *outError = "Несовпадение LEN/NLEN в несжатом блоке DEFLATE.";
                return false;
            }
            std::array<uint8_t, 4096> rawChunk{};
            size_t rem = len;
            while (rem > 0) {
                const size_t n = std::min(rem, rawChunk.size());
                if (!br.ReadAlignedBytes(rawChunk.data(), n)) {
                    if (outError) *outError = "Усечённый несжатый блок DEFLATE.";
                    return false;
                }
                for (size_t i = 0; i < n; ++i) {
                    if (!sink.EmitByte(rawChunk[i])) {
                        return false;
                    }
                }
                rem -= n;
            }
            continue;
        }

        const HuffmanTree* litPtr = nullptr;
        const HuffmanTree* distPtr = nullptr;
        HuffmanTree dynLit;
        HuffmanTree dynDist;

        if (btype == 1) {
            litPtr = &GetFixedLitTree();
            distPtr = &GetFixedDistTree();
        } else {
            uint32_t hlitRaw = 0;
            uint32_t hdistRaw = 0;
            uint32_t hclenRaw = 0;
            if (!br.ReadBits(5, hlitRaw) || !br.ReadBits(5, hdistRaw) || !br.ReadBits(4, hclenRaw)) {
                if (outError) *outError = "Усечён заголовок динамического дерева DEFLATE.";
                return false;
            }
            const int hlit = static_cast<int>(hlitRaw) + 257;
            const int hdist = static_cast<int>(hdistRaw) + 1;
            const int hclen = static_cast<int>(hclenRaw) + 4;
            if (hlit > 286 || hdist > 30) {
                if (outError) *outError = "Недопустимые размеры таблиц Хаффмана в потоке DEFLATE.";
                return false;
            }
            std::array<uint8_t, 19> clLengths{};
            for (int i = 0; i < hclen; ++i) {
                uint32_t v = 0;
                if (!br.ReadBits(3, v)) {
                    if (outError) *outError = "Повреждена таблица длин кодов DEFLATE.";
                    return false;
                }
                clLengths[kCodeLenOrder[i]] = static_cast<uint8_t>(v);
            }
            HuffmanTree clTree;
            if (!clTree.Build(clLengths.data(), 19)) {
                if (outError) *outError = "Повреждено дерево длин кодов DEFLATE.";
                return false;
            }
            const int totalCodes = hlit + hdist;
            std::array<uint8_t, 286 + 30> lengths{};
            int idx = 0;
            while (idx < totalCodes) {
                int sym = 0;
                if (!clTree.Decode(br, sym)) {
                    if (outError) *outError = "Ошибка декодирования длин кодов DEFLATE.";
                    return false;
                }
                if (sym < 16) {
                    lengths[idx++] = static_cast<uint8_t>(sym);
                } else {
                    uint8_t repeatVal = 0;
                    uint32_t extra = 0;
                    int repeatCount = 0;
                    if (sym == 16) {
                        if (idx == 0 || !br.ReadBits(2, extra)) {
                            if (outError) *outError = "Некорректный повтор длины кода (sym=16) в DEFLATE.";
                            return false;
                        }
                        repeatVal = lengths[idx - 1];
                        repeatCount = 3 + static_cast<int>(extra);
                    } else if (sym == 17) {
                        if (!br.ReadBits(3, extra)) {
                            if (outError) *outError = "Некорректный повтор нулей (sym=17) в DEFLATE.";
                            return false;
                        }
                        repeatCount = 3 + static_cast<int>(extra);
                    } else {
                        if (!br.ReadBits(7, extra)) {
                            if (outError) *outError = "Некорректный повтор нулей (sym=18) в DEFLATE.";
                            return false;
                        }
                        repeatCount = 11 + static_cast<int>(extra);
                    }
                    if (idx + repeatCount > totalCodes) {
                        if (outError) *outError = "Выход повтора длин за границы таблицы DEFLATE.";
                        return false;
                    }
                    for (int r = 0; r < repeatCount; ++r) {
                        lengths[idx++] = repeatVal;
                    }
                }
            }
            if (lengths[256] == 0) {
                if (outError) *outError = "Отсутствует код конца блока (256) в таблице DEFLATE.";
                return false;
            }
            if (!dynLit.Build(lengths.data(), hlit) ||
                !dynDist.Build(lengths.data() + hlit, hdist)) {
                if (outError) *outError = "Переполненное дерево Хаффмана в потоке DEFLATE.";
                return false;
            }
            litPtr = &dynLit;
            distPtr = &dynDist;
        }

        for (;;) {
            int sym = 0;
            if (!litPtr->Decode(br, sym)) {
                if (outError) *outError = "Ошибка декодирования символа DEFLATE.";
                return false;
            }
            if (sym < 256) {
                if (!sink.EmitByte(static_cast<uint8_t>(sym))) {
                    return false;
                }
            } else if (sym == 256) {
                break;
            } else {
                if (sym > 285) {
                    if (outError) *outError = "Недопустимый код длины совпадения в DEFLATE.";
                    return false;
                }
                const int lenIdx = sym - 257;
                uint32_t lenExtraVal = 0;
                if (kLenExtra[lenIdx] > 0 && !br.ReadBits(kLenExtra[lenIdx], lenExtraVal)) {
                    if (outError) *outError = "Усечены дополнительные биты длины в DEFLATE.";
                    return false;
                }
                const size_t matchLen = static_cast<size_t>(kLenBase[lenIdx]) + lenExtraVal;

                int distSym = 0;
                if (!distPtr->Decode(br, distSym) || distSym >= 30) {
                    if (outError) *outError = "Недопустимый код дистанции в DEFLATE.";
                    return false;
                }
                uint32_t distExtraVal = 0;
                if (kDistExtra[distSym] > 0 && !br.ReadBits(kDistExtra[distSym], distExtraVal)) {
                    if (outError) *outError = "Усечены дополнительные биты дистанции в DEFLATE.";
                    return false;
                }
                const size_t matchDist = static_cast<size_t>(kDistBase[distSym]) + distExtraVal;
                if (matchDist == 0 || static_cast<uint64_t>(matchDist) > sink.entryBytes || matchDist > 32768u) {
                    if (outError) *outError = "Дистанция обратного копирования выходит за пределы окна DEFLATE.";
                    return false;
                }
                for (size_t k = 0; k < matchLen; ++k) {
                    const size_t srcIdx = (sink.winPos + 32768u - matchDist) & 0x7FFFu;
                    const uint8_t b = sink.window[srcIdx];
                    if (!sink.EmitByte(b)) {
                        return false;
                    }
                }
            }
        }
    } while (bfinal == 0);

    return sink.Flush();
}

bool EnsureSafeSubdir(
    const std::filesystem::path& destDir,
    const std::wstring& destCanonPrefix,
    const std::vector<std::wstring>& segments,
    size_t segmentCount,
    ExtractCleanupGuard& cleanup,
    std::filesystem::path& outDirPath,
    std::string* outError) {
    std::filesystem::path cur = destDir;
    for (size_t i = 0; i < segmentCount; ++i) {
        cur /= segments[i];
        if (cur.wstring().size() >= MAX_PATH - 4) {
            if (outError) *outError = "Слишком длинный путь внутри ZIP-архива.";
            return false;
        }
        if (CreateDirectoryW(cur.c_str(), nullptr)) {
            cleanup.createdDirs.push_back(cur);
        } else {
            const DWORD err = GetLastError();
            if (err != ERROR_ALREADY_EXISTS) {
                if (outError) *outError = "Не удалось создать подкаталог распаковки: " + FormatWin32Error(err);
                return false;
            }
        }
        HandleGuard dirHandle(CreateFileW(
            cur.c_str(),
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
            if (outError) *outError = "Обнаружена точка повторного анализа (reparse point) в каталоге распаковки.";
            return false;
        }
        const std::wstring dirCanon = GetFinalPathString(dirHandle.Get());
        if (dirCanon.size() <= destCanonPrefix.size() ||
            CompareStringOrdinal(
                dirCanon.c_str(),
                static_cast<int>(destCanonPrefix.size()),
                destCanonPrefix.c_str(),
                static_cast<int>(destCanonPrefix.size()),
                TRUE) != CSTR_EQUAL) {
            if (outError) *outError = "Путь каталога выходит за пределы каталога распаковки.";
            return false;
        }
    }
    outDirPath = std::move(cur);
    return true;
}

} // namespace

bool ExtractZipSafely(
    const std::filesystem::path& zipPath,
    const std::filesystem::path& destDir,
    const std::atomic<bool>* cancelFlag,
    std::string* outError,
    const ZipExtractLimits& limits) {
    if (outError) {
        outError->clear();
    }
    if (cancelFlag != nullptr && cancelFlag->load()) {
        if (outError) *outError = "Распаковка ZIP-архива отменена.";
        return false;
    }
    if (zipPath.empty() || destDir.empty() || limits.maxFiles == 0 ||
        limits.maxTotalBytes == 0 || limits.maxDepth <= 0) {
        if (outError) *outError = "Некорректные параметры распаковки ZIP.";
        return false;
    }

    HandleGuard zipHandle(CreateFileW(
        zipPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!zipHandle.IsValid()) {
        if (outError) *outError = "Не удалось открыть ZIP-архив: " + FormatWin32Error(GetLastError());
        return false;
    }
    BY_HANDLE_FILE_INFORMATION zipInfo{};
    if (!GetFileInformationByHandle(zipHandle.Get(), &zipInfo) ||
        (zipInfo.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        if (outError) *outError = "ZIP-архив является каталогом или точкой повторного анализа.";
        return false;
    }
    LARGE_INTEGER liSize{};
    if (!GetFileSizeEx(zipHandle.Get(), &liSize) || liSize.QuadPart < 22) {
        if (outError) *outError = "ZIP-архив пуст или повреждён.";
        return false;
    }
    const uint64_t fileSize = static_cast<uint64_t>(liSize.QuadPart);

    std::error_code ec;
    std::filesystem::create_directories(destDir, ec);
    if (ec) {
        if (outError) *outError = "Не удалось создать каталог распаковки ZIP.";
        return false;
    }
    HandleGuard dstRootHandle(CreateFileW(
        destDir.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    BY_HANDLE_FILE_INFORMATION dstInfo{};
    if (!dstRootHandle.IsValid() || !GetFileInformationByHandle(dstRootHandle.Get(), &dstInfo) ||
        (dstInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (dstInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        if (outError) *outError = "Целевой каталог распаковки недоступен или является точкой повторного анализа.";
        return false;
    }
    std::wstring destCanonPrefix = GetFinalPathString(dstRootHandle.Get());
    dstRootHandle.Reset();
    if (destCanonPrefix.empty()) {
        if (outError) *outError = "Не удалось определить канонический путь каталога распаковки.";
        return false;
    }
    if (destCanonPrefix.back() != L'\\') {
        destCanonPrefix.push_back(L'\\');
    }

    const size_t tailLen = static_cast<size_t>(std::min<uint64_t>(fileSize, 65557ULL));
    const uint64_t tailOffset = fileSize - tailLen;
    std::vector<uint8_t> tailBuf(tailLen);
    if (!ReadExactAt(zipHandle.Get(), tailOffset, tailBuf.data(), tailLen, cancelFlag, outError)) {
        return false;
    }

    size_t eocdTailPos = std::string::npos;
    for (size_t i = tailLen - 22 + 1; i > 0; --i) {
        const size_t pos = i - 1;
        if (ReadU32LE(tailBuf.data() + pos) == kEocdSig) {
            const uint16_t commentLen = ReadU16LE(tailBuf.data() + pos + 20);
            if (pos + 22 + commentLen == tailLen) {
                eocdTailPos = pos;
                break;
            }
        }
    }
    if (eocdTailPos == std::string::npos) {
        if (outError) *outError = "В ZIP-архиве отсутствует корректный заголовок EOCD.";
        return false;
    }

    const uint8_t* eocd = tailBuf.data() + eocdTailPos;
    const uint64_t eocdFileOffset = tailOffset + eocdTailPos;
    uint32_t diskNum = ReadU16LE(eocd + 4);
    uint32_t cdDisk = ReadU16LE(eocd + 6);
    uint64_t entriesOnDisk = ReadU16LE(eocd + 8);
    uint64_t totalEntries = ReadU16LE(eocd + 10);
    uint64_t cdSize = ReadU32LE(eocd + 12);
    uint64_t cdOffset = ReadU32LE(eocd + 16);

    const bool needsZip64 =
        diskNum == 0xFFFFu || cdDisk == 0xFFFFu ||
        entriesOnDisk == 0xFFFFu || totalEntries == 0xFFFFu ||
        cdSize == 0xFFFFFFFFu || cdOffset == 0xFFFFFFFFu;
    if (needsZip64) {
        if (eocdFileOffset < 20) {
            if (outError) *outError = "Отсутствует локатор ZIP64 EOCD.";
            return false;
        }
        uint8_t locBuf[20]{};
        if (!ReadExactAt(zipHandle.Get(), eocdFileOffset - 20, locBuf, sizeof(locBuf), cancelFlag, outError)) {
            return false;
        }
        if (ReadU32LE(locBuf) != kZip64LocatorSig ||
            ReadU32LE(locBuf + 4) != 0u ||
            ReadU32LE(locBuf + 16) > 1u) {
            if (outError) *outError = "Некорректный или многотомный локатор ZIP64.";
            return false;
        }
        const uint64_t z64Off = ReadU64LE(locBuf + 8);
        if (z64Off > fileSize || fileSize - z64Off < 56) {
            if (outError) *outError = "Смещение ZIP64 EOCD выходит за пределы файла.";
            return false;
        }
        uint8_t z64[56]{};
        if (!ReadExactAt(zipHandle.Get(), z64Off, z64, sizeof(z64), cancelFlag, outError)) {
            return false;
        }
        if (ReadU32LE(z64) != kZip64EocdSig) {
            if (outError) *outError = "Некорректная сигнатура ZIP64 EOCD.";
            return false;
        }
        diskNum = ReadU32LE(z64 + 16);
        cdDisk = ReadU32LE(z64 + 20);
        entriesOnDisk = ReadU64LE(z64 + 24);
        totalEntries = ReadU64LE(z64 + 32);
        cdSize = ReadU64LE(z64 + 40);
        cdOffset = ReadU64LE(z64 + 48);
    }

    if (diskNum != 0 || cdDisk != 0 || entriesOnDisk != totalEntries) {
        if (outError) *outError = "Многотомные ZIP-архивы не поддерживаются.";
        return false;
    }
    if (totalEntries == 0 || totalEntries > limits.maxFiles) {
        if (outError) *outError = "Недопустимое число файлов в ZIP-архиве.";
        return false;
    }
    if (cdSize == 0 || cdSize > kMaxCentralDirBytes ||
        cdOffset > fileSize || cdSize > fileSize - cdOffset) {
        if (outError) *outError = "Повреждён или выходит за пределы файла центральный каталог ZIP.";
        return false;
    }

    std::vector<uint8_t> cd(static_cast<size_t>(cdSize));
    if (!ReadExactAt(zipHandle.Get(), cdOffset, cd.data(), cd.size(), cancelFlag, outError)) {
        return false;
    }

    std::vector<ZipEntryMeta> entries;
    entries.reserve(static_cast<size_t>(totalEntries));
    std::unordered_set<std::wstring> seenFiles;
    std::unordered_set<std::wstring> seenDirs;
    uint64_t declaredTotalBytes = 0;

    size_t cdPos = 0;
    for (uint64_t idx = 0; idx < totalEntries; ++idx) {
        if (cdPos + 46 > cd.size()) {
            if (outError) *outError = "Усечена запись центрального каталога ZIP.";
            return false;
        }
        const uint8_t* rec = cd.data() + cdPos;
        if (ReadU32LE(rec) != kCentralDirHeaderSig) {
            if (outError) *outError = "Некорректная сигнатура записи в центральном каталоге ZIP.";
            return false;
        }
        const uint16_t verMadeBy = ReadU16LE(rec + 4);
        const uint16_t gpFlag = ReadU16LE(rec + 8);
        const uint16_t method = ReadU16LE(rec + 10);
        const uint32_t crc32 = ReadU32LE(rec + 16);
        uint64_t compSize = ReadU32LE(rec + 20);
        uint64_t uncompSize = ReadU32LE(rec + 24);
        const uint16_t nameLen = ReadU16LE(rec + 28);
        const uint16_t extraLen = ReadU16LE(rec + 30);
        const uint16_t commentLen = ReadU16LE(rec + 32);
        uint32_t diskStart = ReadU16LE(rec + 34);
        const uint32_t extAttrs = ReadU32LE(rec + 38);
        uint64_t localOff = ReadU32LE(rec + 42);

        const size_t totalRecSize =
            46u + static_cast<size_t>(nameLen) + static_cast<size_t>(extraLen) + static_cast<size_t>(commentLen);
        if (totalRecSize > cd.size() - cdPos) {
            if (outError) *outError = "Запись центрального каталога выходит за границы блока.";
            return false;
        }

        if ((gpFlag & 0x0001u) != 0) {
            if (outError) *outError = "Зашифрованные ZIP-архивы не поддерживаются.";
            return false;
        }
        if (method != 0 && method != 8) {
            if (outError) *outError = "Неподдерживаемый метод сжатия в ZIP-архиве.";
            return false;
        }
        if ((verMadeBy >> 8) == 3u && (((extAttrs >> 16) & 0xF000u) == 0xA000u)) {
            if (outError) *outError = "Символические ссылки внутри ZIP-архива запрещены.";
            return false;
        }

        if (uncompSize == 0xFFFFFFFFu || compSize == 0xFFFFFFFFu ||
            localOff == 0xFFFFFFFFu || diskStart == 0xFFFFu) {
            bool foundZ64 = false;
            const uint8_t* extra = rec + 46 + nameLen;
            size_t exPos = 0;
            while (exPos + 4 <= extraLen) {
                const uint16_t tag = ReadU16LE(extra + exPos);
                const uint16_t fSize = ReadU16LE(extra + exPos + 2);
                if (exPos + 4 + fSize > extraLen) {
                    if (outError) *outError = "Повреждено расширение ZIP64.";
                    return false;
                }
                if (tag == 0x0001u) {
                    size_t zPos = 0;
                    const uint8_t* zData = extra + exPos + 4;
                    if (uncompSize == 0xFFFFFFFFu) {
                        if (zPos + 8 > fSize) {
                            if (outError) *outError = "Неполное поле uncompSize в ZIP64.";
                            return false;
                        }
                        uncompSize = ReadU64LE(zData + zPos);
                        zPos += 8;
                    }
                    if (compSize == 0xFFFFFFFFu) {
                        if (zPos + 8 > fSize) {
                            if (outError) *outError = "Неполное поле compSize в ZIP64.";
                            return false;
                        }
                        compSize = ReadU64LE(zData + zPos);
                        zPos += 8;
                    }
                    if (localOff == 0xFFFFFFFFu) {
                        if (zPos + 8 > fSize) {
                            if (outError) *outError = "Неполное поле localOffset в ZIP64.";
                            return false;
                        }
                        localOff = ReadU64LE(zData + zPos);
                        zPos += 8;
                    }
                    if (diskStart == 0xFFFFu) {
                        if (zPos + 4 > fSize) {
                            if (outError) *outError = "Неполное поле diskStart в ZIP64.";
                            return false;
                        }
                        diskStart = ReadU32LE(zData + zPos);
                    }
                    foundZ64 = true;
                    break;
                }
                exPos += 4 + fSize;
            }
            if (!foundZ64) {
                if (outError) *outError = "Отсутствуют обязательные метаданные ZIP64.";
                return false;
            }
        }

        if (diskStart != 0) {
            if (outError) *outError = "Многотомные ZIP-архивы не поддерживаются.";
            return false;
        }

        std::wstring wideName;
        if (!DecodeZipEntryName(rec + 46, nameLen, (gpFlag & 0x0800u) != 0, wideName)) {
            if (outError) *outError = "Недопустимая кодировка имени файла в ZIP-архиве.";
            return false;
        }

        ZipEntryMeta meta;
        std::wstring lowerKey;
        if (!ParseSafeZipPath(std::move(wideName), limits.maxDepth, meta.segments, meta.isDir, lowerKey)) {
            if (outError) *outError = "Небезопасный путь внутри ZIP-архива.";
            return false;
        }
        if (meta.isDir) {
            if (seenFiles.count(lowerKey) != 0) {
                if (outError) *outError = "Конфликт имени файла и каталога в ZIP-архиве.";
                return false;
            }
            seenDirs.insert(lowerKey);
            if (uncompSize != 0 || compSize != 0) {
                if (outError) *outError = "Некорректный ненулевой размер у записи каталога ZIP.";
                return false;
            }
        } else {
            if (seenDirs.count(lowerKey) != 0 || !seenFiles.insert(lowerKey).second) {
                if (outError) *outError = "Дублирующийся путь файла в ZIP-архиве.";
                return false;
            }
            if (uncompSize > limits.maxTotalBytes ||
                declaredTotalBytes > limits.maxTotalBytes - uncompSize) {
                if (outError) *outError = "Суммарный размер ZIP-архива превышает безопасный лимит.";
                return false;
            }
            declaredTotalBytes += uncompSize;
            if (method == 0 && compSize != uncompSize) {
                if (outError) *outError = "Несовпадение размеров Stored-файла в ZIP-архиве.";
                return false;
            }
        }

        meta.method = method;
        meta.crc32 = crc32;
        meta.compSize = compSize;
        meta.uncompSize = uncompSize;
        meta.localHeaderOffset = localOff;
        entries.push_back(std::move(meta));
        cdPos += totalRecSize;
    }

    ExtractCleanupGuard cleanup;
    uint64_t totalExtractedBytes = 0;
    size_t extractedFiles = 0;

    for (const auto& entry : entries) {
        if (cancelFlag != nullptr && cancelFlag->load()) {
            if (outError) *outError = "Распаковка ZIP-архива отменена.";
            return false;
        }
        if (entry.isDir) {
            std::filesystem::path unusedDir;
            if (!EnsureSafeSubdir(
                    destDir, destCanonPrefix, entry.segments, entry.segments.size(),
                    cleanup, unusedDir, outError)) {
                return false;
            }
            continue;
        }

        std::filesystem::path parentDir = destDir;
        if (entry.segments.size() > 1) {
            if (!EnsureSafeSubdir(
                    destDir, destCanonPrefix, entry.segments, entry.segments.size() - 1,
                    cleanup, parentDir, outError)) {
                return false;
            }
        }
        const std::filesystem::path outPath = parentDir / entry.segments.back();
        if (outPath.wstring().size() >= MAX_PATH) {
            if (outError) *outError = "Слишком длинный итоговый путь файла при распаковке ZIP.";
            return false;
        }

        if (entry.localHeaderOffset > fileSize || fileSize - entry.localHeaderOffset < 30) {
            if (outError) *outError = "Некорректное смещение локального заголовка ZIP.";
            return false;
        }
        uint8_t lfh[30]{};
        if (!ReadExactAt(zipHandle.Get(), entry.localHeaderOffset, lfh, sizeof(lfh), cancelFlag, outError)) {
            return false;
        }
        if (ReadU32LE(lfh) != kLocalFileHeaderSig) {
            if (outError) *outError = "Некорректная сигнатура локального заголовка ZIP.";
            return false;
        }
        const uint16_t localMethod = ReadU16LE(lfh + 8);
        if (localMethod != entry.method) {
            if (outError) *outError = "Метод сжатия в локальном заголовке не совпадает с каталогом.";
            return false;
        }
        const uint64_t localExtraSkip =
            static_cast<uint64_t>(ReadU16LE(lfh + 26)) + static_cast<uint64_t>(ReadU16LE(lfh + 28));
        const uint64_t dataStartBase = entry.localHeaderOffset + 30ULL;
        if (localExtraSkip > fileSize - dataStartBase) {
            if (outError) *outError = "Локальный заголовок ZIP выходит за пределы файла.";
            return false;
        }
        const uint64_t dataOffset = dataStartBase + localExtraSkip;
        if (entry.compSize > fileSize - dataOffset) {
            if (outError) *outError = "Сжатые данные записи ZIP выходят за пределы файла.";
            return false;
        }

        HandleGuard outFile(CreateFileW(
            outPath.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        if (!outFile.IsValid()) {
            if (outError) *outError = "Не удалось создать файл при распаковке ZIP: " + FormatWin32Error(GetLastError());
            return false;
        }
        cleanup.createdFiles.push_back(outPath);

        uint32_t computedCrc = 0;
        if (entry.method == 0) {
            uint64_t remaining = entry.compSize;
            uint64_t curOff = dataOffset;
            uint32_t crc = 0xFFFFFFFFu;
            std::array<uint8_t, 32768> copyBuf{};
            while (remaining > 0) {
                if (cancelFlag != nullptr && cancelFlag->load()) {
                    if (outError) *outError = "Распаковка ZIP-архива отменена.";
                    return false;
                }
                const size_t n = static_cast<size_t>(std::min<uint64_t>(remaining, copyBuf.size()));
                if (n > limits.maxTotalBytes - totalExtractedBytes) {
                    if (outError) *outError = "Превышен общий лимит распакованных данных ZIP-архива.";
                    return false;
                }
                if (!ReadExactAt(zipHandle.Get(), curOff, copyBuf.data(), n, cancelFlag, outError)) {
                    return false;
                }
                crc = UpdateCrc32(crc, copyBuf.data(), n);
                DWORD writtenOffset = 0;
                const DWORD totalWrite = static_cast<DWORD>(n);
                while (writtenOffset < totalWrite) {
                    DWORD written = 0;
                    if (!WriteFile(outFile.Get(), copyBuf.data() + writtenOffset, totalWrite - writtenOffset, &written, nullptr) ||
                        written == 0) {
                        if (outError) *outError = "Ошибка записи файла при распаковке ZIP.";
                        return false;
                    }
                    writtenOffset += written;
                }
                totalExtractedBytes += n;
                curOff += n;
                remaining -= n;
            }
            computedCrc = crc ^ 0xFFFFFFFFu;
        } else {
            BitReader br(zipHandle.Get(), dataOffset, entry.compSize, cancelFlag);
            OutputSink sink{
                outFile.Get(),
                entry.uncompSize,
                limits.maxTotalBytes,
                totalExtractedBytes,
                cancelFlag,
                outError};
            if (!InflateStream(br, sink, outError)) {
                return false;
            }
            if (sink.entryBytes != entry.uncompSize) {
                if (outError) *outError = "Фактический размер распакованных данных меньше заявленного в ZIP.";
                return false;
            }
            computedCrc = sink.crc ^ 0xFFFFFFFFu;
        }

        if (computedCrc != entry.crc32) {
            if (outError) *outError = "Ошибка контрольной суммы CRC-32 при распаковке ZIP.";
            return false;
        }
        if (!FlushFileBuffers(outFile.Get())) {
            if (outError) *outError = "Ошибка сброса буферов файла при распаковке ZIP.";
            return false;
        }
        outFile.Reset();
        ++extractedFiles;
    }

    if (extractedFiles == 0) {
        if (outError) *outError = "В ZIP-архиве отсутствуют файлы для распаковки.";
        return false;
    }

    cleanup.armed = false;
    return true;
}

} // namespace hdm
