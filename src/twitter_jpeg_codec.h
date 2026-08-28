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
// Produces the progressive 4:2:0 JFIF carrier while retaining source
// quantization. Ordinary YCbCr 4:2:0 inputs take a coefficient-domain path,
// avoiding another lossy JPEG generation. Other supported inputs are sampled
// to 4:2:0 with their source-derived tables. Source quality above Q97 is capped.
[[nodiscard]] Bytes prepareProgressiveSourceQuality(
    std::span<const std::uint8_t> jpeg);
[[nodiscard]] std::vector<std::uint8_t> decodeLuminance(
    std::span<const std::uint8_t> jpeg,
    ImageInfo* decoded_info = nullptr);
[[nodiscard]] CoefficientImage readCoefficients(std::span<const std::uint8_t> jpeg);
[[nodiscard]] Bytes writeProgressiveCoefficients(
    std::span<const std::uint8_t> source_jpeg,
    std::span<const std::int16_t> luminance_coefficients);

} // namespace twitter_steg_internal
