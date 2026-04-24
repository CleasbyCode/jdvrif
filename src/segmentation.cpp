#include "segmentation.h"
#include "binary_io.h"
#include "embedded_layout.h"
#include "file_utils.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <stdexcept>
#include <utility>

namespace {
constexpr std::size_t SOI_SIG_LENGTH = 2, EOI_SIG_LENGTH = 2, SEGMENT_SIG_LENGTH = 2, SEGMENT_HEADER_LENGTH = ICC_SEGMENT_LAYOUT.segment_header_length,
                      SEGMENT_DATA_SIZE = ICC_SEGMENT_LAYOUT.per_segment_payload_size, INITIAL_HEADER_BYTES = ICC_SEGMENT_LAYOUT.initial_header_bytes,
                      PROFILE_DATA_SIZE = ICC_SEGMENT_LAYOUT.profile_data_size, PROFILE_SIZE_DIFF = ICC_SEGMENT_LAYOUT.profile_size_diff, VALUE_BYTE_LENGTH = 4,
                      MAX_SINGLE_SEGMENT_SIZE = SEGMENT_DATA_SIZE + SOI_SIG_LENGTH + SEGMENT_SIG_LENGTH + SEGMENT_HEADER_LENGTH,
                      PAYLOAD_COPY_CHUNK_SIZE = 2 * 1024 * 1024;

constexpr Byte PADDING_START = 33, PADDING_RANGE = 94;

constexpr std::size_t PADDING_SIZE = 8000;
constexpr std::size_t REDDIT_PADDING_TOTAL = 4 + PADDING_SIZE;
constexpr const char* WRITE_COMPLETE_ERROR = "Write Error: Failed to write complete output file.";

constexpr auto ICC_HEADER_TEMPLATE = std::to_array<Byte>({
    0xFF, 0xE2, 0x00, 0x00,
    'I','C','C','_','P','R','O','F','I','L','E',
    0x00, 0x00, 0x01
});

[[nodiscard]] std::array<Byte, 18> makeIccHeader(uint16_t segment_length, std::size_t segment_index) {
    auto header = ICC_HEADER_TEMPLATE;
    header[2] = static_cast<Byte>(segment_length >> 8);
    header[3] = static_cast<Byte>(segment_length & 0xFF);
    header[15] = static_cast<Byte>(segment_index >> 8);
    header[16] = static_cast<Byte>(segment_index & 0xFF);
    return header;
}

struct PayloadSource {
    std::span<const Byte> template_payload{};
    std::ifstream encrypted_input{};
    std::size_t template_offset{0};
    std::size_t encrypted_left{0};

    PayloadSource(std::span<const Byte> template_payload_arg, const fs::path& encrypted_path, std::size_t encrypted_size)
        : template_payload(template_payload_arg), encrypted_input(openBinaryInputOrThrow(encrypted_path, "Read Error: Failed to open encrypted payload.")), encrypted_left(encrypted_size) {}

    void readExact(Byte* dst, std::size_t size) {
        std::size_t copied = 0;
        if (template_offset < template_payload.size()) {
            const std::size_t take = std::min(size, template_payload.size() - template_offset);
            std::memcpy(dst, template_payload.data() + static_cast<std::ptrdiff_t>(template_offset), take);
            template_offset += take;
            copied = take;
        }

        const std::size_t remaining = size - copied;
        if (remaining > 0) {
            if (remaining > encrypted_left) throw std::runtime_error("Read Error: Encrypted payload is shorter than expected.");
            readExactOrThrow(encrypted_input, dst + static_cast<std::ptrdiff_t>(copied), remaining, "Read Error: Failed while reading encrypted payload.");
            encrypted_left -= remaining;
        }
    }
    [[nodiscard]] bool exhausted() const noexcept { return template_offset == template_payload.size() && encrypted_left == 0; }
};

void writeOutput(std::ostream& output, std::span<const Byte> bytes) { writeBytesOrThrow(output, bytes, WRITE_COMPLETE_ERROR); }

void copyPayloadToOutput(PayloadSource& payload_source, std::ostream& output, std::size_t length) {
    thread_local vBytes buffer(PAYLOAD_COPY_CHUNK_SIZE);

    std::size_t left = length;
    while (left > 0) {
        const std::size_t chunk = std::min(left, buffer.size());
        payload_source.readExact(buffer.data(), chunk);
        writeOutput(output, std::span<const Byte>(buffer.data(), chunk));
        left -= chunk;
    }
}

void copyFileToOutput(const fs::path& input_path, std::ostream& output, std::size_t length) {
    PayloadSource payload_source(std::span<const Byte>{}, input_path, length);
    copyPayloadToOutput(payload_source, output, length);
    if (!payload_source.exhausted()) throw std::runtime_error("Read Error: Encrypted payload is longer than expected.");
}

void writeRedditPaddingToOutput(std::ostream& output) {
    constexpr auto PADDING_PREFIX = std::to_array<Byte>({0xFF, 0xE2, 0x1F, 0x42});
    writeOutput(output, PADDING_PREFIX);

    std::array<Byte, 1024> padding_chunk{};
    std::size_t remaining = PADDING_SIZE;
    while (remaining > 0) {
        const std::size_t chunk = std::min(remaining, padding_chunk.size());
        for (std::size_t i = 0; i < chunk; ++i) {
            padding_chunk[i] = PADDING_START + static_cast<Byte>(randombytes_uniform(PADDING_RANGE));
        }
        writeOutput(output, std::span<const Byte>(padding_chunk.data(), chunk));
        remaining -= chunk;
    }
}

[[nodiscard]] std::size_t requiredIccSegments(std::size_t payload_size) {
    const std::size_t segments_required = payload_size / SEGMENT_DATA_SIZE + ((payload_size % SEGMENT_DATA_SIZE) != 0 ? 1 : 0);
    if (segments_required == 0 || segments_required > static_cast<std::size_t>(std::numeric_limits<uint16_t>::max())) {
        throw std::runtime_error("File Size Error: Segment count exceeds supported limit.");
    }
    return segments_required;
}

[[nodiscard]] SegmentedEmbedSummary writeIccDataToOutput(std::ostream& output, vBytes& segment_vec, const fs::path& encrypted_path, std::size_t encrypted_size, bool omit_soi) {
    if (segment_vec.size() < INITIAL_HEADER_BYTES) throw std::runtime_error("File Extraction Error: Corrupt segment header.");

    const std::size_t single_segment_size = checkedAdd(segment_vec.size(), encrypted_size, "File Size Error: Segment output size overflow.");

    if (single_segment_size <= MAX_SINGLE_SEGMENT_SIZE) {
        const std::size_t segment_size = single_segment_size - (SOI_SIG_LENGTH + SEGMENT_SIG_LENGTH);
        const std::size_t profile_size = segment_size - PROFILE_SIZE_DIFF;

        updateValue(segment_vec, ICC_SEGMENT_LAYOUT.segment_header_size_index, segment_size);
        updateValue(segment_vec, ICC_SEGMENT_LAYOUT.profile_size_index, profile_size);
        updateValue(segment_vec, ICC_SEGMENT_LAYOUT.encrypted_file_size_index, single_segment_size - PROFILE_DATA_SIZE, VALUE_BYTE_LENGTH);

        const std::span<const Byte> leading = omit_soi ? std::span<const Byte>(segment_vec).subspan(SOI_SIG_LENGTH) : std::span<const Byte>(segment_vec);
        writeOutput(output, leading);
        copyFileToOutput(encrypted_path, output, encrypted_size);

        return SegmentedEmbedSummary{
            .embedded_image_size = single_segment_size,
            .first_segment_size = static_cast<uint16_t>(segment_size),
            .total_segments = 0
        };
    }

    const std::size_t payload_prefix_size = segment_vec.size() - INITIAL_HEADER_BYTES;
    const std::size_t payload_size = checkedAdd(payload_prefix_size, encrypted_size, "File Size Error: Segment output size overflow.");
    const std::size_t segments_required = requiredIccSegments(payload_size);

    updateValue(segment_vec, ICC_SEGMENT_LAYOUT.template_total_profile_header_segments_index, segments_required);

    const std::size_t total_header_bytes = checkedMul(segments_required, SEGMENT_SIG_LENGTH + SEGMENT_HEADER_LENGTH, "File Size Error: Segment output size overflow.");
    const std::size_t icc_data_size = checkedAdd(SOI_SIG_LENGTH, checkedAdd(payload_size, total_header_bytes, "File Size Error: Segment output size overflow."), "File Size Error: Segment output size overflow.");
    updateValue(segment_vec, ICC_SEGMENT_LAYOUT.encrypted_file_size_index, icc_data_size - PROFILE_DATA_SIZE, VALUE_BYTE_LENGTH);

    if (!omit_soi) writeOutput(output, std::span<const Byte>(segment_vec.data(), SOI_SIG_LENGTH));

    PayloadSource payload_source(std::span<const Byte>(segment_vec.data() + INITIAL_HEADER_BYTES, payload_prefix_size), encrypted_path, encrypted_size);

    std::size_t payload_left = payload_size;
    for (std::size_t seg = 1; seg <= segments_required; ++seg) {
        const std::size_t data_size = std::min(payload_left, SEGMENT_DATA_SIZE);
        const auto header = makeIccHeader(static_cast<uint16_t>(data_size + SEGMENT_HEADER_LENGTH), seg);
        writeOutput(output, header);
        copyPayloadToOutput(payload_source, output, data_size);
        payload_left -= data_size;
    }

    if (payload_left != 0 || !payload_source.exhausted()) throw std::runtime_error("Read Error: Encrypted payload size mismatch.");

    return SegmentedEmbedSummary{
        .embedded_image_size = icc_data_size,
        .first_segment_size = static_cast<uint16_t>(std::min(payload_size, SEGMENT_DATA_SIZE) + SEGMENT_HEADER_LENGTH),
        .total_segments = static_cast<uint16_t>(segments_required)
    };
}
}

void filterPlatforms(vString& platforms_vec, std::size_t embedded_size, uint16_t first_segment_size, uint16_t total_segments) {
    std::erase_if(platforms_vec, [&](const std::string& platform) {
        for (const auto& [name, max_img, max_seg, max_segs] : PLATFORM_LIMITS) {
            if (platform == name) {
                return embedded_size > max_img
                    || first_segment_size > max_seg
                    || total_segments > max_segs;
            }
        }
        return false;
    });

    if (platforms_vec.empty()) {
        platforms_vec.emplace_back(
            "\b\bUnknown!\n\n Due to the large file size of the output JPG image, "
            "I'm unaware of any\n compatible platforms that this image can be "
            "posted on. Local use only?");
    }
}

[[nodiscard]] SegmentedEmbedSummary writeEmbeddedJpgFromEncryptedFile(
    std::ostream& output,
    vBytes& segment_vec,
    const fs::path& encrypted_path,
    std::span<const Byte> jpg_vec,
    bool has_reddit_option) {

    const std::size_t encrypted_size = checkedFileSize(
        encrypted_path,
        "Read Error: Invalid encrypted payload size.",
        true);

    if (!has_reddit_option) {
        SegmentedEmbedSummary summary = writeIccDataToOutput(output, segment_vec, encrypted_path, encrypted_size, false);
        writeOutput(output, jpg_vec);
        summary.embedded_image_size = checkedAdd(
            summary.embedded_image_size,
            jpg_vec.size(),
            "File Size Error: Embedded image size overflow.");
        return summary;
    }

    if (jpg_vec.size() < EOI_SIG_LENGTH || segment_vec.size() < SOI_SIG_LENGTH) {
        throw std::runtime_error("File Extraction Error: Corrupt image or data segment.");
    }

    writeOutput(output, std::span<const Byte>(segment_vec.data(), SOI_SIG_LENGTH));
    writeOutput(output, jpg_vec.first(jpg_vec.size() - EOI_SIG_LENGTH));
    writeRedditPaddingToOutput(output);

    SegmentedEmbedSummary summary = writeIccDataToOutput(output, segment_vec, encrypted_path, encrypted_size, true);
    writeOutput(output, jpg_vec.last(EOI_SIG_LENGTH));

    summary.embedded_image_size = checkedAdd(
        checkedAdd(jpg_vec.size(), REDDIT_PADDING_TOTAL, "File Size Error: Embedded image size overflow."),
        summary.embedded_image_size,
        "File Size Error: Embedded image size overflow.");
    return summary;
}
