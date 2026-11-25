#include "encryption.h"
#include "encryption_internal.h"
#include "base64.h"
#include "binary_io.h"
#include "embedded_layout.h"
#include "file_utils.h"
#include "template_assets.h"

#include <algorithm>
#include <cstring>
#include <span>

namespace {

constexpr auto XMP_FOOTER = std::to_array<Byte>({
    0x3C, 0x2F, 0x72, 0x64, 0x66, 0x3A, 0x6C, 0x69, 0x3E, 0x3C, 0x2F, 0x72, 0x64, 0x66, 0x3A, 0x53,
    0x65, 0x71, 0x3E, 0x3C, 0x2F, 0x64, 0x63, 0x3A, 0x63, 0x72, 0x65, 0x61, 0x74, 0x6F, 0x72, 0x3E,
    0x3C, 0x2F, 0x72, 0x64, 0x66, 0x3A, 0x44, 0x65, 0x73, 0x63, 0x72, 0x69, 0x70, 0x74, 0x69, 0x6F,
    0x6E, 0x3E, 0x3C, 0x2F, 0x72, 0x64, 0x66, 0x3A, 0x52, 0x44, 0x46, 0x3E, 0x3C, 0x2F, 0x78, 0x3A,
    0x78, 0x6D, 0x70, 0x6D, 0x65, 0x74, 0x61, 0x3E, 0x0A, 0x3C, 0x3F, 0x78, 0x70, 0x61, 0x63, 0x6B,
    0x65, 0x74, 0x20, 0x65, 0x6E, 0x64, 0x3D, 0x22, 0x77, 0x22, 0x3F, 0x3E
});
static_assert(XMP_FOOTER.size() == BLUESKY_SEGMENT_LAYOUT.xmp_footer_size);

constexpr auto DATASET_MARKER_BASE = std::to_array<Byte>({0x1C, 0x08, 0x0A});

constexpr std::size_t FIRST_MARKER_BYTES_SIZE = 4;
constexpr std::size_t SEGMENT_MARKER_BYTES_SIZE = 2;
constexpr std::size_t DATASET_MARKER_BYTES_SIZE = DATASET_MARKER_BASE.size() + 2;
constexpr std::size_t VALUE_BYTE_LENGTH = 4;

constexpr const char* SIZE_OVERFLOW_ERROR = "File Size Error: Bluesky segment size overflow.";

[[nodiscard]] vBytes makePhotoshopSegmentTemplate() {
    std::span<const Byte> template_bytes = photoshopSegmentTemplateBytes();
    throwIf(
        template_bytes.size() != BLUESKY_SEGMENT_LAYOUT.photoshop_default_size,
        "Internal Error: Corrupt Bluesky Photoshop segment template.");
    return copyTemplateBytes(template_bytes);
}

[[nodiscard]] vBytes makeXmpSegmentTemplate() {
    std::span<const Byte> template_bytes = xmpSegmentTemplateBytes();
    throwIf(
        template_bytes.empty() || template_bytes.size() > BLUESKY_SEGMENT_LAYOUT.xmp_segment_size_limit,
        "Internal Error: Corrupt Bluesky XMP segment template.");
    return copyTemplateBytes(template_bytes);
}

void appendBytes(vBytes& output, std::span<const Byte> bytes) {
    if (bytes.empty()) return;

    const std::size_t base = output.size();
    const std::size_t final_size = checkedAdd(base, bytes.size(), SIZE_OVERFLOW_ERROR);

    output.resize(final_size);
    std::memcpy(output.data() + static_cast<std::ptrdiff_t>(base), bytes.data(), bytes.size());
}

// The Photoshop segment as built: template, first dataset, then a marker and
// the second dataset when there is one.
[[nodiscard]] std::size_t photoshopSegmentSize(const BlueskySegmentSplit& split) {
    std::size_t size = checkedAdd(photoshopSegmentTemplateBytes().size(), split.first_dataset, SIZE_OVERFLOW_ERROR);
    if (split.second_dataset != 0) {
        size = checkedAdd(
            checkedAdd(size, DATASET_MARKER_BYTES_SIZE, SIZE_OVERFLOW_ERROR),
            split.second_dataset,
            SIZE_OVERFLOW_ERROR);
    }
    return size;
}

// The XMP segment as built: template, the overflow in base64, footer.
[[nodiscard]] std::size_t xmpSegmentSize(const BlueskySegmentSplit& split) {
    return checkedAdd(
        checkedAdd(xmpSegmentTemplateBytes().size(), base64EncodedSize(split.xmp), SIZE_OVERFLOW_ERROR),
        XMP_FOOTER.size(),
        SIZE_OVERFLOW_ERROR);
}

[[nodiscard]] std::size_t blueskySegmentOutputSize(std::size_t template_size, const BlueskySegmentSplit& split) {
    std::size_t size = checkedAdd(template_size, split.exif, SIZE_OVERFLOW_ERROR);
    if (split.usesPhotoshop()) size = checkedAdd(size, photoshopSegmentSize(split), SIZE_OVERFLOW_ERROR);
    if (split.usesXmp()) size = checkedAdd(size, xmpSegmentSize(split), SIZE_OVERFLOW_ERROR);
    return size;
}

void insertExifPayload(vBytes& segment_vec, std::span<const Byte> exif_payload) {
    segment_vec.insert(
        segment_vec.begin() + static_cast<std::ptrdiff_t>(BLUESKY_SEGMENT_LAYOUT.exif_segment_data_insert_index),
        exif_payload.begin(),
        exif_payload.end());
}

void appendDatasetMarker(vBytes& pshop_vec, std::size_t dataset_size) {
    appendBytes(pshop_vec, DATASET_MARKER_BASE);
    pshop_vec.emplace_back(static_cast<Byte>((dataset_size >> 8) & 0xFF));
    pshop_vec.emplace_back(static_cast<Byte>(dataset_size & 0xFF));
}

// The ciphertext past the EXIF share: the Photoshop datasets, then (when
// there is more) the XMP overflow, appended XMP first, then Photoshop.
// `overflow` is exactly what the split puts in those segments.
void appendOverflowPayloadSegments(
    vBytes& segment_vec,
    std::span<const Byte> overflow,
    const BlueskySegmentSplit& split) {

    if (split.usesXmp()) {
        vBytes xmp_vec = makeXmpSegmentTemplate();
        xmp_vec.reserve(xmpSegmentSize(split));
        binaryToBase64(overflow.subspan(split.first_dataset + split.second_dataset), xmp_vec);
        appendBytes(xmp_vec, XMP_FOOTER);
        updateValue(xmp_vec, BLUESKY_SEGMENT_LAYOUT.segment_size_index, xmp_vec.size() - SEGMENT_MARKER_BYTES_SIZE);
        appendBytes(segment_vec, xmp_vec);
    }

    vBytes pshop_vec = makePhotoshopSegmentTemplate();
    pshop_vec.reserve(photoshopSegmentSize(split));
    updateValue(pshop_vec, BLUESKY_SEGMENT_LAYOUT.first_dataset_size_index, split.first_dataset);
    appendBytes(pshop_vec, overflow.first(split.first_dataset));
    if (split.second_dataset != 0) {
        appendDatasetMarker(pshop_vec, split.second_dataset);
        appendBytes(pshop_vec, overflow.subspan(split.first_dataset, split.second_dataset));
    }
    const std::size_t pshop_segment_data_size = pshop_vec.size() - SEGMENT_MARKER_BYTES_SIZE;
    updateValue(pshop_vec, BLUESKY_SEGMENT_LAYOUT.segment_size_index, pshop_segment_data_size);
    updateValue(
        pshop_vec,
        BLUESKY_SEGMENT_LAYOUT.bim_section_size_index,
        pshop_segment_data_size - BLUESKY_SEGMENT_LAYOUT.bim_section_size_diff);
    appendBytes(segment_vec, pshop_vec);
}

} // namespace

void buildBlueskySegments(vBytes& segment_vec, const vBytes& data_vec) {
    throwIf(segment_vec.size() < FIRST_MARKER_BYTES_SIZE ||
        BLUESKY_SEGMENT_LAYOUT.exif_segment_data_insert_index > segment_vec.size(),
        "Internal Error: Corrupt Bluesky segment template.");
    requireSpanRange(
        segment_vec,
        BLUESKY_SEGMENT_LAYOUT.compressed_file_size_index,
        VALUE_BYTE_LENGTH,
        "Internal Error: Corrupt Bluesky metadata index.");
    requireSpanRange(
        segment_vec,
        BLUESKY_SEGMENT_LAYOUT.exif_segment_size_index,
        2,
        "Internal Error: Corrupt Bluesky metadata index.");
    requireSpanRange(
        segment_vec,
        BLUESKY_SEGMENT_LAYOUT.artist_field_size_index,
        VALUE_BYTE_LENGTH,
        "Internal Error: Corrupt Bluesky metadata index.");

    const std::size_t encrypted_vec_size = data_vec.size();
    const BlueskySegmentSplit split = blueskySegmentSplit(encrypted_vec_size);
    // Refuse before touching segment_vec: the XMP segment's final size is
    // known exactly from the split. (conceal checks the cap first; this
    // keeps the packer safe on its own.)
    throwIf(
        split.usesXmp() && xmpSegmentSize(split) > BLUESKY_SEGMENT_LAYOUT.xmp_segment_size_limit,
        "File Size Error: Data file exceeds segment size limit for Bluesky.");
    segment_vec.reserve(blueskySegmentOutputSize(segment_vec.size(), split));
    const std::size_t segment_vec_data_size = segment_vec.size() - FIRST_MARKER_BYTES_SIZE;
    const std::size_t exif_payload_size = split.exif;
    const std::size_t exif_segment_data_size = checkedAdd(
        exif_payload_size,
        segment_vec_data_size,
        SIZE_OVERFLOW_ERROR);
    throwIf(
        exif_segment_data_size < BLUESKY_SEGMENT_LAYOUT.artist_field_size_diff,
        "Internal Error: Corrupt Bluesky segment size.");
    const std::size_t artist_field_size = exif_segment_data_size - BLUESKY_SEGMENT_LAYOUT.artist_field_size_diff;

    updateValue(segment_vec, BLUESKY_SEGMENT_LAYOUT.compressed_file_size_index, encrypted_vec_size, VALUE_BYTE_LENGTH);

    updateValue(segment_vec, BLUESKY_SEGMENT_LAYOUT.artist_field_size_index, artist_field_size, VALUE_BYTE_LENGTH);
    updateValue(segment_vec, BLUESKY_SEGMENT_LAYOUT.exif_segment_size_index, exif_segment_data_size);
    const std::span<const Byte> payload(data_vec);
    insertExifPayload(segment_vec, payload.first(exif_payload_size));

    if (!split.usesPhotoshop()) return;
    appendOverflowPayloadSegments(segment_vec, payload.subspan(exif_payload_size), split);
}
