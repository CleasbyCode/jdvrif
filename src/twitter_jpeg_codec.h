#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace twitter_steg_internal {

using Bytes = std::vector<std::uint8_t>;

struct ImageInfo {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t y_blocks_wide{};
    std::uint32_t y_blocks_high{};
    int components{};
    bool progressive{};
    bool is_420{};
    bool is_ycbcr{};
    int estimated_quality{};

    [[nodiscard]] std::uint64_t pixelCount() const;
    [[nodiscard]] std::uint64_t yBlockCount() const;
};

struct CoefficientImage {
    ImageInfo info;
    std::vector<std::int16_t> luminance;
    std::array<std::uint16_t, 64> luminance_quantization{};
};

[[nodiscard]] ImageInfo inspectJpeg(std::span<const std::uint8_t> jpeg);
// Returns the JPEG the carrier is written from, keeping source quantization.
// Ordinary YCbCr 4:2:0 inputs (Q97 or below) need no re-encode: the input
// itself is returned, since writeProgressiveCoefficients rewrites its
// coefficients losslessly as progressive JFIF. Other supported inputs are
// re-encoded to 4:2:0 with their source-derived tables (above Q97 capped).
// coefficients_out, when given and the input is returned as is, receives its
// coefficients, described as the progressive carrier they will be written as
// (left untouched otherwise).
[[nodiscard]] Bytes prepareProgressiveSourceQuality(
    std::span<const std::uint8_t> jpeg,
    CoefficientImage* coefficients_out = nullptr);
[[nodiscard]] std::vector<std::uint8_t> decodeLuminance(
    std::span<const std::uint8_t> jpeg);
// reject_warnings: refuse the image on any libjpeg warning (cover preparation,
// see jpeg_codec::rejectWarnings) instead of decoding on.
[[nodiscard]] CoefficientImage readCoefficients(
    std::span<const std::uint8_t> jpeg,
    bool reject_warnings = false);
[[nodiscard]] Bytes writeProgressiveCoefficients(
    std::span<const std::uint8_t> source_jpeg,
    std::span<const std::int16_t> luminance_coefficients);

} // namespace twitter_steg_internal
