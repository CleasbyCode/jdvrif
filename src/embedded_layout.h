#pragma once

#include "common.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

struct EmbeddedCipherLayout {
    std::size_t template_kdf_metadata_index{};
    std::size_t embedded_kdf_metadata_index{};
    std::size_t file_size_index{};
    std::size_t encrypted_payload_start_index{};
};

struct IccSegmentLayout {
    std::size_t segment_header_size_index{};
    std::size_t profile_size_index{};
    std::size_t compression_flag_index{};
    std::size_t embedded_total_profile_header_segments_index{};
    std::size_t template_total_profile_header_segments_index{};
    std::size_t encrypted_file_size_index{};
    std::size_t profile_header_insert_index{};
    std::size_t profile_header_length{};
    std::size_t per_segment_payload_size{};
    std::size_t per_segment_stride{};
    std::size_t marker_back_offset{};
    std::size_t profile_data_size{};
    std::size_t profile_size_diff{};
    std::size_t segment_header_length{};
    std::size_t initial_header_bytes{};
};

struct BlueskySegmentLayout {
    std::size_t exif_segment_data_size_limit{};
    std::size_t compressed_file_size_index{};
    std::size_t exif_segment_data_insert_index{};
    std::size_t exif_segment_size_index{};
    std::size_t artist_field_size_index{};
    std::size_t artist_field_size_diff{};
    std::size_t first_dataset_size_index{};
    std::size_t first_dataset_size_limit{};
    std::size_t last_dataset_size_limit{};
    std::size_t xmp_segment_size_limit{};
    std::size_t xmp_footer_size{};
    std::size_t photoshop_default_size{};
    std::size_t segment_size_index{};
    std::size_t bim_section_size_index{};
    std::size_t bim_section_size_diff{};
    std::size_t dataset_max_size{};
    std::size_t pshop_segment_size_index_diff{};
    std::size_t first_dataset_size_index_diff{};
    std::size_t dataset_file_index_diff{};
    std::size_t second_dataset_size_index_diff{};
    std::size_t xmp_base64_max_scan_bytes{};
    Byte base64_end_sig{};
};

inline constexpr EmbeddedCipherLayout ICC_CIPHER_LAYOUT{
    .template_kdf_metadata_index = 0x313,
    .embedded_kdf_metadata_index = 0x2FB,
    .file_size_index = 0x2CA,
    .encrypted_payload_start_index = 0x33B
};

inline constexpr EmbeddedCipherLayout BLUESKY_CIPHER_LAYOUT{
    .template_kdf_metadata_index = 0x18D,
    .embedded_kdf_metadata_index = 0x18D,
    .file_size_index = 0x1CD,
    .encrypted_payload_start_index = 0x1D1
};

inline constexpr IccSegmentLayout ICC_SEGMENT_LAYOUT{
    .segment_header_size_index = 0x04,
    .profile_size_index = 0x16,
    .compression_flag_index = 0x68,
    .embedded_total_profile_header_segments_index = 0x2C8,
    .template_total_profile_header_segments_index = 0x2E0,
    .encrypted_file_size_index = 0x2E2,
    .profile_header_insert_index = 0xFCB0,
    .profile_header_length = 18,
    .per_segment_payload_size = 65519,
    .per_segment_stride = 65537,
    .marker_back_offset = 0x16,
    .profile_data_size = 851,
    .profile_size_diff = 16,
    .segment_header_length = 16,
    .initial_header_bytes = 20
};

inline constexpr BlueskySegmentLayout BLUESKY_SEGMENT_LAYOUT{
    .exif_segment_data_size_limit = 65027,
    .compressed_file_size_index = 0x1CD,
    .exif_segment_data_insert_index = 0x1D1,
    .exif_segment_size_index = 0x04,
    .artist_field_size_index = 0x4A,
    .artist_field_size_diff = 140,
    .first_dataset_size_index = 0x21,
    .first_dataset_size_limit = 32767,
    .last_dataset_size_limit = 32730,
    .xmp_segment_size_limit = 60033,
    .xmp_footer_size = 92,
    .photoshop_default_size = 35,
    .segment_size_index = 0x02,
    .bim_section_size_index = 0x1C,
    .bim_section_size_diff = 28,
    .dataset_max_size = 32800,
    .pshop_segment_size_index_diff = 7,
    .first_dataset_size_index_diff = 24,
    .dataset_file_index_diff = 2,
    .second_dataset_size_index_diff = 3,
    .xmp_base64_max_scan_bytes = 128 * 1024,
    .base64_end_sig = 0x3C
};

inline constexpr auto JDVRIF_SIGNATURE = std::to_array<Byte>({0xB4, 0x6A, 0x3E, 0xEA, 0x5E, 0x9D, 0xF9});
inline constexpr auto ICC_PROFILE_SIGNATURE = std::to_array<Byte>({0x6D, 0x6E, 0x74, 0x72, 0x52, 0x47, 0x42});
inline constexpr auto JPEG_APP2_MARKER = std::to_array<Byte>({0xFF, 0xE2});
inline constexpr auto JPEG_EXIF_MARKER = std::to_array<Byte>({0xFF, 0xE1});
inline constexpr auto BLUESKY_PHOTOSHOP_SIGNATURE = std::to_array<Byte>({0x73, 0x68, 0x6F, 0x70, 0x20, 0x33, 0x2E});
inline constexpr auto BLUESKY_XMP_CREATOR_SIGNATURE = std::to_array<Byte>({0x3C, 0x72, 0x64, 0x66, 0x3A, 0x6C, 0x69});
inline constexpr std::size_t ICC_PROFILE_SIGNATURE_OFFSET = 8;
inline constexpr std::size_t JDVRIF_TO_ICC_SIGNATURE_OFFSET = 811;
inline constexpr std::size_t DEFAULT_METADATA_PREFIX_BYTES = 0x353;
inline constexpr std::size_t DEFAULT_TEMPLATE_BASE_OFFSET = 24;
inline constexpr std::size_t DEFAULT_ICC_SIGNATURE_INDEX_ABS =
    DEFAULT_TEMPLATE_BASE_OFFSET + ICC_PROFILE_SIGNATURE_OFFSET;
inline constexpr std::size_t DEFAULT_JDVRIF_SIGNATURE_INDEX_ABS =
    DEFAULT_TEMPLATE_BASE_OFFSET + JDVRIF_TO_ICC_SIGNATURE_OFFSET + ICC_PROFILE_SIGNATURE_OFFSET;

// conceal writes the default template at file offset 0, but an upload pipeline
// may put its own segments in front of it (X-Twitter adds a JFIF APP0) while
// keeping the rest of the file byte-for-byte. recover therefore walks leading
// APPn/COM segments, up to these bounds, to the first APP2, which must be
// jdvrif's ICC profile. Every later read is relative to where it was found.
inline constexpr std::size_t MAX_LEADING_SEGMENT_BYTES = 64 * 1024;
inline constexpr std::size_t MAX_LEADING_SEGMENTS = 16;
inline constexpr std::size_t DEFAULT_SIGNATURE_PREFIX_BYTES =
    MAX_LEADING_SEGMENT_BYTES + DEFAULT_JDVRIF_SIGNATURE_INDEX_ABS + JDVRIF_SIGNATURE.size();

// Returns the absolute index of the ICC profile signature of a default-mode
// image, or nullopt. `prefix` holds the file's first bytes from offset 0.
[[nodiscard]] inline std::optional<std::size_t> findDefaultIccSignatureIndex(std::span<const Byte> prefix) noexcept {
    const auto has_at = [prefix](std::size_t at, std::span<const Byte> sig) {
        return at <= prefix.size() && sig.size() <= prefix.size() - at &&
               std::equal(sig.begin(), sig.end(), prefix.begin() + static_cast<std::ptrdiff_t>(at));
    };
    if (prefix.size() < 2 || prefix[0] != 0xFF || prefix[1] != 0xD8) return std::nullopt;

    std::size_t pos = 2;
    for (std::size_t skipped = 0; skipped <= MAX_LEADING_SEGMENTS; ++skipped) {
        if (pos > prefix.size() || prefix.size() - pos < 4 || prefix[pos] != 0xFF) return std::nullopt;
        if (pos - 2 > MAX_LEADING_SEGMENT_BYTES) return std::nullopt;
        const Byte marker = prefix[pos + 1];
        if (marker == 0xE2) {
            const std::size_t base = pos - 2 + DEFAULT_TEMPLATE_BASE_OFFSET;
            const std::size_t icc_index = base + ICC_PROFILE_SIGNATURE_OFFSET;
            const bool signed_template =
                has_at(icc_index, ICC_PROFILE_SIGNATURE) &&
                has_at(icc_index + JDVRIF_TO_ICC_SIGNATURE_OFFSET, JDVRIF_SIGNATURE);
            return signed_template ? std::optional<std::size_t>{icc_index} : std::nullopt;
        }
        const bool skippable = (marker >= 0xE0 && marker <= 0xEF) || marker == 0xFE;
        if (!skippable) return std::nullopt;
        const std::size_t length = (static_cast<std::size_t>(prefix[pos + 2]) << 8) | prefix[pos + 3];
        if (length < 2) return std::nullopt;
        pos += 2 + length;
    }
    return std::nullopt;
}
// Where the Bluesky EXIF template (also written at file offset 0) carries the
// jdvrif signature. conceal checks the template against this.
inline constexpr std::size_t BLUESKY_JDVRIF_SIGNATURE_INDEX_ABS = 453;
static_assert(BLUESKY_JDVRIF_SIGNATURE_INDEX_ABS + JDVRIF_SIGNATURE.size() <=
              BLUESKY_CIPHER_LAYOUT.encrypted_payload_start_index);

[[nodiscard]] constexpr const EmbeddedCipherLayout& embeddedCipherLayout(bool is_bluesky_file) noexcept {
    return is_bluesky_file ? BLUESKY_CIPHER_LAYOUT : ICC_CIPHER_LAYOUT;
}

[[nodiscard]] constexpr std::optional<std::size_t> iccTrailingMarkerIndex(std::uint16_t total_profile_header_segments) noexcept {
    if (total_profile_header_segments == 0) return std::nullopt;

    const std::size_t segment_count = static_cast<std::size_t>(total_profile_header_segments) - 1;
    if (segment_count > std::numeric_limits<std::size_t>::max() / ICC_SEGMENT_LAYOUT.per_segment_stride) {
        return std::nullopt;
    }

    const std::size_t marker_offset = segment_count * ICC_SEGMENT_LAYOUT.per_segment_stride;
    if (marker_offset < ICC_SEGMENT_LAYOUT.marker_back_offset) return std::nullopt;
    return marker_offset - ICC_SEGMENT_LAYOUT.marker_back_offset;
}
