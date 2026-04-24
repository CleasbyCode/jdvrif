#pragma once

#include "common.h"

#include <iosfwd>
#include <span>

struct SegmentedEmbedSummary { std::size_t embedded_image_size{0}; uint16_t first_segment_size{0}; uint16_t total_segments{0}; };

void filterPlatforms(vString& platforms_vec, std::size_t embedded_size, uint16_t first_segment_size, uint16_t total_segments);
[[nodiscard]] SegmentedEmbedSummary writeEmbeddedJpgFromEncryptedFile(std::ostream& output, vBytes& segment_vec, const fs::path& encrypted_path, std::span<const Byte> jpg_vec, bool has_reddit_option);
