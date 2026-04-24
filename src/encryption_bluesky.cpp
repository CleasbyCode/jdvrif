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

}

void buildBlueskySegments(vBytes& segment_vec, const vBytes& data_vec) {
    constexpr std::size_t
        FIRST_MARKER_BYTES_SIZE        = 4,
        VALUE_BYTE_LENGTH              = 4;

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
    const std::size_t exif_segment_data_size = encrypted_vec_size > BLUESKY_SEGMENT_LAYOUT.exif_segment_data_size_limit
        ? checkedAdd(BLUESKY_SEGMENT_LAYOUT.exif_segment_data_size_limit, segment_vec_data_size,
            "File Size Error: Bluesky segment size overflow.")
        : checkedAdd(encrypted_vec_size, segment_vec_data_size,
            "File Size Error: Bluesky segment size overflow.");
    if (exif_segment_data_size < BLUESKY_SEGMENT_LAYOUT.artist_field_size_diff) {
        throw std::runtime_error("Internal Error: Corrupt Bluesky segment size.");
    }
    const std::size_t artist_field_size = exif_segment_data_size - BLUESKY_SEGMENT_LAYOUT.artist_field_size_diff;

    bool hasXmpSegment = false;

    updateValue(segment_vec, BLUESKY_SEGMENT_LAYOUT.compressed_file_size_index, encrypted_vec_size, VALUE_BYTE_LENGTH);

    if (encrypted_vec_size <= BLUESKY_SEGMENT_LAYOUT.exif_segment_data_size_limit) {
        updateValue(segment_vec, BLUESKY_SEGMENT_LAYOUT.artist_field_size_index, artist_field_size, VALUE_BYTE_LENGTH);
        updateValue(segment_vec, BLUESKY_SEGMENT_LAYOUT.exif_segment_size_index, exif_segment_data_size);
        segment_vec.insert(
            segment_vec.begin() + static_cast<std::ptrdiff_t>(BLUESKY_SEGMENT_LAYOUT.exif_segment_data_insert_index),
            data_vec.begin(),
            data_vec.end());
        return;
    }

    // Data exceeds single EXIF segment - split across IPTC/XMP segments.
    segment_vec.insert(
        segment_vec.begin() + static_cast<std::ptrdiff_t>(BLUESKY_SEGMENT_LAYOUT.exif_segment_data_insert_index),
        data_vec.begin(),
        data_vec.begin() + static_cast<std::ptrdiff_t>(BLUESKY_SEGMENT_LAYOUT.exif_segment_data_size_limit));

    vBytes pshop_vec(
        PHOTOSHOP_SEGMENT.begin(), PHOTOSHOP_SEGMENT.end()
    );

    std::size_t
        remaining_data_size = encrypted_vec_size - BLUESKY_SEGMENT_LAYOUT.exif_segment_data_size_limit,
        data_file_index     = BLUESKY_SEGMENT_LAYOUT.exif_segment_data_size_limit;

    pshop_vec.reserve(pshop_vec.size() + remaining_data_size);

    const std::size_t first_copy_size = std::min(BLUESKY_SEGMENT_LAYOUT.first_dataset_size_limit, remaining_data_size);

    if (BLUESKY_SEGMENT_LAYOUT.first_dataset_size_limit > first_copy_size) {
        updateValue(pshop_vec, BLUESKY_SEGMENT_LAYOUT.first_dataset_size_index, first_copy_size);
    }

    pshop_vec.insert(
        pshop_vec.end(),
        data_vec.begin() + static_cast<std::ptrdiff_t>(data_file_index),
        data_vec.begin() + static_cast<std::ptrdiff_t>(data_file_index + first_copy_size));

    vBytes xmp_vec (XMP_SEGMENT.begin(), XMP_SEGMENT.end());

    if (remaining_data_size > BLUESKY_SEGMENT_LAYOUT.first_dataset_size_limit) {
        remaining_data_size -= BLUESKY_SEGMENT_LAYOUT.first_dataset_size_limit;
        data_file_index += BLUESKY_SEGMENT_LAYOUT.first_dataset_size_limit;

        const std::size_t last_copy_size = std::min(BLUESKY_SEGMENT_LAYOUT.last_dataset_size_limit, remaining_data_size);

        constexpr auto DATASET_MARKER_BASE = std::to_array<Byte>({ 0x1C, 0x08, 0x0A });

        pshop_vec.insert(pshop_vec.end(), DATASET_MARKER_BASE.begin(), DATASET_MARKER_BASE.end());
        pshop_vec.emplace_back(static_cast<Byte>((last_copy_size >> 8) & 0xFF));
        pshop_vec.emplace_back(static_cast<Byte>(last_copy_size & 0xFF));
        pshop_vec.insert(
            pshop_vec.end(),
            data_vec.begin() + static_cast<std::ptrdiff_t>(data_file_index),
            data_vec.begin() + static_cast<std::ptrdiff_t>(data_file_index + last_copy_size));

        if (remaining_data_size > BLUESKY_SEGMENT_LAYOUT.last_dataset_size_limit) {
            hasXmpSegment = true;

            remaining_data_size -= BLUESKY_SEGMENT_LAYOUT.last_dataset_size_limit;
            data_file_index += BLUESKY_SEGMENT_LAYOUT.last_dataset_size_limit;

            const std::size_t base64_size = ((remaining_data_size + 2) / 3) * 4;
            xmp_vec.reserve(xmp_vec.size() + base64_size + BLUESKY_SEGMENT_LAYOUT.xmp_footer_size);

            std::span<const Byte> remaining_data(data_vec.data() + data_file_index, remaining_data_size);
            binaryToBase64(remaining_data, xmp_vec);

            constexpr auto XMP_FOOTER = std::to_array<Byte>({
                0x3C, 0x2F, 0x72, 0x64, 0x66, 0x3A, 0x6C, 0x69, 0x3E, 0x3C, 0x2F, 0x72, 0x64, 0x66, 0x3A, 0x53,
                0x65, 0x71, 0x3E, 0x3C, 0x2F, 0x64, 0x63, 0x3A, 0x63, 0x72, 0x65, 0x61, 0x74, 0x6F, 0x72, 0x3E,
                0x3C, 0x2F, 0x72, 0x64, 0x66, 0x3A, 0x44, 0x65, 0x73, 0x63, 0x72, 0x69, 0x70, 0x74, 0x69, 0x6F,
                0x6E, 0x3E, 0x3C, 0x2F, 0x72, 0x64, 0x66, 0x3A, 0x52, 0x44, 0x46, 0x3E, 0x3C, 0x2F, 0x78, 0x3A,
                0x78, 0x6D, 0x70, 0x6D, 0x65, 0x74, 0x61, 0x3E, 0x0A, 0x3C, 0x3F, 0x78, 0x70, 0x61, 0x63, 0x6B,
                0x65, 0x74, 0x20, 0x65, 0x6E, 0x64, 0x3D, 0x22, 0x77, 0x22, 0x3F, 0x3E
            });
            xmp_vec.insert(xmp_vec.end(), XMP_FOOTER.begin(), XMP_FOOTER.end());

            if (xmp_vec.size() > BLUESKY_SEGMENT_LAYOUT.xmp_segment_size_limit) {
                throw std::runtime_error("File Size Error: Data file exceeds segment size limit for Bluesky.");
            }
        }
    }

    // Finalize segment sizes and append to segment_vec.
    constexpr std::size_t SEGMENT_MARKER_BYTES_SIZE = 2;

    if (hasXmpSegment) {
        updateValue(xmp_vec, BLUESKY_SEGMENT_LAYOUT.segment_size_index, xmp_vec.size() - SEGMENT_MARKER_BYTES_SIZE);
        segment_vec.insert(segment_vec.end(), xmp_vec.begin(), xmp_vec.end());
    }

    if (pshop_vec.size() > BLUESKY_SEGMENT_LAYOUT.photoshop_default_size) {
        const std::size_t
            pshop_segment_data_size = pshop_vec.size() - SEGMENT_MARKER_BYTES_SIZE,
            bim_section_size        = pshop_segment_data_size - BLUESKY_SEGMENT_LAYOUT.bim_section_size_diff;

        if (!hasXmpSegment) {
            updateValue(pshop_vec, BLUESKY_SEGMENT_LAYOUT.segment_size_index, pshop_segment_data_size);
            updateValue(pshop_vec, BLUESKY_SEGMENT_LAYOUT.bim_section_size_index, bim_section_size);
        }
        segment_vec.insert(segment_vec.end(), pshop_vec.begin(), pshop_vec.end());
    }
}

