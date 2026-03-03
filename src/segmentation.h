#pragma once

#include "common.h"

#include <span>

[[nodiscard]] vBytes buildMultiSegmentICC(vBytes& segment_vec, std::span<const Byte> soi_bytes);
[[nodiscard]] vBytes buildSingleSegmentICC(vBytes& segment_vec);
void applyRedditPadding(vBytes& jpg_vec, vBytes& data_vec, const vBytes& soi_bytes);
void segmentDataFile(vBytes& segment_vec, vBytes& data_vec, vBytes& jpg_vec, vString& platforms_vec, bool hasRedditOption);
void filterPlatforms(vString& platforms_vec, std::size_t embedded_size, uint16_t first_segment_size, uint16_t total_segments);
