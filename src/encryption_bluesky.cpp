#include "encryption.h"
#include "base64.h"
#include "binary_io.h"
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
        COMPRESSED_FILE_SIZE_INDEX     = 0x1CD,
        EXIF_SEGMENT_DATA_SIZE_LIMIT   = 65027,
        EXIF_SEGMENT_DATA_INSERT_INDEX = 0x1D1,
        EXIF_SEGMENT_SIZE_INDEX        = 0x04,
        ARTIST_FIELD_SIZE_INDEX        = 0x4A,
        ARTIST_FIELD_SIZE_DIFF         = 140,
        FIRST_MARKER_BYTES_SIZE        = 4,
        VALUE_BYTE_LENGTH              = 4;

    if (segment_vec.size() < FIRST_MARKER_BYTES_SIZE ||
        EXIF_SEGMENT_DATA_INSERT_INDEX > segment_vec.size()) {
        throw std::runtime_error("Internal Error: Corrupt Bluesky segment template.");
    }
    requireSpanRange(segment_vec, COMPRESSED_FILE_SIZE_INDEX, VALUE_BYTE_LENGTH, "Internal Error: Corrupt Bluesky metadata index.");
    requireSpanRange(segment_vec, EXIF_SEGMENT_SIZE_INDEX, 2, "Internal Error: Corrupt Bluesky metadata index.");
    requireSpanRange(segment_vec, ARTIST_FIELD_SIZE_INDEX, VALUE_BYTE_LENGTH, "Internal Error: Corrupt Bluesky metadata index.");

    const std::size_t
        encrypted_vec_size     = data_vec.size(),
        segment_vec_data_size  = segment_vec.size() - FIRST_MARKER_BYTES_SIZE,
        exif_segment_data_size = encrypted_vec_size > EXIF_SEGMENT_DATA_SIZE_LIMIT
            ? EXIF_SEGMENT_DATA_SIZE_LIMIT + segment_vec_data_size
            : encrypted_vec_size + segment_vec_data_size,
        artist_field_size      = exif_segment_data_size - ARTIST_FIELD_SIZE_DIFF;

    bool hasXmpSegment = false;

    updateValue(segment_vec, COMPRESSED_FILE_SIZE_INDEX, encrypted_vec_size, VALUE_BYTE_LENGTH);

    if (encrypted_vec_size <= EXIF_SEGMENT_DATA_SIZE_LIMIT) {
        updateValue(segment_vec, ARTIST_FIELD_SIZE_INDEX, artist_field_size, VALUE_BYTE_LENGTH);
        updateValue(segment_vec, EXIF_SEGMENT_SIZE_INDEX, exif_segment_data_size);
        segment_vec.insert(segment_vec.begin() + EXIF_SEGMENT_DATA_INSERT_INDEX, data_vec.begin(), data_vec.end());
        return;
    }

    // Data exceeds single EXIF segment - split across IPTC/XMP segments.
    segment_vec.insert(segment_vec.begin() + EXIF_SEGMENT_DATA_INSERT_INDEX, data_vec.begin(), data_vec.begin() + EXIF_SEGMENT_DATA_SIZE_LIMIT);

    constexpr std::size_t
        FIRST_DATASET_SIZE_LIMIT = 32767,
        LAST_DATASET_SIZE_LIMIT  = 32730,
        FIRST_DATASET_SIZE_INDEX = 0x21;

    vBytes pshop_vec(
        PHOTOSHOP_SEGMENT.begin(), PHOTOSHOP_SEGMENT.end()
    );

    std::size_t
        remaining_data_size = encrypted_vec_size - EXIF_SEGMENT_DATA_SIZE_LIMIT,
        data_file_index     = EXIF_SEGMENT_DATA_SIZE_LIMIT;

    pshop_vec.reserve(pshop_vec.size() + remaining_data_size);

    const std::size_t first_copy_size = std::min(FIRST_DATASET_SIZE_LIMIT, remaining_data_size);

    if (FIRST_DATASET_SIZE_LIMIT > first_copy_size) {
        updateValue(pshop_vec, FIRST_DATASET_SIZE_INDEX, first_copy_size);
    }

    pshop_vec.insert(
        pshop_vec.end(),
        data_vec.begin() + static_cast<std::ptrdiff_t>(data_file_index),
        data_vec.begin() + static_cast<std::ptrdiff_t>(data_file_index + first_copy_size));

    vBytes xmp_vec (XMP_SEGMENT.begin(), XMP_SEGMENT.end());

    if (remaining_data_size > FIRST_DATASET_SIZE_LIMIT) {
        remaining_data_size -= FIRST_DATASET_SIZE_LIMIT;
        data_file_index += FIRST_DATASET_SIZE_LIMIT;

        const std::size_t last_copy_size = std::min(LAST_DATASET_SIZE_LIMIT, remaining_data_size);

        constexpr auto DATASET_MARKER_BASE = std::to_array<Byte>({ 0x1C, 0x08, 0x0A });

        pshop_vec.insert(pshop_vec.end(), DATASET_MARKER_BASE.begin(), DATASET_MARKER_BASE.end());
        pshop_vec.emplace_back(static_cast<Byte>((last_copy_size >> 8) & 0xFF));
        pshop_vec.emplace_back(static_cast<Byte>(last_copy_size & 0xFF));
        pshop_vec.insert(
            pshop_vec.end(),
            data_vec.begin() + static_cast<std::ptrdiff_t>(data_file_index),
            data_vec.begin() + static_cast<std::ptrdiff_t>(data_file_index + last_copy_size));

        if (remaining_data_size > LAST_DATASET_SIZE_LIMIT) {
            hasXmpSegment = true;

            remaining_data_size -= LAST_DATASET_SIZE_LIMIT;
            data_file_index += LAST_DATASET_SIZE_LIMIT;

            constexpr std::size_t
                XMP_SEGMENT_SIZE_LIMIT = 60033,
                XMP_FOOTER_SIZE        = 92;

            const std::size_t base64_size = ((remaining_data_size + 2) / 3) * 4;
            xmp_vec.reserve(xmp_vec.size() + base64_size + XMP_FOOTER_SIZE);

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

            if (xmp_vec.size() > XMP_SEGMENT_SIZE_LIMIT) {
                throw std::runtime_error("File Size Error: Data file exceeds segment size limit for Bluesky.");
            }
        }
    }

    // Finalize segment sizes and append to segment_vec.
    constexpr std::size_t
        PSHOP_VEC_DEFAULT_SIZE    = 35,
        SEGMENT_MARKER_BYTES_SIZE = 2,
        SEGMENT_SIZE_INDEX        = 0x2,
        BIM_SECTION_SIZE_INDEX    = 0x1C,
        BIM_SECTION_SIZE_DIFF     = 28;

    if (hasXmpSegment) {
        updateValue(xmp_vec, SEGMENT_SIZE_INDEX, xmp_vec.size() - SEGMENT_MARKER_BYTES_SIZE);
        segment_vec.insert(segment_vec.end(), xmp_vec.begin(), xmp_vec.end());
    }

    if (pshop_vec.size() > PSHOP_VEC_DEFAULT_SIZE) {
        const std::size_t
            pshop_segment_data_size = pshop_vec.size() - SEGMENT_MARKER_BYTES_SIZE,
            bim_section_size        = pshop_segment_data_size - BIM_SECTION_SIZE_DIFF;

        if (!hasXmpSegment) {
            updateValue(pshop_vec, SEGMENT_SIZE_INDEX, pshop_segment_data_size);
            updateValue(pshop_vec, BIM_SECTION_SIZE_INDEX, bim_section_size);
        }
        segment_vec.insert(segment_vec.end(), pshop_vec.begin(), pshop_vec.end());
    }
}

void writePinAttempts(const fs::path& path, std::streamoff offset, Byte value) {
    if (offset < 0) {
        throw std::runtime_error("File Error: Invalid file write offset.");
    }

    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file) {
        throw std::runtime_error("File Error: Unable to open image file for updating.");
    }
    file.seekp(offset, std::ios::beg);
    if (!file) {
        throw std::runtime_error("File Error: Failed to seek write position.");
    }
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
    if (!file) {
        throw std::runtime_error("File Error: Failed to update image file.");
    }
}

void destroyImageFile(const fs::path& path) {
    // Intentional: truncate the cover image file to zero bytes after too many failed PIN attempts.
    std::ofstream ofs(path, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!ofs) {
        throw std::runtime_error("File Error: Unable to truncate image file.");
    }
}

void reassembleBlueskyData(vBytes& jpg_vec, std::size_t sig_length) {
    constexpr auto
        PSHOP_SEGMENT_SIG = std::to_array<Byte>({ 0x73, 0x68, 0x6F, 0x70, 0x20, 0x33, 0x2E }),
        XMP_CREATOR_SIG   = std::to_array<Byte>({ 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x6C, 0x69 });
    constexpr const char* CORRUPT_FILE_ERROR =
        "File Extraction Error: Embedded data file is corrupt!";

    auto index_opt = searchSig(jpg_vec, PSHOP_SEGMENT_SIG);
    if (!index_opt) return;

    constexpr std::size_t
        DATASET_MAX_SIZE              = 32800,
        PSHOP_SEGMENT_SIZE_INDEX_DIFF = 7,
        FIRST_DATASET_SIZE_INDEX_DIFF = 24,
        DATASET_FILE_INDEX_DIFF       = 2;

    const std::size_t pshop_segment_sig_index = *index_opt;
    if (pshop_segment_sig_index < PSHOP_SEGMENT_SIZE_INDEX_DIFF) {
        throw std::runtime_error(CORRUPT_FILE_ERROR);
    }

    const std::size_t
        pshop_segment_size_index = pshop_segment_sig_index  - PSHOP_SEGMENT_SIZE_INDEX_DIFF,
        first_dataset_size_index = pshop_segment_sig_index  + FIRST_DATASET_SIZE_INDEX_DIFF,
        first_dataset_file_index = first_dataset_size_index + DATASET_FILE_INDEX_DIFF;
    requireSpanRange(jpg_vec, pshop_segment_size_index, 2, CORRUPT_FILE_ERROR);
    requireSpanRange(jpg_vec, first_dataset_size_index, 2, CORRUPT_FILE_ERROR);

    const std::uint16_t
        pshop_segment_size = static_cast<std::uint16_t>(getValue(jpg_vec, pshop_segment_size_index)),
        first_dataset_size = static_cast<std::uint16_t>(getValue(jpg_vec, first_dataset_size_index));
    requireSpanRange(jpg_vec, first_dataset_file_index, first_dataset_size, CORRUPT_FILE_ERROR);

    vBytes file_parts_vec;
    if (first_dataset_size > std::numeric_limits<std::size_t>::max() / 5) {
        throw std::runtime_error(CORRUPT_FILE_ERROR);
    }
    file_parts_vec.reserve(static_cast<std::size_t>(first_dataset_size) * 5);
    file_parts_vec.insert(
        file_parts_vec.end(),
        jpg_vec.begin() + static_cast<std::ptrdiff_t>(first_dataset_file_index),
        jpg_vec.begin() + static_cast<std::ptrdiff_t>(first_dataset_file_index + first_dataset_size));

    bool has_xmp_segment = false;
    std::size_t xmp_creator_sig_index = 0;

    if (pshop_segment_size > DATASET_MAX_SIZE) {
        constexpr std::size_t SECOND_DATASET_SIZE_INDEX_DIFF = 3;
        const std::size_t
           second_dataset_size_index = first_dataset_file_index  + first_dataset_size + SECOND_DATASET_SIZE_INDEX_DIFF,
           second_dataset_file_index = second_dataset_size_index + DATASET_FILE_INDEX_DIFF;
        requireSpanRange(jpg_vec, second_dataset_size_index, 2, CORRUPT_FILE_ERROR);

        const std::uint16_t second_dataset_size = static_cast<std::uint16_t>(getValue(jpg_vec, second_dataset_size_index));
        requireSpanRange(jpg_vec, second_dataset_file_index, second_dataset_size, CORRUPT_FILE_ERROR);

        file_parts_vec.insert(
            file_parts_vec.end(),
            jpg_vec.begin() + static_cast<std::ptrdiff_t>(second_dataset_file_index),
            jpg_vec.begin() + static_cast<std::ptrdiff_t>(second_dataset_file_index + second_dataset_size));

        auto xmp_opt = searchSig(jpg_vec, XMP_CREATOR_SIG);
        if (xmp_opt) {
            has_xmp_segment = true;
            xmp_creator_sig_index = *xmp_opt;

            constexpr Byte BASE64_END_SIG = 0x3C;
            if (xmp_creator_sig_index > jpg_vec.size() || sig_length + 1 > jpg_vec.size() - xmp_creator_sig_index) {
                throw std::runtime_error(CORRUPT_FILE_ERROR);
            }
            const std::size_t base64_begin_index = xmp_creator_sig_index + sig_length + 1;
            auto base64_end_it = std::ranges::find(
                jpg_vec.begin() + static_cast<std::ptrdiff_t>(base64_begin_index),
                jpg_vec.end(),
                BASE64_END_SIG);
            if (base64_end_it == jpg_vec.end()) {
                throw std::runtime_error(CORRUPT_FILE_ERROR);
            }
            const std::size_t base64_end_index = static_cast<std::size_t>(base64_end_it - jpg_vec.begin());
            if (base64_end_index <= base64_begin_index) {
                throw std::runtime_error(CORRUPT_FILE_ERROR);
            }

            std::span<const Byte> base64_span(jpg_vec.data() + base64_begin_index, base64_end_index - base64_begin_index);

            appendBase64AsBinary(base64_span, file_parts_vec);
        }
    }
    const std::size_t
        exif_data_end_index_diff = has_xmp_segment ? 351 : 55,
        exif_data_anchor         = has_xmp_segment ? xmp_creator_sig_index : pshop_segment_sig_index;
    if (exif_data_anchor < exif_data_end_index_diff) {
        throw std::runtime_error(CORRUPT_FILE_ERROR);
    }
    const std::size_t exif_data_end_index = exif_data_anchor - exif_data_end_index_diff;
    requireSpanRange(jpg_vec, exif_data_end_index, file_parts_vec.size(), CORRUPT_FILE_ERROR);

    std::ranges::copy(file_parts_vec, jpg_vec.begin() + static_cast<std::ptrdiff_t>(exif_data_end_index));
}
