#pragma once

#include "twitter_jpeg_codec.h"

#include <cstdint>
#include <span>
#include <vector>

namespace twitter_steg_internal {

struct CarrierLayout {
    std::vector<std::uint32_t> coefficient_offsets;
    std::uint64_t nonzero_ac_count{};
};

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

[[nodiscard]] CarrierLayout makeCarrierLayout(
    const CoefficientImage& coefficients,
    std::uint64_t seed);

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
