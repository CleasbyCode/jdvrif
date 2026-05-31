#pragma once

#include "common.h"

#include <fstream>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

[[nodiscard]] constexpr bool spanHasRange(std::span<const Byte> data, std::size_t index, std::size_t length) {
    return index <= data.size() && length <= (data.size() - index);
}

inline void requireSpanRange(std::span<const Byte> data, std::size_t index, std::size_t length, const char* error_message) {
    if (!spanHasRange(data, index, length)) {
        throw std::runtime_error(error_message);
    }
}

[[nodiscard]] inline std::size_t checkedAdd(std::size_t a, std::size_t b, const char* error_message) {
    if (a > std::numeric_limits<std::size_t>::max() - b) {
        throw std::overflow_error(error_message);
    }
    return a + b;
}

[[nodiscard]] inline std::size_t checkedMul(std::size_t a, std::size_t b, const char* error_message) {
    if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) {
        throw std::overflow_error(error_message);
    }
    return a * b;
}

[[nodiscard]] inline std::streamoff checkedStreamOffset(std::size_t offset, const char* error_message) {
    if (offset > static_cast<std::size_t>(std::numeric_limits<std::streamoff>::max())) {
        throw std::overflow_error(error_message);
    }
    return static_cast<std::streamoff>(offset);
}

[[nodiscard]] bool hasSafeEmbeddedFilename(const fs::path& p);
[[nodiscard]] bool hasFileExtension(const fs::path& p, std::initializer_list<std::string_view> exts);
[[nodiscard]] std::optional<fs::path> makeUniqueRandomizedPath(
    const fs::path& parent_dir,
    std::string_view prefix,
    std::string_view suffix,
    std::size_t max_attempts = 1024,
    std::size_t token_hex_chars = 16);
[[nodiscard]] fs::path uniqueRandomizedPathOrThrow(
    const fs::path& parent_dir,
    std::string_view prefix,
    std::string_view suffix,
    std::size_t max_attempts,
    std::string_view error_message,
    std::size_t token_hex_chars = 16);
[[nodiscard]] std::size_t checkedFileSize(const fs::path& path, std::string_view error_message, bool require_non_empty = false);
void ensureStreamStateOrThrow(const std::ios& stream, std::string_view error_message);
void closeOutputOrThrow(std::ofstream& output, std::string_view error_message);
void writeBytesOrThrow(std::ostream& output, std::span<const Byte> bytes, std::string_view error_message);
[[nodiscard]] std::ifstream openBinaryInputOrThrow(const fs::path& path, std::string_view error_message);
[[nodiscard]] std::ofstream openBinaryOutputForWriteOrThrow(const fs::path& path);
void requireNoTrailingDataOrThrow(std::istream& input, const char* error_message);
[[nodiscard]] std::streamsize readSomeOrThrow(std::istream& input, Byte* dst, std::size_t size, const char* error_message);
void readExactOrThrow(std::istream& input, Byte* dst, std::size_t size, const char* error_message);
[[nodiscard]] bool tryReadExact(std::istream& input, Byte* dst, std::size_t size);
void cleanupPathNoThrow(const fs::path& path) noexcept;
[[nodiscard]] bool tryCommitStagedFileNoReplace(const fs::path& staged_path, const fs::path& output_path, std::string_view error_message);
void commitStagedFileNoReplaceOrThrow(const fs::path& staged_path, const fs::path& output_path, std::string_view error_message);
[[nodiscard]] std::size_t validateFileForRead(
    const fs::path& path,
    FileTypeCheck file_type = FileTypeCheck::data_file);
[[nodiscard]] vBytes readFile(
    const fs::path& path,
    FileTypeCheck file_type = FileTypeCheck::data_file);

// Buffered, fd-backed output sink for the staged final-image write. Replaces a
// std::ofstream + pubsetbuf: the internal buffer coalesces the many small
// segment-header writes into few write(2) calls (the property pubsetbuf gave us),
// while sendFrom() streams the bulk encrypted payload straight from one fd to
// another via sendfile(2) — no userspace bounce. Opens O_EXCL + mode 0600, so it
// matches openBinaryOutputForWriteOrThrow's atomic-create + owner-only semantics.
// Linux/POSIX only (consistent with the recover path, which already uses sendfile).
class OutputFile {
public:
    OutputFile(const fs::path& path, std::size_t buffer_capacity);
    ~OutputFile() noexcept;

    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;

    // Buffered append. Spans at least as large as the buffer bypass it to avoid
    // a redundant copy (as a >streambuf write did with the old ofstream).
    void write(std::span<const Byte> bytes, std::string_view error_message);

    // Zero-copy append of `length` bytes from in_fd at in_offset via sendfile(2).
    // Flushes the buffer first so output byte order is preserved; falls back to a
    // read/write copy if the kernel/filesystem rejects sendfile.
    void sendFrom(int in_fd, std::size_t in_offset, std::size_t length, std::string_view error_message);

    // Flush remaining buffered bytes and close, surfacing any write error.
    void close(std::string_view error_message);

private:
    void drain(std::string_view error_message);
    void writeRaw(const Byte* data, std::size_t size, std::string_view error_message);
    void sendFromFallback(int in_fd, std::size_t in_offset, std::size_t length, std::string_view error_message);

    int         fd_ = -1;
    vBytes      buffer_;
    std::size_t fill_ = 0;
};

struct TempFileCleanupGuard {
    fs::path path{};
    bool active{false};

    TempFileCleanupGuard() = default;
    explicit TempFileCleanupGuard(fs::path p) { set(std::move(p)); }
    TempFileCleanupGuard(const TempFileCleanupGuard&) = delete;
    TempFileCleanupGuard& operator=(const TempFileCleanupGuard&) = delete;

    TempFileCleanupGuard(TempFileCleanupGuard&& other) noexcept
        : path(std::move(other.path)), active(other.active) {
        other.dismiss();
    }

    TempFileCleanupGuard& operator=(TempFileCleanupGuard&& other) noexcept {
        if (this != &other) {
            if (active) {
                cleanupPathNoThrow(path);
            }
            path = std::move(other.path);
            active = other.active;
            other.dismiss();
        }
        return *this;
    }

    void set(fs::path p) {
        if (active) {
            cleanupPathNoThrow(path);
        }
        path = std::move(p);
        active = !path.empty();
    }

    void dismiss() noexcept {
        path.clear();
        active = false;
    }

    ~TempFileCleanupGuard() {
        if (active) {
            cleanupPathNoThrow(path);
        }
    }
};
