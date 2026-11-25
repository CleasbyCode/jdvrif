#include "twitter_juniward.h"
#include "carrier_common.h"
#include "signal_utils.h"
#include "carrier_parallel.h"

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
// Each residual strip uses dynamic chunks so a slowed worker takes fewer
// chunks. carrier_parallel joins every worker before propagating errors or
// cancellation, and falls back to the caller when threads cannot be created.
// Partial sums per sub-band in the cost kernel (see computeJUniwardCosts).
// Part of the cost definition: changing it changes the rounding of the costs.
constexpr std::size_t kCostLanes = 4;
constexpr float kSigma = 0.015625F;
constexpr float kWetCost = 1.0e13F;
constexpr std::uint64_t kDirectionDomain = 0x5844535443444952ULL;

// The kCostLanes partial sums of one sub-band (a GCC/Clang vector type:
// plain SSE2/NEON code without -march, AVX2 with it).
using CostLanes = double __attribute__((vector_size(kCostLanes * sizeof(double))));
static_assert(kCostLanes == 4, "the cost kernel folds exactly four lanes");

[[nodiscard]] inline double widen(float value) { return static_cast<double>(value); }

// sum += impact[0..3] * window[0..3], each product formed in double. The
// vectors are built element by element: GCC 14 splits __builtin_convertvector
// of four floats into two halves, and this form becomes one vcvtps2pd. The sum
// is passed by reference: returning a 256-bit vector draws GCC's -Wpsabi note
// on builds without AVX.
inline void accumulateCostLanes(CostLanes& sum, const float* impact, const float* window) {
    const CostLanes impacts{widen(impact[0]), widen(impact[1]), widen(impact[2]), widen(impact[3])};
    const CostLanes windows{widen(window[0]), widen(window[1]), widen(window[2]), widen(window[3])};
    sum += impacts * windows;
}

// The last three columns of an impact row go to lanes 0..2. Constant lane
// indices keep the sums in registers; a loop over them spills the vectors.
static_assert(kImpactSize % kCostLanes == 3, "addCostTail covers three columns");
inline void addCostTail(CostLanes& sum, const float* impact, const float* window) {
    sum[0] += widen(impact[0]) * widen(window[0]);
    sum[1] += widen(impact[1]) * widen(window[1]);
    sum[2] += widen(impact[2]) * widen(window[2]);
}

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
// so 32 block rows need 31 * 8 + 23 = 271 rows -- about 14 MiB for the three
// float planes and the byte plane at the 4096-pixel width limit, instead of
// ~211 MiB for whole-image planes. The filter scratch is one row per thread. Neighbouring strips recompute their 15 shared rows (~6%).
constexpr std::size_t kStripBlockRows = 32;

// The J-UNIWARD residuals for a run of rows from first_row of the padded
// image (which is the luminance reflected out by kPadding on every side).
// Buffers keep their capacity from one strip to the next.
struct ResidualStrip {
    std::size_t width{};      // padded width
    std::size_t first_row{};  // padded row held in row 0
    std::vector<std::uint8_t> padded;
    std::vector<float> lh;
    std::vector<float> hl;
    std::vector<float> hh;
};

// sum[i] += source[i] * weight for i < count. The rows never overlap; saying
// so (__restrict on parameters, which GCC honours where it ignores it on
// locals) lets the loop vectorise without a run-time alias check.
template<typename Source>
void addWeightedRow(double* __restrict sum, const Source* __restrict source,
                    std::size_t count, double weight) {
    for (std::size_t index = 0; index < count; ++index) {
        sum[index] += static_cast<double>(source[index]) * weight;
    }
}

// One row of the vertical pass over the whole padded image: padded row
// `global_row`, written to `output`. `padded` holds padded rows from
// padded_first on; taps reaching outside the full padded height contribute
// nothing, exactly as they did when the whole image was filtered at once.
//
// Both passes run tap by tap across the whole row, not column by column across
// the taps: each column still adds its taps in ascending order, so every sum
// is the same number as before, but the inner loop is now a plain
// multiply-add over a row with no bounds test in it, which vectorises.
// `sums` is the caller's scratch row of at least `width` doubles.
void verticalCorrelationRow(std::span<const std::uint8_t> padded,
                            std::int64_t padded_first,
                            std::size_t width,
                            std::int64_t full_height,
                            const std::array<double, kFilterSize>& filter,
                            std::int64_t global_row,
                            std::span<double> sums,
                            std::span<float> output) {
    double* const sum = sums.data();
    std::fill_n(sum, width, 0.0);
    for (int tap = 0; tap < kFilterSize; ++tap) {
        const std::int64_t source_row = global_row + tap - 8;
        if (source_row < 0 || source_row >= full_height) continue;
        addWeightedRow(sum,
                       padded.data() + static_cast<std::size_t>(source_row - padded_first) * width,
                       width, filter[static_cast<std::size_t>(tap)]);
    }
    float* const out = output.data();
    for (std::size_t column = 0; column < width; ++column) {
        out[column] = static_cast<float>(sum[column]);
    }
}

// One row of the horizontal pass, stored as the reciprocal residual.
void horizontalCorrelationReciprocalRow(
    std::span<const float> vertical,
    const std::array<double, kFilterSize>& filter,
    std::span<double> sums,
    std::span<float> output) {
    const auto width = static_cast<std::int64_t>(vertical.size());
    double* const sum = sums.data();
    std::fill_n(sum, vertical.size(), 0.0);
    const float* const source = vertical.data();
    for (int tap = 0; tap < kFilterSize; ++tap) {
        // Column c reads source column c + tap - 8; keep that inside the row.
        const std::int64_t shift = tap - 8;
        const std::int64_t first = std::max<std::int64_t>(0, -shift);
        const std::int64_t last = std::min<std::int64_t>(width, width - shift);
        if (first >= last) continue;
        addWeightedRow(sum + first, source + first + shift,
                       static_cast<std::size_t>(last - first),
                       filter[static_cast<std::size_t>(tap)]);
    }
    float* const out = output.data();
    for (std::int64_t column = 0; column < width; ++column) {
        out[column] = 1.0F / (static_cast<float>(std::abs(sum[column])) + kSigma);
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
    strip.padded.resize(padded_rows * width);
    // Mirror padding is a plain copy of at most 286 rows, so it runs on this
    // thread: the interior of a row is the image row itself, and only the
    // kPadding columns on each side are reflected.
    throwIfSignalCancellationRequested();
    for (std::size_t row = 0; row < padded_rows; ++row) {
        const int original_row = reflectIndex(
            static_cast<int>(padded_first) + static_cast<int>(row) - kPadding,
            static_cast<int>(image_height));
        const std::uint8_t* const source =
            luminance.data() + static_cast<std::size_t>(original_row) * image_width;
        std::uint8_t* const target = strip.padded.data() + row * width;
        for (int column = 0; column < kPadding; ++column) {
            target[column] = source[reflectIndex(
                column - kPadding, static_cast<int>(image_width))];
            target[static_cast<std::size_t>(kPadding) + image_width +
                   static_cast<std::size_t>(column)] = source[reflectIndex(
                static_cast<int>(image_width) + column, static_cast<int>(image_width))];
        }
        std::copy_n(source, image_width, target + kPadding);
    }

    const std::size_t area = rows * width;
    strip.lh.resize(area);
    strip.hl.resize(area);
    strip.hh.resize(area);
    // A residual row depends only on the padded rows around it, so all three
    // residuals of a row are finished in one loop iteration: its vertical
    // low-pass and high-pass each feed the horizontal passes of that same row
    // and nothing else. `vertical` and `sums` are one row of scratch per
    // thread, reused for every row it takes, so they stay in cache.
    const std::span<const std::uint8_t> padded(strip.padded);
    const auto signed_rows = static_cast<std::int64_t>(rows);
    const auto signed_padded_first = static_cast<std::int64_t>(padded_first);
    const auto signed_first_row = static_cast<std::int64_t>(first_row);
    const auto signed_height = static_cast<std::int64_t>(height);
    carrier_parallel::forEach(0, signed_rows, 4, [&](std::int64_t row) {
        thread_local std::vector<float> vertical_scratch;
        thread_local std::vector<double> sums_scratch;
        // Sized separately: a bad_alloc between the two resizes must not
        // leave one short for a later call on this thread.
        if (vertical_scratch.size() < width) vertical_scratch.resize(width);
        if (sums_scratch.size() < width) sums_scratch.resize(width);
        const std::size_t row_begin = static_cast<std::size_t>(row) * width;
        const std::span<float> vertical(vertical_scratch.data(), width);
        const std::span<double> sums(sums_scratch.data(), width);
        const std::int64_t global_row = signed_first_row + row;
        verticalCorrelationRow(padded, signed_padded_first, width, signed_height,
                               kLowPass, global_row, sums, vertical);
        horizontalCorrelationReciprocalRow(
            vertical, kHighPass, sums, std::span<float>(strip.lh.data() + row_begin, width));
        verticalCorrelationRow(padded, signed_padded_first, width, signed_height,
                               kHighPass, global_row, sums, vertical);
        horizontalCorrelationReciprocalRow(
            vertical, kLowPass, sums, std::span<float>(strip.hl.data() + row_begin, width));
        horizontalCorrelationReciprocalRow(
            vertical, kHighPass, sums, std::span<float>(strip.hh.data() + row_begin, width));
    });
    throwIfSignalCancellationRequested();
}

using ImpactTable = std::array<float, 64U * 3U * kImpactArea>;

// The impact-table loops below count with the int constants above; every
// array index they form is non-negative, so this conversion is exact.
[[nodiscard]] constexpr std::size_t arrayIndex(int value) noexcept {
    return static_cast<std::size_t>(value);
}

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
                    sum += spatial[arrayIndex(source_row * kDctSize + column)] *
                           vertical_filter[arrayIndex(tap)];
                }
            }
            vertical[arrayIndex(output_row * kDctSize + column)] = sum;
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
                    sum += vertical[arrayIndex(row * kDctSize + source_column)] *
                           horizontal_filter[arrayIndex(tap)];
                }
            }
            result[arrayIndex(row * kImpactSize + output_column)] = sum;
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
                    spatial[arrayIndex(row * kDctSize + column)] = alpha_row * alpha_column *
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
                impacts[arrayIndex((mode * 3 + 0) * kImpactArea + pixel)] =
                    static_cast<float>(std::abs(lh[arrayIndex(pixel)]));
                impacts[arrayIndex((mode * 3 + 1) * kImpactArea + pixel)] =
                    static_cast<float>(std::abs(hl[arrayIndex(pixel)]));
                impacts[arrayIndex((mode * 3 + 2) * kImpactArea + pixel)] =
                    static_cast<float>(std::abs(hh[arrayIndex(pixel)]));
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
    // The CLI's decoders refuse empty images first; checked here too for
    // callers that come straight to this API.
    if (info.width == 0 || info.height == 0) {
        fail("carrier has no pixels.");
    }
    // Offsets map to pixels as one 8x8 block per 8x8 pixels, so the grid must
    // cover the whole image. Blocks past the right and bottom edges are MCU
    // padding and hold no candidates; libjpeg's largest sampling factor (4)
    // bounds that padding to 3 blocks.
    const auto coversWithPadding = [](std::uint64_t blocks, std::uint32_t pixels) {
        const std::uint64_t needed = (static_cast<std::uint64_t>(pixels) + 7U) / 8U;
        return blocks >= needed && blocks <= needed + 3U;
    };
    if (!coversWithPadding(info.y_blocks_wide, info.width) ||
        !coversWithPadding(info.y_blocks_high, info.height)) {
        fail("luminance block grid does not match the image dimensions.");
    }
    // Offsets run to yBlockCount() * 64 - 1 and are stored as uint32.
    if (info.yBlockCount() > (std::uint64_t{std::numeric_limits<std::uint32_t>::max()} + 1U) / 64U) {
        fail("candidate offsets exceed the 32-bit prototype format.");
    }
    if (coefficients.luminance.size() != info.yBlockCount() * 64U) {
        fail("luminance coefficient array has the wrong size.");
    }
}

} // namespace

// Counts every payload-eligible luminance coefficient, walking them in natural
// (block-raster) order. The coefficient at DCT frequency (v, u) -- row v,
// column u of its block's 8x8 coefficient array -- is a candidate when
// block_row * 8 + v and block_column * 8 + u fall inside the image height and
// width. The test treats frequency indices as if they were pixel positions:
// it keeps all 63 AC terms of an interior block, only the low-frequency corner
// of a partial edge block, and nothing from blocks wholly past the edges. That
// is a quirk, not a pixel-coverage rule, but the carrier format (JXSTEG4) pins
// it, so it must not change. The DC term is never a candidate. CandidateOffsets
// enumerates the same set in the same order.
CarrierCandidateCounts countCarrierCandidates(
    const CoefficientImage& coefficients) {
    validateCarrierGeometry(coefficients);
    const ImageInfo& info = coefficients.info;
    CarrierCandidateCounts counts;
    for (std::uint32_t block_row = 0;
         block_row < info.y_blocks_high;
         ++block_row) {
        for (std::uint32_t block_column = 0;
             block_column < info.y_blocks_wide;
             ++block_column) {
            const std::size_t block =
                static_cast<std::size_t>(block_row) * info.y_blocks_wide +
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
                    ++counts.candidate_count;
                    if (coefficients.luminance[block * 64U + row * 8U + column] != 0) {
                        ++counts.nonzero_ac_count;
                    }
                }
            }
        }
    }
    return counts;
}

// countCarrierCandidates visits block rows, then block columns, then the
// coefficient positions of each block in row-major order, skipping DC, and
// keeps frequency (v, u) only where v < r and u < c for a block that overlaps
// the image by r pixel rows and c pixel columns (see the frequency quirk
// noted there). Such a block therefore holds r * c - 1 candidates, and its
// k-th is position k + 1 of an r-by-c grid of frequencies. Every block row but
// the last has r = 8 and every block column but the last c = 8; blocks past
// the right or bottom edge hold none.
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
        within_row = index - block_row * full_row_count;
        rows = 8U;
        // Nearly every candidate is in a whole 8x8 block: 63 AC positions,
        // numbered row-major from 1, so the block's offset plus the position
        // is the coefficient offset. Dividing by the constant 63 is a
        // multiply, where the general path below divides three more times by
        // run-time values. Same result as that path.
        if (within_row < full_columns_ * 63U) {
            const std::uint64_t column = within_row / 63U;
            const std::uint64_t position = within_row - column * 63U + 1U;
            return static_cast<std::uint32_t>(
                (block_row * blocks_wide_ + column) * 64U + position);
        }
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
    validateCarrierGeometry(coefficients);
    const ImageInfo& info = coefficients.info;
    if (luminance.size() !=
        static_cast<std::size_t>(info.width) * info.height) {
        fail("decoded luminance size does not match the JPEG dimensions.");
    }
    const ImpactTable& impacts = impactTable();
    std::vector<float> costs(selected_offsets.size());
    const carrier_common::WipeOnThrow costs_wipe{costs};
    const std::uint64_t coefficient_count = coefficients.luminance.size();
    const std::size_t block_width = info.y_blocks_wide;
    const std::size_t residual_height =
        static_cast<std::size_t>(info.height) + 2U * kPadding;

    // The carrier order is a keyed shuffle, so visiting it directly scatters
    // the 23x23 residual reads across the whole image. Visit the positions
    // block by block, top to bottom, instead, and write each cost back to its
    // carrier slot. Each cost depends only on its own offset, so the result is
    // bit-identical whatever the order. Key = offset << 32 | carrier slot.
    //
    // A counting scatter by block (O(n)) gives that order; a full sort by
    // offset took ~460 ms for 6M keys. Within a block the keys stay in slot
    // order, which costs nothing: the block's window is the same for all its
    // modes. Out-of-range offsets go to one final bucket; they are wet.
    if (selected_offsets.size() > std::numeric_limits<std::uint32_t>::max()) {
        fail("too many selected coefficients.");
    }
    const std::size_t block_count = static_cast<std::size_t>(coefficient_count / 64U);
    // Bucket sizes, stored shifted by one so the prefix sum turns them
    // directly into per-bucket write cursors. 4 bytes per block: an eighth of
    // the luminance coefficients already held.
    // Which blocks the keyed selection touches, and how often: wiped on release.
    carrier_common::WipedVector<std::uint32_t> bucket_cursor(block_count + 2U, 0);
    const auto bucket_of = [&](std::uint32_t offset) {
        return offset < coefficient_count ? static_cast<std::size_t>(offset / 64U) : block_count;
    };
    for (const std::uint32_t offset : selected_offsets) {
        // A block's impact window stays inside the padded image only if the
        // block starts inside the image, as every carrier candidate does.
        if (offset < coefficient_count) {
            const std::size_t block = offset / 64U;
            if ((block / block_width) * 8U >= info.height ||
                (block % block_width) * 8U >= info.width) {
                fail("selected coefficient lies outside the image.");
            }
        }
        ++bucket_cursor[bucket_of(offset) + 1U];
    }
    for (std::size_t bucket = 1; bucket < bucket_cursor.size(); ++bucket) {
        bucket_cursor[bucket] += bucket_cursor[bucket - 1U];
    }
    carrier_common::WipedVector<std::uint64_t> visit_order(selected_offsets.size());
    for (std::size_t slot = 0; slot < selected_offsets.size(); ++slot) {
        const std::uint32_t offset = selected_offsets[slot];
        visit_order[bucket_cursor[bucket_of(offset)]++] =
            (static_cast<std::uint64_t>(offset) << 32U) | slot;
    }
    carrier_common::WipedVector<std::uint32_t>{}.swap(bucket_cursor);
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
    // the uint32 range). visit_order is ordered by block, not by offset, so
    // the binary searches below only ever look for block boundaries
    // (multiples of 64): every key before one is smaller, every key after it
    // is not.
    const auto key_bound = [](std::uint64_t offset) {
        return offset > std::numeric_limits<std::uint32_t>::max()
            ? std::numeric_limits<std::uint64_t>::max()
            : offset << 32U;
    };

    // Out-of-range offsets come last; they are wet.
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

        const auto visit_begin = static_cast<std::int64_t>(strip_begin);
        const auto visit_end = static_cast<std::int64_t>(strip_end);
        carrier_parallel::forEach(visit_begin, visit_end, 256, [&](std::int64_t visit) {
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
            // The 23x23x3 window sum, in kCostLanes independent partial
            // sums per sub-band instead of one running total. A single
            // accumulator makes all 1,587 additions wait on each other.
            // The order is spelled out here -- column c of every row goes
            // to lane c % kCostLanes, lanes are folded pairwise, sub-bands
            // added LH, HL, HH -- so it is the same on every compiler and
            // CPU. (Each product of two floats is exact in a double, so
            // fused multiply-add cannot change it either.) The lanes are
            // held in vector types: left as scalar arrays, GCC unrolls the
            // lane loop and then fails to pack it, leaving the hot loop
            // scalar.
            const float* const window_lh = strip.lh.data() + residual_row * strip.width + residual_column;
            const float* const window_hl = strip.hl.data() + residual_row * strip.width + residual_column;
            const float* const window_hh = strip.hh.data() + residual_row * strip.width + residual_column;
            const float* const impact_lh = impacts.data() + (mode * 3U + 0U) * kImpactArea;
            const float* const impact_hl = impacts.data() + (mode * 3U + 1U) * kImpactArea;
            const float* const impact_hh = impacts.data() + (mode * 3U + 2U) * kImpactArea;
            CostLanes sum_lh{};
            CostLanes sum_hl{};
            CostLanes sum_hh{};
            for (std::size_t row = 0; row < kImpactSize; ++row) {
                const std::size_t residual_base = row * strip.width;
                const std::size_t impact_base = row * kImpactSize;
                std::size_t column = 0;
                for (; column + kCostLanes <= kImpactSize; column += kCostLanes) {
                    accumulateCostLanes(sum_lh, impact_lh + impact_base + column,
                                        window_lh + residual_base + column);
                    accumulateCostLanes(sum_hl, impact_hl + impact_base + column,
                                        window_hl + residual_base + column);
                    accumulateCostLanes(sum_hh, impact_hh + impact_base + column,
                                        window_hh + residual_base + column);
                }
                addCostTail(sum_lh, impact_lh + impact_base + column, window_lh + residual_base + column);
                addCostTail(sum_hl, impact_hl + impact_base + column, window_hl + residual_base + column);
                addCostTail(sum_hh, impact_hh + impact_base + column, window_hh + residual_base + column);
            }
            double rho =
                ((sum_lh[0] + sum_lh[1]) + (sum_lh[2] + sum_lh[3])) +
                ((sum_hl[0] + sum_hl[1]) + (sum_hl[2] + sum_hl[3])) +
                ((sum_hh[0] + sum_hh[1]) + (sum_hh[2] + sum_hh[3]));
            rho *= coefficients.luminance_quantization[mode];
            if (!std::isfinite(rho) || rho > kWetCost) {
                rho = kWetCost;
            }
            costs[key_slot(key)] = static_cast<float>(rho);
        });
        throwIfSignalCancellationRequested();
        strip_begin = strip_end;
    }
    return costs;
}

std::vector<std::uint8_t> coefficientParityBits(
    std::span<const std::int16_t> coefficients,
    std::span<const std::uint32_t> selected_offsets) {
    std::vector<std::uint8_t> bits(selected_offsets.size());
    // Parities in keyed order: wiped if a bad offset stops the fill partway.
    const carrier_common::WipeOnThrow bits_wipe{bits};
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
            --coefficient;  // +/-1 always flips the parity
        }
        ++changes;
    }
    return changes;
}

} // namespace twitter_steg_internal
