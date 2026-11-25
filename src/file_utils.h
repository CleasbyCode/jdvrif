#pragma once

#include "common.h"

#include <limits>
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

inline constexpr const char* WRITE_COMPLETE_ERROR = "Write Error: Failed to write complete output file.";

// A bare filename (no separators) safe to create and to print. Checks the
// bytes in place, allocating nothing: the recovered name is checked before
// any copy of it is made.
[[nodiscard]] bool isSafeEmbeddedFilename(std::string_view name) noexcept;
// The same check, on the path's filename component.
[[nodiscard]] bool hasSafeEmbeddedFilename(const fs::path& p);
// The path's filename, safe to print to a terminal: returned unchanged when it is
// well-formed UTF-8 free of controls, default-ignorables and non-ASCII whitespace;
// otherwise every byte outside printable ASCII is shown as \xNN.
[[nodiscard]] std::string displaySafeFilename(const fs::path& p);
// The whole path, escaped as displaySafeFilename does: for every diagnostic
// that names a user-supplied path, since validation only checks the basename.
[[nodiscard]] std::string displaySafePath(const fs::path& p);
// A message for the terminal, made safe line by line as displaySafeFilename
// makes a name safe: its newlines are kept, and any line holding a control
// character, invalid UTF-8 or hidden/BiDi code point is shown with \xNN
// escapes. Messages already build their paths from the displaySafe helpers,
// so this changes nothing today; it keeps the top-level error sink safe if a
// future message forgets to.
[[nodiscard]] std::string terminalSafeMessage(std::string_view message);
[[nodiscard]] fs::path uniqueRandomizedPathOrThrow(
    const fs::path& parent_dir,
    std::string_view prefix,
    std::string_view suffix,
    std::size_t max_attempts,
    std::string_view error_message,
    std::size_t token_hex_chars = 16);

// A vetted regular file, read with read(2)
// straight into the caller's buffers. The plaintext readers use this rather
// than std::ifstream, whose internal buffer keeps a copy of whatever passes
// through it and is freed without being wiped.
class InputFile {
public:
    InputFile(const fs::path& path, std::string_view error_message);
    ~InputFile() noexcept;

    InputFile(InputFile&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    InputFile(const InputFile&) = delete;
    InputFile& operator=(const InputFile&) = delete;
    InputFile& operator=(InputFile&&) = delete;

    [[nodiscard]] int fd() const noexcept { return fd_; }
    // Size of the opened inode.
    [[nodiscard]] std::size_t size(std::string_view error_message) const;
    // Fills `out` unless the file ends first; returns the bytes read.
    [[nodiscard]] std::size_t readSome(std::span<Byte> out, std::string_view error_message);
    // Fills `out`; ending first is an error.
    void readExact(std::span<Byte> out, std::string_view error_message);
    // Throws unless the file ends here, i.e. it has not grown since measured.
    void requireEnd(std::string_view error_message);

private:
    int fd_ = -1;
};
void cleanupPathNoThrow(const fs::path& path) noexcept;
void writeAllToFd(int fd, std::span<const Byte> bytes, std::string_view error_message);
// Fill `dst` from `fd` at `offset`, retrying short reads; a short read that hits
// EOF is an error, not a partial result.
void preadExactFromFd(int fd, std::span<Byte> dst, std::size_t offset, std::string_view error_message);
// Kernel-to-kernel copy of [in_offset, in_offset + length) from in_fd to
// out_fd's current position via sendfile(2), capped per call at the Linux
// limit. If the kernel/filesystem rejects sendfile outright (EINVAL/ENOSYS on
// the first call), falls back to a pread/write copy loop.
void sendFileRangeToFd(int out_fd, int in_fd, std::size_t in_offset, std::size_t length, std::string_view error_message);

class StagingFile;
// Give a link-free staging inode its first name, never replacing an existing
// entry and never exposing a partial file under `output_path`. Normally the
// O_TMPFILE inode is linked in directly; where that is impossible the contents
// are copied to a hidden temporary beside the output and renamed into place
// (see copyStagingFileNoReplace for that path's tradeoff). Returns false when
// `output_path` is already taken (so the caller can try the next candidate);
// throws on real I/O failure.
[[nodiscard]] bool tryCommitStagingFileNoReplace(const StagingFile& staged, const fs::path& output_path, std::string_view error_message);
// fsync the staging inode itself, so its contents are durable before it is
// linked into the directory tree and reported to the user.
void syncStagingFileOrThrow(const StagingFile& staged, std::string_view error_message);
// The size rules a cover image is read under in each mode.
[[nodiscard]] constexpr FileTypeCheck coverCheckFor(Option option) noexcept {
    switch (option) {
        case Option::Reddit:  return FileTypeCheck::reddit_cover_image;
        case Option::Twitter: return FileTypeCheck::twitter_cover_image;
        default:              return FileTypeCheck::cover_image;
    }
}
[[nodiscard]] std::size_t validateFileForRead(
    const fs::path& path,
    FileTypeCheck file_type = FileTypeCheck::data_file);
// The caller's bound is checked against the opened file before allocating;
// a prior path-size validation cannot enforce it if the input changes later.
[[nodiscard]] vBytes readFile(
    const fs::path& path,
    FileTypeCheck file_type = FileTypeCheck::data_file,
    std::size_t max_bytes = std::numeric_limits<std::size_t>::max());

// Buffered, fd-backed output sink over an existing StagingFile inode, which it
// truncates. write() batches small appends into the internal buffer.
//
// The buffer pays off on runs of small consecutive writes: the default-mode
// image, whose ICC writer interleaves each 18-byte segment header with that
// segment's slice of the ciphertext as encryption produces it, and the Bluesky
// and Reddit whole-image writes.
//
// It is also the writer for every staged plaintext or plaintext-equivalent
// stream (decrypted and inflated output, deflated input). Unlike a std::ofstream,
// whose internal buffer is freed without being cleared, it zeroes its buffer on
// destruction, so short tails that pass through it do not linger in the heap.
// Only close() writes out what is still buffered: an OutputFile destroyed
// without it (an error path) discards its buffered bytes instead.
class OutputFile {
public:
    // The StagingFile's own inode, truncated. Taking the StagingFile rather
    // than a path means only a staging inode can ever be opened this way:
    // there is no path to get wrong, and so none to truncate by mistake.
    OutputFile(const StagingFile& staged, std::size_t buffer_capacity);
    ~OutputFile() noexcept;

    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;

    // Buffered append. Spans at least as large as the buffer bypass it to avoid
    // a redundant copy.
    void write(std::span<const Byte> bytes, std::string_view error_message);

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
    // The inode's current size.
    [[nodiscard]] std::size_t size(std::string_view error_message) const;

private:
    // Only commit asks whether the inode can be linked.
    friend bool tryCommitStagingFileNoReplace(const StagingFile&, const fs::path&, std::string_view);

    int      fd_ = -1;
    // True when the inode can still be given a name with linkat(2) -- i.e. it
    // came from O_TMPFILE. The create-and-unlink fallback below already spent
    // its only link, so such an inode can never be re-linked and its contents
    // have to be copied out instead.
    bool     can_link_ = false;
    fs::path path_;
};
