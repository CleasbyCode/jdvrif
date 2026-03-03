#include "segmentation.h"
#include "binary_io.h"
#include "file_utils.h"

#include <algorithm>
#include <limits>
#include <string>
#include <stdexcept>
#include <utility>

namespace {
constexpr std::size_t
    SOI_SIG_LENGTH                = 2,
    EOI_SIG_LENGTH                = 2,
    SEGMENT_SIG_LENGTH            = 2,
    SEGMENT_HEADER_LENGTH         = 16,
    SEGMENT_DATA_SIZE             = 65519,
    INITIAL_HEADER_BYTES          = SOI_SIG_LENGTH + SEGMENT_SIG_LENGTH + SEGMENT_HEADER_LENGTH,
    PROFILE_DATA_SIZE             = 851,
    PROFILE_SIZE_DIFF             = 16,
    SEGMENT_HEADER_SIZE_INDEX     = 0x04,
    PROFILE_SIZE_INDEX            = 0x16,
    SEGMENTS_TOTAL_VAL_INDEX      = 0x2E0,
    DEFLATED_DATA_FILE_SIZE_INDEX = 0x2E2,
    VALUE_BYTE_LENGTH             = 4,
    REDDIT_PLATFORM_INDEX         = 5,
    BLUESKY_PLATFORM_INDEX        = 2,
    SIZE_THRESHOLD                = 20 * 1024 * 1024;

constexpr Byte
    PADDING_START = 33,
    PADDING_RANGE = 94;

constexpr std::size_t PADDING_SIZE = 8000;

constexpr auto ICC_HEADER_TEMPLATE = std::to_array<Byte>({
    0xFF, 0xE2, 0x00, 0x00,
    'I','C','C','_','P','R','O','F','I','L','E',
    0x00, 0x00, 0x01
});

[[nodiscard]] std::array<Byte, 18> makeIccHeader(uint16_t segment_length, std::size_t segment_index) {
    auto header = ICC_HEADER_TEMPLATE;
    header[2] = static_cast<Byte>(segment_length >> 8);
    header[3] = static_cast<Byte>(segment_length & 0xFF);

    // ICC spec defines sequence number and total as 1 byte each,
    // but we use 2 bytes (big-endian) for the sequence value to
    // support >255 segments. Tested across major image viewers,
    // editors without issue.
    header[15] = static_cast<Byte>(segment_index >> 8);
    header[16] = static_cast<Byte>(segment_index & 0xFF);
    return header;
}

void requirePlatformSlots(const vString& platforms_vec) {
    if (platforms_vec.size() <= REDDIT_PLATFORM_INDEX) {
        throw std::runtime_error("Internal Error: Corrupt platform compatibility list.");
    }
}
}

// Data file is too large for a single ICC segment, so split the data file and store it within multiple segments.
vBytes buildMultiSegmentICC(vBytes& segment_vec, std::span<const Byte> soi_bytes) {
    if (segment_vec.size() < INITIAL_HEADER_BYTES || soi_bytes.size() < SOI_SIG_LENGTH) {
        throw std::runtime_error("File Extraction Error: Corrupt segment header.");
    }

    const std::size_t payload_size = segment_vec.size() - INITIAL_HEADER_BYTES;
    if (payload_size == 0) {
        throw std::runtime_error("File Extraction Error: Corrupt segment payload.");
    }

    const std::size_t segments_required =
        (payload_size / SEGMENT_DATA_SIZE) + ((payload_size % SEGMENT_DATA_SIZE) != 0 ? 1 : 0);
    if (segments_required == 0 ||
        segments_required > static_cast<std::size_t>(std::numeric_limits<uint16_t>::max())) {
        throw std::runtime_error("File Size Error: Segment count exceeds supported limit.");
    }

    updateValue(segment_vec, SEGMENTS_TOTAL_VAL_INDEX, segments_required);

    vBytes result;
    const std::size_t per_segment_overhead = SEGMENT_SIG_LENGTH + SEGMENT_HEADER_LENGTH;
    if (segments_required > (std::numeric_limits<std::size_t>::max() - SOI_SIG_LENGTH - payload_size) / per_segment_overhead) {
        throw std::runtime_error("File Size Error: Segment output size overflow.");
    }
    result.reserve(SOI_SIG_LENGTH + payload_size + (segments_required * per_segment_overhead));
    result.insert(result.end(), soi_bytes.begin(), soi_bytes.begin() + SOI_SIG_LENGTH);

    const std::span<const Byte> payload(segment_vec.data() + INITIAL_HEADER_BYTES, payload_size);

    for (std::size_t seg = 1; seg <= segments_required; ++seg) {
        const std::size_t offset = (seg - 1) * SEGMENT_DATA_SIZE;
        const std::size_t data_size = std::min(SEGMENT_DATA_SIZE, payload_size - offset);
        const uint16_t seg_length = static_cast<uint16_t>(data_size + SEGMENT_HEADER_LENGTH);

        const auto header = makeIccHeader(seg_length, seg);

        if (!spanHasRange(payload, offset, data_size)) {
            throw std::runtime_error("File Extraction Error: Corrupt segment index.");
        }

        const std::span<const Byte> chunk = payload.subspan(offset, data_size);
        result.insert(result.end(), header.begin(), header.end());
        result.insert(result.end(), chunk.begin(), chunk.end());
    }

    // Free segment_vec memory — it's been consumed.
    segment_vec.clear();

    return result;
}

// Data file is small enough to fit within the single, default ICC profile.
vBytes buildSingleSegmentICC(vBytes& segment_vec) {
    if (segment_vec.size() < SOI_SIG_LENGTH + SEGMENT_SIG_LENGTH + PROFILE_SIZE_DIFF) {
        throw std::runtime_error("File Extraction Error: Corrupt ICC segment.");
    }

    std::size_t
        segment_size = segment_vec.size() - (SOI_SIG_LENGTH + SEGMENT_SIG_LENGTH),
        profile_size = segment_size - PROFILE_SIZE_DIFF;

    updateValue(segment_vec, SEGMENT_HEADER_SIZE_INDEX, segment_size);
    updateValue(segment_vec, PROFILE_SIZE_INDEX, profile_size);

    return std::move(segment_vec);
}

void applyRedditPadding(vBytes& jpg_vec, vBytes& data_vec, const vBytes& soi_bytes) {
    if (soi_bytes.size() < SOI_SIG_LENGTH || jpg_vec.size() < EOI_SIG_LENGTH || data_vec.size() < SOI_SIG_LENGTH) {
        throw std::runtime_error("File Extraction Error: Corrupt image or data segment.");
    }

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
    std::size_t max_first_segment_size = SEGMENT_DATA_SIZE + SOI_SIG_LENGTH + SEGMENT_SIG_LENGTH + SEGMENT_HEADER_LENGTH;

    if (segment_vec.size() < SOI_SIG_LENGTH) {
        throw std::runtime_error("File Extraction Error: Corrupt segment header.");
    }

    // Preserve SOI (start of image bytes) before any changes.
    vBytes soi_bytes(segment_vec.begin(), segment_vec.begin() + SOI_SIG_LENGTH);

    if (segment_vec.size() > max_first_segment_size) {
        data_vec = buildMultiSegmentICC(segment_vec, soi_bytes);
    } else {
        data_vec = buildSingleSegmentICC(segment_vec);
    }

    if (data_vec.size() < PROFILE_DATA_SIZE) {
        throw std::runtime_error("File Extraction Error: Corrupt profile size metadata.");
    }

    updateValue(data_vec, DEFLATED_DATA_FILE_SIZE_INDEX, data_vec.size() - PROFILE_DATA_SIZE, VALUE_BYTE_LENGTH);

    if (hasRedditOption) {
        requirePlatformSlots(platforms_vec);
        applyRedditPadding(jpg_vec, data_vec, soi_bytes);

        // Keep only the Reddit compatibility report entry.
        platforms_vec[0] = std::move(platforms_vec[REDDIT_PLATFORM_INDEX]);
        platforms_vec.resize(1);
    } else {
        requirePlatformSlots(platforms_vec);
        segment_vec = std::move(data_vec);

        // Remove Bluesky and Reddit from the compatibility report.
        platforms_vec.erase(platforms_vec.begin() + static_cast<std::ptrdiff_t>(REDDIT_PLATFORM_INDEX));
        platforms_vec.erase(platforms_vec.begin() + static_cast<std::ptrdiff_t>(BLUESKY_PLATFORM_INDEX));

        if (segment_vec.size() < SIZE_THRESHOLD) {
            segment_vec.reserve(segment_vec.size() + jpg_vec.size());
            segment_vec.insert(segment_vec.end(), jpg_vec.begin(), jpg_vec.end());
            jpg_vec = std::move(segment_vec);
        }
    }
    data_vec.clear();
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
