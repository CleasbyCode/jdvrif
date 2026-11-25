#include "file_utils.h"
#include "signal_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <format>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>

static_assert(sizeof(void*) == 8 && sizeof(std::size_t) == 8 && sizeof(off_t) >= 8,
              "jdvrif supports 64-bit targets only.");

namespace {
constexpr std::size_t MINIMUM_IMAGE_SIZE = 134;
constexpr std::size_t MAX_IMAGE_SIZE = 8 * 1024 * 1024;
constexpr std::size_t MAX_FILE_SIZE = 3uz * 1024 * 1024 * 1024;

// Rejects path separators, the Windows-reserved punctuation, C0 controls and DEL.
[[nodiscard]] bool isValidFilenameChar(unsigned char c) {
    constexpr std::string_view REJECTED = R"(/\:*?"<>|)";
    return c >= 0x20 && c != 0x7F && !REJECTED.contains(static_cast<char>(c));
}

[[nodiscard]] bool isReservedEmbeddedFilename(std::string_view filename) {
    return filename == "." || filename == ".." ||
           (!filename.empty() && (filename.front() == '.' || filename.front() == '-' ||
                                  filename.back() == ' ' || filename.back() == '.'));
}

// Embedded filenames must be well-formed UTF-8 (no overlongs, surrogates or
// code points past U+10FFFF) and free of C1 controls, default-ignorable code
// points (including bidi controls, joiners, variation selectors and tags), and
// non-ASCII whitespace. These can hide filename differences or rewrite a
// terminal display. Reject them rather than stripping or normalizing names.
// Ranges: Unicode 17.0.0 DerivedCoreProperties.txt (Default_Ignorable_Code_Point)
// and PropList.txt (White_Space), https://www.unicode.org/Public/17.0.0/ucd/.
// C0 controls and DEL are already refused byte-wise by isValidFilenameChar.
[[nodiscard]] bool isUnsafeCodePoint(char32_t cp) {
    return (cp >= 0x80 && cp <= 0x9F) ||
           cp == 0x00A0 || cp == 0x00AD || cp == 0x034F || cp == 0x061C ||
           (cp >= 0x115F && cp <= 0x1160) || cp == 0x1680 ||
           (cp >= 0x17B4 && cp <= 0x17B5) ||
           (cp >= 0x180B && cp <= 0x180F) ||
           (cp >= 0x2000 && cp <= 0x200F) ||
           (cp >= 0x2028 && cp <= 0x202F) ||
           (cp >= 0x205F && cp <= 0x206F) ||
           cp == 0x3000 || cp == 0x3164 ||
           (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF || cp == 0xFFA0 ||
           (cp >= 0xFFF0 && cp <= 0xFFF8) ||
           (cp >= 0x1BCA0 && cp <= 0x1BCA3) ||
           (cp >= 0x1D173 && cp <= 0x1D17A) ||
           (cp >= 0xE0000 && cp <= 0xE0FFF);
}

[[nodiscard]] bool hasSafeUnicode(std::string_view name) {
    std::size_t i = 0;
    while (i < name.size()) {
        const auto lead = static_cast<unsigned char>(name[i]);
        std::size_t length = 0;
        char32_t cp = 0;
        char32_t minimum = 0;
        if (lead < 0x80)                { i += 1; continue; }
        else if ((lead & 0xE0) == 0xC0) { length = 2; cp = lead & 0x1Fu; minimum = 0x80; }
        else if ((lead & 0xF0) == 0xE0) { length = 3; cp = lead & 0x0Fu; minimum = 0x800; }
        else if ((lead & 0xF8) == 0xF0) { length = 4; cp = lead & 0x07u; minimum = 0x10000; }
        else return false;

        if (name.size() - i < length) return false;
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(name[i + k]);
            if ((next & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (next & 0x3Fu);
        }
        if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        if (isUnsafeCodePoint(cp)) return false;
        i += length;
    }
    return true;
}

// Text for a terminal: unchanged when it is well-formed UTF-8 free of control
// characters, default-ignorables and non-ASCII whitespace; otherwise every byte
// outside printable ASCII is shown as \xNN, exposing hidden filename differences
// and preventing a crafted name from rewriting the terminal display.
[[nodiscard]] std::string escapeForDisplay(std::string_view text) {
    const bool printable = std::ranges::all_of(text, [](char ch) {
        const auto c = static_cast<unsigned char>(ch);
        return c >= 0x20 && c != 0x7F;
    });
    if (printable && hasSafeUnicode(text)) return std::string(text);

    std::string out;
    out.reserve(text.size() * 4);
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c >= 0x20 && c < 0x7F) {
            out.push_back(ch);
        } else {
            out += std::format("\\x{:02X}", c);
        }
    }
    return out;
}

[[nodiscard]] bool hasValidFilename(const fs::path& p) {
    if (p.empty()) return false;
    const std::string filename = p.filename().string();
    if (filename.empty()) return false;
    return std::ranges::all_of(filename, isValidFilenameChar);
}

[[nodiscard]] std::size_t safeFileSize(const fs::path& path) {
    std::error_code ec;
    const std::uintmax_t raw_file_size = fs::file_size(path, ec);
    throwIf(
        ec || raw_file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()),
        std::format("Error: Failed to get file size for \"{}\".", displaySafePath(path)));
    return static_cast<std::size_t>(raw_file_size);
}

[[nodiscard]] std::string randomPathToken(std::size_t token_hex_chars) {
    constexpr auto HEX_DIGITS = std::to_array<char>({
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'
    });

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
    throw std::runtime_error(std::format("Error: Failed to inspect path \"{}\": {}", displaySafePath(path), ec.message()));
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

// True when `ec` may hide the destination path already being taken. Callers
// handle EEXIST, which says so outright, first. EACCES/EPERM are included
// because a filesystem can refuse link(2) for reasons unrelated to the name
// (protected_hardlinks, a no-hardlink filesystem), and those must not be
// mistaken for "the name is free". Callers therefore treat this as "worth
// checking" and must confirm with pathEntryExists() before concluding the
// output already exists.
[[nodiscard]] bool mayIndicateExistingPath(const std::error_code& ec) {
    return ec == std::make_error_code(std::errc::permission_denied) ||
           ec == std::make_error_code(std::errc::operation_not_permitted);
}

// Create a new regular file with O_EXCL and mode 0600. The mode is applied at
// open(2) time (subject only to umask removing bits), so the file is never
// group/world-readable — unlike create-with-default-mode then chmod(0600).
// `access_mode` is O_WRONLY for plain output files; staging inodes ask for
// O_RDWR so the same descriptor can also be read back (see StagingFile::fd).
// The process signal handlers are installed without SA_RESTART, so a signal
// can fail a slow call -- open(2) or fsync(2) on FUSE/NFS, for one -- with
// EINTR. Let a cancellation through as SignalCancellation, and otherwise just
// make the call again. errno is left as the final attempt set it. Not for
// close(2): on Linux the descriptor is gone even when close reports EINTR.
template<class Call>
[[nodiscard]] int retryOnInterrupt(Call call) {
    while (true) {
        const int result = call();
        if (result >= 0 || errno != EINTR) return result;
        throwIfSignalCancellationRequested();
    }
}

[[nodiscard]] int openExclusiveOwnerOnlyFdOrThrow(const fs::path& path, std::string_view error_message, int access_mode = O_WRONLY) {
    const int fd = retryOnInterrupt([&] {
        return ::open(path.c_str(), access_mode | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    });
    throwIf(fd < 0, error_message);
    return fd;
}

constexpr const char* OUTPUT_CREATE_ERROR =
    "Write Error: Unable to create output file. Make sure you have WRITE permissions for this location.";

void requireReadableRegularFile(const fs::path& path) {
    std::error_code ec;
    const auto status = fs::status(path, ec);
    if (ec || !fs::is_regular_file(status)) {
        throw std::runtime_error(std::format("Error: File \"{}\" not found or not a regular file.", displaySafePath(path)));
    }
}
} // namespace

bool isSafeEmbeddedFilename(std::string_view name) noexcept {
    return !name.empty() &&
           std::ranges::all_of(name, isValidFilenameChar) &&
           !isReservedEmbeddedFilename(name) &&
           hasSafeUnicode(name);
}

bool hasSafeEmbeddedFilename(const fs::path& p) {
    return !p.empty() && isSafeEmbeddedFilename(p.filename().native());
}

std::string terminalSafeMessage(std::string_view message) {
    std::string out;
    out.reserve(message.size());
    std::size_t start = 0;
    while (true) {
        const std::size_t end = message.find('\n', start);
        out += escapeForDisplay(message.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) break;
        out.push_back('\n');
        start = end + 1;
    }
    return out;
}

std::string displaySafeFilename(const fs::path& p) {
    return escapeForDisplay(p.filename().string());
}

std::string displaySafePath(const fs::path& p) {
    return escapeForDisplay(p.string());
}

namespace {
[[nodiscard]] bool hasFileExtension(const fs::path& p, std::initializer_list<std::string_view> exts) {
    auto e = p.extension().string();
    std::ranges::transform(e, e.begin(), [](unsigned char c) static {
        return static_cast<char>(std::tolower(c));
    });
    return std::ranges::any_of(exts, [&e](std::string_view ext) { return e == ext; });
}
} // namespace

namespace {
[[nodiscard]] std::optional<fs::path> makeUniqueRandomizedPath(const fs::path& parent_dir, std::string_view prefix, std::string_view suffix, std::size_t max_attempts, std::size_t token_hex_chars) {
    if (max_attempts == 0 || token_hex_chars == 0) return std::nullopt;

    for (std::size_t i = 0; i < max_attempts; ++i) {
        // An empty parent_dir contributes no separator, leaving a bare filename.
        const fs::path candidate =
            parent_dir / std::format("{}{}{}", prefix, randomPathToken(token_hex_chars), suffix);
        if (!pathEntryExists(candidate)) return candidate;
    }

    return std::nullopt;
}
} // namespace

fs::path uniqueRandomizedPathOrThrow(const fs::path& parent_dir, std::string_view prefix, std::string_view suffix, std::size_t max_attempts, std::string_view error_message, std::size_t token_hex_chars) {
    if (auto path = makeUniqueRandomizedPath(parent_dir, prefix, suffix, max_attempts, token_hex_chars)) return *path;
    throwError(error_message);
}


namespace {
// Read-only open that refuses anything but a regular file, checked with fstat
// on the descriptor actually opened (so a path swapped for a FIFO after
// validation neither blocks the open nor gets read). Returns an O_CLOEXEC fd the
// caller owns.
[[nodiscard]] int openRegularFileFdOrThrow(const fs::path& path, std::string_view error_message) {
    // O_NONBLOCK so that a FIFO or device swapped in for a path that was
    // validated earlier cannot park us inside open(2); the fstat below then
    // rejects anything that is not a regular file, on the inode actually opened.
    // O_NOCTTY because that rejection comes after the open: a terminal swapped
    // in must not become a session leader's controlling terminal on the way.
    const int fd = retryOnInterrupt([&] { return ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY); });
    throwIf(fd < 0, error_message);

    struct stat status {};
    const int flags = ::fcntl(fd, F_GETFL);
    if (::fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || flags < 0 ||
        ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != 0) {
        ::close(fd);
        throwError(error_message);
    }
    return fd;
}
} // namespace

InputFile::InputFile(const fs::path& path, std::string_view error_message)
    : fd_(openRegularFileFdOrThrow(path, error_message)) {
    // Every reader goes front to back. Best-effort hint: prefetch ahead and
    // drop pages behind.
    (void)::posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
}

InputFile::~InputFile() noexcept {
    if (fd_ >= 0) ::close(fd_);
}

std::size_t InputFile::size(std::string_view error_message) const {
    struct stat status {};
    throwIf(::fstat(fd_, &status) != 0 || status.st_size < 0, error_message);
    return static_cast<std::size_t>(status.st_size);
}

std::size_t InputFile::readSome(std::span<Byte> out, std::string_view error_message) {
    std::size_t filled = 0;
    while (filled < out.size()) {
        throwIfSignalCancellationRequested();
        const ssize_t got = ::read(fd_, out.data() + filled, out.size() - filled);
        if (got < 0) {
            if (errno == EINTR) continue;
            throwError(error_message);
        }
        if (got == 0) break;
        filled += static_cast<std::size_t>(got);
    }
    throwIfSignalCancellationRequested();
    return filled;
}

void InputFile::readExact(std::span<Byte> out, std::string_view error_message) {
    throwIf(readSome(out, error_message) != out.size(), error_message);
}

void InputFile::requireEnd(std::string_view error_message) {
    std::array<Byte, 1> probe{};
    WipePodGuard<std::array<Byte, 1>> probe_wipe{probe};
    throwIf(readSome(probe, error_message) != 0, error_message);
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
    fd_ = retryOnInterrupt([&] { return ::open(dir.c_str(), O_RDWR | O_TMPFILE | O_CLOEXEC, 0600); });
    // Only an O_TMPFILE inode still has a link to spend (see can_link_).
    can_link_ = (fd_ >= 0);
    if (fd_ < 0) {
        fd_ = openUnlinkedNamedStagingFdOrThrow(dir, tag);
    }
    // The destructor does not run for a constructor that throws, so a failed
    // allocation here must close the descriptor itself.
    try {
        path_ = std::format("/proc/self/fd/{}", fd_);
    } catch (...) {
        ::close(fd_);
        throw;
    }
}

std::size_t StagingFile::size(std::string_view error_message) const {
    struct stat status {};
    throwIf(::fstat(fd_, &status) != 0 || status.st_size < 0, error_message);
    return static_cast<std::size_t>(status.st_size);
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
            if (errno == EINTR) continue;  // the loop head checks cancellation
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
            if (errno == EINTR) continue;  // the loop head checks cancellation
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
    // The range can be recovered plaintext (staging-file publication).
    WipeBytesGuard buffer_wipe{buffer};

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
            if (errno == EINTR) continue;  // the loop head checks cancellation
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

namespace {
// Closes `fd` (and sets it to -1), surfacing any write error -- including one
// the kernel only reports at close(2) (NFS, delayed-allocation ENOSPC/EDQUOT).
// With `durable`, fsync first so the bytes have reached stable storage.
void closeFdOrThrow(int& fd, std::string_view error_message, bool durable) {
    if (durable) {
        int rc = -1;
        try {
            rc = retryOnInterrupt([&] { return ::fsync(fd); });
        } catch (...) {
            ::close(std::exchange(fd, -1));
            throw;
        }
        if (rc != 0) {
            ::close(std::exchange(fd, -1));
            throwError(error_message);
        }
    }
    if (::close(std::exchange(fd, -1)) != 0) {
        if (errno == EINTR) throwIfSignalCancellationRequested();
        throwError(error_message);
    }
}
} // namespace

OutputFile::OutputFile(const StagingFile& staged, std::size_t buffer_capacity)
    : buffer_(buffer_capacity) {
    // The staging inode reached through /proc/self/fd: it already exists, so
    // exclusive create is neither possible nor needed. A fresh open (not a
    // dup) gives this writer its own file offset. (No O_NOFOLLOW: that path is
    // a magic link and only opens by being followed.)
    fd_ = retryOnInterrupt([&] { return ::open(staged.path().c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOCTTY); });
    throwIf(fd_ < 0, OUTPUT_CREATE_ERROR);
}

OutputFile::~OutputFile() noexcept {
    // The buffer may have held plaintext (see file_utils.h); clear it before
    // the vector releases it, whatever state the descriptor is in.
    const WipeBytesGuard buffer_wipe{buffer_};
    if (fd_ < 0) return;
    // Every success path calls close(), the only one that can report a write
    // failure. Reaching here with the descriptor open is therefore an error
    // path -- a failed authentication or inflate, a cancellation -- whose file
    // is discarded straight after, so whatever is still buffered is dropped,
    // not written: flushing it would only put more recovered plaintext on disk
    // for nothing.
    ::close(fd_);
}

void OutputFile::drain(std::string_view error_message) {
    if (fill_ == 0) return;
    // Empty the buffer before writing: a write that fails partway has already
    // landed part of it, so a later close() must not write it again from the
    // start (the destructor writes nothing).
    const std::size_t pending = std::exchange(fill_, 0);
    writeAllToFd(fd_, std::span<const Byte>(buffer_.data(), pending), error_message);
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

void OutputFile::close(std::string_view error_message, bool durable) {
    if (fd_ < 0) return;
    throwIfSignalCancellationRequested();
    drain(error_message);
    closeFdOrThrow(fd_, error_message, durable);
}

namespace {
// Best-effort fsync of the directory holding `path`, so a freshly created link
// survives a crash. Failure is not fatal: some filesystems refuse directory
// fsync, and the data itself is already durable by this point.
void syncParentDirectoryNoThrow(const fs::path& path) noexcept {
    const fs::path parent = path.parent_path();
    const int fd = ::open(parent.empty() ? "." : parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    // Best effort and noexcept: retry an interrupted sync, unless cancelling.
    while (::fsync(fd) != 0 && errno == EINTR && pendingSignalCancellation() == 0) {
    }
    (void)::close(fd);
}
} // namespace

void cleanupPathNoThrow(const fs::path& path) noexcept {
    if (path.empty()) return;

    std::error_code ec;
    fs::remove(path, ec);
}

namespace {
// Copy the whole staging inode into `path`, a brand new 0600 file, and fsync
// it. Returns false, creating nothing, when `path` is already taken. On any
// later failure the partial file is removed before the exception propagates.
[[nodiscard]] bool copyStagingFileToNewPath(const StagingFile& staged, const fs::path& path, std::string_view error_message) {
    const std::size_t staged_size = staged.size(error_message);

    int fd = -1;
    try {
        fd = openExclusiveOwnerOnlyFdOrThrow(path, OUTPUT_CREATE_ERROR);
    } catch (const SignalCancellation&) {
        throw;  // an interrupted open says nothing about the name
    } catch (const std::exception&) {
        if (pathEntryExists(path)) return false;
        throw std::runtime_error(std::format("{}: unable to create output file", error_message));
    }

    try {
        throwIfSignalCancellationRequested();
        sendFileRangeToFd(fd, staged.fd(), 0, staged_size, error_message);
        closeFdOrThrow(fd, error_message, /*durable=*/true);
    } catch (...) {
        if (fd >= 0) ::close(fd);
        cleanupPathNoThrow(path);
        throw;
    }
    return true;
}

// Copy the staged contents into a hidden private file in the destination's
// directory -- the same filesystem, so it can be renamed into place.
[[nodiscard]] fs::path copyStagingFileToPublishTemp(const StagingFile& staged, const fs::path& output_path, std::string_view error_message) {
    constexpr std::size_t MAX_ATTEMPTS = 1024;
    const std::string create_error = std::format("{}: unable to create temporary output file", error_message);
    for (std::size_t attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
        const fs::path temp_path = uniqueRandomizedPathOrThrow(
            output_path.parent_path(), ".jdvrif_publish_", ".tmp", MAX_ATTEMPTS, create_error);
        // false: another process took the name between the check and the create.
        if (copyStagingFileToNewPath(staged, temp_path, error_message)) return temp_path;
    }
    throwError(create_error);
}

enum class PublishOutcome { published, name_taken, unsupported };

// Give the finished temporary file the final name without ever replacing an
// existing entry: renameat2(RENAME_NOREPLACE), or link(2) + unlink(2) where the
// filesystem lacks that flag. `unsupported` means neither primitive works here.
[[nodiscard]] PublishOutcome publishTempNoReplace(const fs::path& temp_path, const fs::path& output_path, std::string_view error_message) {
    if (::renameat2(AT_FDCWD, temp_path.c_str(), AT_FDCWD, output_path.c_str(), RENAME_NOREPLACE) == 0) {
        return PublishOutcome::published;
    }
    const std::error_code rename_ec(errno, std::generic_category());
    if (rename_ec == std::errc::file_exists) return PublishOutcome::name_taken;
    if (mayIndicateExistingPath(rename_ec) && pathEntryExists(output_path)) return PublishOutcome::name_taken;
    if (rename_ec != std::errc::invalid_argument &&
        rename_ec != std::errc::function_not_supported &&
        rename_ec != std::errc::operation_not_supported) {
        throwError(error_message);
    }

    if (::link(temp_path.c_str(), output_path.c_str()) == 0) {
        // Published; the temporary name is now just a second link to drop.
        cleanupPathNoThrow(temp_path);
        return PublishOutcome::published;
    }
    const std::error_code link_ec(errno, std::generic_category());
    if (link_ec == std::errc::file_exists) return PublishOutcome::name_taken;
    if (mayIndicateExistingPath(link_ec) && pathEntryExists(output_path)) return PublishOutcome::name_taken;
    if (link_ec == std::errc::operation_not_permitted ||
        link_ec == std::errc::operation_not_supported ||
        link_ec == std::errc::function_not_supported ||
        link_ec == std::errc::too_many_links) {
        return PublishOutcome::unsupported;
    }
    throwError(error_message);
}

// Publish a staging inode that linkat(2) cannot name: it came from the
// create-and-unlink fallback, or linkat was refused for a reason that says
// nothing about the destination name (a filesystem without hard links).
//
// The copy goes to a hidden ".jdvrif_publish_*.tmp" file beside the output,
// is fsynced, and only then renamed into place without replacement, so no
// process ever sees a partial file under the final name. The tradeoff,
// whenever this path runs (no O_TMPFILE support, or an O_TMPFILE inode that
// linkat refused): a SIGKILL, crash or power loss mid-copy can leave that
// hidden 0600 temporary behind (ordinary errors and Ctrl+C remove it). It
// holds the same bytes the output would have held.
//
// Where the filesystem has neither RENAME_NOREPLACE nor hard links, there is no
// atomic no-replace primitive at all; the copy then goes straight to the final
// name with O_EXCL, as the only remaining way to publish.
[[nodiscard]] bool copyStagingFileNoReplace(const StagingFile& staged, const fs::path& output_path, std::string_view error_message) {
    // Cheap early out: no point copying for a name that is already taken.
    if (pathEntryExists(output_path)) return false;

    const fs::path temp_path = copyStagingFileToPublishTemp(staged, output_path, error_message);
    PublishOutcome outcome{};
    try {
        throwIfSignalCancellationRequested();
        outcome = publishTempNoReplace(temp_path, output_path, error_message);
    } catch (...) {
        cleanupPathNoThrow(temp_path);
        throw;
    }
    if (outcome == PublishOutcome::published) return true;

    cleanupPathNoThrow(temp_path);
    if (outcome == PublishOutcome::name_taken) return false;
    return copyStagingFileToNewPath(staged, output_path, error_message);
}
} // namespace

void syncStagingFileOrThrow(const StagingFile& staged, std::string_view error_message) {
    throwIfSignalCancellationRequested();
    throwIf(retryOnInterrupt([&] { return ::fsync(staged.fd()); }) != 0, error_message);
}

bool tryCommitStagingFileNoReplace(const StagingFile& staged, const fs::path& output_path, std::string_view error_message) {
    throwIfSignalCancellationRequested();

    if (staged.can_link_) {
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
    if (file_type == FileTypeCheck::cover_image ||
        file_type == FileTypeCheck::reddit_cover_image ||
        file_type == FileTypeCheck::twitter_cover_image) {
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
    // A cover image has a floor, and each mode its own ceiling.
    std::size_t max_cover_size = 0;
    const char* cover_too_large = nullptr;
    switch (file_type) {
        case FileTypeCheck::cover_image:
            max_cover_size = MAX_IMAGE_SIZE;
            cover_too_large = "Image File Error: Cover image file exceeds maximum size limit.";
            break;
        case FileTypeCheck::reddit_cover_image:
            max_cover_size = REDDIT_UPLOAD_SIZE_LIMIT;
            cover_too_large = "Image File Size Error: Cover image exceeds Reddit's 20 MiB upload size limit.";
            break;
        case FileTypeCheck::twitter_cover_image:
            max_cover_size = TWITTER_UPLOAD_SIZE_LIMIT;
            cover_too_large = "Image File Size Error: Cover image exceeds X-Twitter's 5 MiB upload size limit.";
            break;
        default:
            break;
    }
    if (cover_too_large != nullptr) {
        if (MINIMUM_IMAGE_SIZE > file_size) throw std::runtime_error("File Error: Invalid image file size.");
        if (file_size > max_cover_size) throw std::runtime_error(cover_too_large);
    }
    if (file_size > MAX_FILE_SIZE) throw std::runtime_error("Error: File exceeds program size limit.");
}

} // namespace

std::size_t validateFileForRead(const fs::path& path, FileTypeCheck file_type) {
    validateFilenameProperties(path, file_type);
    requireReadableRegularFile(path);

    const std::size_t file_size = safeFileSize(path);
    validateSizeAgainstType(file_size, file_type);
    return file_size;
}

vBytes readFile(const fs::path& path, FileTypeCheck file_type, std::size_t max_bytes) {
    validateFilenameProperties(path, file_type);
    requireReadableRegularFile(path);

    InputFile file(path, std::format("Failed to open file: {}", displaySafePath(path)));
    const std::size_t file_size = file.size(std::format("Error: Failed to measure size of \"{}\".", displaySafePath(path)));
    validateSizeAgainstType(file_size, file_type);
    throwIf(file_size > max_bytes, "Read Error: Input file exceeds the permitted size limit.");

    vBytes vec(file_size);
    file.readExact(vec, "Failed to read full file: partial read");
    file.requireEnd("Read Error: Input file changed while reading.");
    return vec;
}
