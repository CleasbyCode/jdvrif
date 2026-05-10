#include "encryption.h"
#include "base64.h"
#include "binary_io.h"
#include "embedded_layout.h"
#include "file_utils.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>

namespace {

constexpr auto PHOTOSHOP_SEGMENT = std::to_array<Byte>({
    0xFF, 0xED, 0xFF, 0xFF, 0x50, 0x68, 0x6F, 0x74, 0x6F, 0x73, 0x68, 0x6F, 0x70, 0x20, 0x33, 0x2E,
    0x30, 0x00, 0x38, 0x42, 0x49, 0x4D, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xE3, 0x1C, 0x08,
    0x0A, 0x7F, 0xFF
});

constexpr auto XMP_SEGMENT = std::to_array<Byte>({
    0xFF, 0xE1, 0x01, 0x93, 0x68, 0x74, 0x74, 0x70, 0x3A, 0x2F, 0x2F, 0x6E, 0x73, 0x2E, 0x61, 0x64,
    0x6F, 0x62, 0x65, 0x2E, 0x63, 0x6F, 0x6D, 0x2F, 0x78, 0x61, 0x70, 0x2F, 0x31, 0x2E, 0x30, 0x2F,
    0x00, 0x3C, 0x3F, 0x78, 0x70, 0x61, 0x63, 0x6B, 0x65, 0x74, 0x20, 0x62, 0x65, 0x67, 0x69, 0x6E,
    0x3D, 0x22, 0x22, 0x20, 0x69, 0x64, 0x3D, 0x22, 0x57, 0x35, 0x4D, 0x30, 0x4D, 0x70, 0x43, 0x65,
    0x68, 0x69, 0x48, 0x7A, 0x72, 0x65, 0x53, 0x7A, 0x4E, 0x54, 0x63, 0x7A, 0x6B, 0x63, 0x39, 0x64,
    0x22, 0x3F, 0x3E, 0x0A, 0x3C, 0x78, 0x3A, 0x78, 0x6D, 0x70, 0x6D, 0x65, 0x74, 0x61, 0x20, 0x78,
    0x6D, 0x6C, 0x6E, 0x73, 0x3A, 0x78, 0x3D, 0x22, 0x61, 0x64, 0x6F, 0x62, 0x65, 0x3A, 0x6E, 0x73,
    0x3A, 0x6D, 0x65, 0x74, 0x61, 0x2F, 0x22, 0x20, 0x78, 0x3A, 0x78, 0x6D, 0x70, 0x74, 0x6B, 0x3D,
    0x22, 0x47, 0x6F, 0x20, 0x58, 0x4D, 0x50, 0x20, 0x53, 0x44, 0x4B, 0x20, 0x31, 0x2E, 0x30, 0x22,
    0x3E, 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x52, 0x44, 0x46, 0x20, 0x78, 0x6D, 0x6C, 0x6E, 0x73, 0x3A,
    0x72, 0x64, 0x66, 0x3D, 0x22, 0x68, 0x74, 0x74, 0x70, 0x3A, 0x2F, 0x2F, 0x77, 0x77, 0x77, 0x2E,
    0x77, 0x33, 0x2E, 0x6F, 0x72, 0x67, 0x2F, 0x31, 0x39, 0x39, 0x39, 0x2F, 0x30, 0x32, 0x2F, 0x32,
    0x32, 0x2D, 0x72, 0x64, 0x66, 0x2D, 0x73, 0x79, 0x6E, 0x74, 0x61, 0x78, 0x2D, 0x6E, 0x73, 0x23,
    0x22, 0x3E, 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x44, 0x65, 0x73, 0x63, 0x72, 0x69, 0x70, 0x74, 0x69,
    0x6F, 0x6E, 0x20, 0x78, 0x6D, 0x6C, 0x6E, 0x73, 0x3A, 0x64, 0x63, 0x3D, 0x22, 0x68, 0x74, 0x74,
    0x70, 0x3A, 0x2F, 0x2F, 0x70, 0x75, 0x72, 0x6C, 0x2E, 0x6F, 0x72, 0x67, 0x2F, 0x64, 0x63, 0x2F,
    0x65, 0x6C, 0x65, 0x6D, 0x65, 0x6E, 0x74, 0x73, 0x2F, 0x31, 0x2E, 0x31, 0x2F, 0x22, 0x20, 0x72,
    0x64, 0x66, 0x3A, 0x61, 0x62, 0x6F, 0x75, 0x74, 0x3D, 0x22, 0x22, 0x3E, 0x3C, 0x64, 0x63, 0x3A,
    0x63, 0x72, 0x65, 0x61, 0x74, 0x6F, 0x72, 0x3E, 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x53, 0x65, 0x71,
    0x3E, 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x6C, 0x69, 0x3E
});

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
constexpr std::size_t VALUE_BYTE_LENGTH = 4;

void appendDataRange(vBytes& output, const vBytes& data, std::size_t offset, std::size_t size) {
    const std::size_t end = checkedAdd(offset, size, "Internal Error: Bluesky data range overflow.");
    if (end > data.size()) {
        throw std::runtime_error("Internal Error: Bluesky data range out of bounds.");
    }

    output.insert(
        output.end(),
        data.begin() + static_cast<std::ptrdiff_t>(offset),
        data.begin() + static_cast<std::ptrdiff_t>(end));
}

void insertExifPayload(vBytes& segment_vec, const vBytes& data_vec, std::size_t exif_payload_size) {
    segment_vec.insert(
        segment_vec.begin() + static_cast<std::ptrdiff_t>(BLUESKY_SEGMENT_LAYOUT.exif_segment_data_insert_index),
        data_vec.begin(),
        data_vec.begin() + static_cast<std::ptrdiff_t>(exif_payload_size));
}

void appendDatasetMarker(vBytes& pshop_vec, std::size_t dataset_size) {
    pshop_vec.insert(pshop_vec.end(), DATASET_MARKER_BASE.begin(), DATASET_MARKER_BASE.end());
    pshop_vec.emplace_back(static_cast<Byte>((dataset_size >> 8) & 0xFF));
    pshop_vec.emplace_back(static_cast<Byte>(dataset_size & 0xFF));
}

void appendXmpPayload(
    vBytes& xmp_vec,
    const vBytes& data_vec,
    std::size_t data_file_index,
    std::size_t remaining_data_size) {

    const std::size_t base64_size = checkedMul(
        checkedAdd(remaining_data_size, 2, "File Size Error: Bluesky segment size overflow.") / 3,
        4,
        "File Size Error: Bluesky segment size overflow.");
    const std::size_t reserve_size = checkedAdd(
        checkedAdd(xmp_vec.size(), base64_size, "File Size Error: Bluesky segment size overflow."),
        XMP_FOOTER.size(),
        "File Size Error: Bluesky segment size overflow.");

    xmp_vec.reserve(reserve_size);

    std::span<const Byte> remaining_data(data_vec.data() + data_file_index, remaining_data_size);
    binaryToBase64(remaining_data, xmp_vec);
    xmp_vec.insert(xmp_vec.end(), XMP_FOOTER.begin(), XMP_FOOTER.end());

    if (xmp_vec.size() > BLUESKY_SEGMENT_LAYOUT.xmp_segment_size_limit) {
        throw std::runtime_error("File Size Error: Data file exceeds segment size limit for Bluesky.");
    }
}

void appendAuxiliarySegments(vBytes& segment_vec, vBytes& pshop_vec, vBytes& xmp_vec, bool has_xmp_segment) {
    if (has_xmp_segment) {
        updateValue(
            xmp_vec,
            BLUESKY_SEGMENT_LAYOUT.segment_size_index,
            xmp_vec.size() - SEGMENT_MARKER_BYTES_SIZE);
        segment_vec.insert(segment_vec.end(), xmp_vec.begin(), xmp_vec.end());
    }

    if (pshop_vec.size() <= BLUESKY_SEGMENT_LAYOUT.photoshop_default_size) return;

    const std::size_t pshop_segment_data_size = pshop_vec.size() - SEGMENT_MARKER_BYTES_SIZE;
    const std::size_t bim_section_size =
        pshop_segment_data_size - BLUESKY_SEGMENT_LAYOUT.bim_section_size_diff;

    if (!has_xmp_segment) {
        updateValue(pshop_vec, BLUESKY_SEGMENT_LAYOUT.segment_size_index, pshop_segment_data_size);
        updateValue(pshop_vec, BLUESKY_SEGMENT_LAYOUT.bim_section_size_index, bim_section_size);
    }
    segment_vec.insert(segment_vec.end(), pshop_vec.begin(), pshop_vec.end());
}

void appendOverflowPayloadSegments(vBytes& segment_vec, const vBytes& data_vec, std::size_t exif_payload_size) {
    vBytes pshop_vec(PHOTOSHOP_SEGMENT.begin(), PHOTOSHOP_SEGMENT.end());
    vBytes xmp_vec(XMP_SEGMENT.begin(), XMP_SEGMENT.end());

    std::size_t remaining_data_size = data_vec.size() - exif_payload_size;
    std::size_t data_file_index = exif_payload_size;
    bool has_xmp_segment = false;

    pshop_vec.reserve(pshop_vec.size() + remaining_data_size);

    const std::size_t first_copy_size = std::min(
        BLUESKY_SEGMENT_LAYOUT.first_dataset_size_limit,
        remaining_data_size);

    if (BLUESKY_SEGMENT_LAYOUT.first_dataset_size_limit > first_copy_size) {
        updateValue(pshop_vec, BLUESKY_SEGMENT_LAYOUT.first_dataset_size_index, first_copy_size);
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
    if (segment_vec.size() < FIRST_MARKER_BYTES_SIZE ||
        BLUESKY_SEGMENT_LAYOUT.exif_segment_data_insert_index > segment_vec.size()) {
        throw std::runtime_error("Internal Error: Corrupt Bluesky segment template.");
    }
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
    const std::size_t segment_vec_data_size = segment_vec.size() - FIRST_MARKER_BYTES_SIZE;
    const std::size_t exif_payload_size = std::min(
        encrypted_vec_size,
        BLUESKY_SEGMENT_LAYOUT.exif_segment_data_size_limit);
    const std::size_t exif_segment_data_size = checkedAdd(
        exif_payload_size,
        segment_vec_data_size,
        "File Size Error: Bluesky segment size overflow.");
    if (exif_segment_data_size < BLUESKY_SEGMENT_LAYOUT.artist_field_size_diff) {
        throw std::runtime_error("Internal Error: Corrupt Bluesky segment size.");
    }
    const std::size_t artist_field_size = exif_segment_data_size - BLUESKY_SEGMENT_LAYOUT.artist_field_size_diff;

    updateValue(segment_vec, BLUESKY_SEGMENT_LAYOUT.compressed_file_size_index, encrypted_vec_size, VALUE_BYTE_LENGTH);

    if (encrypted_vec_size <= BLUESKY_SEGMENT_LAYOUT.exif_segment_data_size_limit) {
        updateValue(segment_vec, BLUESKY_SEGMENT_LAYOUT.artist_field_size_index, artist_field_size, VALUE_BYTE_LENGTH);
        updateValue(segment_vec, BLUESKY_SEGMENT_LAYOUT.exif_segment_size_index, exif_segment_data_size);
        insertExifPayload(segment_vec, data_vec, exif_payload_size);
        return;
    }

    insertExifPayload(segment_vec, data_vec, exif_payload_size);
    appendOverflowPayloadSegments(segment_vec, data_vec, exif_payload_size);
}
