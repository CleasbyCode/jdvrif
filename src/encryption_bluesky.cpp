#include "encryption.h"
#include "base64.h"
#include "binary_io.h"
#include "embedded_layout.h"
#include "file_utils.h"
#include "template_assets.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <stdexcept>

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

[[nodiscard]] std::size_t base64EncodedSize(std::size_t binary_size) {
    return checkedMul(checkedAdd(binary_size, 2, SIZE_OVERFLOW_ERROR) / 3, 4, SIZE_OVERFLOW_ERROR);
}

void appendBytes(vBytes& output, std::span<const Byte> bytes) {
    if (bytes.empty()) return;

    const std::size_t base = output.size();
    const std::size_t final_size = checkedAdd(base, bytes.size(), SIZE_OVERFLOW_ERROR);

    output.resize(final_size);
    std::memcpy(output.data() + static_cast<std::ptrdiff_t>(base), bytes.data(), bytes.size());
}

[[nodiscard]] std::size_t blueskySegmentOutputSize(std::size_t template_size, std::size_t encrypted_size) {
    const std::size_t exif_payload_size = std::min(
        encrypted_size,
        BLUESKY_SEGMENT_LAYOUT.exif_segment_data_size_limit);
    std::size_t output_size = checkedAdd(template_size, exif_payload_size, SIZE_OVERFLOW_ERROR);

    std::size_t remaining = encrypted_size - exif_payload_size;
    if (remaining == 0) return output_size;

    std::size_t pshop_size = photoshopSegmentTemplateBytes().size();
    const std::size_t first_copy_size = std::min(BLUESKY_SEGMENT_LAYOUT.first_dataset_size_limit, remaining);
    pshop_size = checkedAdd(pshop_size, first_copy_size, SIZE_OVERFLOW_ERROR);

    if (remaining > BLUESKY_SEGMENT_LAYOUT.first_dataset_size_limit) {
        remaining -= BLUESKY_SEGMENT_LAYOUT.first_dataset_size_limit;

        const std::size_t last_copy_size = std::min(BLUESKY_SEGMENT_LAYOUT.last_dataset_size_limit, remaining);
        pshop_size = checkedAdd(
            checkedAdd(pshop_size, DATASET_MARKER_BYTES_SIZE, SIZE_OVERFLOW_ERROR),
            last_copy_size,
            SIZE_OVERFLOW_ERROR);

        if (remaining > BLUESKY_SEGMENT_LAYOUT.last_dataset_size_limit) {
            remaining -= BLUESKY_SEGMENT_LAYOUT.last_dataset_size_limit;
            const std::size_t xmp_size = checkedAdd(
                checkedAdd(xmpSegmentTemplateBytes().size(), base64EncodedSize(remaining), SIZE_OVERFLOW_ERROR),
                XMP_FOOTER.size(),
                SIZE_OVERFLOW_ERROR);
            output_size = checkedAdd(output_size, xmp_size, SIZE_OVERFLOW_ERROR);
        }
    }

    if (pshop_size > BLUESKY_SEGMENT_LAYOUT.photoshop_default_size) {
        output_size = checkedAdd(output_size, pshop_size, SIZE_OVERFLOW_ERROR);
    }

    return output_size;
}

void appendDataRange(vBytes& output, const vBytes& data, std::size_t offset, std::size_t size) {
    const std::size_t end = checkedAdd(offset, size, "Internal Error: Bluesky data range overflow.");
    throwIf(end > data.size(), "Internal Error: Bluesky data range out of bounds.");

    appendBytes(output, std::span<const Byte>(data).subspan(offset, size));
}

void insertExifPayload(vBytes& segment_vec, const vBytes& data_vec, std::size_t exif_payload_size) {
    segment_vec.insert(
        segment_vec.begin() + static_cast<std::ptrdiff_t>(BLUESKY_SEGMENT_LAYOUT.exif_segment_data_insert_index),
        data_vec.begin(),
        data_vec.begin() + static_cast<std::ptrdiff_t>(exif_payload_size));
}

void appendDatasetMarker(vBytes& pshop_vec, std::size_t dataset_size) {
    appendBytes(pshop_vec, DATASET_MARKER_BASE);
    pshop_vec.emplace_back(static_cast<Byte>((dataset_size >> 8) & 0xFF));
    pshop_vec.emplace_back(static_cast<Byte>(dataset_size & 0xFF));
}

void appendXmpPayload(
    vBytes& xmp_vec,
    const vBytes& data_vec,
    std::size_t data_file_index,
    std::size_t remaining_data_size) {

    const std::size_t base64_size = base64EncodedSize(remaining_data_size);
    const std::size_t reserve_size = checkedAdd(
        checkedAdd(xmp_vec.size(), base64_size, SIZE_OVERFLOW_ERROR),
        XMP_FOOTER.size(),
        SIZE_OVERFLOW_ERROR);

    xmp_vec.reserve(reserve_size);

    std::span<const Byte> remaining_data(data_vec.data() + data_file_index, remaining_data_size);
    binaryToBase64(remaining_data, xmp_vec);
    appendBytes(xmp_vec, XMP_FOOTER);

    throwIf(
        xmp_vec.size() > BLUESKY_SEGMENT_LAYOUT.xmp_segment_size_limit,
        "File Size Error: Data file exceeds segment size limit for Bluesky.");
}

void appendAuxiliarySegments(vBytes& segment_vec, vBytes& pshop_vec, vBytes& xmp_vec, bool has_xmp_segment) {
    if (has_xmp_segment) {
        updateValue(xmp_vec, BLUESKY_SEGMENT_LAYOUT.segment_size_index, xmp_vec.size() - SEGMENT_MARKER_BYTES_SIZE);
        appendBytes(segment_vec, xmp_vec);
    }

    if (pshop_vec.size() <= BLUESKY_SEGMENT_LAYOUT.photoshop_default_size) return;

    const std::size_t pshop_segment_data_size = pshop_vec.size() - SEGMENT_MARKER_BYTES_SIZE;
    const std::size_t bim_section_size =
        pshop_segment_data_size - BLUESKY_SEGMENT_LAYOUT.bim_section_size_diff;

    if (!has_xmp_segment) {
        updateValue(pshop_vec, BLUESKY_SEGMENT_LAYOUT.segment_size_index, pshop_segment_data_size);
        updateValue(pshop_vec, BLUESKY_SEGMENT_LAYOUT.bim_section_size_index, bim_section_size);
    } else {
        // With an XMP overflow segment both Photoshop datasets are packed to
        // their limits, so the segment is always its maximum size and the
        // template already carries those values pre-filled. Skipping the writes
        // is only correct while that stays true -- verify rather than assume,
        // so a template edit fails loudly instead of silently emitting a
        // Photoshop segment whose declared sizes disagree with its contents.
        throwIf(getValue(pshop_vec, BLUESKY_SEGMENT_LAYOUT.segment_size_index, 2) != pshop_segment_data_size ||
            getValue(pshop_vec, BLUESKY_SEGMENT_LAYOUT.bim_section_size_index, 2) != bim_section_size,
            "Internal Error: Corrupt Bluesky Photoshop segment template.");
    }
    appendBytes(segment_vec, pshop_vec);
}

void appendOverflowPayloadSegments(vBytes& segment_vec, const vBytes& data_vec, std::size_t exif_payload_size) {
    vBytes pshop_vec = makePhotoshopSegmentTemplate();
    vBytes xmp_vec = makeXmpSegmentTemplate();

    std::size_t remaining_data_size = data_vec.size() - exif_payload_size;
    std::size_t data_file_index = exif_payload_size;
    bool has_xmp_segment = false;

    pshop_vec.reserve(pshop_vec.size() + remaining_data_size);

    const std::size_t first_copy_size = std::min(
        BLUESKY_SEGMENT_LAYOUT.first_dataset_size_limit,
        remaining_data_size);

    if (BLUESKY_SEGMENT_LAYOUT.first_dataset_size_limit > first_copy_size) {
        updateValue(pshop_vec, BLUESKY_SEGMENT_LAYOUT.first_dataset_size_index, first_copy_size);
    } else if (getValue(pshop_vec, BLUESKY_SEGMENT_LAYOUT.first_dataset_size_index) != first_copy_size) {
        // A full first dataset relies on the template already declaring the
        // limit; check it rather than trusting the asset (see appendAuxiliarySegments).
        throw std::runtime_error("Internal Error: Corrupt Bluesky Photoshop segment template.");
    }
    appendDataRange(pshop_vec, data_vec, data_file_index, first_copy_size);

    if (remaining_data_size > BLUESKY_SEGMENT_LAYOUT.first_dataset_size_limit) {
        remaining_data_size -= BLUESKY_SEGMENT_LAYOUT.first_dataset_size_limit;
        data_file_index += BLUESKY_SEGMENT_LAYOUT.first_dataset_size_limit;

        const std::size_t last_copy_size = std::min(
            BLUESKY_SEGMENT_LAYOUT.last_dataset_size_limit,
            remaining_data_size);

        appendDatasetMarker(pshop_vec, last_copy_size);
        appendDataRange(pshop_vec, data_vec, data_file_index, last_copy_size);

        if (remaining_data_size > BLUESKY_SEGMENT_LAYOUT.last_dataset_size_limit) {
            has_xmp_segment = true;
            remaining_data_size -= BLUESKY_SEGMENT_LAYOUT.last_dataset_size_limit;
            data_file_index += BLUESKY_SEGMENT_LAYOUT.last_dataset_size_limit;
            appendXmpPayload(xmp_vec, data_vec, data_file_index, remaining_data_size);
        }
    }

    appendAuxiliarySegments(segment_vec, pshop_vec, xmp_vec, has_xmp_segment);
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
    segment_vec.reserve(blueskySegmentOutputSize(segment_vec.size(), encrypted_vec_size));
    const std::size_t segment_vec_data_size = segment_vec.size() - FIRST_MARKER_BYTES_SIZE;
    const std::size_t exif_payload_size = std::min(
        encrypted_vec_size,
        BLUESKY_SEGMENT_LAYOUT.exif_segment_data_size_limit);
    const std::size_t exif_segment_data_size = checkedAdd(
        exif_payload_size,
        segment_vec_data_size,
        SIZE_OVERFLOW_ERROR);
    throwIf(
        exif_segment_data_size < BLUESKY_SEGMENT_LAYOUT.artist_field_size_diff,
        "Internal Error: Corrupt Bluesky segment size.");
    const std::size_t artist_field_size = exif_segment_data_size - BLUESKY_SEGMENT_LAYOUT.artist_field_size_diff;

    updateValue(segment_vec, BLUESKY_SEGMENT_LAYOUT.compressed_file_size_index, encrypted_vec_size, VALUE_BYTE_LENGTH);

    if (encrypted_vec_size <= BLUESKY_SEGMENT_LAYOUT.exif_segment_data_size_limit) {
        updateValue(segment_vec, BLUESKY_SEGMENT_LAYOUT.artist_field_size_index, artist_field_size, VALUE_BYTE_LENGTH);
        updateValue(segment_vec, BLUESKY_SEGMENT_LAYOUT.exif_segment_size_index, exif_segment_data_size);
        insertExifPayload(segment_vec, data_vec, exif_payload_size);
        return;
    }

    // Overflow path: the EXIF payload is always packed to exif_segment_data_size_limit,
    // so the segment is at its maximum size and the template already carries those
    // values pre-filled. Skipping the writes is only correct while that stays true --
    // verify rather than assume, exactly as appendAuxiliarySegments and
    // appendOverflowPayloadSegments do for the Photoshop segment, so a template edit
    // fails loudly instead of silently emitting an EXIF segment whose declared sizes
    // disagree with its contents.
    throwIf(
        getValue(segment_vec, BLUESKY_SEGMENT_LAYOUT.exif_segment_size_index, 2) != exif_segment_data_size ||
            getValue(segment_vec, BLUESKY_SEGMENT_LAYOUT.artist_field_size_index, VALUE_BYTE_LENGTH) != artist_field_size,
        "Internal Error: Corrupt Bluesky segment template.");

    insertExifPayload(segment_vec, data_vec, exif_payload_size);
    appendOverflowPayloadSegments(segment_vec, data_vec, exif_payload_size);
}
