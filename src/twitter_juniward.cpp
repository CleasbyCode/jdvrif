#include "twitter_juniward.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace twitter_steg_internal {
namespace {

constexpr int kDctSize = 8;
constexpr int kFilterSize = 16;
constexpr int kPadding = 16;
constexpr int kImpactSize = 23;
constexpr int kImpactArea = kImpactSize * kImpactSize;
// Offset from a block's first pixel to the start of its impact window in the
// padded residual. See the derivation at the use site in computeJUniwardCosts.
constexpr std::size_t kImpactOrigin = static_cast<std::size_t>(kPadding) - 7U;
constexpr float kSigma = 0.015625F;
constexpr float kWetCost = 1.0e13F;
constexpr std::uint64_t kDirectionDomain = 0x5844535443444952ULL;

// Daubechies-8 analysis filters used by the original J-UNIWARD reference
// implementation. See Holub, Fridrich, and Denemark, "Universal Distortion
// Function for Steganography in an Arbitrary Domain" (EURASIP JIS, 2014).
constexpr std::array<double, kFilterSize> kLowPass{
    -0.00011747678400228192,
     0.00067544940599855677,
    -0.00039174037299597711,
    -0.0048703529930106603,
     0.0087460940470156547,
     0.013981027917015516,
    -0.044088253931064719,
    -0.017369301002022108,
     0.12874742662018601,
     0.00047248457399797254,
    -0.28401554296242809,
    -0.015829105256023893,
     0.58535468365486909,
     0.67563073629801285,
     0.31287159091446592,
     0.054415842243081609
};

constexpr std::array<double, kFilterSize> kHighPass{
    -0.054415842243081609,
     0.31287159091446592,
    -0.67563073629801285,
     0.58535468365486909,
     0.015829105256023893,
    -0.28401554296242809,
    -0.00047248457399797254,
     0.12874742662018601,
     0.017369301002022108,
    -0.044088253931064719,
    -0.013981027917015516,
     0.0087460940470156547,
     0.0048703529930106603,
    -0.00039174037299597711,
    -0.00067544940599855677,
    -0.00011747678400228192
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("J-UNIWARD error: " + message);
}

[[nodiscard]] std::uint64_t mix64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t seed) : state_(seed) {}

    [[nodiscard]] std::uint64_t next() {
        state_ += 0x9e3779b97f4a7c15ULL;
        std::uint64_t value = state_;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }

    [[nodiscard]] std::uint64_t bounded(std::uint64_t bound) {
        if (bound == 0) {
            fail("zero PRNG bound.");
        }
        const std::uint64_t threshold = (0U - bound) % bound;
        for (;;) {
            const std::uint64_t value = next();
            if (value >= threshold) {
                return value % bound;
            }
        }
    }

private:
    std::uint64_t state_;
};

[[nodiscard]] int reflectIndex(int index, int length) {
    if (length <= 0) {
        fail("invalid image dimension during mirror padding.");
    }
    while (index < 0 || index >= length) {
        if (index < 0) {
            index = -index - 1;
        } else {
            index = 2 * length - index - 1;
        }
    }
    return index;
}

struct ReciprocalResiduals {
    std::size_t width{};
    std::size_t height{};
    std::vector<float> lh;
    std::vector<float> hl;
    std::vector<float> hh;
};

void verticalCorrelation(std::span<const std::uint8_t> padded,
                         std::size_t width,
                         std::size_t height,
                         const std::array<double, kFilterSize>& filter,
                         std::span<float> output) {
    const auto signed_height = static_cast<std::int64_t>(height);
    const auto signed_width = static_cast<std::int64_t>(width);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (std::int64_t row = 0; row < signed_height; ++row) {
        for (std::int64_t column = 0; column < signed_width; ++column) {
            double sum = 0.0;
            for (int tap = 0; tap < kFilterSize; ++tap) {
                const std::int64_t source_row = row + tap - 8;
                if (source_row >= 0 && source_row < signed_height) {
                    sum += static_cast<double>(padded[
                        static_cast<std::size_t>(source_row) * width +
                        static_cast<std::size_t>(column)]) * filter[tap];
                }
            }
            output[static_cast<std::size_t>(row) * width +
                   static_cast<std::size_t>(column)] =
                static_cast<float>(sum);
        }
    }
}

void horizontalCorrelationReciprocal(
    std::span<const float> vertical,
    std::size_t width,
    std::size_t height,
    const std::array<double, kFilterSize>& filter,
    std::span<float> output) {
    const auto signed_height = static_cast<std::int64_t>(height);
    const auto signed_width = static_cast<std::int64_t>(width);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (std::int64_t row = 0; row < signed_height; ++row) {
        for (std::int64_t column = 0; column < signed_width; ++column) {
            double sum = 0.0;
            for (int tap = 0; tap < kFilterSize; ++tap) {
                const std::int64_t source_column = column + tap - 8;
                if (source_column >= 0 && source_column < signed_width) {
                    sum += static_cast<double>(vertical[
                        static_cast<std::size_t>(row) * width +
                        static_cast<std::size_t>(source_column)]) * filter[tap];
                }
            }
            output[static_cast<std::size_t>(row) * width +
                   static_cast<std::size_t>(column)] =
                1.0F / (static_cast<float>(std::abs(sum)) + kSigma);
        }
    }
}

[[nodiscard]] ReciprocalResiduals makeResiduals(
    std::span<const std::uint8_t> luminance,
    std::uint32_t image_width,
    std::uint32_t image_height) {
    const std::size_t width = static_cast<std::size_t>(image_width) +
        2U * kPadding;
    const std::size_t height = static_cast<std::size_t>(image_height) +
        2U * kPadding;
    if (luminance.size() !=
        static_cast<std::size_t>(image_width) * image_height) {
        fail("decoded luminance size does not match the JPEG dimensions.");
    }
    if (height > std::numeric_limits<std::size_t>::max() / width) {
        fail("residual array dimensions overflow.");
    }
    const std::size_t area = width * height;
    std::vector<std::uint8_t> padded(area);
    const auto signed_width = static_cast<std::int64_t>(width);
    const auto signed_height = static_cast<std::int64_t>(height);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (std::int64_t row = 0; row < signed_height; ++row) {
        const int original_row = reflectIndex(
            static_cast<int>(row) - kPadding,
            static_cast<int>(image_height));
        for (std::int64_t column = 0; column < signed_width; ++column) {
            const int original_column = reflectIndex(
                static_cast<int>(column) - kPadding,
                static_cast<int>(image_width));
            padded[static_cast<std::size_t>(row) * width +
                   static_cast<std::size_t>(column)] =
                luminance[static_cast<std::size_t>(original_row) * image_width +
                          static_cast<std::size_t>(original_column)];
        }
    }

    std::vector<float> vertical(area);
    ReciprocalResiduals residuals{
        .width = width,
        .height = height,
        .lh = std::vector<float>(area),
        .hl = std::vector<float>(area),
        .hh = std::vector<float>(area)
    };
    verticalCorrelation(padded, width, height, kLowPass, vertical);
    horizontalCorrelationReciprocal(vertical, width, height,
                                    kHighPass, residuals.lh);
    verticalCorrelation(padded, width, height, kHighPass, vertical);
    horizontalCorrelationReciprocal(vertical, width, height,
                                    kLowPass, residuals.hl);
    horizontalCorrelationReciprocal(vertical, width, height,
                                    kHighPass, residuals.hh);
    return residuals;
}

using ImpactTable = std::array<float, 64U * 3U * kImpactArea>;

[[nodiscard]] std::array<double, kImpactArea> fullCorrelation(
    const std::array<double, 64>& spatial,
    const std::array<double, kFilterSize>& vertical_filter,
    const std::array<double, kFilterSize>& horizontal_filter) {
    std::array<double, kImpactSize * kDctSize> vertical{};
    for (int output_row = 0; output_row < kImpactSize; ++output_row) {
        for (int column = 0; column < kDctSize; ++column) {
            double sum = 0.0;
            for (int tap = 0; tap < kFilterSize; ++tap) {
                const int source_row = output_row + tap - (kFilterSize - 1);
                if (source_row >= 0 && source_row < kDctSize) {
                    sum += spatial[static_cast<std::size_t>(source_row) *
                                   kDctSize + column] * vertical_filter[tap];
                }
            }
            vertical[static_cast<std::size_t>(output_row) * kDctSize +
                     column] = sum;
        }
    }

    std::array<double, kImpactArea> result{};
    for (int row = 0; row < kImpactSize; ++row) {
        for (int output_column = 0;
             output_column < kImpactSize;
             ++output_column) {
            double sum = 0.0;
            for (int tap = 0; tap < kFilterSize; ++tap) {
                const int source_column =
                    output_column + tap - (kFilterSize - 1);
                if (source_column >= 0 && source_column < kDctSize) {
                    sum += vertical[static_cast<std::size_t>(row) *
                                    kDctSize + source_column] *
                           horizontal_filter[tap];
                }
            }
            result[static_cast<std::size_t>(row) * kImpactSize +
                   output_column] = sum;
        }
    }
    return result;
}

[[nodiscard]] ImpactTable makeImpactTable() {
    constexpr double pi = 3.141592653589793238462643383279502884;
    ImpactTable impacts{};
    for (int frequency_row = 0;
         frequency_row < kDctSize;
         ++frequency_row) {
        for (int frequency_column = 0;
             frequency_column < kDctSize;
             ++frequency_column) {
            const int mode = frequency_row * kDctSize + frequency_column;
            const double alpha_row = frequency_row == 0
                ? std::sqrt(1.0 / 8.0) : std::sqrt(2.0 / 8.0);
            const double alpha_column = frequency_column == 0
                ? std::sqrt(1.0 / 8.0) : std::sqrt(2.0 / 8.0);
            std::array<double, 64> spatial{};
            for (int row = 0; row < kDctSize; ++row) {
                for (int column = 0; column < kDctSize; ++column) {
                    spatial[static_cast<std::size_t>(row) * kDctSize +
                            column] = alpha_row * alpha_column *
                        std::cos((pi / 8.0) * (row + 0.5) * frequency_row) *
                        std::cos((pi / 8.0) * (column + 0.5) *
                                 frequency_column);
                }
            }
            const std::array<double, kImpactArea> lh =
                fullCorrelation(spatial, kLowPass, kHighPass);
            const std::array<double, kImpactArea> hl =
                fullCorrelation(spatial, kHighPass, kLowPass);
            const std::array<double, kImpactArea> hh =
                fullCorrelation(spatial, kHighPass, kHighPass);
            for (int pixel = 0; pixel < kImpactArea; ++pixel) {
                impacts[(static_cast<std::size_t>(mode) * 3U + 0U) *
                        kImpactArea + pixel] =
                    static_cast<float>(std::abs(lh[pixel]));
                impacts[(static_cast<std::size_t>(mode) * 3U + 1U) *
                        kImpactArea + pixel] =
                    static_cast<float>(std::abs(hl[pixel]));
                impacts[(static_cast<std::size_t>(mode) * 3U + 2U) *
                        kImpactArea + pixel] =
                    static_cast<float>(std::abs(hh[pixel]));
            }
        }
    }
    return impacts;
}

[[nodiscard]] const ImpactTable& impactTable() {
    static const ImpactTable table = makeImpactTable();
    return table;
}

[[nodiscard]] bool coefficientParity(std::int16_t coefficient) {
    return (static_cast<std::uint16_t>(coefficient) & 1U) != 0U;
}

void validateCarrierGeometry(const CoefficientImage& coefficients) {
    const ImageInfo& info = coefficients.info;
    if (info.y_blocks_wide == 0 || info.y_blocks_high == 0) {
        fail("carrier has no luminance blocks.");
    }
    if (coefficients.luminance.size() != info.yBlockCount() * 64U) {
        fail("luminance coefficient array has the wrong size.");
    }
    if (info.pixelCount() > std::numeric_limits<std::uint32_t>::max()) {
        fail("candidate offsets exceed the 32-bit prototype format.");
    }
}

// Every payload-eligible luminance coefficient, in natural (block-raster)
// order: an AC position whose pixel lies inside the image proper. Padding
// columns/rows past the right and bottom edges carry no image content, and the
// DC term is never a candidate. Shared by the counting and layout builders so
// the two can never disagree about what a candidate is.
template<typename VisitFn>
void forEachCandidateOffset(const CoefficientImage& coefficients, VisitFn&& visit) {
    const ImageInfo& info = coefficients.info;
    for (std::uint32_t block_row = 0;
         block_row < info.y_blocks_high;
         ++block_row) {
        for (std::uint32_t block_column = 0;
             block_column < info.y_blocks_wide;
             ++block_column) {
            const std::uint64_t block =
                static_cast<std::uint64_t>(block_row) * info.y_blocks_wide +
                block_column;
            for (std::uint32_t row = 0; row < 8; ++row) {
                const std::uint32_t image_row = block_row * 8U + row;
                if (image_row >= info.height) {
                    continue;
                }
                for (std::uint32_t column = 0; column < 8; ++column) {
                    const std::uint32_t image_column =
                        block_column * 8U + column;
                    if (image_column >= info.width ||
                        (row == 0 && column == 0)) {
                        continue;
                    }
                    const std::uint64_t offset = block * 64U + row * 8U +
                        column;
                    if (offset > std::numeric_limits<std::uint32_t>::max()) {
                        fail("coefficient offset exceeds the prototype format.");
                    }
                    visit(static_cast<std::uint32_t>(offset));
                }
            }
        }
    }
}

} // namespace

CarrierCandidateCounts countCarrierCandidates(
    const CoefficientImage& coefficients) {
    validateCarrierGeometry(coefficients);
    CarrierCandidateCounts counts;
    forEachCandidateOffset(coefficients, [&](std::uint32_t offset) {
        ++counts.candidate_count;
        if (coefficients.luminance[offset] != 0) {
            ++counts.nonzero_ac_count;
        }
    });
    return counts;
}

CarrierLayout makeCarrierLayout(const CoefficientImage& coefficients,
                                std::uint64_t seed) {
    validateCarrierGeometry(coefficients);
    const ImageInfo& info = coefficients.info;
    CarrierLayout layout;
    layout.coefficient_offsets.reserve(
        static_cast<std::size_t>(info.pixelCount()));
    forEachCandidateOffset(coefficients, [&](std::uint32_t offset) {
        layout.coefficient_offsets.push_back(offset);
        if (coefficients.luminance[offset] != 0) {
            ++layout.nonzero_ac_count;
        }
    });

    SplitMix64 random(seed ^ info.pixelCount() ^
                      (static_cast<std::uint64_t>(info.width) << 32U) ^
                      info.height);
    for (std::size_t remaining = layout.coefficient_offsets.size();
         remaining > 1;
         --remaining) {
        const std::size_t other = static_cast<std::size_t>(
            random.bounded(remaining));
        std::swap(layout.coefficient_offsets[remaining - 1U],
                  layout.coefficient_offsets[other]);
    }
    return layout;
}

std::vector<float> computeJUniwardCosts(
    std::span<const std::uint8_t> luminance,
    const CoefficientImage& coefficients,
    std::span<const std::uint32_t> selected_offsets) {
    const ReciprocalResiduals residuals = makeResiduals(
        luminance, coefficients.info.width, coefficients.info.height);
    const ImpactTable& impacts = impactTable();
    std::vector<float> costs(selected_offsets.size());
    const auto selected_count =
        static_cast<std::int64_t>(selected_offsets.size());
    const std::uint64_t coefficient_count = coefficients.luminance.size();
    const std::size_t block_width = coefficients.info.y_blocks_wide;
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 64)
#endif
    for (std::int64_t selected = 0;
         selected < selected_count;
         ++selected) {
        const std::uint32_t offset =
            selected_offsets[static_cast<std::size_t>(selected)];
        if (offset >= coefficient_count) {
            costs[static_cast<std::size_t>(selected)] = kWetCost;
            continue;
        }
        const std::size_t block = offset / 64U;
        const std::size_t mode = offset % 64U;
        const std::size_t block_row = block / block_width;
        const std::size_t block_column = block % block_width;
        // Where this block's 23x23 impact window starts in the padded residual.
        // The block's first pixel sits at padded coordinate block*8 + kPadding,
        // and the two correlations are centred differently: makeResiduals uses
        // `index + tap - 8` while fullCorrelation uses `index + tap - 15`, so
        // impact-table entry 0 corresponds to padded position (block*8 + 16) - 7.
        // Reading one position later misaligns every cost against the region the
        // DCT mode actually perturbs.
        const std::size_t residual_row = block_row * 8U + kImpactOrigin;
        const std::size_t residual_column = block_column * 8U + kImpactOrigin;
        double rho = 0.0;
        for (int row = 0; row < kImpactSize; ++row) {
            const std::size_t residual_base =
                (residual_row + static_cast<std::size_t>(row)) *
                    residuals.width + residual_column;
            const std::size_t impact_base =
                static_cast<std::size_t>(row) * kImpactSize;
            for (int column = 0; column < kImpactSize; ++column) {
                const std::size_t residual_index = residual_base +
                    static_cast<std::size_t>(column);
                const std::size_t impact_index = impact_base +
                    static_cast<std::size_t>(column);
                rho += static_cast<double>(
                    impacts[(mode * 3U + 0U) * kImpactArea + impact_index]) *
                    residuals.lh[residual_index];
                rho += static_cast<double>(
                    impacts[(mode * 3U + 1U) * kImpactArea + impact_index]) *
                    residuals.hl[residual_index];
                rho += static_cast<double>(
                    impacts[(mode * 3U + 2U) * kImpactArea + impact_index]) *
                    residuals.hh[residual_index];
            }
        }
        rho *= coefficients.luminance_quantization[mode];
        if (!std::isfinite(rho) || rho > kWetCost) {
            rho = kWetCost;
        }
        costs[static_cast<std::size_t>(selected)] =
            static_cast<float>(rho);
    }
    return costs;
}

std::vector<std::uint8_t> coefficientParityBits(
    std::span<const std::int16_t> coefficients,
    std::span<const std::uint32_t> selected_offsets) {
    std::vector<std::uint8_t> bits(selected_offsets.size());
    for (std::size_t index = 0; index < selected_offsets.size(); ++index) {
        const std::uint32_t offset = selected_offsets[index];
        if (offset >= coefficients.size()) {
            fail("selected coefficient offset is out of range.");
        }
        bits[index] = coefficientParity(coefficients[offset]) ? 1U : 0U;
    }
    return bits;
}

std::uint64_t applyParityBits(
    std::span<std::int16_t> coefficients,
    std::span<const std::uint32_t> selected_offsets,
    std::span<const std::uint8_t> desired_bits,
    std::uint64_t seed) {
    if (selected_offsets.size() != desired_bits.size()) {
        fail("selected coefficient and desired-bit lengths differ.");
    }
    std::uint64_t changes = 0;
    for (std::size_t index = 0; index < selected_offsets.size(); ++index) {
        const std::uint32_t offset = selected_offsets[index];
        if (offset >= coefficients.size()) {
            fail("selected coefficient offset is out of range.");
        }
        std::int16_t& coefficient = coefficients[offset];
        const bool desired = desired_bits[index] != 0;
        if (coefficientParity(coefficient) == desired) {
            continue;
        }
        const bool prefer_positive =
            (mix64(seed ^ kDirectionDomain ^ offset ^
                   (static_cast<std::uint64_t>(index) << 32U)) & 1U) != 0U;
        if (coefficient <= -1023) {
            ++coefficient;
        } else if (coefficient >= 1023) {
            --coefficient;
        } else if (prefer_positive) {
            ++coefficient;
        } else {
            --coefficient;
        }
        if (coefficientParity(coefficient) != desired) {
            fail("coefficient parity update failed.");
        }
        ++changes;
    }
    return changes;
}

} // namespace twitter_steg_internal
