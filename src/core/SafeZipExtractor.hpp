#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace hdm {

struct ZipExtractLimits {
    size_t maxFiles{1024};
    uint64_t maxTotalBytes{512ULL * 1024ULL * 1024ULL};
    int maxDepth{16};
};

/// Synchronously extracts a ZIP archive (Stored + RFC 1951 DEFLATE) with strict byte/file/depth limits,
/// CRC-32 verification, segment-by-segment non-reparse directory checks, and immediate cancellation.
bool ExtractZipSafely(
    const std::filesystem::path& zipPath,
    const std::filesystem::path& destDir,
    const std::atomic<bool>* cancelFlag = nullptr,
    std::string* outError = nullptr,
    const ZipExtractLimits& limits = {});

} // namespace hdm
