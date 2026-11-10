#pragma once

// Test-only: the X-Twitter carrier's unshuffled candidate list, written out
// from its definition rather than taken from jdvrif's own enumeration. A
// candidate is a luminance AC coefficient whose pixel lies inside the image;
// they are listed block row by block row (top to bottom), block column by
// block column, and row-major within a block. jdvrif never builds this list
// -- it maps indices with CandidateOffsets -- so the tests use it to check
// that mapping, and the keyed order drawn over it, independently.

#include "twitter_jpeg_codec.h"

#include <cstdint>
#include <vector>

struct ReferenceCarrierLayout {
    std::vector<std::uint32_t> coefficient_offsets;
    std::uint64_t nonzero_ac_count{};
};

[[nodiscard]] inline ReferenceCarrierLayout referenceCarrierLayout(
    const twitter_steg_internal::CoefficientImage& coefficients) {
    const auto& info = coefficients.info;
    ReferenceCarrierLayout layout;
    for (std::uint64_t block_row = 0; block_row < info.y_blocks_high; ++block_row) {
        for (std::uint64_t block_column = 0; block_column < info.y_blocks_wide; ++block_column) {
            for (std::uint64_t row = 0; row < 8; ++row) {
                for (std::uint64_t column = 0; column < 8; ++column) {
                    const bool dc = row == 0 && column == 0;
                    const bool inside = block_row * 8 + row < info.height &&
                                        block_column * 8 + column < info.width;
                    if (dc || !inside) continue;
                    const std::uint64_t offset =
                        (block_row * info.y_blocks_wide + block_column) * 64 + row * 8 + column;
                    layout.coefficient_offsets.push_back(static_cast<std::uint32_t>(offset));
                    if (coefficients.luminance[offset] != 0) ++layout.nonzero_ac_count;
                }
            }
        }
    }
    return layout;
}
