#pragma once

#include "jpeg_header_info.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace twitter_steg_internal {

using Bytes = std::vector<std::uint8_t>;

struct ImageInfo : jpeg_codec::HeaderInfo {
    int estimated_quality{};
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
// (left untouched otherwise). input_info_out, when given, receives the
// input's own header description, so a caller needing it does not parse the
// header again.
[[nodiscard]] Bytes prepareProgressiveSourceQuality(
    std::span<const std::uint8_t> jpeg,
    CoefficientImage* coefficients_out = nullptr,
    ImageInfo* input_info_out = nullptr);
// Ownership-taking overload: a compatible cover retains its allocation rather
// than copying the JPEG. The span overload keeps borrowed callers independent.
[[nodiscard]] Bytes prepareProgressiveSourceQuality(
    Bytes&& jpeg,
    CoefficientImage* coefficients_out = nullptr,
    ImageInfo* input_info_out = nullptr);
// The luminance pixels libjpeg decodes (JDCT_ISLOW) from the JPEG
// `coefficients` was read from, computed from the coefficients already in
// hand instead of entropy-decoding the file a second time. Requires a full-resolution
// luminance component, which every prepared carrier has.
[[nodiscard]] std::vector<std::uint8_t> luminanceFromCoefficients(
    const CoefficientImage& coefficients);
// cover: the image is a cover the carrier is prepared from as it is. Refuse
// it on any libjpeg warning (see jpeg_codec::rejectWarnings) instead of
// decoding on, and on any quantization table writeProgressiveCoefficients
// would carry into a non-conformant carrier: a zero entry (T.81 B.2.4.1
// requires Qk >= 1), or a luminance table redefined between scans.
[[nodiscard]] CoefficientImage readCoefficients(
    std::span<const std::uint8_t> jpeg,
    bool cover = false);
[[nodiscard]] Bytes writeProgressiveCoefficients(
    std::span<const std::uint8_t> source_jpeg,
    std::span<const std::int16_t> luminance_coefficients);

} // namespace twitter_steg_internal
