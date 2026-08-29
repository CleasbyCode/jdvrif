#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace twitter_steg_internal {

constexpr unsigned int kStcConstraintHeight = 7;
constexpr unsigned int kRateNumerator = 2;
constexpr unsigned int kRateDenominator = 5;

struct StcEmbedResult {
    std::vector<std::uint8_t> stego_bits;
    std::uint64_t changes{};
    double distortion{};
};

[[nodiscard]] std::uint64_t requiredCoverSymbols(std::uint64_t message_bits);

[[nodiscard]] StcEmbedResult stcEmbed(
    std::span<const std::uint8_t> cover_bits,
    std::span<const float> flip_costs,
    std::span<const std::uint8_t> message_bits);

[[nodiscard]] std::vector<std::uint8_t> stcExtract(
    std::span<const std::uint8_t> stego_bits,
    std::size_t message_bit_count);

} // namespace twitter_steg_internal
