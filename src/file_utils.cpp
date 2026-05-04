#include "file_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <fstream>
#include <ios>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <system_error>
#include <vector>
#include <version>

#ifndef __cpp_lib_ios_noreplace
#error "jdvrif requires std::ios::noreplace for secure output file creation."
#endif

static_assert(__cpp_lib_ios_noreplace >= 202207L, "jdvrif requires std::ios::noreplace for secure output file creation.");

namespace {
constexpr std::size_t MINIMUM_IMAGE_SIZE = 134, MAX_IMAGE_SIZE = 8 * 1024 * 1024, MAX_FILE_SIZE = 3ULL * 1024 * 1024 * 1024,
                      MAX_FILENAME_STREAM_SIZE = static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max());

[[nodiscard]] bool isValidFilenameChar(unsigned char c) {
    switch (c) {
        case '/':
        case '\\':
        case ':':
        case '*':
        case '?':
        case '"':
        case '<':
        case '>':
        case '|':
            return false;
        default:
            return c >= 0x20 && c != 0x7F;
    }
}

[[nodiscard]] bool isReservedEmbeddedFilename(std::string_view filename) {
    return filename == "." || filename == ".." ||
           (!filename.empty() && (filename.front() == '.' || filename.front() == '-' ||
                                  filename.back() == ' ' || filename.back() == '.'));
}

[[nodiscard]] bool isImageType(FileTypeCheck file_type) {
    return file_type == FileTypeCheck::cover_image || file_type == FileTypeCheck::embedded_image;
}

[[nodiscard]] std::size_t checkedPathSize(const fs::path& path, std::string_view error_message, bool require_non_empty = false, bool require_stream_sized = false) {
    std::error_code ec;
    const std::uintmax_t raw_file_size = fs::file_size(path, ec);
    if (raw_file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
        (require_stream_sized && raw_file_size > static_cast<std::uintmax_t>(MAX_FILENAME_STREAM_SIZE)) ||
        ec) {
        throw std::runtime_error(std::string(error_message));
    }
    const std::size_t file_size = static_cast<std::size_t>(raw_file_size);
    if (require_non_empty && file_size == 0) throw std::runtime_error(std::string(error_message));
    return file_size;
}

[[nodiscard]] std::size_t safeFileSize(const fs::path& path) { return checkedPathSize(path, std::format("Error: Failed to get file size for \"{}\".", path.string()), false, true); }

[[nodiscard]] std::string randomPathToken(std::size_t token_hex_chars) {
    constexpr auto HEX_DIGITS = std::to_array<char>({
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'
    });

    if (token_hex_chars == 0) return {};

    const std::size_t byte_count = (token_hex_chars + 1) / 2;
    std::string token(token_hex_chars, '\0');
    std::vector<Byte> random_bytes(byte_count);
    randombytes_buf(random_bytes.data(), random_bytes.size());

    std::size_t token_index = 0;
    for (const Byte value : random_bytes) {
        token[token_index++] = HEX_DIGITS[value >> 4];
        if (token_index == token_hex_chars) break;
        token[token_index++] = HEX_DIGITS[value & 0x0F];
    }
    return token;
}

[[noreturn]] void throwPathInspectError(const fs::path& path, const std::error_code& ec) {
    throw std::runtime_error(std::format("Error: Failed to inspect path \"{}\": {}", path.string(), ec.message()));
}

void restrictOutputPermissionsOrThrow(const fs::path& path, std::string_view error_message) {
#if defined(_WIN32)
    (void)path;
    (void)error_message;
#else
    std::error_code ec;
    fs::permissions(
        path,
        fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::replace,
        ec);
    if (ec) throw std::runtime_error(std::format("{}: {}", error_message, ec.message()));
#endif
}

[[nodiscard]] bool isExistingPathError(const fs::path& path) {
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (ec) throwPathInspectError(path, ec);
    return exists;
}

[[nodiscard]] bool isFileExistsError(const std::error_code& ec) {
    return ec == std::make_error_code(std::errc::file_exists) ||
           ec == std::make_error_code(std::errc::permission_denied);
}

[[nodiscard]] bool copyFileNoReplace(const fs::path& source_path, const fs::path& output_path, std::string_view error_message) {
    std::ifstream input = openBinaryInputOrThrow(source_path, std::format("{}: failed to reopen staged file", error_message));
    std::ofstream output(output_path, std::ios::binary | std::ios::out | std::ios::noreplace);
    if (!output) {
        if (isExistingPathError(output_path)) return false;
        throw std::runtime_error(std::format("{}: unable to create output file", error_message));
    }

    try {
        restrictOutputPermissionsOrThrow(output_path, error_message);

        constexpr std::size_t COPY_CHUNK_SIZE = 2 * 1024 * 1024;
        std::array<Byte, COPY_CHUNK_SIZE> buffer{};
        while (true) {
            const std::streamsize got = readSomeOrThrow(
                input,
                buffer.data(),
                buffer.size(),
                "Read Error: Failed while committing staged output.");
            if (got == 0) break;
            writeBytesOrThrow(
                output,
                std::span<const Byte>(buffer.data(), static_cast<std::size_t>(got)),
                error_message);
        }

        closeOutputOrThrow(output, error_message);
    } catch (...) {
        cleanupPathNoThrow(output_path);
        throw;
    }

    cleanupPathNoThrow(source_path);
    return true;
}
}

bool hasValidFilename(const fs::path& p) {
    if (p.empty()) return false;
    const std::string filename = p.filename().string();
    if (filename.empty()) return false;
    return std::ranges::all_of(filename, isValidFilenameChar);
}
bool hasSafeEmbeddedFilename(const fs::path& p) { return hasValidFilename(p) && !isReservedEmbeddedFilename(p.filename().string()); }

bool hasFileExtension(const fs::path& p, std::initializer_list<std::string_view> exts) {
    auto e = p.extension().string();
    std::ranges::transform(e, e.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return std::ranges::any_of(exts, [&e](std::string_view ext) { return e == ext; });
}

std::optional<fs::path> makeUniqueRandomizedPath(const fs::path& parent_dir, std::string_view prefix, std::string_view suffix, std::size_t max_attempts, std::size_t token_hex_chars) {
    if (max_attempts == 0 || token_hex_chars == 0) return std::nullopt;

    std::error_code ec;
    std::string filename;

    for (std::size_t i = 0; i < max_attempts; ++i) {
        const std::string token = randomPathToken(token_hex_chars);

        filename.clear();
        filename.reserve(prefix.size() + token.size() + suffix.size());
        filename.append(prefix);
        filename.append(token);
        filename.append(suffix);

        const fs::path candidate = parent_dir.empty() ? fs::path(filename) : (parent_dir / filename);
        if (fs::exists(candidate, ec)) {
            ec.clear();
            continue;
        }
        if (ec) throwPathInspectError(candidate, ec);
        return candidate;
    }

    return std::nullopt;
}

fs::path uniqueRandomizedPathOrThrow(const fs::path& parent_dir, std::string_view prefix, std::string_view suffix, std::size_t max_attempts, std::string_view error_message, std::size_t token_hex_chars) {
    if (auto path = makeUniqueRandomizedPath(parent_dir, prefix, suffix, max_attempts, token_hex_chars)) return *path;
    throw std::runtime_error(std::string(error_message));
}

std::size_t checkedFileSize(const fs::path& path, std::string_view error_message, bool require_non_empty) { return checkedPathSize(path, error_message, require_non_empty); }

std::streamsize checkedStreamWriteSize(std::size_t size, std::string_view error_message) {
    if (size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) throw std::runtime_error(std::string(error_message));
    return static_cast<std::streamsize>(size);
}

void ensureStreamStateOrThrow(const std::ios& stream, std::string_view error_message) { if (!stream) throw std::runtime_error(std::string(error_message)); }

void flushOutputOrThrow(std::ostream& output, std::string_view error_message) { output.flush(); ensureStreamStateOrThrow(output, error_message); }

void closeOutputOrThrow(std::ofstream& output, std::string_view error_message) { output.close(); ensureStreamStateOrThrow(output, error_message); }

void writeBytesOrThrow(std::ostream& output, std::span<const Byte> bytes, std::string_view error_message) {
    if (bytes.empty()) return;
    output.write(reinterpret_cast<const char*>(bytes.data()), checkedStreamWriteSize(bytes.size(), error_message));
    ensureStreamStateOrThrow(output, error_message);
}

std::ifstream openBinaryInputOrThrow(const fs::path& path, std::string_view error_message) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error(std::string(error_message));
    return input;
}

std::ofstream openBinaryOutputForWriteOrThrow(const fs::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::out | std::ios::noreplace);
    if (!output) throw std::runtime_error("Write Error: Unable to create output file. Make sure you have WRITE permissions for this location.");
    restrictOutputPermissionsOrThrow(path, "Write Error: Failed to secure output file permissions");
    return output;
}

void requireNoTrailingDataOrThrow(std::istream& input, const char* error_message) {
    const auto next = input.peek();
    if (input.bad()) throw std::runtime_error(error_message);
    if (next != std::char_traits<char>::eof()) throw std::runtime_error(error_message);
}

std::streamsize readSomeOrThrow(std::istream& input, Byte* dst, std::size_t size, const char* error_message) {
    if (size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) throw std::runtime_error(error_message);
    input.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(size));
    const std::streamsize got = input.gcount();
    if (got < 0 || (!input && !input.eof())) throw std::runtime_error(error_message);
    return got;
}

void readExactOrThrow(std::istream& input, Byte* dst, std::size_t size, const char* error_message) {
    if (readSomeOrThrow(input, dst, size, error_message) != static_cast<std::streamsize>(size)) throw std::runtime_error(error_message);
}

bool tryReadExact(std::istream& input, Byte* dst, std::size_t size) {
    if (size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) return false;
    input.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(size));
    return input.gcount() == static_cast<std::streamsize>(size);
}

void cleanupPathNoThrow(const fs::path& path) noexcept { if (path.empty()) return; std::error_code ec; fs::remove(path, ec); }

bool tryCommitStagedFileNoReplace(const fs::path& staged_path, const fs::path& output_path, std::string_view error_message) {
    std::error_code ec;
    fs::create_hard_link(staged_path, output_path, ec);
    if (!ec) {
        cleanupPathNoThrow(staged_path);
        return true;
    }
    if (isFileExistsError(ec) && isExistingPathError(output_path)) return false;

    return copyFileNoReplace(staged_path, output_path, error_message);
}

void commitStagedFileNoReplaceOrThrow(const fs::path& staged_path, const fs::path& output_path, std::string_view error_message) {
    if (!tryCommitStagedFileNoReplace(staged_path, output_path, error_message)) {
        throw std::runtime_error(std::format("{}: output file already exists", error_message));
    }
}

namespace {
void validateFilenameProperties(const fs::path& path, FileTypeCheck FileType) {
    if (!hasValidFilename(path)) {
        throw std::runtime_error("Invalid Input Error: Unsupported control or path-separator characters in filename arguments.");
    }
    if (isImageType(FileType) && !hasFileExtension(path, {".jpg", ".jpeg", ".jfif"})) {
        throw std::runtime_error("File Type Error: Invalid image extension. Only expecting \".jpg\", \".jpeg\", \".jfif\".");
    }
}

void validateSizeAgainstType(std::size_t file_size, FileTypeCheck FileType) {
    if (!file_size) throw std::runtime_error("Error: File is empty.");
    if (FileType == FileTypeCheck::cover_image) {
        if (MINIMUM_IMAGE_SIZE > file_size) throw std::runtime_error("File Error: Invalid image file size.");
        if (file_size > MAX_IMAGE_SIZE) throw std::runtime_error("Image File Error: Cover image file exceeds maximum size limit.");
    }
    if (file_size > MAX_FILE_SIZE) throw std::runtime_error("Error: File exceeds program size limit.");
}

[[nodiscard]] std::size_t measureOpenedStreamSize(std::ifstream& stream, const fs::path& path) {
    stream.seekg(0, std::ios::end);
    const std::streamoff end_off = static_cast<std::streamoff>(stream.tellg());
    if (!stream || end_off < 0) {
        throw std::runtime_error(std::format("Error: Failed to measure size of \"{}\".", path.string()));
    }

    const auto end_raw = static_cast<std::uintmax_t>(end_off);
    if (end_raw > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
        end_raw > static_cast<std::uintmax_t>(MAX_FILENAME_STREAM_SIZE)) {
        throw std::runtime_error(std::format("Error: File \"{}\" exceeds addressable size.", path.string()));
    }

    stream.seekg(0, std::ios::beg);
    if (!stream) throw std::runtime_error(std::format("Error: Failed to rewind \"{}\".", path.string()));
    return static_cast<std::size_t>(end_raw);
}
}

std::size_t validateFileForRead(const fs::path& path, FileTypeCheck FileType) {
    validateFilenameProperties(path, FileType);

    std::error_code ec;
    const bool exists = fs::exists(path, ec), regular = exists && !ec && fs::is_regular_file(path, ec);
    if (ec || !exists || !regular) throw std::runtime_error(std::format("Error: File \"{}\" not found or not a regular file.", path.string()));

    const std::size_t file_size = safeFileSize(path);
    validateSizeAgainstType(file_size, FileType);
    return file_size;
}

vBytes readFile(const fs::path& path, FileTypeCheck FileType) {
    validateFilenameProperties(path, FileType);

    std::error_code ec;
    const bool exists = fs::exists(path, ec), regular = exists && !ec && fs::is_regular_file(path, ec);
    if (ec || !exists || !regular) throw std::runtime_error(std::format("Error: File \"{}\" not found or not a regular file.", path.string()));

    std::ifstream file = openBinaryInputOrThrow(path, std::format("Failed to open file: {}", path.string()));
    const std::size_t file_size = measureOpenedStreamSize(file, path);
    validateSizeAgainstType(file_size, FileType);

    vBytes vec(file_size);
    readExactOrThrow(file, vec.data(), vec.size(), "Failed to read full file: partial read");
    requireNoTrailingDataOrThrow(file, "Read Error: Input file changed while reading.");
    return vec;
}
