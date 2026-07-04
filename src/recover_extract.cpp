#include "recover_internal.h"
#include "base64.h"
#include "binary_io.h"
#include "embedded_layout.h"
#include "file_utils.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <unistd.h>

void readExactAt(std::ifstream& input, std::size_t offset, std::span<Byte> out) {
    input.clear();
    input.seekg(checkedStreamOffset(offset, "Read Error: Seek offset overflow."), std::ios::beg);
    if (!input) {
        throw std::runtime_error("Read Error: Failed to seek read position.");
    }
    readExactOrThrow(input, out.data(), out.size(), "Read Error: Failed to read expected bytes.");
}

namespace {
constexpr std::size_t EXTRACT_OUTPUT_BUFFER_SIZE = 64 * 1024;

// ---------------------------------------------------------------------------
// POSIX fd input wrapper. Used by the extract paths so they can sendfile(2)
// bytes straight from the input image to the staging cipher file (via
// OutputFile::sendFrom) without dragging the payload through user space. Sig
// scans and the small reads at known offsets (preadU16At) continue to use
// std::ifstream / pread — they are not bandwidth-bound.
// ---------------------------------------------------------------------------

class FdInputFile {
public:
    FdInputFile(const fs::path& path, std::string_view error_message) {
        fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd_ < 0) {
            throw std::runtime_error(std::string(error_message));
        }
        // Hint the kernel that the embedded image is read sequentially so it
        // prefetches aggressively and drops pages after use. Best-effort: any
        // failure here is informational, not fatal.
        (void)::posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
    }

    ~FdInputFile() noexcept {
        if (fd_ >= 0) ::close(fd_);
    }

    FdInputFile(const FdInputFile&) = delete;
    FdInputFile& operator=(const FdInputFile&) = delete;

    [[nodiscard]] int fd() const noexcept { return fd_; }

private:
    int fd_ = -1;
};

void preadExact(int fd, std::span<Byte> dst, std::size_t offset, std::string_view error_message) {
    Byte*       p    = dst.data();
    std::size_t rem  = dst.size();
    off_t       off  = static_cast<off_t>(offset);
    while (rem > 0) {
        const ssize_t got = ::pread(fd, p, rem, off);
        if (got < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string(error_message));
        }
        if (got == 0) {
            throw std::runtime_error(std::string(error_message));   // unexpected EOF
        }
        p   += got;
        rem -= static_cast<std::size_t>(got);
        off += got;
    }
}

[[nodiscard]] std::uint16_t preadU16At(int fd, std::size_t offset, std::string_view error_message) {
    std::array<Byte, 2> bytes{};
    preadExact(fd, bytes, offset, error_message);
    return static_cast<std::uint16_t>(getValue(bytes, 0));
}

// Process-lifetime scratch buffer reused across signature scans so we don't
// re-allocate a ~1 MiB scan window per call. Intentionally never released: it
// only ever holds cover/ciphertext bytes (never plaintext secrets), so there is
// nothing sensitive to zeroize, and the program is single-threaded and one-shot.
[[nodiscard]] vBytes& signatureScanBuffer(std::size_t required_size) {
    thread_local vBytes buffer;
    if (buffer.size() < required_size) buffer.resize(required_size);
    return buffer;
}

void seekInputOrThrow(std::istream& input, std::size_t offset, std::ios::seekdir dir, const char* error_message) {
    input.clear();
    input.seekg(checkedStreamOffset(offset, "Read Error: Seek offset overflow."), dir);
    if (!input) {
        throw std::runtime_error(error_message);
    }
}

void requireFileRange(std::size_t file_size, std::size_t offset, std::size_t length, const char* error_message) {
    if (offset > file_size || length > file_size - offset) {
        throw std::runtime_error(error_message);
    }
}

template<typename VisitorFn>
void scanStreamWindows(std::ifstream& input, std::size_t overlap, std::size_t search_limit, VisitorFn&& visitor, std::size_t start_offset = 0) {
    seekInputOrThrow(input, start_offset, std::ios::beg, "Read Error: Failed to seek read position.");

    constexpr std::size_t CHUNK_SIZE = 1 * 1024 * 1024;
    if (overlap > std::numeric_limits<std::size_t>::max() - CHUNK_SIZE) {
        throw std::runtime_error("File Extraction Error: Signature scan buffer size overflow.");
    }
    vBytes& buffer = signatureScanBuffer(CHUNK_SIZE + overlap);
    std::size_t carry = 0;
    std::size_t consumed = start_offset;

    while (true) {
        if (search_limit && consumed >= search_limit) break;

        std::size_t to_read = CHUNK_SIZE;
        if (search_limit) to_read = std::min(to_read, search_limit - consumed);

        const std::streamsize got = readSomeOrThrow(input, buffer.data() + carry, to_read, "Read Error: Failed while scanning image file.");
        if (got == 0) break;

        const std::size_t window_size = carry + static_cast<std::size_t>(got);
        const std::span<const Byte> window(buffer.data(), window_size);
        const std::size_t base = consumed - carry;
        if (visitor(window, base)) break;

        if (overlap > 0) {
            carry = std::min(overlap, window_size);
            std::memmove(buffer.data(), buffer.data() + (window_size - carry), carry);
        } else {
            carry = 0;
        }

        consumed += static_cast<std::size_t>(got);
    }
}

[[nodiscard]] std::optional<std::size_t> findSignatureInOpenStream(std::ifstream& input, std::span<const Byte> sig, std::size_t search_limit, std::size_t start_offset) {
    if (sig.empty() || (search_limit != 0 && search_limit <= start_offset)) return std::nullopt;

    std::optional<std::size_t> found{};
    const std::size_t overlap = (sig.size() > 1) ? sig.size() - 1 : 0;

    scanStreamWindows(input, overlap, search_limit, [&](std::span<const Byte> window, std::size_t base) {
        if (auto pos = searchSig(window, sig); pos) {
            found = base + *pos;
            return true;
        }
        return false;
    }, start_offset);

    return found;
}

[[nodiscard]] std::size_t streamDecodeBase64UntilDelimiterToOutput(
    int in_fd,
    std::size_t offset,
    Byte delimiter,
    std::size_t max_bytes,
    std::size_t expected_decoded_size,
    OutputFile& output,
    const char* corrupt_error) {

    if (expected_decoded_size == 0 || max_bytes == 0) {
        throw std::runtime_error(corrupt_error);
    }

    const std::size_t encoded_size = checkedMul(checkedAdd(expected_decoded_size, 2, corrupt_error) / 3, 4, corrupt_error);
    if (checkedAdd(encoded_size, 1, corrupt_error) > max_bytes) {
        throw std::runtime_error(corrupt_error);
    }

    vBytes encoded(encoded_size);
    preadExact(in_fd, std::span<Byte>(encoded), offset,
               "Read Error: Failed while scanning XMP payload.");

    std::array<Byte, 1> delimiter_byte{};
    preadExact(in_fd, std::span<Byte>(delimiter_byte), offset + encoded_size, corrupt_error);
    if (delimiter_byte[0] != delimiter) {
        throw std::runtime_error(corrupt_error);
    }

    vBytes decoded;
    decoded.reserve(expected_decoded_size);
    try {
        appendBase64AsBinary(std::span<const Byte>(encoded), decoded);
    } catch (const std::exception&) {
        throw std::runtime_error(corrupt_error);
    }

    if (decoded.size() != expected_decoded_size) {
        throw std::runtime_error(corrupt_error);
    }

    output.write(std::span<const Byte>(decoded), WRITE_COMPLETE_ERROR);
    return decoded.size();
}

void validateDefaultCiphertextRange(
    std::size_t image_size,
    std::size_t base_offset,
    std::size_t embedded_file_size) {

    constexpr const char* CORRUPT_FILE_ERROR = "File Extraction Error: Embedded data file is corrupt!";

    if (base_offset > image_size ||
        ICC_CIPHER_LAYOUT.encrypted_payload_start_index > image_size - base_offset) {
        throw std::runtime_error(CORRUPT_FILE_ERROR);
    }

    const std::size_t payload_start = base_offset + ICC_CIPHER_LAYOUT.encrypted_payload_start_index;
    if (embedded_file_size > image_size - payload_start) {
        throw std::runtime_error(CORRUPT_FILE_ERROR);
    }
}

void validateIccTrailingMarker(
    int in_fd,
    std::size_t image_size,
    std::size_t base_offset,
    std::uint16_t total_profile_header_segments) {

    if (total_profile_header_segments == 0) return;

    constexpr const char* CORRUPT_FILE_ERROR = "File Extraction Error: Embedded data file is corrupt!";

    const auto marker_index = iccTrailingMarkerIndex(total_profile_header_segments);
    if (!marker_index) {
        throw std::runtime_error(CORRUPT_FILE_ERROR);
    }
    if (*marker_index > image_size - base_offset || image_size - base_offset - *marker_index < 2) {
        throw std::runtime_error(CORRUPT_FILE_ERROR);
    }

    std::array<Byte, 2> marker{};
    preadExact(in_fd, marker, base_offset + *marker_index,
               "File Extraction Error: Missing segments detected. Embedded data file is corrupt!");
    if (!std::equal(JPEG_APP2_MARKER.begin(), JPEG_APP2_MARKER.end(), marker.begin())) {
        throw std::runtime_error("File Extraction Error: Missing segments detected. Embedded data file is corrupt!");
    }
}

[[nodiscard]] std::size_t copyDefaultCiphertextPayload(
    int in_fd,
    OutputFile& output,
    std::size_t payload_start,
    std::size_t embedded_file_size,
    bool has_profile_headers) {

    // Logical cursor into the embedded ciphertext (0..embedded_file_size).
    // The same offset on disk is payload_start + cursor.
    std::size_t cursor      = 0;
    std::size_t next_header = ICC_SEGMENT_LAYOUT.profile_header_insert_index;
    std::size_t written     = 0;

    if (!has_profile_headers) {
        // Single contiguous run: one sendfile call moves the whole payload
        // kernel-to-kernel.
        output.sendFrom(in_fd, payload_start, embedded_file_size,
                        "Read Error: Failed while extracting encrypted payload.");
        return embedded_file_size;
    }

    while (cursor < embedded_file_size) {
        const std::size_t header_end = next_header + ICC_SEGMENT_LAYOUT.profile_header_length;

        if (cursor >= next_header && cursor < header_end) {
            // Inside a profile-header gap: advance the logical cursor without
            // copying. sendfile()'s source-offset argument lets us skip on
            // the source side too, so we just bump `cursor`.
            const std::size_t skip_target = std::min(header_end, embedded_file_size);
            cursor = skip_target;
            if (cursor == header_end) {
                next_header = checkedAdd(
                    next_header,
                    ICC_SEGMENT_LAYOUT.per_segment_stride,
                    "File Extraction Error: Embedded data file is corrupt!");
            }
            continue;
        }

        const std::size_t cap_by_header = std::min(embedded_file_size, next_header) - cursor;
        if (cap_by_header == 0) continue;

        output.sendFrom(in_fd, payload_start + cursor, cap_by_header,
                        "Read Error: Failed while extracting encrypted payload.");
        cursor  += cap_by_header;
        written += cap_by_header;
    }

    return written;
}

class BlueskyCiphertextExtractor {
public:
    BlueskyCiphertextExtractor(
        const fs::path& image_path,
        std::size_t image_size,
        std::size_t embedded_file_size,
        const fs::path& output_path)
        : image_path_(image_path),
          image_size_(image_size),
          embedded_file_size_(embedded_file_size),
          input_fd_(image_path_, "Read Error: Failed to open image file."),
          output_(output_path, EXTRACT_OUTPUT_BUFFER_SIZE) {

        validateInitialRange();
        // Sig scans and the small windowed reads continue to use std::ifstream
        // -- they reuse scanStreamWindows/searchSig which are not bandwidth-
        // bound. The fd handle on input_fd_ is reserved for sendfile sources.
        input_stream_ = openBinaryInputOrThrow(image_path_, "Read Error: Failed to open image file.");
    }

    [[nodiscard]] std::size_t run() {
        output_.sendFrom(
            input_fd_.fd(),
            BLUESKY_CIPHER_LAYOUT.encrypted_payload_start_index,
            exif_chunk_size_,
            "Read Error: Failed while extracting encrypted payload.");
        written_ += exif_chunk_size_;
        remaining_ = embedded_file_size_ - exif_chunk_size_;
        if (finishIfComplete()) return written_;

        const std::size_t pshop_sig_index = findPhotoshopSignature();
        if (pshop_sig_index < BLUESKY_SEGMENT_LAYOUT.pshop_segment_size_index_diff) {
            throw std::runtime_error(CORRUPT_FILE_ERROR);
        }

        const std::size_t pshop_segment_size_index =
            pshop_sig_index - BLUESKY_SEGMENT_LAYOUT.pshop_segment_size_index_diff;
        const std::size_t first_dataset_size_index = checkedAdd(
            pshop_sig_index,
            BLUESKY_SEGMENT_LAYOUT.first_dataset_size_index_diff,
            CORRUPT_FILE_ERROR);

        requireFileRange(image_size_, pshop_segment_size_index, 2, CORRUPT_FILE_ERROR);
        requireFileRange(image_size_, first_dataset_size_index, 2, CORRUPT_FILE_ERROR);

        const std::uint16_t pshop_segment_size = preadU16At(input_fd_.fd(), pshop_segment_size_index, CORRUPT_FILE_ERROR);
        const std::uint16_t first_dataset_size = copyDataset(first_dataset_size_index);
        if (finishIfComplete()) return written_;

        if (pshop_segment_size <= BLUESKY_SEGMENT_LAYOUT.dataset_max_size) {
            throw std::runtime_error(CORRUPT_FILE_ERROR);
        }

        copySecondDataset(first_dataset_size_index, first_dataset_size);
        if (finishIfComplete()) return written_;

        copyXmpRemainder(pshop_segment_size_index, pshop_segment_size);
        output_.close(WRITE_COMPLETE_ERROR);
        if (written_ != embedded_file_size_) {
            throw std::runtime_error(CORRUPT_FILE_ERROR);
        }

        return written_;
    }

private:
    void validateInitialRange() {
        if (embedded_file_size_ == 0 ||
            BLUESKY_CIPHER_LAYOUT.encrypted_payload_start_index > image_size_) {
            throw std::runtime_error(CORRUPT_FILE_ERROR);
        }

        exif_chunk_size_ = std::min(
            embedded_file_size_,
            BLUESKY_SEGMENT_LAYOUT.exif_segment_data_size_limit);
        if (exif_chunk_size_ > image_size_ - BLUESKY_CIPHER_LAYOUT.encrypted_payload_start_index) {
            throw std::runtime_error(CORRUPT_FILE_ERROR);
        }
    }

    [[nodiscard]] bool finishIfComplete() {
        if (remaining_ != 0) return false;
        output_.close(WRITE_COMPLETE_ERROR);
        return true;
    }

    [[nodiscard]] std::size_t findPhotoshopSignature() {
        const auto pshop_sig_opt = findSignatureInOpenStream(
            input_stream_,
            BLUESKY_PHOTOSHOP_SIGNATURE,
            0,
            0);
        if (!pshop_sig_opt) {
            throw std::runtime_error(CORRUPT_FILE_ERROR);
        }
        return *pshop_sig_opt;
    }

    [[nodiscard]] std::uint16_t copyDataset(std::size_t dataset_size_index) {
        requireFileRange(image_size_, dataset_size_index, 2, CORRUPT_FILE_ERROR);

        const std::size_t dataset_file_index = checkedAdd(
            dataset_size_index,
            BLUESKY_SEGMENT_LAYOUT.dataset_file_index_diff,
            CORRUPT_FILE_ERROR);

        const std::uint16_t dataset_size = preadU16At(input_fd_.fd(), dataset_size_index, CORRUPT_FILE_ERROR);
        if (dataset_size == 0 || dataset_size > remaining_) {
            throw std::runtime_error(CORRUPT_FILE_ERROR);
        }

        requireFileRange(image_size_, dataset_file_index, dataset_size, CORRUPT_FILE_ERROR);
        output_.sendFrom(input_fd_.fd(), dataset_file_index, dataset_size,
                         "Read Error: Failed while extracting encrypted payload.");
        written_ += dataset_size;
        remaining_ -= dataset_size;

        return dataset_size;
    }

    void copySecondDataset(std::size_t first_dataset_size_index, std::uint16_t first_dataset_size) {
        const std::size_t first_dataset_file_index = checkedAdd(
            first_dataset_size_index,
            BLUESKY_SEGMENT_LAYOUT.dataset_file_index_diff,
            CORRUPT_FILE_ERROR);
        const std::size_t second_dataset_size_index = checkedAdd(
            checkedAdd(first_dataset_file_index, first_dataset_size, CORRUPT_FILE_ERROR),
            BLUESKY_SEGMENT_LAYOUT.second_dataset_size_index_diff,
            CORRUPT_FILE_ERROR);
        (void)copyDataset(second_dataset_size_index);
    }

    void copyXmpRemainder(std::size_t pshop_segment_size_index, std::uint16_t pshop_segment_size) {
        requireFileRange(image_size_, pshop_segment_size_index, pshop_segment_size, CORRUPT_FILE_ERROR);

        const std::size_t xmp_search_start =
            BLUESKY_CIPHER_LAYOUT.encrypted_payload_start_index + exif_chunk_size_;
        if (xmp_search_start > pshop_segment_size_index) {
            throw std::runtime_error(CORRUPT_FILE_ERROR);
        }

        const auto xmp_sig_opt = findSignatureInOpenStream(
            input_stream_,
            BLUESKY_XMP_CREATOR_SIGNATURE,
            pshop_segment_size_index,
            xmp_search_start);
        if (!xmp_sig_opt) {
            throw std::runtime_error(CORRUPT_FILE_ERROR);
        }

        const std::size_t base64_begin_index = checkedAdd(
            checkedAdd(*xmp_sig_opt, BLUESKY_XMP_CREATOR_SIGNATURE.size(), CORRUPT_FILE_ERROR),
            1,
            CORRUPT_FILE_ERROR);
        requireFileRange(image_size_, base64_begin_index, 0, CORRUPT_FILE_ERROR);

        written_ += streamDecodeBase64UntilDelimiterToOutput(
            input_fd_.fd(),
            base64_begin_index,
            BLUESKY_SEGMENT_LAYOUT.base64_end_sig,
            BLUESKY_SEGMENT_LAYOUT.xmp_base64_max_scan_bytes,
            remaining_,
            output_,
            CORRUPT_FILE_ERROR);
    }

    static constexpr const char* CORRUPT_FILE_ERROR = "File Extraction Error: Embedded data file is corrupt!";

    const fs::path& image_path_;
    std::size_t image_size_{0};
    std::size_t embedded_file_size_{0};
    FdInputFile  input_fd_;
    OutputFile   output_;
    std::ifstream input_stream_{};
    std::size_t exif_chunk_size_{0};
    std::size_t written_{0};
    std::size_t remaining_{0};
};

} // namespace

[[nodiscard]] std::optional<std::size_t> findSignatureInFile(
    const fs::path& path,
    std::span<const Byte> sig,
    std::size_t search_limit,
    std::size_t start_offset) {
    std::ifstream input = openBinaryInputOrThrow(path, "Read Error: Failed to open image file.");
    return findSignatureInOpenStream(input, sig, search_limit, start_offset);
}

[[nodiscard]] std::optional<std::size_t> findSignaturePairInFile(
    const fs::path& path,
    std::span<const Byte> first_sig,
    std::span<const Byte> second_sig,
    std::size_t second_sig_offset) {

    if (first_sig.empty() || second_sig.empty() || second_sig_offset < first_sig.size()) {
        return std::nullopt;
    }

    // Bytes needed from a candidate's start to verify both signatures.
    const std::size_t verify_span = checkedAdd(
        second_sig_offset,
        second_sig.size(),
        "File Extraction Error: Signature scan size overflow.");

    std::ifstream input = openBinaryInputOrThrow(path, "Read Error: Failed to open image file.");
    std::optional<std::size_t> found{};

    // Single forward pass: every candidate is verified inside the buffered
    // window. A candidate whose verification range crosses the window edge is
    // re-presented at the start of the next window by the overlap carry, so a
    // file full of decoy first-signature hits costs one scan, not one stream
    // restart per hit. A candidate too close to EOF for the full span cannot
    // be valid, so dropping it when no next window arrives is correct.
    scanStreamWindows(input, verify_span - 1, 0, [&](std::span<const Byte> window, std::size_t base) {
        std::size_t pos = 0;
        while (pos < window.size()) {
            const auto rel_opt = searchSig(window.subspan(pos), first_sig);
            if (!rel_opt) return false;

            const std::size_t candidate = pos + *rel_opt;
            if (window.size() - candidate < verify_span) return false;

            if (std::ranges::equal(
                    window.subspan(candidate + second_sig_offset, second_sig.size()),
                    second_sig)) {
                found = base + candidate;
                return true;
            }
            pos = candidate + 1;
        }
        return false;
    });

    return found;
}

[[nodiscard]] std::size_t extractDefaultCiphertextToFile(
    const fs::path& image_path,
    std::size_t image_size,
    std::size_t base_offset,
    std::size_t embedded_file_size,
    std::uint16_t total_profile_header_segments,
    const fs::path& output_path) {
    validateDefaultCiphertextRange(image_size, base_offset, embedded_file_size);

    const std::size_t payload_start = base_offset + ICC_CIPHER_LAYOUT.encrypted_payload_start_index;

    FdInputFile input(image_path, "Read Error: Failed to open image file.");
    OutputFile  output(output_path, EXTRACT_OUTPUT_BUFFER_SIZE);

    validateIccTrailingMarker(input.fd(), image_size, base_offset, total_profile_header_segments);

    const bool has_profile_headers = (total_profile_header_segments != 0);
    const std::size_t written = copyDefaultCiphertextPayload(
        input.fd(),
        output,
        payload_start,
        embedded_file_size,
        has_profile_headers);

    output.close(WRITE_COMPLETE_ERROR);
    return written;
}

[[nodiscard]] std::size_t extractBlueskyCiphertextToFile(
    const fs::path& image_path,
    std::size_t image_size,
    std::size_t embedded_file_size,
    const fs::path& output_path) {
    return BlueskyCiphertextExtractor(
        image_path,
        image_size,
        embedded_file_size,
        output_path).run();
}
