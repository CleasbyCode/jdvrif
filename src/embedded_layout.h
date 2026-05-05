#pragma once

#include "common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

struct EmbeddedCipherLayout { std::size_t template_kdf_metadata_index{}, embedded_kdf_metadata_index{}, file_size_index{}, encrypted_payload_start_index{}; };

struct IccSegmentLayout {
    std::size_t segment_header_size_index{}, profile_size_index{}, compression_flag_index{}, embedded_total_profile_header_segments_index{};
    std::size_t template_total_profile_header_segments_index{}, encrypted_file_size_index{}, profile_header_insert_index{}, profile_header_length{};
    std::size_t per_segment_payload_size{}, per_segment_stride{}, marker_back_offset{}, profile_data_size{};
    std::size_t profile_size_diff{}, segment_header_length{}, initial_header_bytes{};
};

struct BlueskySegmentLayout {
    std::size_t exif_segment_data_size_limit{}, compressed_file_size_index{}, exif_segment_data_insert_index{}, exif_segment_size_index{};
    std::size_t artist_field_size_index{}, artist_field_size_diff{}, first_dataset_size_index{}, first_dataset_size_limit{};
    std::size_t last_dataset_size_limit{}, xmp_segment_size_limit{}, xmp_footer_size{}, photoshop_default_size{};
    std::size_t segment_size_index{}, bim_section_size_index{}, bim_section_size_diff{}, dataset_max_size{};
    std::size_t pshop_segment_size_index_diff{}, first_dataset_size_index_diff{}, dataset_file_index_diff{}, second_dataset_size_index_diff{};
    std::size_t xmp_base64_max_scan_bytes{}, exif_data_end_without_xmp{}, exif_data_end_with_xmp{}; Byte base64_end_sig{};
};

inline constexpr EmbeddedCipherLayout ICC_CIPHER_LAYOUT{
    .template_kdf_metadata_index = 0x313, .embedded_kdf_metadata_index = 0x2FB, .file_size_index = 0x2CA, .encrypted_payload_start_index = 0x33B};

inline constexpr EmbeddedCipherLayout BLUESKY_CIPHER_LAYOUT{
    .template_kdf_metadata_index = 0x18D, .embedded_kdf_metadata_index = 0x18D, .file_size_index = 0x1CD, .encrypted_payload_start_index = 0x1D1};

inline constexpr IccSegmentLayout ICC_SEGMENT_LAYOUT{
    .segment_header_size_index = 0x04, .profile_size_index = 0x16, .compression_flag_index = 0x68, .embedded_total_profile_header_segments_index = 0x2C8,
    .template_total_profile_header_segments_index = 0x2E0, .encrypted_file_size_index = 0x2E2, .profile_header_insert_index = 0xFCB0, .profile_header_length = 18,
    .per_segment_payload_size = 65519, .per_segment_stride = 65537, .marker_back_offset = 0x16, .profile_data_size = 851, .profile_size_diff = 16,
    .segment_header_length = 16, .initial_header_bytes = 20};

inline constexpr BlueskySegmentLayout BLUESKY_SEGMENT_LAYOUT{
    .exif_segment_data_size_limit = 65027, .compressed_file_size_index = 0x1CD, .exif_segment_data_insert_index = 0x1D1, .exif_segment_size_index = 0x04,
    .artist_field_size_index = 0x4A, .artist_field_size_diff = 140, .first_dataset_size_index = 0x21, .first_dataset_size_limit = 32767,
    .last_dataset_size_limit = 32730, .xmp_segment_size_limit = 60033, .xmp_footer_size = 92, .photoshop_default_size = 35, .segment_size_index = 0x02,
    .bim_section_size_index = 0x1C, .bim_section_size_diff = 28, .dataset_max_size = 32800, .pshop_segment_size_index_diff = 7, .first_dataset_size_index_diff = 24,
    .dataset_file_index_diff = 2, .second_dataset_size_index_diff = 3, .xmp_base64_max_scan_bytes = 128 * 1024, .exif_data_end_without_xmp = 55,
    .exif_data_end_with_xmp = 351, .base64_end_sig = 0x3C};

inline constexpr auto JDVRIF_SIGNATURE = std::to_array<Byte>({0xB4, 0x6A, 0x3E, 0xEA, 0x5E, 0x9D, 0xF9});
inline constexpr auto ICC_PROFILE_SIGNATURE = std::to_array<Byte>({0x6D, 0x6E, 0x74, 0x72, 0x52, 0x47, 0x42});
inline constexpr auto JPEG_APP2_MARKER = std::to_array<Byte>({0xFF, 0xE2});
inline constexpr auto JPEG_EXIF_MARKER = std::to_array<Byte>({0xFF, 0xE1});
inline constexpr auto BLUESKY_PHOTOSHOP_SIGNATURE = std::to_array<Byte>({0x73, 0x68, 0x6F, 0x70, 0x20, 0x33, 0x2E});
inline constexpr auto BLUESKY_XMP_CREATOR_SIGNATURE = std::to_array<Byte>({0x3C, 0x72, 0x64, 0x66, 0x3A, 0x6C, 0x69});
inline constexpr std::size_t ICC_PROFILE_SIGNATURE_OFFSET = 8;
inline constexpr std::size_t ICC_SCAN_BACKWARD_WINDOW = 128 * 1024;
inline constexpr std::size_t JDVRIF_TO_ICC_SIGNATURE_OFFSET = 811;

[[nodiscard]] constexpr const EmbeddedCipherLayout& embeddedCipherLayout(bool is_bluesky_file) noexcept { return is_bluesky_file ? BLUESKY_CIPHER_LAYOUT : ICC_CIPHER_LAYOUT; }

[[nodiscard]] constexpr std::size_t templateKdfMetadataIndex(bool has_bluesky_option) noexcept { return embeddedCipherLayout(has_bluesky_option).template_kdf_metadata_index; }

[[nodiscard]] constexpr std::optional<std::size_t> iccTrailingMarkerIndex(std::uint16_t total_profile_header_segments) noexcept {
    if (total_profile_header_segments == 0) return std::nullopt;

    const std::size_t segment_count = static_cast<std::size_t>(total_profile_header_segments) - 1;
    if (segment_count > std::numeric_limits<std::size_t>::max() / ICC_SEGMENT_LAYOUT.per_segment_stride) return std::nullopt;

    const std::size_t marker_offset = segment_count * ICC_SEGMENT_LAYOUT.per_segment_stride;
    if (marker_offset < ICC_SEGMENT_LAYOUT.marker_back_offset) return std::nullopt;
    return marker_offset - ICC_SEGMENT_LAYOUT.marker_back_offset;
}
