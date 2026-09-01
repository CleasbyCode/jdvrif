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
    throwIf(!spanHasRange(data, index, length), error_message);
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

inline constexpr const char* WRITE_COMPLETE_ERROR = "Write Error: Failed to write complete output file.";

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
// Truncating write-open of a file that already exists -- specifically a
// StagingFile addressed through /proc/self/fd, where the inode is created up
// front and the exclusive-create helper above would fail with EEXIST.
[[nodiscard]] std::ofstream openBinaryOutputForStagingOrThrow(const fs::path& path);
void requireNoTrailingDataOrThrow(std::istream& input, const char* error_message);
[[nodiscard]] std::streamsize readSomeOrThrow(std::istream& input, Byte* dst, std::size_t size, const char* error_message);
void readExactOrThrow(std::istream& input, Byte* dst, std::size_t size, const char* error_message);
[[nodiscard]] bool tryReadExact(std::istream& input, Byte* dst, std::size_t size);
void cleanupPathNoThrow(const fs::path& path) noexcept;
// Force the file's data+metadata to stable storage. Opens the path read-only
// (fsync(2) is permitted on an O_RDONLY descriptor on Linux) so it can be used
// after an ofstream that has already been closed. Call before announcing that a
// file is safely written.
void syncPathDataOrThrow(const fs::path& path, std::string_view error_message);
// Best-effort fsync of the directory holding `path`, so a freshly created link
// survives a crash. Failure is not fatal: some filesystems refuse directory
// fsync, and the data itself is already durable by this point.
void syncParentDirectoryNoThrow(const fs::path& path) noexcept;
void writeAllToFd(int fd, std::span<const Byte> bytes, std::string_view error_message);
// Fill `dst` from `fd` at `offset`, retrying short reads; a short read that hits
// EOF is an error, not a partial result.
void preadExactFromFd(int fd, std::span<Byte> dst, std::size_t offset, std::string_view error_message);
// Kernel-to-kernel copy of [in_offset, in_offset + length) from in_fd to
// out_fd's current position via sendfile(2), capped per call at the Linux
// limit. If the kernel/filesystem rejects sendfile outright (EINVAL/ENOSYS on
// the first call), falls back to a pread/write copy loop.
void sendFileRangeToFd(int out_fd, int in_fd, std::size_t in_offset, std::size_t length, std::string_view error_message);
[[nodiscard]] bool tryCommitStagedFileNoReplace(const fs::path& staged_path, const fs::path& output_path, std::string_view error_message);
void commitStagedFileNoReplaceOrThrow(const fs::path& staged_path, const fs::path& output_path, std::string_view error_message);

class StagingFile;
// Give a link-free staging inode its first name, without the contents ever
// existing under any other name. Returns false when `output_path` is already
// taken (so the caller can try the next candidate); throws on real I/O failure.
[[nodiscard]] bool tryCommitStagingFileNoReplace(const StagingFile& staged, const fs::path& output_path, std::string_view error_message);
// fsync the staging inode itself, so its contents are durable before it is
// linked into the directory tree and reported to the user.
void syncStagingFileOrThrow(const StagingFile& staged, std::string_view error_message);
[[nodiscard]] std::size_t validateFileForRead(
    const fs::path& path,
    FileTypeCheck file_type = FileTypeCheck::data_file);
[[nodiscard]] vBytes readFile(const fs::path& path, FileTypeCheck file_type = FileTypeCheck::data_file);

// Buffered, fd-backed output sink for the staged final-image write. Replaces a
// std::ofstream + pubsetbuf: write() batches small appends into the internal
// buffer, while sendFrom() streams the bulk encrypted payload straight from one
// fd to another via sendfile(2) — no userspace bounce. Opens O_EXCL + mode 0600
// at create time (same helper as openBinaryOutputForWriteOrThrow — no create-
// then-chmod window). Linux/POSIX only (consistent with sendfile elsewhere).
//
// Note what the buffer does and does not buy. sendFrom() has to drain first, so
// output byte order is preserved — which means an interleaved
// header/sendFrom/header/sendFrom stream (the multi-segment ICC writer) gets one
// write(2) per header regardless of buffer size: two syscalls per 64 KiB
// segment, measured. The buffering pays off on the runs that are actually
// consecutive: the Bluesky and Reddit whole-image writes, and the staged-output
// copy in copyStagingFileNoReplace.
// How OutputFile should obtain its destination inode.
enum class OutputCreate {
    exclusive_new,     // O_CREAT | O_EXCL at mode 0600 -- a brand new named file
    existing_staging,  // O_TRUNC onto an existing inode (a StagingFile fd path)
};

class OutputFile {
public:
    OutputFile(const fs::path& path, std::size_t buffer_capacity, OutputCreate create = OutputCreate::exclusive_new);
    ~OutputFile() noexcept;

    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;

    // Buffered append. Spans at least as large as the buffer bypass it to avoid
    // a redundant copy (as a >streambuf write did with the old ofstream).
    void write(std::span<const Byte> bytes, std::string_view error_message);

    // Zero-copy append of `length` bytes from in_fd at in_offset via
    // sendFileRangeToFd. Flushes the buffer first so output byte order is
    // preserved.
    void sendFrom(int in_fd, std::size_t in_offset, std::size_t length, std::string_view error_message);

    // Flush remaining buffered bytes and close, surfacing any write error --
    // including one the kernel only reports at close(2) (NFS, delayed-allocation
    // ENOSPC/EDQUOT). With `durable`, fsync first so the bytes have reached
    // stable storage before the caller reports success.
    void close(std::string_view error_message, bool durable = false);

private:
    void drain(std::string_view error_message);

    int         fd_ = -1;
    vBytes      buffer_;
    std::size_t fill_ = 0;
};

// Link-free scratch inode (O_TMPFILE, or create-and-unlink). Invisible in
// directory listings and gone on last close. Writers must use the staging
// open path because the inode already exists.
class StagingFile {
public:
    // `parent_dir` selects the filesystem; empty means the current directory.
    StagingFile(const fs::path& parent_dir, std::string_view tag);
    ~StagingFile() noexcept;

    StagingFile(const StagingFile&) = delete;
    StagingFile& operator=(const StagingFile&) = delete;

    // /proc/self/fd/N -- valid for as long as this object lives.
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }
    // Open O_RDWR on both creation paths, so callers may read the staged
    // contents back through it (the commit copy path does exactly that).
    [[nodiscard]] int fd() const noexcept { return fd_; }

    // True when the inode can still be given a name with linkat(2) -- i.e. it
    // came from O_TMPFILE. The create-and-unlink fallback below already spent
    // its only link, so such an inode can never be re-linked and its contents
    // have to be copied out instead.
    [[nodiscard]] bool canLink() const noexcept { return can_link_; }

private:
    int      fd_ = -1;
    bool     can_link_ = false;
    fs::path path_;
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
