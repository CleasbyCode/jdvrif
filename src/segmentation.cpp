#include "segmentation.h"
#include "binary_io.h"
#include "embedded_layout.h"
#include "file_utils.h"
#include "signal_utils.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <string_view>


namespace {
constexpr std::size_t
    SOI_SIG_LENGTH           = 2,
    SEGMENT_SIG_LENGTH       = 2,
    SEGMENT_HEADER_LENGTH    = ICC_SEGMENT_LAYOUT.segment_header_length,
    SEGMENT_DATA_SIZE        = ICC_SEGMENT_LAYOUT.per_segment_payload_size,
    INITIAL_HEADER_BYTES     = ICC_SEGMENT_LAYOUT.initial_header_bytes,
    PROFILE_DATA_SIZE        = ICC_SEGMENT_LAYOUT.profile_data_size,
    PROFILE_SIZE_DIFF        = ICC_SEGMENT_LAYOUT.profile_size_diff,
    VALUE_BYTE_LENGTH        = 4,
    MAX_SINGLE_SEGMENT_SIZE  = SEGMENT_DATA_SIZE + SOI_SIG_LENGTH + SEGMENT_SIG_LENGTH + SEGMENT_HEADER_LENGTH;

constexpr const char* SIZE_OVERFLOW_ERROR = "File Size Error: Segment output size overflow.";

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

void writeOutput(OutputFile& output, std::span<const Byte> bytes) {
    output.write(bytes, WRITE_COMPLETE_ERROR);
}

// payload_size is nonzero: iccLayout only gets here when the data overflows
// a single segment.
[[nodiscard]] std::size_t requiredIccSegments(std::size_t payload_size) {
    const std::size_t segments_required = 1 + ((payload_size - 1) / SEGMENT_DATA_SIZE);
    throwIf(
        segments_required > static_cast<std::size_t>(std::numeric_limits<uint16_t>::max()),
        "File Size Error: Segment count exceeds supported limit.");
    return segments_required;
}

// How the template and payload are laid out ahead of the cover. The one place
// that sizes it, shared by the up-front size check and the writer.
struct IccLayout {
    bool single_segment{false};
    std::size_t payload_size{0};      // Multi-segment: template tail + payload.
    std::size_t segments_required{0}; // Multi-segment only.
    std::size_t icc_data_size{0};     // Bytes written before the cover.
};

[[nodiscard]] IccLayout iccLayout(std::size_t template_size, std::size_t encrypted_size) {
    // begin() rewrites header fields up to PROFILE_DATA_SIZE and subtracts it
    // from the layout's sizes, so a shorter template cannot be laid out.
    throwIf(template_size < std::max(INITIAL_HEADER_BYTES, PROFILE_DATA_SIZE),
            "File Extraction Error: Corrupt segment header.");

    const std::size_t single_segment_size = checkedAdd(template_size, encrypted_size, SIZE_OVERFLOW_ERROR);
    if (single_segment_size <= MAX_SINGLE_SEGMENT_SIZE) {
        return IccLayout{.single_segment = true, .icc_data_size = single_segment_size};
    }

    const std::size_t payload_size =
        checkedAdd(template_size - INITIAL_HEADER_BYTES, encrypted_size, SIZE_OVERFLOW_ERROR);
    const std::size_t segments_required = requiredIccSegments(payload_size);
    const std::size_t total_header_bytes = checkedMul(
        segments_required,
        SEGMENT_SIG_LENGTH + SEGMENT_HEADER_LENGTH,
        SIZE_OVERFLOW_ERROR);
    return IccLayout{
        .payload_size = payload_size,
        .segments_required = segments_required,
        .icc_data_size = checkedAdd(
            SOI_SIG_LENGTH,
            checkedAdd(payload_size, total_header_bytes, SIZE_OVERFLOW_ERROR),
            SIZE_OVERFLOW_ERROR),
    };
}

} // namespace

std::size_t embeddedJpgSize(std::size_t template_size, std::size_t encrypted_size, std::size_t jpg_size) {
    return checkedAdd(
        iccLayout(template_size, encrypted_size).icc_data_size,
        jpg_size,
        "File Size Error: Embedded image size overflow.");
}

vString compatiblePlatforms(const SegmentedEmbedSummary& summary) {
    struct PlatformLimits {
        std::string_view name;
        std::size_t max_image_size;
        std::size_t max_first_segment;
        uint16_t max_segments;
    };
    static constexpr auto PLATFORM_LIMITS = std::to_array<PlatformLimits>({
        {"X-Twitter",  5   * 1024 * 1024,  10 * 1024,  UINT16_MAX},
        {"Tumblr",     SIZE_MAX,           65534,      UINT16_MAX},
        {"Mastodon",   16  * 1024 * 1024,  SIZE_MAX,   100       },
        {"Pixelfed",   15  * 1024 * 1024,  SIZE_MAX,   UINT16_MAX},
        {"PostImage",  32  * 1024 * 1024,  SIZE_MAX,   UINT16_MAX},
        {"ImgBB",      32  * 1024 * 1024,  SIZE_MAX,   UINT16_MAX},
        {"ImgPile",    100 * 1024 * 1024,  SIZE_MAX,   UINT16_MAX},
        {"Flickr",     200 * 1024 * 1024,  SIZE_MAX,   UINT16_MAX},
    });

    vString platforms;
    for (const auto& [name, max_img, max_seg, max_segs] : PLATFORM_LIMITS) {
        if (summary.embedded_image_size <= max_img
            && summary.first_segment_size <= max_seg
            && summary.total_segments <= max_segs) {
            platforms.emplace_back(name);
        }
    }
    return platforms;
}

EmbeddedJpgWriter::EmbeddedJpgWriter(
    OutputFile& output,
    vBytes& segment_vec,
    std::size_t encrypted_size,
    std::span<const Byte> leading_segments)
    : output_(output),
      segment_vec_(segment_vec),
      leading_segments_(leading_segments),
      encrypted_size_(encrypted_size),
      encrypted_left_(encrypted_size) {}

void EmbeddedJpgWriter::begin() {
    throwIf(begun_, "Internal Error: Embedded image output started twice.");
    begun_ = true;

    const IccLayout layout = iccLayout(segment_vec_.size(), encrypted_size_);
    single_segment_ = layout.single_segment;

    if (single_segment_) {
        const std::size_t single_segment_size = layout.icc_data_size;
        const std::size_t segment_size = single_segment_size - (SOI_SIG_LENGTH + SEGMENT_SIG_LENGTH);
        const std::size_t profile_size = segment_size - PROFILE_SIZE_DIFF;

        updateValue(segment_vec_, ICC_SEGMENT_LAYOUT.segment_header_size_index, segment_size);
        updateValue(segment_vec_, ICC_SEGMENT_LAYOUT.profile_size_index, profile_size);
        updateValue(segment_vec_, ICC_SEGMENT_LAYOUT.encrypted_file_size_index, single_segment_size - PROFILE_DATA_SIZE, VALUE_BYTE_LENGTH);

        summary_ = SegmentedEmbedSummary{
            .embedded_image_size = single_segment_size,
            .first_segment_size = static_cast<uint16_t>(segment_size),
            .total_segments = 0,
        };
    } else {
        payload_left_ = layout.payload_size;
        segments_required_ = layout.segments_required;

        updateValue(segment_vec_, ICC_SEGMENT_LAYOUT.template_total_profile_header_segments_index, segments_required_);
        updateValue(segment_vec_, ICC_SEGMENT_LAYOUT.encrypted_file_size_index, layout.icc_data_size - PROFILE_DATA_SIZE, VALUE_BYTE_LENGTH);

        summary_ = SegmentedEmbedSummary{
            .embedded_image_size = layout.icc_data_size,
            .first_segment_size = static_cast<uint16_t>(std::min(payload_left_, SEGMENT_DATA_SIZE) + SEGMENT_HEADER_LENGTH),
            .total_segments = static_cast<uint16_t>(segments_required_),
        };
    }

    const std::span<const Byte> segment_template(segment_vec_);
    writeOutput(output_, segment_template.first(SOI_SIG_LENGTH));
    writeOutput(output_, leading_segments_);
    if (single_segment_) {
        // The template's own segment takes the whole payload, which follows
        // it without any further header.
        writeOutput(output_, segment_template.subspan(SOI_SIG_LENGTH));
    } else {
        // The payload stream opens with the template's bytes beyond its
        // header; the ciphertext follows as it arrives.
        appendPayload(segment_template.subspan(INITIAL_HEADER_BYTES));
    }
}

// Multi-segment only: each segment is an 18-byte ICC header followed by its
// data portion. Small pieces coalesce in OutputFile's buffer.
void EmbeddedJpgWriter::appendPayload(std::span<const Byte> bytes) {
    while (!bytes.empty()) {
        if (segment_room_ == 0) {
            throwIf(
                payload_left_ == 0 || segment_index_ >= segments_required_,
                "Read Error: Encrypted payload is longer than expected.");
            throwIfSignalCancellationRequested();
            ++segment_index_;
            segment_room_ = std::min(payload_left_, SEGMENT_DATA_SIZE);
            const auto header = makeIccHeader(
                static_cast<uint16_t>(segment_room_ + SEGMENT_HEADER_LENGTH), segment_index_);
            writeOutput(output_, std::span<const Byte>(header));
        }
        const std::size_t take = std::min(bytes.size(), segment_room_);
        writeOutput(output_, bytes.first(take));
        bytes = bytes.subspan(take);
        segment_room_ -= take;
        payload_left_ -= take;
    }
}

void EmbeddedJpgWriter::appendEncrypted(std::span<const Byte> bytes) {
    throwIf(!begun_, "Internal Error: Embedded image output not started.");
    throwIf(bytes.size() > encrypted_left_, "Read Error: Encrypted payload is longer than expected.");
    encrypted_left_ -= bytes.size();
    if (single_segment_) {
        writeOutput(output_, bytes);
    } else {
        appendPayload(bytes);
    }
}

SegmentedEmbedSummary EmbeddedJpgWriter::finish(std::span<const Byte> jpg_vec) {
    throwIf(!begun_, "Internal Error: Embedded image output not started.");
    throwIf(
        encrypted_left_ != 0 || payload_left_ != 0 || segment_room_ != 0,
        "Read Error: Encrypted payload size mismatch.");

    writeOutput(output_, jpg_vec);
    SegmentedEmbedSummary summary = summary_;
    summary.embedded_image_size = checkedAdd(
        summary.embedded_image_size,
        checkedAdd(leading_segments_.size(), jpg_vec.size(), "File Size Error: Embedded image size overflow."),
        "File Size Error: Embedded image size overflow.");
    return summary;
}
