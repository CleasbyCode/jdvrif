#include "file_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <fstream>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <system_error>

namespace {
constexpr std::size_t
    MINIMUM_IMAGE_SIZE       = 134,
    MAX_IMAGE_SIZE           = 8 * 1024 * 1024,
    MAX_FILE_SIZE            = 3ULL * 1024 * 1024 * 1024,
    MAX_FILENAME_STREAM_SIZE = static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max());

[[nodiscard]] bool isValidFilenameChar(unsigned char c) {
    return std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == '@' || c == '%';
}

[[nodiscard]] bool isImageType(FileTypeCheck file_type) {
    return file_type == FileTypeCheck::cover_image || file_type == FileTypeCheck::embedded_image;
}

[[nodiscard]] std::size_t safeFileSize(const fs::path& path) {
    std::error_code ec;
    const std::uintmax_t raw_file_size = fs::file_size(path, ec);
    if (ec) {
        throw std::runtime_error(std::format("Error: Failed to get file size for \"{}\".", path.string()));
    }
    if (raw_file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
        raw_file_size > static_cast<std::uintmax_t>(MAX_FILENAME_STREAM_SIZE)) {
        throw std::runtime_error("Error: File is too large for this build.");
    }
    return static_cast<std::size_t>(raw_file_size);
}
}

bool hasValidFilename(const fs::path& p) {
    if (p.empty()) {
        return false;
    }
    std::string filename = p.filename().string();
    if (filename.empty()) {
        return false;
    }
    return std::ranges::all_of(filename, isValidFilenameChar);
}

bool hasFileExtension(const fs::path& p, std::initializer_list<std::string_view> exts) {
    auto e = p.extension().string();
    std::ranges::transform(e, e.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return std::ranges::any_of(exts, [&e](std::string_view ext) {
        return e == ext;
    });
}

std::optional<fs::path> makeUniqueRandomizedPath(
    const fs::path& parent_dir,
    std::string_view prefix,
    std::string_view suffix,
    std::size_t max_attempts) {

    if (max_attempts == 0) {
        return std::nullopt;
    }

    std::error_code ec;
    std::array<char, 16> rand_buf{};
    std::string filename;

    for (std::size_t i = 0; i < max_attempts; ++i) {
        const uint32_t rand_num = 100000 + randombytes_uniform(900000);
        const auto [ptr, to_chars_ec] = std::to_chars(rand_buf.data(), rand_buf.data() + rand_buf.size(), rand_num);
        if (to_chars_ec != std::errc{}) {
            continue;
        }

        filename.clear();
        filename.reserve(prefix.size() + static_cast<std::size_t>(ptr - rand_buf.data()) + suffix.size());
        filename.append(prefix);
        filename.append(rand_buf.data(), ptr);
        filename.append(suffix);

        const fs::path candidate = parent_dir.empty()
            ? fs::path(filename)
            : (parent_dir / filename);

        if (!fs::exists(candidate, ec)) {
            return candidate;
        }
        ec.clear();
    }

    return std::nullopt;
}

fs::path uniqueRandomizedPathOrThrow(
    const fs::path& parent_dir,
    std::string_view prefix,
    std::string_view suffix,
    std::size_t max_attempts,
    std::string_view error_message) {

    if (auto path = makeUniqueRandomizedPath(parent_dir, prefix, suffix, max_attempts)) {
        return *path;
    }
    throw std::runtime_error(std::string(error_message));
}

std::size_t checkedFileSize(const fs::path& path, std::string_view error_message, bool require_non_empty) {
    std::error_code ec;
    const std::uintmax_t raw_size = fs::file_size(path, ec);
    if (ec || raw_size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error(std::string(error_message));
    }

    const std::size_t size = static_cast<std::size_t>(raw_size);
    if (require_non_empty && size == 0) {
        throw std::runtime_error(std::string(error_message));
    }
    return size;
}

std::streamsize checkedStreamWriteSize(std::size_t size, std::string_view error_message) {
    if (size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error(std::string(error_message));
    }
    return static_cast<std::streamsize>(size);
}

void ensureStreamStateOrThrow(const std::ios& stream, std::string_view error_message) {
    if (!stream) {
        throw std::runtime_error(std::string(error_message));
    }
}

void flushOutputOrThrow(std::ostream& output, std::string_view error_message) {
    output.flush();
    ensureStreamStateOrThrow(output, error_message);
}

void closeOutputOrThrow(std::ofstream& output, std::string_view error_message) {
    output.close();
    ensureStreamStateOrThrow(output, error_message);
}

void writeBytesOrThrow(std::ostream& output, std::span<const Byte> bytes, std::string_view error_message) {
    if (bytes.empty()) {
        return;
    }
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        checkedStreamWriteSize(bytes.size(), error_message));
    ensureStreamStateOrThrow(output, error_message);
}

std::ifstream openBinaryInputOrThrow(const fs::path& path, std::string_view error_message) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(std::string(error_message));
    }
    return input;
}

std::ofstream openBinaryOutputTruncOrThrow(const fs::path& path, std::string_view error_message) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error(std::string(error_message));
    }
    return output;
}

std::ofstream openBinaryOutputForWriteOrThrow(const fs::path& path) {
    return openBinaryOutputTruncOrThrow(
        path,
        "Write Error: Unable to write to file. Make sure you have WRITE permissions for this location.");
}

std::streamsize readSomeOrThrow(
    std::istream& input,
    Byte* dst,
    std::size_t size,
    const char* error_message) {

    input.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(size));
    const std::streamsize got = input.gcount();
    if (got < 0 || (!input && !input.eof())) {
        throw std::runtime_error(error_message);
    }
    return got;
}

void readExactOrThrow(
    std::istream& input,
    Byte* dst,
    std::size_t size,
    const char* error_message) {

    if (readSomeOrThrow(input, dst, size, error_message) != static_cast<std::streamsize>(size)) {
        throw std::runtime_error(error_message);
    }
}

bool tryReadExact(std::istream& input, Byte* dst, std::size_t size) {
    input.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(size));
    return input.gcount() == static_cast<std::streamsize>(size);
}

void cleanupPathNoThrow(const fs::path& path) noexcept {
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    fs::remove(path, ec);
}

std::size_t validateFileForRead(const fs::path& path, FileTypeCheck FileType) {
    if (!hasValidFilename(path)) {
        throw std::runtime_error("Invalid Input Error: Unsupported characters in filename arguments.");
    }

    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    const bool regular = exists && !ec && fs::is_regular_file(path, ec);
    if (ec || !exists || !regular) {
        throw std::runtime_error(std::format("Error: File \"{}\" not found or not a regular file.", path.string()));
    }

    const std::size_t file_size = safeFileSize(path);

    if (!file_size) {
        throw std::runtime_error("Error: File is empty.");
    }

    if (isImageType(FileType)) {
        if (!hasFileExtension(path, {".png", ".jpg", ".jpeg", ".jfif"})) {
            throw std::runtime_error("File Type Error: Invalid image extension. Only expecting \".jpg\", \".jpeg\", \".jfif\" or \".png\".");
        }

        if (FileType == FileTypeCheck::cover_image) {
            if (MINIMUM_IMAGE_SIZE > file_size) {
                throw std::runtime_error("File Error: Invalid image file size.");
            }

            if (file_size > MAX_IMAGE_SIZE) {
                throw std::runtime_error("Image File Error: Cover image file exceeds maximum size limit.");
            }
        }
    }

    if (file_size > MAX_FILE_SIZE) {
        throw std::runtime_error("Error: File exceeds program size limit.");
    }
    return file_size;
}

vBytes readFile(const fs::path& path, FileTypeCheck FileType) {
    const std::size_t file_size = validateFileForRead(path, FileType);
    std::ifstream file(path, std::ios::binary);

    if (!file) {
        throw std::runtime_error(std::format("Failed to open file: {}", path.string()));
    }

    vBytes vec(file_size);
    file.read(reinterpret_cast<char*>(vec.data()), static_cast<std::streamsize>(file_size));
    if (!file) {
        throw std::runtime_error("Failed to read full file: partial read");
    }
    return vec;
}
