#pragma once

#include "twitter_jpeg_codec.h"

#include <cstdint>
#include <span>
#include <vector>

namespace twitter_steg_internal {

// Candidate tallies only -- the two numbers calculateCapacity() needs. Both are
// invariant under the keyed shuffle, so capacity can be answered without
// building (or permuting) the offset array, which at the 4096x4096 ceiling is a
// 64 MB allocation and a 16-million-element Fisher-Yates pass.
struct CarrierCandidateCounts {
    std::uint64_t candidate_count{};
    std::uint64_t nonzero_ac_count{};
};

[[nodiscard]] CarrierCandidateCounts countCarrierCandidates(
    const CoefficientImage& coefficients);

// The carrier candidates in their unshuffled order -- every luminance AC
// coefficient whose pixel lies inside the image, block rows top to bottom,
// block columns left to right, positions row-major within a block -- mapped
// one index at a time instead of listed: the carriers draw a keyed prefix of
// indices (carrier_common::KeyedIndexPrefix) and map only those, so the
// 4-bytes-per-candidate list is never built.
class CandidateOffsets {
public:
    // Checks the coefficient array's geometry first.
    explicit CandidateOffsets(const CoefficientImage& coefficients);

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    // index < size().
    [[nodiscard]] std::uint32_t operator[](std::uint64_t index) const noexcept;

private:
    // Candidates in one block row whose blocks are `rows` pixel rows tall.
    [[nodiscard]] std::uint64_t rowCount(std::uint64_t rows) const noexcept;

    std::uint64_t blocks_wide_{};
    std::uint64_t full_columns_{};
    std::uint64_t partial_width_{};
    std::uint64_t full_rows_{};
    std::uint64_t partial_height_{};
    std::uint64_t size_{};
};

[[nodiscard]] std::vector<float> computeJUniwardCosts(
    std::span<const std::uint8_t> luminance,
    const CoefficientImage& coefficients,
    std::span<const std::uint32_t> selected_offsets);

[[nodiscard]] std::vector<std::uint8_t> coefficientParityBits(
    std::span<const std::int16_t> coefficients,
    std::span<const std::uint32_t> selected_offsets);

[[nodiscard]] std::uint64_t applyParityBits(
    std::span<std::int16_t> coefficients,
    std::span<const std::uint32_t> selected_offsets,
    std::span<const std::uint8_t> desired_bits,
    std::uint64_t seed);

} // namespace twitter_steg_internal
