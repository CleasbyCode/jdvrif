#pragma once

// Helpers shared by the metadata-stripping coefficient carriers (Reddit C3 and
// X-Twitter J-UNIWARD/STC) and by the JPEG quality gate in jpeg_utils.cpp.
//
// This header is compiled into the C++ CLI, the web variant and the Rust port's
// FFI build (C++20 there), so it depends on the standard library alone. Keep it
// byte-identical across the three trees.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

// Both keys a carrier recovery may need.
//
// `current` is the Argon2id-derived key that locates RQSTEG2 / JXSTEG3 carriers
// (deriveCarrierKeyFromPin). Deriving it costs one Argon2id run, so a PIN guess
// cannot be tested against a carrier header any faster than against the
// encrypted envelope itself.
//
// `legacy` is the cheap BLAKE2b key used by the jdvrif v9.0 formats (RQSTEG1 /
// JXSTEG2). Recovery still reads those images, but nothing writes them.
struct CarrierKeys {
    std::uint64_t current{0};
    std::uint64_t legacy{0};
};

namespace carrier_common {

// SplitMix64 finaliser: the keyed hash behind every carrier permutation,
// whitening stream, dither offset and parity-change direction.
[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

// CRC-32 (IEEE 802.3, reflected). Integrity only -- never authentication; the
// payload it covers is secretstream-authenticated separately.
[[nodiscard]] inline std::uint32_t crc32(std::span<const std::uint8_t> data) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

inline void appendU32Le(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

[[nodiscard]] inline std::uint32_t readU32Le(
    std::span<const std::uint8_t> input,
    std::size_t offset,
    const char* truncated_error) {

    if (offset > input.size() || input.size() - offset < 4U) {
        throw std::runtime_error(truncated_error);
    }
    std::uint32_t value = 0;
    for (unsigned int byte = 0; byte < 4; ++byte) {
        value |= static_cast<std::uint32_t>(input[offset + byte]) << (byte * 8U);
    }
    return value;
}

using QuantizationTable = std::array<unsigned int, 64>;

// IJG (Annex K) quality-50 base tables, natural order.
inline constexpr QuantizationTable STD_LUMA_Q50{
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,
    24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99
};

inline constexpr QuantizationTable STD_CHROMA_Q50{
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};

// libjpeg's jpeg_quality_scaling + baseline clamp, applied to `base`.
[[nodiscard]] constexpr QuantizationTable scaledQuantTable(
    const QuantizationTable& base,
    int quality) noexcept {

    const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    QuantizationTable result{};
    for (std::size_t i = 0; i < base.size(); ++i) {
        int value = (static_cast<int>(base[i]) * scale + 50) / 100;
        if (value < 1) value = 1;
        if (value > 255) value = 255;
        result[i] = static_cast<unsigned int>(value);
    }
    return result;
}

// Estimated IJG quality (1..100) of a luminance table, from the sum of its 64
// entries: the quality whose standard scaled table has the nearest sum.
//
// This is the one estimator for the whole program. It decides both the default
// conceal path's Q97 ceiling and which X-Twitter images count as carriers, so an
// exact tie between two qualities resolves to the LOWER quality -- the rule the
// X-Twitter carrier format was defined with.
[[nodiscard]] constexpr int estimateQualityFromLumaSum(std::uint64_t table_sum) noexcept {
    int best_quality = 1;
    std::uint64_t best_distance = std::numeric_limits<std::uint64_t>::max();
    for (int quality = 1; quality <= 100; ++quality) {
        std::uint64_t expected_sum = 0;
        for (const unsigned int value : scaledQuantTable(STD_LUMA_Q50, quality)) {
            expected_sum += value;
        }
        const std::uint64_t distance = table_sum > expected_sum
            ? table_sum - expected_sum
            : expected_sum - table_sum;
        if (distance < best_distance) {
            best_distance = distance;
            best_quality = quality;
        }
    }
    return best_quality;
}

// 186 is equidistant from the Q97 (221) and Q98 (151) table sums.
static_assert(estimateQualityFromLumaSum(186) == 97 &&
              estimateQualityFromLumaSum(221) == 97 &&
              estimateQualityFromLumaSum(64) == 100 &&
              estimateQualityFromLumaSum(16320) == 1,
              "quality estimator tie rule or table drifted");

} // namespace carrier_common
