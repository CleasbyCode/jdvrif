#include "file_utils.h"
#include "signal_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <format>
#include <fstream>
#include <ios>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <system_error>
#include <version>

#include <fcntl.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>

static_assert(sizeof(void*) == 8 && sizeof(std::size_t) == 8 && sizeof(off_t) >= 8,
              "jdvrif supports 64-bit targets only.");

#ifndef __cpp_lib_ios_noreplace
#error "jdvrif requires std::ios::noreplace for secure output file creation."
#endif

static_assert(__cpp_lib_ios_noreplace >= 202207L, "jdvrif requires std::ios::noreplace for secure output file creation.");

namespace {
constexpr std::size_t MINIMUM_IMAGE_SIZE = 134;
constexpr std::size_t MAX_IMAGE_SIZE = 8 * 1024 * 1024;
constexpr std::size_t MAX_FILE_SIZE = 3ULL * 1024 * 1024 * 1024;
constexpr std::size_t MAX_FILENAME_STREAM_SIZE =
    static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max());

// Rejects path separators, the Windows-reserved punctuation, C0 controls and DEL.
[[nodiscard]] bool isValidFilenameChar(unsigned char c) {
    constexpr std::string_view REJECTED = R"(/\:*?"<>|)";
    return c >= 0x20 && c != 0x7F && REJECTED.find(static_cast<char>(c)) == std::string_view::npos;
}

[[nodiscard]] bool isReservedEmbeddedFilename(std::string_view filename) {
    return filename == "." || filename == ".." ||
           (!filename.empty() && (filename.front() == '.' || filename.front() == '-' ||
                                  filename.back() == ' ' || filename.back() == '.'));
}

[[nodiscard]] bool hasValidFilename(const fs::path& p) {
    if (p.empty()) return false;
    const std::string filename = p.filename().string();
    if (filename.empty()) return false;
    return std::ranges::all_of(filename, isValidFilenameChar);
}

[[nodiscard]] std::streamsize checkedStreamWriteSize(std::size_t size, std::string_view error_message) {
    throwIf(size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()), error_message);
    return static_cast<std::streamsize>(size);
}

[[nodiscard]] std::size_t checkedPathSize(const fs::path& path, std::string_view error_message, bool require_non_empty = false, bool require_stream_sized = false) {
    std::error_code ec;
    const std::uintmax_t raw_file_size = fs::file_size(path, ec);
    throwIf(
        ec || raw_file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()) ||
            (require_stream_sized && raw_file_size > static_cast<std::uintmax_t>(MAX_FILENAME_STREAM_SIZE)),
        error_message);
    const std::size_t file_size = static_cast<std::size_t>(raw_file_size);
    throwIf(require_non_empty && file_size == 0, error_message);
    return file_size;
}

[[nodiscard]] std::size_t safeFileSize(const fs::path& path) {
    return checkedPathSize(
        path,
        std::format("Error: Failed to get file size for \"{}\".", path.string()),
        false,
        true);
}

[[nodiscard]] std::string randomPathToken(std::size_t token_hex_chars) {
    constexpr auto HEX_DIGITS = std::to_array<char>({
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'
    });

    if (token_hex_chars == 0) return {};

    constexpr std::size_t MAX_TOKEN_BYTES = 32;
    const std::size_t byte_count = (token_hex_chars + 1) / 2;
    if (byte_count > MAX_TOKEN_BYTES) {
        throw std::out_of_range(std::format("randomPathToken: token exceeds maximum of {} hex characters", MAX_TOKEN_BYTES * 2));
    }

    std::array<Byte, MAX_TOKEN_BYTES> random_bytes;
    randombytes_buf(random_bytes.data(), byte_count);

    std::string token(token_hex_chars, '\0');
    std::size_t token_index = 0;
    for (std::size_t i = 0; i < byte_count; ++i) {
        token[token_index++] = HEX_DIGITS[random_bytes[i] >> 4];
        if (token_index == token_hex_chars) break;
        token[token_index++] = HEX_DIGITS[random_bytes[i] & 0x0F];
    }
    return token;
}

[[noreturn]] void throwPathInspectError(const fs::path& path, const std::error_code& ec) {
    throw std::runtime_error(std::format("Error: Failed to inspect path \"{}\": {}", path.string(), ec.message()));
}

[[nodiscard]] bool pathEntryExists(const fs::path& path) {
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory) return false;
        throwPathInspectError(path, ec);
    }
    return status.type() != fs::file_type::not_found;
}

// True when `ec` is consistent with the destination path already being taken.
// EEXIST says so outright; EACCES/EPERM are included because a filesystem can
// refuse link(2) for reasons unrelated to the name (protected_hardlinks, a
// no-hardlink filesystem), and those must not be mistaken for "the name is
// free". Callers therefore treat this as "worth checking" and must confirm with
// pathEntryExists() before concluding the output already exists.
[[nodiscard]] bool mayIndicateExistingPath(const std::error_code& ec) {
    return ec == std::make_error_code(std::errc::file_exists) ||
           ec == std::make_error_code(std::errc::permission_denied);
}

// Create a new regular file with O_EXCL and mode 0600. The mode is applied at
// open(2) time (subject only to umask removing bits), so the file is never
// group/world-readable — unlike create-with-default-mode then chmod(0600).
// `access_mode` is O_WRONLY for plain output files; staging inodes ask for
// O_RDWR so the same descriptor can also be read back (see StagingFile::fd).
[[nodiscard]] int openExclusiveOwnerOnlyFdOrThrow(const fs::path& path, std::string_view error_message, int access_mode = O_WRONLY) {
    const int fd = ::open(path.c_str(), access_mode | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    throwIf(fd < 0, error_message);
    return fd;
}

constexpr const char* OUTPUT_CREATE_ERROR =
    "Write Error: Unable to create output file. Make sure you have WRITE permissions for this location.";

// Adopt an already-opened O_WRONLY fd into an ofstream without re-opening the
// path (path re-open would reintroduce a replace TOCTOU). Uses /proc/self/fd so
// the stream writes to the same inode created with mode 0600. Linux-only, same
// as sendfile / O_CLOEXEC elsewhere in this file.
[[nodiscard]] std::ofstream ofstreamFromExclusiveFdOrThrow(int fd, const fs::path& path) {
    std::ofstream output;
    try {
        output.open(std::format("/proc/self/fd/{}", fd), std::ios::binary | std::ios::out);
    } catch (...) {
        ::close(fd);
        cleanupPathNoThrow(path);
        throw;
    }

    // ofstream holds its own fd (via the /proc open); drop ours either way.
    ::close(fd);

    if (!output) {
        cleanupPathNoThrow(path);
        throw std::runtime_error(OUTPUT_CREATE_ERROR);
    }
    return output;
}

[[nodiscard]] bool copyFileNoReplace(const fs::path& source_path, const fs::path& output_path, std::string_view error_message) {
    std::ifstream input = openBinaryInputOrThrow(source_path, std::format("{}: failed to reopen staged file", error_message));

    // Same exclusive 0600 create path as openBinaryOutputForWriteOrThrow — no
    // create-then-chmod window on the committed output either.
    std::ofstream output;
    try {
        output = openBinaryOutputForWriteOrThrow(output_path);
    } catch (const std::exception&) {
        if (pathEntryExists(output_path)) return false;
        throw std::runtime_error(std::format("{}: unable to create output file", error_message));
    }

    try {
        constexpr std::size_t COPY_CHUNK_SIZE = 2 * 1024 * 1024;
        vBytes buffer(COPY_CHUNK_SIZE);
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
        // The staged original was already fsynced; keep the committed copy just
        // as durable before the caller announces success.
        syncPathDataOrThrow(output_path, error_message);
    } catch (...) {
        cleanupPathNoThrow(output_path);
        throw;
    }

    cleanupPathNoThrow(source_path);
    return true;
}

void requireReadableRegularFile(const fs::path& path) {
    std::error_code ec;
    const auto status = fs::status(path, ec);
    if (ec || !fs::is_regular_file(status)) {
        throw std::runtime_error(std::format("Error: File \"{}\" not found or not a regular file.", path.string()));
    }
}
} // namespace

bool hasSafeEmbeddedFilename(const fs::path& p) {
    return hasValidFilename(p) && !isReservedEmbeddedFilename(p.filename().string());
}

bool hasFileExtension(const fs::path& p, std::initializer_list<std::string_view> exts) {
    auto e = p.extension().string();
    std::ranges::transform(e, e.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return std::ranges::any_of(exts, [&e](std::string_view ext) { return e == ext; });
}

std::optional<fs::path> makeUniqueRandomizedPath(const fs::path& parent_dir, std::string_view prefix, std::string_view suffix, std::size_t max_attempts, std::size_t token_hex_chars) {
    if (max_attempts == 0 || token_hex_chars == 0) return std::nullopt;

    for (std::size_t i = 0; i < max_attempts; ++i) {
        // An empty parent_dir contributes no separator, leaving a bare filename.
        const fs::path candidate =
            parent_dir / std::format("{}{}{}", prefix, randomPathToken(token_hex_chars), suffix);
        if (!pathEntryExists(candidate)) return candidate;
    }

    return std::nullopt;
}

fs::path uniqueRandomizedPathOrThrow(const fs::path& parent_dir, std::string_view prefix, std::string_view suffix, std::size_t max_attempts, std::string_view error_message, std::size_t token_hex_chars) {
    if (auto path = makeUniqueRandomizedPath(parent_dir, prefix, suffix, max_attempts, token_hex_chars)) return *path;
    throwError(error_message);
}

std::size_t checkedFileSize(const fs::path& path, std::string_view error_message, bool require_non_empty) {
    return checkedPathSize(path, error_message, require_non_empty);
}

void ensureStreamStateOrThrow(const std::ios& stream, std::string_view error_message) {
    throwIf(!stream, error_message);
}

void closeOutputOrThrow(std::ofstream& output, std::string_view error_message) {
    throwIfSignalCancellationRequested();
    output.close();
    throwIfSignalCancellationRequested();
    ensureStreamStateOrThrow(output, error_message);
}

void writeBytesOrThrow(std::ostream& output, std::span<const Byte> bytes, std::string_view error_message) {
    if (bytes.empty()) return;
    throwIfSignalCancellationRequested();
    output.write(reinterpret_cast<const char*>(bytes.data()), checkedStreamWriteSize(bytes.size(), error_message));
    throwIfSignalCancellationRequested();
    ensureStreamStateOrThrow(output, error_message);
}

std::ifstream openBinaryInputOrThrow(const fs::path& path, std::string_view error_message) {
    std::ifstream input(path, std::ios::binary);
    throwIf(!input, error_message);
    return input;
}

std::ofstream openBinaryOutputForWriteOrThrow(const fs::path& path) {
    // Atomic exclusive create at mode 0600 (see openExclusiveOwnerOnlyFdOrThrow),
    // then bind ofstream to that inode. Do not use ofstream+noreplace alone: the
    // iostreams create path uses mode 0666&~umask, then a later chmod leaves a
    // window where compressed/encrypted/plaintext temps can be group/world readable.
    const int fd = openExclusiveOwnerOnlyFdOrThrow(path, OUTPUT_CREATE_ERROR);
    return ofstreamFromExclusiveFdOrThrow(fd, path);
}

std::ofstream openBinaryOutputForStagingOrThrow(const fs::path& path) {
    // The StagingFile inode already exists and carries no directory entry, so
    // there is no name to race over and nothing to clean up on failure; the
    // exclusive-create helper would simply fail with EEXIST here.
    std::ofstream output(path, std::ios::binary | std::ios::out | std::ios::trunc);
    throwIf(!output, OUTPUT_CREATE_ERROR);
    return output;
}

namespace {
constexpr const char* STAGING_CREATE_ERROR =
    "Write Error: Unable to create a private staging file. Make sure you have WRITE permissions for this location.";

// Named-and-immediately-unlinked fallback for filesystems that reject
// O_TMPFILE. The name exists only between open(2) and unlink(2); after that the
// inode is link-free, exactly as in the O_TMPFILE case.
[[nodiscard]] int openUnlinkedNamedStagingFdOrThrow(const fs::path& parent_dir, std::string_view tag) {
    constexpr std::size_t MAX_ATTEMPTS = 1024;

    const fs::path staging_path = uniqueRandomizedPathOrThrow(
        parent_dir,
        std::format(".jdvrif_{}_", tag),
        ".tmp",
        MAX_ATTEMPTS,
        STAGING_CREATE_ERROR);

    // O_RDWR to match the O_TMPFILE case: StagingFile::fd() must be readable
    // either way, because the commit path may have to copy the inode out
    // through it (a link-free file created this way can never be re-linked).
    const int fd = openExclusiveOwnerOnlyFdOrThrow(staging_path, STAGING_CREATE_ERROR, O_RDWR);
    std::error_code ec;
    fs::remove(staging_path, ec);
    if (ec) {
        // Refuse to proceed with a file we cannot detach: leaving a named
        // plaintext temporary behind is the exact exposure this class exists
        // to remove.
        ::close(fd);
        cleanupPathNoThrow(staging_path);
        throw std::runtime_error(STAGING_CREATE_ERROR);
    }
    return fd;
}
} // namespace

StagingFile::StagingFile(const fs::path& parent_dir, std::string_view tag) {
    const fs::path dir = parent_dir.empty() ? fs::path(".") : parent_dir;

    // O_RDWR so the /proc/self/fd reopen is permitted in either direction.
    fd_ = ::open(dir.c_str(), O_RDWR | O_TMPFILE | O_CLOEXEC, 0600);
    // Only an O_TMPFILE inode still has a link to spend (see canLink()).
    can_link_ = (fd_ >= 0);
    if (fd_ < 0) {
        fd_ = openUnlinkedNamedStagingFdOrThrow(dir, tag);
    }
    path_ = std::format("/proc/self/fd/{}", fd_);
}

StagingFile::~StagingFile() noexcept {
    // Last reference to a link-free inode: closing it releases the blocks.
    if (fd_ >= 0) ::close(fd_);
}

void preadExactFromFd(int fd, std::span<Byte> dst, std::size_t offset, std::string_view error_message) {
    Byte*       p    = dst.data();
    std::size_t left = dst.size();
    off_t       off  = static_cast<off_t>(offset);
    while (left > 0) {
        throwIfSignalCancellationRequested();
        const ssize_t got = ::pread(fd, p, left, off);
        if (got < 0) {
            if (errno == EINTR) {
                throwIfSignalCancellationRequested();
                continue;
            }
            throwError(error_message);
        }
        throwIf(got == 0, error_message);   // unexpected EOF
        p    += got;
        left -= static_cast<std::size_t>(got);
        off  += got;
    }
}

void writeAllToFd(int fd, std::span<const Byte> bytes, std::string_view error_message) {
    const Byte* data = bytes.data();
    std::size_t left = bytes.size();
    while (left > 0) {
        throwIfSignalCancellationRequested();
        const ssize_t got = ::write(fd, data, left);
        if (got < 0) {
            if (errno == EINTR) {
                throwIfSignalCancellationRequested();
                continue;
            }
            throwError(error_message);
        }
        throwIf(got == 0, error_message);
        data += got;
        left -= static_cast<std::size_t>(got);
    }
}

namespace {
void sendRangeFallbackFdToFd(int out_fd, int in_fd, std::size_t in_offset, std::size_t length, std::string_view error_message) {
    constexpr std::size_t FALLBACK_CHUNK_SIZE = 1 * 1024 * 1024;
    vBytes buffer(std::min(length, FALLBACK_CHUNK_SIZE));

    std::size_t off = in_offset;
    std::size_t left = length;
    while (left > 0) {
        const std::size_t want = std::min(left, buffer.size());
        preadExactFromFd(in_fd, std::span<Byte>(buffer.data(), want), off, error_message);
        writeAllToFd(out_fd, std::span<const Byte>(buffer.data(), want), error_message);
        off  += want;
        left -= want;
    }
}
} // namespace

void sendFileRangeToFd(int out_fd, int in_fd, std::size_t in_offset, std::size_t length, std::string_view error_message) {
    off_t offset = static_cast<off_t>(in_offset);
    std::size_t left = length;
    while (left > 0) {
        throwIfSignalCancellationRequested();
        constexpr std::size_t SENDFILE_MAX = 0x7ffff000;  // Linux per-call cap (~2 GiB)
        const std::size_t want = std::min(left, SENDFILE_MAX);
        const ssize_t n = ::sendfile(out_fd, in_fd, &offset, want);
        if (n < 0) {
            if (errno == EINTR) {
                throwIfSignalCancellationRequested();
                continue;
            }
            if ((errno == EINVAL || errno == ENOSYS) && left == length) {
                // sendfile unsupported here (e.g. some virtual filesystems):
                // copy the whole region the slow way instead.
                sendRangeFallbackFdToFd(out_fd, in_fd, in_offset, length, error_message);
                return;
            }
            throwError(error_message);
        }
        if (n == 0) throwError(error_message);  // unexpected EOF
        left -= static_cast<std::size_t>(n);
    }
}

OutputFile::OutputFile(const fs::path& path, std::size_t buffer_capacity, OutputCreate create)
    : buffer_(buffer_capacity) {
    if (create == OutputCreate::existing_staging) {
        // A StagingFile inode reached through /proc/self/fd: it already exists,
        // so exclusive create is neither possible nor needed.
        fd_ = ::open(path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
        throwIf(fd_ < 0, OUTPUT_CREATE_ERROR);
        return;
    }
    // Same exclusive owner-only create as openBinaryOutputForWriteOrThrow.
    fd_ = openExclusiveOwnerOnlyFdOrThrow(path, OUTPUT_CREATE_ERROR);
}

OutputFile::~OutputFile() noexcept {
    if (fd_ >= 0) ::close(fd_);
}

void OutputFile::drain(std::string_view error_message) {
    if (fill_ == 0) return;
    writeAllToFd(fd_, std::span<const Byte>(buffer_.data(), fill_), error_message);
    fill_ = 0;
}

void OutputFile::write(std::span<const Byte> bytes, std::string_view error_message) {
    if (bytes.empty()) return;
    throwIfSignalCancellationRequested();
    if (bytes.size() >= buffer_.size()) {
        drain(error_message);
        writeAllToFd(fd_, bytes, error_message);
        return;
    }
    if (fill_ + bytes.size() > buffer_.size()) {
        drain(error_message);
    }
    std::memcpy(buffer_.data() + fill_, bytes.data(), bytes.size());
    fill_ += bytes.size();
}

void OutputFile::sendFrom(int in_fd, std::size_t in_offset, std::size_t length, std::string_view error_message) {
    throwIfSignalCancellationRequested();
    drain(error_message);  // buffered bytes must land before the sendfile region
    sendFileRangeToFd(fd_, in_fd, in_offset, length, error_message);
}

void OutputFile::close(std::string_view error_message, bool durable) {
    if (fd_ < 0) return;
    throwIfSignalCancellationRequested();
    drain(error_message);
    if (durable && ::fsync(fd_) != 0) {
        ::close(fd_);
        fd_ = -1;
        throwError(error_message);
    }
    const int rc = ::close(fd_);
    fd_ = -1;
    if (rc != 0) throwError(error_message);
}

void syncPathDataOrThrow(const fs::path& path, std::string_view error_message) {
    throwIfSignalCancellationRequested();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    throwIf(fd < 0, error_message);
    const int rc = ::fsync(fd);
    const int close_rc = ::close(fd);
    throwIf(rc != 0 || close_rc != 0, error_message);
}

void syncParentDirectoryNoThrow(const fs::path& path) noexcept {
    const fs::path parent = path.parent_path();
    const int fd = ::open(parent.empty() ? "." : parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    (void)::fsync(fd);
    (void)::close(fd);
}

void requireNoTrailingDataOrThrow(std::istream& input, const char* error_message) {
    const auto next = input.peek();
    throwIf(input.bad(), error_message);
    throwIf(next != std::char_traits<char>::eof(), error_message);
}

std::streamsize readSomeOrThrow(std::istream& input, Byte* dst, std::size_t size, const char* error_message) {
    throwIf(size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()), error_message);
    throwIfSignalCancellationRequested();
    input.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(size));
    throwIfSignalCancellationRequested();
    const std::streamsize got = input.gcount();
    throwIf(got < 0 || (!input && !input.eof()), error_message);
    return got;
}

void readExactOrThrow(std::istream& input, Byte* dst, std::size_t size, const char* error_message) {
    throwIf(readSomeOrThrow(input, dst, size, error_message) != static_cast<std::streamsize>(size), error_message);
}

bool tryReadExact(std::istream& input, Byte* dst, std::size_t size) {
    if (size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) return false;
    throwIfSignalCancellationRequested();
    input.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(size));
    throwIfSignalCancellationRequested();
    return input.gcount() == static_cast<std::streamsize>(size);
}

void cleanupPathNoThrow(const fs::path& path) noexcept {
    if (path.empty()) return;

    std::error_code ec;
    fs::remove(path, ec);
}

bool tryCommitStagedFileNoReplace(const fs::path& staged_path, const fs::path& output_path, std::string_view error_message) {
    throwIfSignalCancellationRequested();
    std::error_code ec;
    fs::create_hard_link(staged_path, output_path, ec);
    if (!ec) {
        cleanupPathNoThrow(staged_path);
        // Persist the new directory entry: the file's contents are already
        // fsynced, but the link naming them is not until the directory is.
        syncParentDirectoryNoThrow(output_path);
        return true;
    }
    if (mayIndicateExistingPath(ec) && pathEntryExists(output_path)) return false;

    if (!copyFileNoReplace(staged_path, output_path, error_message)) return false;
    syncParentDirectoryNoThrow(output_path);
    return true;
}

void commitStagedFileNoReplaceOrThrow(const fs::path& staged_path, const fs::path& output_path, std::string_view error_message) {
    if (!tryCommitStagedFileNoReplace(staged_path, output_path, error_message)) {
        throw std::runtime_error(std::format("{}: output file already exists", error_message));
    }
}

namespace {
constexpr std::size_t STAGING_COPY_BUFFER = 1 * 1024 * 1024;

[[nodiscard]] std::size_t stagingFileSizeOrThrow(int fd, std::string_view error_message) {
    struct stat status {};
    throwIf(::fstat(fd, &status) != 0 || status.st_size < 0, error_message);
    return static_cast<std::size_t>(status.st_size);
}

// Copy a link-free staging inode out to a brand new 0600 file. Used when the
// inode cannot be linked at all (create-and-unlink fallback), or when linkat is
// refused for a reason that says nothing about the destination name (a
// filesystem without hard links, a cross-device path).
[[nodiscard]] bool copyStagingFileNoReplace(const StagingFile& staged, const fs::path& output_path, std::string_view error_message) {
    const std::size_t staged_size = stagingFileSizeOrThrow(staged.fd(), error_message);

    std::optional<OutputFile> output;
    try {
        output.emplace(output_path, STAGING_COPY_BUFFER, OutputCreate::exclusive_new);
    } catch (const std::exception&) {
        if (pathEntryExists(output_path)) return false;
        throw std::runtime_error(std::format("{}: unable to create output file", error_message));
    }

    try {
        if (staged_size > 0) {
            output->sendFrom(staged.fd(), 0, staged_size, error_message);
        }
        output->close(error_message, /*durable=*/true);
    } catch (...) {
        output.reset();
        cleanupPathNoThrow(output_path);
        throw;
    }
    return true;
}
} // namespace

void syncStagingFileOrThrow(const StagingFile& staged, std::string_view error_message) {
    throwIfSignalCancellationRequested();
    throwIf(::fsync(staged.fd()) != 0, error_message);
}

bool tryCommitStagingFileNoReplace(const StagingFile& staged, const fs::path& output_path, std::string_view error_message) {
    throwIfSignalCancellationRequested();

    if (staged.canLink()) {
        // AT_SYMLINK_FOLLOW resolves the /proc/self/fd magic link, which is the
        // supported way to give an O_TMPFILE inode its first name. link(2) --
        // and therefore fs::create_hard_link -- cannot do this, which is why
        // the decrypted payload used to need a named temporary instead.
        if (::linkat(AT_FDCWD, staged.path().c_str(), AT_FDCWD, output_path.c_str(), AT_SYMLINK_FOLLOW) == 0) {
            // Contents are already fsynced; persist the directory entry naming them.
            syncParentDirectoryNoThrow(output_path);
            return true;
        }
        const std::error_code link_ec(errno, std::generic_category());
        if (link_ec == std::errc::file_exists) return false;
        if (mayIndicateExistingPath(link_ec) && pathEntryExists(output_path)) return false;
        // Anything else says nothing about the name: fall through and copy.
    }

    if (!copyStagingFileNoReplace(staged, output_path, error_message)) return false;
    syncParentDirectoryNoThrow(output_path);
    return true;
}

namespace {
void validateFilenameProperties(const fs::path& path, FileTypeCheck file_type) {
    throwIf(
        !hasValidFilename(path),
        "Invalid Input Error: Unsupported control or path-separator characters in filename arguments.");
    if (file_type == FileTypeCheck::cover_image) {
        // Conceal cover image: JPEG variants only.
        throwIf(
            !hasFileExtension(path, {".jpg", ".jpeg", ".jfif"}),
            "File Type Error: Invalid image extension. Only expecting \".jpg\", \".jpeg\", \".jfif\".");
    } else if (file_type == FileTypeCheck::embedded_image) {
        // Recovery also accepts ".png": recover only scans the file's bytes for the
        // embedded signature, so a jdvrif image served/renamed as ".png" can still
        // be recovered.
        throwIf(
            !hasFileExtension(path, {".jpg", ".jpeg", ".jfif", ".png"}),
            "File Type Error: Invalid image extension. Only expecting \".jpg\", \".jpeg\", \".jfif\" or \".png\".");
    }
}

void validateSizeAgainstType(std::size_t file_size, FileTypeCheck file_type) {
    if (!file_size) throw std::runtime_error("Error: File is empty.");
    if (file_type == FileTypeCheck::cover_image) {
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
    if (!stream) {
        throw std::runtime_error(std::format("Error: Failed to rewind \"{}\".", path.string()));
    }
    return static_cast<std::size_t>(end_raw);
}
} // namespace

std::size_t validateFileForRead(const fs::path& path, FileTypeCheck file_type) {
    validateFilenameProperties(path, file_type);
    requireReadableRegularFile(path);

    const std::size_t file_size = safeFileSize(path);
    validateSizeAgainstType(file_size, file_type);
    return file_size;
}

vBytes readFile(const fs::path& path, FileTypeCheck file_type) {
    validateFilenameProperties(path, file_type);
    requireReadableRegularFile(path);

    std::ifstream file = openBinaryInputOrThrow(path, std::format("Failed to open file: {}", path.string()));
    const std::size_t file_size = measureOpenedStreamSize(file, path);
    validateSizeAgainstType(file_size, file_type);

    vBytes vec(file_size);
    readExactOrThrow(file, vec.data(), vec.size(), "Failed to read full file: partial read");
    requireNoTrailingDataOrThrow(file, "Read Error: Input file changed while reading.");
    return vec;
}
