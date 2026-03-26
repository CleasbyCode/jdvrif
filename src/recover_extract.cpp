#include "recover_internal.h"
#include "base64.h"
#include "binary_io.h"
#include "embedded_layout.h"
#include "file_utils.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

void readExactAt(std::ifstream& input, std::size_t offset, std::span<Byte> out) {
    input.clear();
    input.seekg(checkedStreamOffset(offset, "Read Error: Seek offset overflow."), std::ios::beg);
    if (!input) throw std::runtime_error("Read Error: Failed to seek read position.");
    readExactOrThrow(input, out.data(), out.size(), "Read Error: Failed to read expected bytes.");
}

[[nodiscard]] bool hasSignatureAt(std::ifstream& input, std::size_t image_size, std::size_t offset, std::span<const Byte> signature) {
    if (signature.empty() || offset > image_size || signature.size() > image_size - offset) return false;

    vBytes bytes(signature.size());
    readExactAt(input, offset, bytes);
    return std::equal(signature.begin(), signature.end(), bytes.begin());
}

namespace {
constexpr const char* WRITE_COMPLETE_ERROR = "Write Error: Failed to write complete output file.";

struct DualSignatureLocations { std::optional<std::size_t> first{}, second{}; };

[[nodiscard]] vBytes& signatureScanBuffer(std::size_t required_size) {
    thread_local vBytes buffer;
    if (buffer.size() < required_size) buffer.resize(required_size);
    return buffer;
}

void seekInputOrThrow(std::istream& input, std::size_t offset, std::ios::seekdir dir, const char* error_message) {
    input.clear();
    input.seekg(checkedStreamOffset(offset, "Read Error: Seek offset overflow."), dir);
    if (!input) throw std::runtime_error(error_message);
}

template<typename VisitorFn>
void scanFileWindows(const fs::path& path, std::size_t overlap, std::size_t search_limit, VisitorFn&& visitor, std::size_t start_offset = 0) {
    std::ifstream input = openBinaryInputOrThrow(path, "Read Error: Failed to open image file.");
    if (start_offset != 0) seekInputOrThrow(input, start_offset, std::ios::beg, "Read Error: Failed to seek read position.");

    constexpr std::size_t CHUNK_SIZE = 1 * 1024 * 1024;
    if (overlap > std::numeric_limits<std::size_t>::max() - CHUNK_SIZE) throw std::runtime_error("File Extraction Error: Signature scan buffer size overflow.");
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

[[nodiscard]] DualSignatureLocations findDualSignaturesInFile(const fs::path& path, std::span<const Byte> first_sig, std::span<const Byte> second_sig, std::size_t search_limit = 0) {
    if (first_sig.empty() || second_sig.empty()) throw std::runtime_error("Internal Error: Invalid signature search arguments.");

    const std::size_t overlap = std::max(first_sig.size(), second_sig.size()) > 1
        ? std::max(first_sig.size(), second_sig.size()) - 1
        : 0;
    DualSignatureLocations locations{};

    scanFileWindows(path, overlap, search_limit, [&](std::span<const Byte> window, std::size_t base) {
        if (!locations.first) if (auto pos = searchSig(window, first_sig); pos) locations.first = base + *pos;
        if (!locations.second) if (auto pos = searchSig(window, second_sig); pos) locations.second = base + *pos;
        return locations.first.has_value() && locations.second.has_value();
    });

    return locations;
}

void copyBytesToOutput(std::ifstream& input, std::ofstream& output, std::size_t length) {
    constexpr std::size_t COPY_CHUNK_SIZE = 2 * 1024 * 1024;
    thread_local vBytes buffer(COPY_CHUNK_SIZE);

    std::size_t left = length;
    while (left > 0) {
        const std::size_t chunk = std::min(left, buffer.size());
        readExactOrThrow(input, buffer.data(), chunk, "Read Error: Failed while extracting encrypted payload.");
        writeBytesOrThrow(output, std::span<const Byte>(buffer.data(), chunk), WRITE_COMPLETE_ERROR);
        left -= chunk;
    }
}

void copyRangeToOutput(std::ifstream& input, std::ofstream& output, std::size_t offset, std::size_t length) {
    seekInputOrThrow(input, offset, std::ios::beg, "Read Error: Failed to seek payload position.");
    copyBytesToOutput(input, output, length);
}

[[nodiscard]] std::uint16_t readU16At(std::ifstream& input, std::size_t offset) { std::array<Byte, 2> bytes{}; readExactAt(input, offset, bytes); return static_cast<std::uint16_t>(getValue(bytes, 0)); }

[[nodiscard]] std::size_t streamDecodeBase64UntilDelimiterToOutput(std::ifstream& input, std::size_t offset, Byte delimiter, std::size_t max_bytes, std::size_t expected_decoded_size, std::ofstream& output, const char* corrupt_error) {
    if (expected_decoded_size == 0 || max_bytes == 0) throw std::runtime_error(corrupt_error);

    const std::size_t encoded_size = checkedMul(checkedAdd(expected_decoded_size, 2, corrupt_error) / 3, 4, corrupt_error);
    if (checkedAdd(encoded_size, 1, corrupt_error) > max_bytes) throw std::runtime_error(corrupt_error);

    seekInputOrThrow(input, offset, std::ios::beg, "Read Error: Failed to seek read position.");

    vBytes encoded(encoded_size);
    readExactOrThrow(input, encoded.data(), encoded.size(), "Read Error: Failed while scanning XMP payload.");

    Byte delimiter_byte = 0;
    if (!tryReadExact(input, &delimiter_byte, 1) || delimiter_byte != delimiter) throw std::runtime_error(corrupt_error);

    vBytes decoded;
    decoded.reserve(expected_decoded_size);
    try {
        appendBase64AsBinary(std::span<const Byte>(encoded), decoded);
    } catch (const std::exception&) {
        throw std::runtime_error(corrupt_error);
    }

    if (decoded.size() != expected_decoded_size) throw std::runtime_error(corrupt_error);

    writeBytesOrThrow(output, decoded, WRITE_COMPLETE_ERROR);
    return decoded.size();
}

}

[[nodiscard]] std::optional<std::size_t> findSignatureInFile(const fs::path& path, std::span<const Byte> sig, std::size_t search_limit, std::size_t start_offset) {
    if (sig.empty() || (search_limit != 0 && search_limit <= start_offset)) return std::nullopt;

    std::optional<std::size_t> found{};
    const std::size_t overlap = (sig.size() > 1) ? sig.size() - 1 : 0;

    scanFileWindows(path, overlap, search_limit, [&](std::span<const Byte> window, std::size_t base) {
        if (auto pos = searchSig(window, sig); pos) { found = base + *pos; return true; }
        return false;
    }, start_offset);

    return found;
}

[[nodiscard]] std::size_t extractDefaultCiphertextToFile(const fs::path& image_path, std::size_t image_size, std::size_t base_offset, std::size_t embedded_file_size, std::uint16_t total_profile_header_segments, const fs::path& output_path) {
    if (base_offset > image_size ||
        ICC_CIPHER_LAYOUT.encrypted_payload_start_index > image_size - base_offset) {
        throw std::runtime_error("File Extraction Error: Embedded data file is corrupt!");
    }
    const std::size_t payload_start = base_offset + ICC_CIPHER_LAYOUT.encrypted_payload_start_index;
    if (embedded_file_size > image_size - payload_start) throw std::runtime_error("File Extraction Error: Embedded data file is corrupt!");

    std::ifstream input = openBinaryInputOrThrow(image_path, "Read Error: Failed to open image file.");
    std::ofstream output = openBinaryOutputForWriteOrThrow(output_path);

    if (total_profile_header_segments) {
        const auto marker_index = iccTrailingMarkerIndex(total_profile_header_segments);
        if (!marker_index) throw std::runtime_error("File Extraction Error: Embedded data file is corrupt!");
        if (*marker_index > image_size - base_offset || image_size - base_offset - *marker_index < 2) throw std::runtime_error("File Extraction Error: Embedded data file is corrupt!");

        std::array<Byte, 2> marker{};
        readExactAt(input, base_offset + *marker_index, marker);
        if (!std::equal(JPEG_APP2_MARKER.begin(), JPEG_APP2_MARKER.end(), marker.begin())) throw std::runtime_error("File Extraction Error: Missing segments detected. Embedded data file is corrupt!");
    }

    const bool has_profile_headers = (total_profile_header_segments != 0);
    seekInputOrThrow(input, payload_start, std::ios::beg, "Read Error: Failed to seek payload position.");

    std::size_t cursor = 0;
    std::size_t next_header = ICC_SEGMENT_LAYOUT.profile_header_insert_index;
    std::size_t written = 0;

    while (cursor < embedded_file_size) {
        if (has_profile_headers && cursor == next_header) {
            const std::size_t skip = std::min(ICC_SEGMENT_LAYOUT.profile_header_length, embedded_file_size - cursor);
            seekInputOrThrow(input, skip, std::ios::cur, "Read Error: Failed while skipping ICC profile header bytes.");
            cursor += skip;
            if (next_header > std::numeric_limits<std::size_t>::max() - ICC_SEGMENT_LAYOUT.per_segment_stride) break;
            next_header += ICC_SEGMENT_LAYOUT.per_segment_stride;
            continue;
        }

        const std::size_t next_cut = has_profile_headers ? std::min(embedded_file_size, next_header) : embedded_file_size;
        const std::size_t run = next_cut - cursor;

        copyBytesToOutput(input, output, run);
        cursor += run;
        written += run;
    }

    flushOutputOrThrow(output, WRITE_COMPLETE_ERROR);

    return written;
}

[[nodiscard]] std::size_t extractBlueskyCiphertextToFile(const fs::path& image_path, std::size_t image_size, std::size_t embedded_file_size, const fs::path& output_path) {
    constexpr const char* CORRUPT_FILE_ERROR = "File Extraction Error: Embedded data file is corrupt!";

    if (embedded_file_size == 0 || BLUESKY_CIPHER_LAYOUT.encrypted_payload_start_index > image_size) throw std::runtime_error(CORRUPT_FILE_ERROR);
    const std::size_t exif_chunk_size = std::min(embedded_file_size, BLUESKY_SEGMENT_LAYOUT.exif_segment_data_size_limit);
    if (exif_chunk_size > image_size - BLUESKY_CIPHER_LAYOUT.encrypted_payload_start_index) throw std::runtime_error(CORRUPT_FILE_ERROR);

    std::ifstream input = openBinaryInputOrThrow(image_path, "Read Error: Failed to open image file.");
    std::ofstream output = openBinaryOutputForWriteOrThrow(output_path);

    std::size_t written = 0;
    copyRangeToOutput(input, output, BLUESKY_CIPHER_LAYOUT.encrypted_payload_start_index, exif_chunk_size);
    written += exif_chunk_size;

    std::size_t remaining = embedded_file_size - exif_chunk_size;
    const auto copy_dataset = [&](std::size_t dataset_size_index) -> std::uint16_t {
        const std::size_t dataset_file_index = dataset_size_index + BLUESKY_SEGMENT_LAYOUT.dataset_file_index_diff;
        if (dataset_file_index > image_size || 2 > image_size - dataset_size_index) throw std::runtime_error(CORRUPT_FILE_ERROR);
        const std::uint16_t dataset_size = readU16At(input, dataset_size_index);
        if (dataset_size == 0 || dataset_size > remaining || dataset_size > image_size - dataset_file_index) throw std::runtime_error(CORRUPT_FILE_ERROR);
        copyRangeToOutput(input, output, dataset_file_index, dataset_size);
        written += dataset_size;
        remaining -= dataset_size;
        return dataset_size;
    };
    const auto finish_if_complete = [&]() -> bool { if (remaining != 0) return false; flushOutputOrThrow(output, WRITE_COMPLETE_ERROR); return true; };
    if (finish_if_complete()) return written;

    const DualSignatureLocations tail_sigs = findDualSignaturesInFile(image_path, BLUESKY_PHOTOSHOP_SIGNATURE, BLUESKY_XMP_CREATOR_SIGNATURE);
    if (!tail_sigs.first) throw std::runtime_error(CORRUPT_FILE_ERROR);

    const std::size_t pshop_sig_index = *tail_sigs.first;
    if (pshop_sig_index < BLUESKY_SEGMENT_LAYOUT.pshop_segment_size_index_diff) throw std::runtime_error(CORRUPT_FILE_ERROR);

    const std::size_t pshop_segment_size_index = pshop_sig_index - BLUESKY_SEGMENT_LAYOUT.pshop_segment_size_index_diff;
    const std::size_t first_dataset_size_index = pshop_sig_index + BLUESKY_SEGMENT_LAYOUT.first_dataset_size_index_diff;
    const std::size_t first_dataset_file_index = first_dataset_size_index + BLUESKY_SEGMENT_LAYOUT.dataset_file_index_diff;
    if (pshop_segment_size_index > image_size || 2 > image_size - pshop_segment_size_index || first_dataset_file_index > image_size || 2 > image_size - first_dataset_size_index) throw std::runtime_error(CORRUPT_FILE_ERROR);

    const std::uint16_t pshop_segment_size = readU16At(input, pshop_segment_size_index), first_dataset_size = copy_dataset(first_dataset_size_index);
    if (finish_if_complete()) return written;

    if (pshop_segment_size <= BLUESKY_SEGMENT_LAYOUT.dataset_max_size || first_dataset_file_index > std::numeric_limits<std::size_t>::max() - first_dataset_size - BLUESKY_SEGMENT_LAYOUT.second_dataset_size_index_diff) throw std::runtime_error(CORRUPT_FILE_ERROR);
    const std::size_t second_dataset_size_index = first_dataset_file_index + first_dataset_size + BLUESKY_SEGMENT_LAYOUT.second_dataset_size_index_diff;
    copy_dataset(second_dataset_size_index);
    if (finish_if_complete()) return written;

    if (!tail_sigs.second) throw std::runtime_error(CORRUPT_FILE_ERROR);

    const std::size_t base64_begin_index = *tail_sigs.second + BLUESKY_XMP_CREATOR_SIGNATURE.size() + 1;
    if (base64_begin_index > image_size) throw std::runtime_error(CORRUPT_FILE_ERROR);

    written += streamDecodeBase64UntilDelimiterToOutput(input, base64_begin_index, BLUESKY_SEGMENT_LAYOUT.base64_end_sig, BLUESKY_SEGMENT_LAYOUT.xmp_base64_max_scan_bytes, remaining, output, CORRUPT_FILE_ERROR);
    flushOutputOrThrow(output, WRITE_COMPLETE_ERROR);
    if (written != embedded_file_size) throw std::runtime_error(CORRUPT_FILE_ERROR);

    return written;
}
