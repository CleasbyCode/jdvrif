#include "recover_internal.h"
#include "base64.h"
#include "binary_io.h"
#include "embedded_layout.h"
#include "file_utils.h"
#include "signal_utils.h"

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {
constexpr const char* CORRUPT_FILE_ERROR = "File Extraction Error: Embedded data file is corrupt!";
constexpr const char* PAYLOAD_READ_ERROR = "Read Error: Failed while extracting encrypted payload.";

[[nodiscard]] std::uint16_t preadU16At(int fd, std::size_t offset, std::string_view error_message) {
    std::array<Byte, 2> bytes{};
    preadExactFromFd(fd, bytes, offset, error_message);
    return static_cast<std::uint16_t>(getValue(bytes, 0));
}

void requireFileRange(std::size_t file_size, std::size_t offset, std::size_t length, const char* error_message) {
    throwIf(offset > file_size || length > file_size - offset, error_message);
}

// Every signature this file looks for sits in a small window just past the
// EXIF payload (see BlueskyCiphertextExtractor), so read the window [begin, end)
// in one go and search it in memory.
[[nodiscard]] std::optional<std::size_t> findSignatureInRange(
    int fd, std::span<const Byte> sig, std::size_t begin, std::size_t end) {
    if (end <= begin) return std::nullopt;
    vBytes window(end - begin);
    preadExactFromFd(fd, window, begin, "Read Error: Failed while scanning image file.");
    if (auto pos = searchSig(window, sig)) return begin + *pos;
    return std::nullopt;
}

[[nodiscard]] std::size_t decodeBase64UntilDelimiterAppending(
    int in_fd,
    std::size_t offset,
    Byte delimiter,
    std::size_t max_bytes,
    std::size_t expected_decoded_size,
    vBytes& output,
    const char* corrupt_error) {

    throwIf(expected_decoded_size == 0 || max_bytes == 0, corrupt_error);

    const std::size_t encoded_size = checkedMul(checkedAdd(expected_decoded_size, 2, corrupt_error) / 3, 4, corrupt_error);
    throwIf(checkedAdd(encoded_size, 1, corrupt_error) > max_bytes, corrupt_error);

    vBytes encoded(encoded_size);
    preadExactFromFd(in_fd, std::span<Byte>(encoded), offset,
               "Read Error: Failed while scanning XMP payload.");

    std::array<Byte, 1> delimiter_byte{};
    preadExactFromFd(in_fd, std::span<Byte>(delimiter_byte), offset + encoded_size, corrupt_error);
    throwIf(delimiter_byte[0] != delimiter, corrupt_error);

    const std::size_t before = output.size();
    try {
        appendBase64AsBinary(std::span<const Byte>(encoded), output);
    } catch (const std::exception&) {
        throw std::runtime_error(corrupt_error);
    }

    throwIf(output.size() - before != expected_decoded_size, corrupt_error);
    return expected_decoded_size;
}

void validateDefaultCiphertextRange(std::size_t image_size, std::size_t base_offset, std::size_t embedded_file_size) {
    throwIf(base_offset > image_size ||
        ICC_CIPHER_LAYOUT.encrypted_payload_start_index > image_size - base_offset,
        CORRUPT_FILE_ERROR);

    const std::size_t payload_start = base_offset + ICC_CIPHER_LAYOUT.encrypted_payload_start_index;
    throwIf(embedded_file_size > image_size - payload_start, CORRUPT_FILE_ERROR);
}

void validateIccTrailingMarker(
    int in_fd,
    std::size_t image_size,
    std::size_t base_offset,
    std::uint16_t total_profile_header_segments) {

    if (total_profile_header_segments == 0) return;

    const auto marker_index = iccTrailingMarkerIndex(total_profile_header_segments);
    throwIf(!marker_index, CORRUPT_FILE_ERROR);
    throwIf(
        *marker_index > image_size - base_offset || image_size - base_offset - *marker_index < 2,
        CORRUPT_FILE_ERROR);

    std::array<Byte, 2> marker{};
    preadExactFromFd(in_fd, marker, base_offset + *marker_index,
               "File Extraction Error: Missing segments detected. Embedded data file is corrupt!");
    throwIf(
        !std::equal(JPEG_APP2_MARKER.begin(), JPEG_APP2_MARKER.end(), marker.begin()),
        "File Extraction Error: Missing segments detected. Embedded data file is corrupt!");
}

// Where the ciphertext sits in the image: one run per stretch between ICC
// profile headers, in order.
struct FileRun {
    std::size_t offset;
    std::size_t length;
};

[[nodiscard]] std::vector<FileRun> defaultCiphertextRuns(
    std::size_t payload_start,
    std::size_t embedded_file_size,
    bool has_profile_headers) {

    if (!has_profile_headers) return {{payload_start, embedded_file_size}};

    // Logical cursor into the embedded span (0..embedded_file_size). The same
    // offset on disk is payload_start + cursor.
    std::vector<FileRun> runs;
    std::size_t cursor      = 0;
    std::size_t next_header = ICC_SEGMENT_LAYOUT.profile_header_insert_index;

    while (cursor < embedded_file_size) {
        const std::size_t header_end = next_header + ICC_SEGMENT_LAYOUT.profile_header_length;

        if (cursor >= next_header && cursor < header_end) {
            // Inside a profile-header gap: skip it.
            cursor = std::min(header_end, embedded_file_size);
            if (cursor == header_end) {
                next_header = checkedAdd(
                    next_header,
                    ICC_SEGMENT_LAYOUT.per_segment_stride,
                    CORRUPT_FILE_ERROR);
            }
            continue;
        }

        const std::size_t cap_by_header = std::min(embedded_file_size, next_header) - cursor;
        if (cap_by_header == 0) {
            // Unreachable while profile_header_length > 0: cursor == next_header
            // is claimed by the skip branch above, so this run is always
            // non-empty. Throwing rather than continuing means a future layout
            // change cannot turn this into a non-terminating loop driven by
            // attacker-supplied segment counts.
            throw std::runtime_error(CORRUPT_FILE_ERROR);
        }

        runs.push_back({payload_start + cursor, cap_by_header});
        cursor += cap_by_header;
    }
    return runs;
}

[[nodiscard]] std::size_t totalRunLength(std::span<const FileRun> runs) noexcept {
    std::size_t total = 0;
    for (const FileRun& run : runs) total += run.length;
    return total;
}

// Reads the ciphertext straight out of the image, run by run.
class FileRunsCipherSource final : public CipherSource {
public:
    FileRunsCipherSource(InputFile input, std::vector<FileRun> runs)
        : CipherSource(totalRunLength(runs)),
          input_(std::move(input)),
          runs_(std::move(runs)) {}

private:
    void readExact(std::span<Byte> out) override {
        while (!out.empty()) {
            const FileRun& run = runs_[run_index_];
            const std::size_t take = std::min(out.size(), run.length - run_position_);
            preadExactFromFd(input_.fd(), out.first(take), run.offset + run_position_, PAYLOAD_READ_ERROR);
            out = out.subspan(take);
            run_position_ += take;
            if (run_position_ == run.length) {
                ++run_index_;
                run_position_ = 0;
            }
        }
    }

    InputFile input_;
    std::vector<FileRun> runs_;
    std::size_t run_index_{0};
    std::size_t run_position_{0};
};

class BlueskyCiphertextExtractor {
public:
    BlueskyCiphertextExtractor(
        const fs::path& image_path,
        std::size_t image_size,
        std::size_t embedded_file_size)
        : image_size_(image_size),
          embedded_file_size_(embedded_file_size),
          input_fd_(image_path, "Read Error: Failed to open image file.") {

        validateInitialRange();
        // Bounded by MAX_EMBEDDED_CIPHERTEXT_BLUESKY (2 MiB) before this runs.
        output_.reserve(embedded_file_size_);
    }

    [[nodiscard]] vBytes run() {
        appendFromImage(BLUESKY_CIPHER_LAYOUT.encrypted_payload_start_index, exif_chunk_size_);
        written_ += exif_chunk_size_;
        remaining_ = embedded_file_size_ - exif_chunk_size_;
        if (remaining_ == 0) return std::move(output_);

        const std::size_t pshop_sig_index = findPhotoshopSignature();
        throwIf(pshop_sig_index < BLUESKY_SEGMENT_LAYOUT.pshop_segment_size_index_diff, CORRUPT_FILE_ERROR);

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
        if (remaining_ == 0) return std::move(output_);

        throwIf(pshop_segment_size <= BLUESKY_SEGMENT_LAYOUT.dataset_max_size, CORRUPT_FILE_ERROR);

        copySecondDataset(first_dataset_size_index, first_dataset_size);
        if (remaining_ == 0) return std::move(output_);

        copyXmpRemainder(pshop_segment_size_index, pshop_segment_size);
        throwIf(written_ != embedded_file_size_, CORRUPT_FILE_ERROR);

        return std::move(output_);
    }

private:
    void validateInitialRange() {
        throwIf(embedded_file_size_ == 0 ||
            BLUESKY_CIPHER_LAYOUT.encrypted_payload_start_index > image_size_,
            CORRUPT_FILE_ERROR);

        exif_chunk_size_ = std::min(embedded_file_size_, BLUESKY_SEGMENT_LAYOUT.exif_segment_data_size_limit);
        throwIf(
            exif_chunk_size_ > image_size_ - BLUESKY_CIPHER_LAYOUT.encrypted_payload_start_index,
            CORRUPT_FILE_ERROR);
    }

    void appendFromImage(std::size_t offset, std::size_t length) {
        const std::size_t start = output_.size();
        output_.resize(start + length);
        preadExactFromFd(input_fd_.fd(), std::span<Byte>(output_).subspan(start), offset, PAYLOAD_READ_ERROR);
    }

    // APP13 ("Photoshop 3.0"): FF ED LL LL | "Photoshop 3.0"…
    // BLUESKY_PHOTOSHOP_SIGNATURE is "shop 3." at offset 5 into that string, so
    // the APP13 marker sits 9 bytes before the signature hit.
    static constexpr std::size_t PHOTOSHOP_SIG_TO_APP13 = 9;

    // After EXIF payload: optional XMP (≤ xmp_segment_size_limit) then APP13.
    // Bound the scan so we never walk the whole SOS entropy looking for a decoy.
    static constexpr std::size_t PHOTOSHOP_SEARCH_WINDOW =
        BLUESKY_SEGMENT_LAYOUT.xmp_segment_size_limit + 128 * 1024;

    [[nodiscard]] std::size_t findPhotoshopSignature() {
        // Never scan EXIF-held ciphertext for "shop 3." (collision / decoy risk).
        const std::size_t search_start = checkedAdd(
            BLUESKY_CIPHER_LAYOUT.encrypted_payload_start_index,
            exif_chunk_size_,
            CORRUPT_FILE_ERROR);
        throwIf(search_start >= image_size_, CORRUPT_FILE_ERROR);

        const std::size_t search_end = std::min(
            image_size_,
            checkedAdd(search_start, PHOTOSHOP_SEARCH_WINDOW, CORRUPT_FILE_ERROR));

        const auto pshop_sig_opt = findSignatureInRange(
            input_fd_.fd(),
            BLUESKY_PHOTOSHOP_SIGNATURE,
            search_start,
            search_end);
        throwIf(!pshop_sig_opt, CORRUPT_FILE_ERROR);

        const std::size_t sig_index = *pshop_sig_opt;
        throwIf(sig_index < PHOTOSHOP_SIG_TO_APP13, CORRUPT_FILE_ERROR);

        // Require a real APP13 marker framing "Photoshop 3.0", not a bare sig hit.
        const std::size_t app13_index = sig_index - PHOTOSHOP_SIG_TO_APP13;
        std::array<Byte, 2> marker{};
        preadExactFromFd(input_fd_.fd(), marker, app13_index, CORRUPT_FILE_ERROR);
        throwIf(marker[0] != 0xFF || marker[1] != 0xED, CORRUPT_FILE_ERROR);

        return sig_index;
    }

    [[nodiscard]] std::uint16_t copyDataset(std::size_t dataset_size_index) {
        requireFileRange(image_size_, dataset_size_index, 2, CORRUPT_FILE_ERROR);

        const std::size_t dataset_file_index = checkedAdd(
            dataset_size_index,
            BLUESKY_SEGMENT_LAYOUT.dataset_file_index_diff,
            CORRUPT_FILE_ERROR);

        const std::uint16_t dataset_size = preadU16At(input_fd_.fd(), dataset_size_index, CORRUPT_FILE_ERROR);
        throwIf(dataset_size == 0 || dataset_size > remaining_, CORRUPT_FILE_ERROR);

        requireFileRange(image_size_, dataset_file_index, dataset_size, CORRUPT_FILE_ERROR);
        appendFromImage(dataset_file_index, dataset_size);
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
        throwIf(xmp_search_start > pshop_segment_size_index, CORRUPT_FILE_ERROR);

        const auto xmp_sig_opt = findSignatureInRange(
            input_fd_.fd(),
            BLUESKY_XMP_CREATOR_SIGNATURE,
            xmp_search_start,
            pshop_segment_size_index);
        throwIf(!xmp_sig_opt, CORRUPT_FILE_ERROR);

        const std::size_t base64_begin_index = checkedAdd(
            checkedAdd(*xmp_sig_opt, BLUESKY_XMP_CREATOR_SIGNATURE.size(), CORRUPT_FILE_ERROR),
            1,
            CORRUPT_FILE_ERROR);
        requireFileRange(image_size_, base64_begin_index, 0, CORRUPT_FILE_ERROR);

        written_ += decodeBase64UntilDelimiterAppending(
            input_fd_.fd(),
            base64_begin_index,
            BLUESKY_SEGMENT_LAYOUT.base64_end_sig,
            BLUESKY_SEGMENT_LAYOUT.xmp_base64_max_scan_bytes,
            remaining_,
            output_,
            CORRUPT_FILE_ERROR);
    }

    std::size_t image_size_{0};
    std::size_t embedded_file_size_{0};
    InputFile  input_fd_;
    vBytes       output_;
    std::size_t exif_chunk_size_{0};
    std::size_t written_{0};
    std::size_t remaining_{0};
};

} // namespace

void readExactAt(const fs::path& path, std::size_t offset, std::span<Byte> out) {
    InputFile input(path, "Read Error: Failed to open image file.");
    preadExactFromFd(input.fd(), out, offset, "Read Error: Failed to read expected bytes.");
}

[[nodiscard]] std::unique_ptr<CipherSource> openDefaultCiphertext(
    const fs::path& image_path,
    std::size_t image_size,
    std::size_t base_offset,
    std::size_t embedded_file_size,
    std::uint16_t total_profile_header_segments) {
    validateDefaultCiphertextRange(image_size, base_offset, embedded_file_size);

    const std::size_t payload_start = base_offset + ICC_CIPHER_LAYOUT.encrypted_payload_start_index;

    InputFile input(image_path, "Read Error: Failed to open image file.");
    validateIccTrailingMarker(input.fd(), image_size, base_offset, total_profile_header_segments);

    return std::make_unique<FileRunsCipherSource>(
        std::move(input),
        defaultCiphertextRuns(payload_start, embedded_file_size, total_profile_header_segments != 0));
}

[[nodiscard]] std::unique_ptr<CipherSource> openBlueskyCiphertext(
    const fs::path& image_path,
    std::size_t image_size,
    std::size_t embedded_file_size) {
    return std::make_unique<MemoryCipherSource>(
        BlueskyCiphertextExtractor(image_path, image_size, embedded_file_size).run());
}
