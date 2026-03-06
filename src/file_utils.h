#pragma once

#include "common.h"

#include <fstream>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

template <typename T>
[[nodiscard]] constexpr bool spanHasRange(std::span<T> data, std::size_t index, std::size_t length) {
    return index <= data.size() && length <= (data.size() - index);
}

[[nodiscard]] constexpr bool spanHasRange(const vBytes& data, std::size_t index, std::size_t length) {
    return spanHasRange(std::span<const Byte>(data), index, length);
}

template <typename T>
inline void requireSpanRange(std::span<T> data, std::size_t index, std::size_t length, const char* error_message) {
    if (!spanHasRange(data, index, length)) {
        throw std::runtime_error(error_message);
    }
}

inline void requireSpanRange(const vBytes& data, std::size_t index, std::size_t length, const char* error_message) {
    if (!spanHasRange(data, index, length)) {
        throw std::runtime_error(error_message);
    }
}

[[nodiscard]] bool hasValidFilename(const fs::path& p);
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
[[nodiscard]] std::size_t checkedFileSize(
    const fs::path& path,
    std::string_view error_message,
    bool require_non_empty = false);
[[nodiscard]] std::streamsize checkedStreamWriteSize(std::size_t size, std::string_view error_message);
void ensureStreamStateOrThrow(const std::ios& stream, std::string_view error_message);
void flushOutputOrThrow(std::ostream& output, std::string_view error_message);
void closeOutputOrThrow(std::ofstream& output, std::string_view error_message);
void writeBytesOrThrow(std::ostream& output, std::span<const Byte> bytes, std::string_view error_message);
[[nodiscard]] std::ifstream openBinaryInputOrThrow(const fs::path& path, std::string_view error_message);
[[nodiscard]] std::ofstream openBinaryOutputTruncOrThrow(const fs::path& path, std::string_view error_message);
[[nodiscard]] std::ofstream openBinaryOutputForWriteOrThrow(const fs::path& path);
[[nodiscard]] std::streamsize readSomeOrThrow(
    std::istream& input,
    Byte* dst,
    std::size_t size,
    const char* error_message);
void readExactOrThrow(
    std::istream& input,
    Byte* dst,
    std::size_t size,
    const char* error_message);
[[nodiscard]] bool tryReadExact(std::istream& input, Byte* dst, std::size_t size);
void cleanupPathNoThrow(const fs::path& path) noexcept;
void commitStagedFileNoReplaceOrThrow(
    const fs::path& staged_path,
    const fs::path& output_path,
    std::string_view error_message);
[[nodiscard]] std::size_t validateFileForRead(const fs::path& path, FileTypeCheck FileType = FileTypeCheck::data_file);
[[nodiscard]] vBytes readFile(const fs::path& path, FileTypeCheck FileType = FileTypeCheck::data_file);

struct TempFileCleanupGuard {
    fs::path path{};
    bool active{false};

    TempFileCleanupGuard() = default;

    explicit TempFileCleanupGuard(fs::path p) {
        set(std::move(p));
    }

    void set(fs::path p) {
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
