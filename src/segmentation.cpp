#include "segmentation.h"
#include "binary_io.h"

#include <string>
#include <utility>

// Data file is too large for a single ICC segment, so split the data file and store it within multiple segments.
vBytes buildMultiSegmentICC(vBytes& segment_vec, std::span<const Byte> soi_bytes) {
    constexpr std::size_t
        SOI_SIG_LENGTH           = 2,
        SEGMENT_SIG_LENGTH       = 2,
        SEGMENT_HEADER_LENGTH    = 16,
        SEGMENT_DATA_SIZE        = 65519,
        SEGMENTS_TOTAL_VAL_INDEX = 0x2E0;

    std::size_t
        adjusted_size          = segment_vec.size() - SEGMENT_HEADER_LENGTH,
        remainder_data         = adjusted_size % SEGMENT_DATA_SIZE,
        header_overhead        = SOI_SIG_LENGTH + SEGMENT_SIG_LENGTH,
        segment_remainder_size = (remainder_data > header_overhead) ? remainder_data - header_overhead : 0,
        total_segments         = adjusted_size / SEGMENT_DATA_SIZE,
        segments_required      = total_segments + (segment_remainder_size > 0);

    updateValue(segment_vec, SEGMENTS_TOTAL_VAL_INDEX, segments_required);

    segment_vec.erase(
        segment_vec.begin(),
        segment_vec.begin() + (SOI_SIG_LENGTH + SEGMENT_SIG_LENGTH + SEGMENT_HEADER_LENGTH));

    vBytes result;
    result.reserve(adjusted_size + (segments_required * (SEGMENT_SIG_LENGTH + SEGMENT_HEADER_LENGTH)));

    constexpr auto ICC_HEADER_TEMPLATE = std::to_array<Byte>({
        0xFF, 0xE2, 0x00, 0x00,
        'I','C','C','_','P','R','O','F','I','L','E',
        0x00, 0x00, 0x01
    });

    uint16_t
        default_segment_length  = static_cast<uint16_t>(SEGMENT_DATA_SIZE + SEGMENT_HEADER_LENGTH),
        last_segment_length     = static_cast<uint16_t>(segment_remainder_size + SEGMENT_HEADER_LENGTH);

    std::size_t offset = 0;

    for (std::size_t seg = 1; seg <= segments_required; ++seg) {
        bool is_last = (seg == segments_required);
        std::size_t data_size = is_last
            ? (segment_vec.size() - offset)
            : SEGMENT_DATA_SIZE;
        uint16_t seg_length = is_last
            ? last_segment_length
            : default_segment_length;

        auto header = ICC_HEADER_TEMPLATE;
        header[2]  = static_cast<Byte>(seg_length >> 8);
        header[3]  = static_cast<Byte>(seg_length & 0xFF);

        // ICC spec defines sequence number and total as 1 byte each,
        // but we use 2 bytes (big-endian) for the sequence value to
        // support >255 segments. Tested across major image viewers,
        // editors without issue.
        header[15] = static_cast<Byte>(seg >> 8);
        header[16] = static_cast<Byte>(seg & 0xFF);

        result.insert(result.end(), header.begin(), header.end());
        result.insert(result.end(),
            segment_vec.cbegin() + offset,
            segment_vec.cbegin() + offset + data_size);

        offset += data_size;
    }

    // Free segment_vec memory — it's been consumed.
    segment_vec.clear();
    segment_vec.shrink_to_fit();

    // Restore SOI at the front.
    result.insert(result.begin(), soi_bytes.begin(), soi_bytes.begin() + SOI_SIG_LENGTH);

    return result;
}

// Data file is small enough to fit within the single, default ICC profile.
vBytes buildSingleSegmentICC(vBytes& segment_vec) {
    constexpr std::size_t
        SOI_SIG_LENGTH            = 2,
        SEGMENT_SIG_LENGTH        = 2,
        SEGMENT_HEADER_SIZE_INDEX = 0x04,
        PROFILE_SIZE_INDEX        = 0x16,
        PROFILE_SIZE_DIFF         = 16;

    std::size_t
        segment_size = segment_vec.size() - (SOI_SIG_LENGTH + SEGMENT_SIG_LENGTH),
        profile_size = segment_size - PROFILE_SIZE_DIFF;

    updateValue(segment_vec, SEGMENT_HEADER_SIZE_INDEX, segment_size);
    updateValue(segment_vec, PROFILE_SIZE_INDEX, profile_size);

    return std::move(segment_vec);
}

void applyRedditPadding(vBytes& jpg_vec, vBytes& data_vec, const vBytes& soi_bytes) {
    constexpr std::size_t
        SOI_SIG_LENGTH = 2,
        EOI_SIG_LENGTH = 2,
        PADDING_SIZE   = 8000;

    constexpr Byte
        PADDING_START = 33,
        PADDING_RANGE = 94;

    jpg_vec.insert(jpg_vec.begin(), soi_bytes.begin(), soi_bytes.begin() + SOI_SIG_LENGTH);

    // Important for Reddit: downloading an embedded image from Reddit can sometimes
    // result in a truncated, corrupt data file. These padding bytes absorb truncation
    // so the actual data is preserved.
    vBytes padding_vec = { 0xFF, 0xE2, 0x1F, 0x42 };
    padding_vec.reserve(padding_vec.size() + PADDING_SIZE);
    for (std::size_t i = 0; i < PADDING_SIZE; ++i) {
        padding_vec.emplace_back(PADDING_START + static_cast<Byte>(randombytes_uniform(PADDING_RANGE)));
    }

    jpg_vec.reserve(jpg_vec.size() + padding_vec.size() + data_vec.size());
    jpg_vec.insert(jpg_vec.end() - EOI_SIG_LENGTH, padding_vec.begin(), padding_vec.end());
    jpg_vec.insert(jpg_vec.end() - EOI_SIG_LENGTH, data_vec.begin() + SOI_SIG_LENGTH, data_vec.end());
}

void segmentDataFile(vBytes& segment_vec, vBytes& data_vec, vBytes& jpg_vec, vString& platforms_vec, bool hasRedditOption) {
    constexpr std::size_t
        SOI_SIG_LENGTH                = 2,
        SEGMENT_SIG_LENGTH            = 2,
        SEGMENT_HEADER_LENGTH         = 16,
        SEGMENT_DATA_SIZE             = 65519,
        PROFILE_DATA_SIZE             = 851,
        DEFLATED_DATA_FILE_SIZE_INDEX = 0x2E2,
        VALUE_BYTE_LENGTH             = 4;

    std::size_t max_first_segment_size = SEGMENT_DATA_SIZE + SOI_SIG_LENGTH + SEGMENT_SIG_LENGTH + SEGMENT_HEADER_LENGTH;

    // Preserve SOI (start of image bytes) before any changes.
    vBytes soi_bytes(segment_vec.begin(), segment_vec.begin() + SOI_SIG_LENGTH);

    if (segment_vec.size() > max_first_segment_size) {
        data_vec = buildMultiSegmentICC(segment_vec, soi_bytes);
    } else {
        data_vec = buildSingleSegmentICC(segment_vec);
    }

    updateValue(data_vec, DEFLATED_DATA_FILE_SIZE_INDEX, data_vec.size() - PROFILE_DATA_SIZE, VALUE_BYTE_LENGTH);

    if (hasRedditOption) {
        applyRedditPadding(jpg_vec, data_vec, soi_bytes);

        // Keep only the Reddit compatibility report entry.
        platforms_vec[0] = std::move(platforms_vec[5]);
        platforms_vec.resize(1);
    } else {
        segment_vec = std::move(data_vec);

        // Remove Bluesky and Reddit from the compatibility report.
        platforms_vec.erase(platforms_vec.begin() + 5);
        platforms_vec.erase(platforms_vec.begin() + 2);

        // Small files: merge now. Large files: leave separate for split write.
        constexpr std::size_t SIZE_THRESHOLD = 20 * 1024 * 1024;

        if (segment_vec.size() < SIZE_THRESHOLD) {
            segment_vec.reserve(segment_vec.size() + jpg_vec.size());
            segment_vec.insert(segment_vec.end(), jpg_vec.begin(), jpg_vec.end());
            jpg_vec = std::move(segment_vec);
        }
    }
    data_vec.clear();
    data_vec.shrink_to_fit();
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
