#include "twitter_juniward.h"
#include "carrier_common.h"
#include "signal_utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
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
// Work between cancellation checks: a few milliseconds each, single-threaded,
// at the 4096x4096 ceiling (64 padded rows per filter pass; 16 Ki J-UNIWARD
// costs, each a 23x23x3 window sum).
constexpr std::int64_t kRowBatch = 64;
constexpr std::int64_t kCostBatch = 16'384;
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

using carrier_common::mix64;

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

// Residual rows a strip holds at most: a block row's impact window spans
// kImpactSize rows starting kImpactOrigin below the block's padded position,
// so 32 block rows need 31 * 8 + 23 = 271 rows -- about 18 MiB for the four
// float planes at the 4096-pixel width limit, instead of ~276 MiB for whole-
// image planes. Neighbouring strips recompute their 15 shared rows (~6%).
constexpr std::size_t kStripBlockRows = 32;

// The J-UNIWARD residuals for rows [first_row, first_row + rows) of the padded
// image (which is the luminance reflected out by kPadding on every side).
// Buffers keep their capacity from one strip to the next.
struct ResidualStrip {
    std::size_t width{};      // padded width
    std::size_t first_row{};  // padded row held in row 0
    std::size_t rows{};
    std::vector<std::uint8_t> padded;
    std::vector<float> vertical;
    std::vector<float> lh;
    std::vector<float> hl;
    std::vector<float> hh;
};

// Rows [out_first, out_first + out_rows) of the vertical pass over the whole
// padded image. `padded` holds padded rows from padded_first on; taps reaching
// outside the full padded height contribute nothing, exactly as they did when
// the whole image was filtered at once.
void verticalCorrelation(std::span<const std::uint8_t> padded,
                         std::size_t padded_first,
                         std::size_t width,
                         std::size_t full_height,
                         const std::array<double, kFilterSize>& filter,
                         std::size_t out_first,
                         std::size_t out_rows,
                         std::span<float> output) {
    const auto signed_rows = static_cast<std::int64_t>(out_rows);
    const auto signed_width = static_cast<std::int64_t>(width);
    const auto signed_height = static_cast<std::int64_t>(full_height);
    const auto signed_out_first = static_cast<std::int64_t>(out_first);
    const auto signed_padded_first = static_cast<std::int64_t>(padded_first);
    for (std::int64_t row_begin = 0; row_begin < signed_rows; row_begin += kRowBatch) {
        // Checked between batches: never thrown through an OpenMP worker.
        throwIfSignalCancellationRequested();
        const std::int64_t row_end = std::min(signed_rows, row_begin + kRowBatch);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
        for (std::int64_t row = row_begin; row < row_end; ++row) {
            const std::int64_t global_row = signed_out_first + row;
            for (std::int64_t column = 0; column < signed_width; ++column) {
                double sum = 0.0;
                for (int tap = 0; tap < kFilterSize; ++tap) {
                    const std::int64_t source_row = global_row + tap - 8;
                    if (source_row >= 0 && source_row < signed_height) {
                        sum += static_cast<double>(padded[
                            static_cast<std::size_t>(source_row - signed_padded_first) * width +
                            static_cast<std::size_t>(column)]) * filter[tap];
                    }
                }
                output[static_cast<std::size_t>(row) * width +
                       static_cast<std::size_t>(column)] =
                    static_cast<float>(sum);
            }
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
    for (std::int64_t row_begin = 0; row_begin < signed_height; row_begin += kRowBatch) {
        // Checked between batches: never thrown through an OpenMP worker.
        throwIfSignalCancellationRequested();
        const std::int64_t row_end = std::min(signed_height, row_begin + kRowBatch);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
        for (std::int64_t row = row_begin; row < row_end; ++row) {
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
}

// Fills `strip` with residual rows [first_row, first_row + rows). The caller
// keeps the range inside the padded height.
void fillResidualStrip(std::span<const std::uint8_t> luminance,
                       std::uint32_t image_width,
                       std::uint32_t image_height,
                       std::size_t first_row,
                       std::size_t rows,
                       ResidualStrip& strip) {
    const std::size_t width = static_cast<std::size_t>(image_width) +
        2U * kPadding;
    const std::size_t height = static_cast<std::size_t>(image_height) +
        2U * kPadding;
    // The vertical taps reach 8 rows up and 7 down.
    const std::size_t padded_first = first_row >= 8U ? first_row - 8U : 0U;
    const std::size_t padded_end = std::min(height, first_row + rows + 7U);
    const std::size_t padded_rows = padded_end - padded_first;

    strip.width = width;
    strip.first_row = first_row;
    strip.rows = rows;
    strip.padded.resize(padded_rows * width);
    const auto signed_padded_rows = static_cast<std::int64_t>(padded_rows);
    const auto signed_width = static_cast<std::int64_t>(width);
    for (std::int64_t row_begin = 0; row_begin < signed_padded_rows; row_begin += kRowBatch) {
        // Checked between batches: never thrown through an OpenMP worker.
        throwIfSignalCancellationRequested();
        const std::int64_t row_end = std::min(signed_padded_rows, row_begin + kRowBatch);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
        for (std::int64_t row = row_begin; row < row_end; ++row) {
            const int original_row = reflectIndex(
                static_cast<int>(padded_first) + static_cast<int>(row) - kPadding,
                static_cast<int>(image_height));
            for (std::int64_t column = 0; column < signed_width; ++column) {
                const int original_column = reflectIndex(
                    static_cast<int>(column) - kPadding,
                    static_cast<int>(image_width));
                strip.padded[static_cast<std::size_t>(row) * width +
                             static_cast<std::size_t>(column)] =
                    luminance[static_cast<std::size_t>(original_row) * image_width +
                              static_cast<std::size_t>(original_column)];
            }
        }
    }

    const std::size_t area = rows * width;
    strip.vertical.resize(area);
    strip.lh.resize(area);
    strip.hl.resize(area);
    strip.hh.resize(area);
    verticalCorrelation(strip.padded, padded_first, width, height, kLowPass,
                        first_row, rows, strip.vertical);
    horizontalCorrelationReciprocal(strip.vertical, width, rows,
                                    kHighPass, strip.lh);
    verticalCorrelation(strip.padded, padded_first, width, height, kHighPass,
                        first_row, rows, strip.vertical);
    horizontalCorrelationReciprocal(strip.vertical, width, rows,
                                    kLowPass, strip.hl);
    horizontalCorrelationReciprocal(strip.vertical, width, rows,
                                    kHighPass, strip.hh);
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

// forEachCandidateOffset visits block rows, then block columns, then the
// in-image positions of each block in row-major order, skipping DC. A block
// that is r pixel rows by c pixel columns inside the image therefore holds
// r * c - 1 candidates, and its k-th is position k + 1 of an r-by-c grid. Every
// block row but the last is 8 rows tall and every block column but the last
// 8 wide; blocks past the right or bottom edge hold none.
CandidateOffsets::CandidateOffsets(const CoefficientImage& coefficients)
    : blocks_wide_(coefficients.info.y_blocks_wide),
      full_columns_(coefficients.info.width / 8U),
      partial_width_(coefficients.info.width % 8U),
      full_rows_(coefficients.info.height / 8U),
      partial_height_(coefficients.info.height % 8U) {
    validateCarrierGeometry(coefficients);
    size_ = full_rows_ * rowCount(8U) +
        (partial_height_ != 0 ? rowCount(partial_height_) : 0U);
}

std::uint64_t CandidateOffsets::rowCount(std::uint64_t rows) const noexcept {
    return full_columns_ * (rows * 8U - 1U) +
        (partial_width_ != 0 ? rows * partial_width_ - 1U : 0U);
}

std::uint32_t CandidateOffsets::operator[](std::uint64_t index) const noexcept {
    std::uint64_t block_row = full_rows_;
    std::uint64_t rows = partial_height_;
    std::uint64_t within_row = index;
    const std::uint64_t full_row_count = rowCount(8U);
    if (full_row_count != 0 && index < full_rows_ * full_row_count) {
        block_row = index / full_row_count;
        within_row = index % full_row_count;
        rows = 8U;
    } else {
        within_row -= full_rows_ * full_row_count;
    }

    const std::uint64_t full_block_count = rows * 8U - 1U;
    std::uint64_t block_column = full_columns_;
    std::uint64_t columns = partial_width_;
    std::uint64_t within_block = within_row;
    if (within_row < full_columns_ * full_block_count) {
        block_column = within_row / full_block_count;
        within_block = within_row % full_block_count;
        columns = 8U;
    } else {
        within_block -= full_columns_ * full_block_count;
    }

    const std::uint64_t position = within_block + 1U; // DC is position 0
    const std::uint64_t offset =
        (block_row * blocks_wide_ + block_column) * 64U +
        (position / columns) * 8U + position % columns;
    return static_cast<std::uint32_t>(offset);
}

std::vector<float> computeJUniwardCosts(
    std::span<const std::uint8_t> luminance,
    const CoefficientImage& coefficients,
    std::span<const std::uint32_t> selected_offsets) {
    const ImageInfo& info = coefficients.info;
    if (luminance.size() !=
        static_cast<std::size_t>(info.width) * info.height) {
        fail("decoded luminance size does not match the JPEG dimensions.");
    }
    const ImpactTable& impacts = impactTable();
    std::vector<float> costs(selected_offsets.size());
    const std::uint64_t coefficient_count = coefficients.luminance.size();
    const std::size_t block_width = info.y_blocks_wide;
    const std::size_t residual_height =
        static_cast<std::size_t>(info.height) + 2U * kPadding;

    // The carrier order is a keyed shuffle, so visiting it directly scatters
    // the 23x23 residual reads across the whole image. Visit the positions in
    // offset order instead (block by block, top to bottom) and write each
    // cost back to its carrier slot. Each cost depends only on its own offset,
    // so the result is bit-identical. Key = offset << 32 | carrier slot.
    if (selected_offsets.size() > std::numeric_limits<std::uint32_t>::max()) {
        fail("too many selected coefficients.");
    }
    std::vector<std::uint64_t> visit_order(selected_offsets.size());
    for (std::size_t slot = 0; slot < selected_offsets.size(); ++slot) {
        const std::uint32_t offset = selected_offsets[slot];
        // A block's impact window stays inside the padded image only if the
        // block starts inside the image, as every carrier candidate does.
        if (offset < coefficient_count) {
            const std::size_t block = offset / 64U;
            if ((block / block_width) * 8U >= info.height ||
                (block % block_width) * 8U >= info.width) {
                fail("selected coefficient lies outside the image.");
            }
        }
        visit_order[slot] =
            (static_cast<std::uint64_t>(offset) << 32U) | slot;
    }
    std::ranges::sort(visit_order);
    const auto key_offset = [](std::uint64_t key) {
        return static_cast<std::uint32_t>(key >> 32U);
    };
    const auto key_slot = [](std::uint64_t key) {
        return static_cast<std::size_t>(key & 0xffffffffULL);
    };
    const auto key_block_row = [&](std::uint64_t key) {
        return (key_offset(key) / 64U) / block_width;
    };
    // The smallest key whose offset is at least `offset` (every key, past
    // the uint32 range).
    const auto key_bound = [](std::uint64_t offset) {
        return offset > std::numeric_limits<std::uint32_t>::max()
            ? std::numeric_limits<std::uint64_t>::max()
            : offset << 32U;
    };

    // Out-of-range offsets sort last; they are wet.
    const auto in_range_end = std::ranges::lower_bound(
        visit_order, key_bound(coefficient_count));
    for (auto it = in_range_end; it != visit_order.end(); ++it) {
        costs[key_slot(*it)] = kWetCost;
    }
    const auto in_range = static_cast<std::size_t>(
        in_range_end - visit_order.begin());

    // Residuals are built one strip of block rows at a time, starting at the
    // first block row that still has a coefficient to cost, and reused.
    ResidualStrip strip;
    std::size_t strip_begin = 0;
    while (strip_begin < in_range) {
        const std::size_t first_block_row = key_block_row(visit_order[strip_begin]);
        const std::uint64_t strip_end_offset =
            static_cast<std::uint64_t>(first_block_row + kStripBlockRows) *
            block_width * 64U;
        const std::size_t strip_end = static_cast<std::size_t>(
            std::lower_bound(visit_order.begin() + static_cast<std::ptrdiff_t>(strip_begin),
                             in_range_end, key_bound(strip_end_offset)) -
            visit_order.begin());
        const std::size_t last_block_row = key_block_row(visit_order[strip_end - 1U]);
        const std::size_t first_row = first_block_row * 8U + kImpactOrigin;
        const std::size_t rows =
            (last_block_row - first_block_row) * 8U + kImpactSize;
        if (first_row + rows > residual_height) {
            fail("impact window exceeds the residual image.");
        }
        fillResidualStrip(luminance, info.width, info.height, first_row, rows, strip);

        const auto visit_stop = static_cast<std::int64_t>(strip_end);
        for (auto visit_begin = static_cast<std::int64_t>(strip_begin);
             visit_begin < visit_stop;
             visit_begin += kCostBatch) {
            // Checked between batches: never thrown through an OpenMP worker.
            throwIfSignalCancellationRequested();
            const std::int64_t visit_end = std::min(visit_stop, visit_begin + kCostBatch);
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 64)
#endif
            for (std::int64_t visit = visit_begin; visit < visit_end; ++visit) {
                const std::uint64_t key = visit_order[static_cast<std::size_t>(visit)];
                const std::size_t block = key_offset(key) / 64U;
                const std::size_t mode = key_offset(key) % 64U;
                const std::size_t block_row = block / block_width;
                const std::size_t block_column = block % block_width;
                // Where this block's 23x23 impact window starts in the padded residual.
                // The block's first pixel sits at padded coordinate block*8 + kPadding,
                // and the two correlations are centred differently: the residual pass uses
                // `index + tap - 8` while fullCorrelation uses `index + tap - 15`, so
                // impact-table entry 0 corresponds to padded position (block*8 + 16) - 7.
                // Reading one position later misaligns every cost against the region the
                // DCT mode actually perturbs. Rows are relative to the strip.
                const std::size_t residual_row =
                    block_row * 8U + kImpactOrigin - strip.first_row;
                const std::size_t residual_column = block_column * 8U + kImpactOrigin;
                double rho = 0.0;
                for (int row = 0; row < kImpactSize; ++row) {
                    const std::size_t residual_base =
                        (residual_row + static_cast<std::size_t>(row)) *
                            strip.width + residual_column;
                    const std::size_t impact_base =
                        static_cast<std::size_t>(row) * kImpactSize;
                    for (int column = 0; column < kImpactSize; ++column) {
                        const std::size_t residual_index = residual_base +
                            static_cast<std::size_t>(column);
                        const std::size_t impact_index = impact_base +
                            static_cast<std::size_t>(column);
                        rho += static_cast<double>(
                            impacts[(mode * 3U + 0U) * kImpactArea + impact_index]) *
                            strip.lh[residual_index];
                        rho += static_cast<double>(
                            impacts[(mode * 3U + 1U) * kImpactArea + impact_index]) *
                            strip.hl[residual_index];
                        rho += static_cast<double>(
                            impacts[(mode * 3U + 2U) * kImpactArea + impact_index]) *
                            strip.hh[residual_index];
                    }
                }
                rho *= coefficients.luminance_quantization[mode];
                if (!std::isfinite(rho) || rho > kWetCost) {
                    rho = kWetCost;
                }
                costs[key_slot(key)] = static_cast<float>(rho);
            }
        }
        strip_begin = strip_end;
    }
    return costs;
}

std::vector<std::uint8_t> coefficientParityBits(
    std::span<const std::int16_t> coefficients,
    std::span<const std::uint32_t> selected_offsets) {
    std::vector<std::uint8_t> bits(selected_offsets.size());
    for (auto [bit, offset] : std::views::zip(bits, selected_offsets)) {
        if (offset >= coefficients.size()) {
            fail("selected coefficient offset is out of range.");
        }
        bit = coefficientParity(coefficients[offset]) ? 1U : 0U;
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
