#include "twitter_jpeg_codec.h"
#include "carrier_parallel.h"
#include "carrier_common.h"
#include "jpeg_codec_common.h"

#include <algorithm>
#include <array>
#include <csetjmp>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace twitter_steg_internal {
namespace {

using carrier_common::QuantizationTable;
using carrier_common::scaledQuantTable;

constexpr int kMaxCarrierQuality = 97;
constexpr std::uint32_t kMaxDimension = 4'096;

// The tables the carrier is encoded with, one per component.
using QuantizationProfile = std::array<QuantizationTable, 3>;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

[[nodiscard]] int estimateLuminanceQuality(const JQUANT_TBL* table) {
    if (table == nullptr) return 0;

    return carrier_common::estimateQualityFromLumaSum(
        std::ranges::fold_left(table->quantval, std::uint64_t{0}, std::plus{}));
}

[[nodiscard]] QuantizationTable copyQuantizationTable(
    const JQUANT_TBL* table) {

    if (table == nullptr) {
        fail("JPEG component has no quantization table.");
    }
    QuantizationTable values{};
    for (std::size_t index = 0; index < values.size(); ++index) {
        values[index] = table->quantval[index];
    }
    return values;
}

[[nodiscard]] QuantizationProfile sourceQuantizationProfile(
    const jpeg_decompress_struct& source) {

    const int source_quality = estimateLuminanceQuality(
        jpeg_codec::componentQuantTable(source, 0));
    if (source_quality == 0) {
        fail("JPEG has no luminance quantization table.");
    }

    QuantizationProfile profile;
    if (source_quality > kMaxCarrierQuality) {
        profile[0] = scaledQuantTable(
            carrier_common::STD_LUMA_Q50,
            kMaxCarrierQuality);
        profile[1] = scaledQuantTable(
            carrier_common::STD_CHROMA_Q50,
            kMaxCarrierQuality);
        profile[2] = profile[1];
        return profile;
    }

    profile[0] = copyQuantizationTable(
        jpeg_codec::componentQuantTable(source, 0));
    if (source.num_components == 3) {
        profile[1] = copyQuantizationTable(
            jpeg_codec::componentQuantTable(source, 1));
        profile[2] = copyQuantizationTable(
            jpeg_codec::componentQuantTable(source, 2));
    } else {
        profile[1] = scaledQuantTable(
            carrier_common::STD_CHROMA_Q50,
            source_quality);
        profile[2] = profile[1];
    }
    return profile;
}

void installQuantizationProfile(
    jpeg_compress_struct& destination,
    const QuantizationProfile& profile) {

    jpeg_add_quant_table(
        &destination,
        0,
        profile[0].data(),
        100,
        TRUE);
    jpeg_add_quant_table(
        &destination,
        1,
        profile[1].data(),
        100,
        TRUE);
    destination.comp_info[0].quant_tbl_no = 0;
    destination.comp_info[1].quant_tbl_no = 1;

    if (profile[2] == profile[1]) {
        destination.comp_info[2].quant_tbl_no = 1;
    } else {
        jpeg_add_quant_table(
            &destination,
            2,
            profile[2].data(),
            100,
            TRUE);
        destination.comp_info[2].quant_tbl_no = 2;
    }
}

[[nodiscard]] ImageInfo infoFromHeader(const jpeg_decompress_struct& source) {
    return ImageInfo{
        jpeg_codec::headerInfo(source),
        estimateLuminanceQuality(jpeg_codec::componentQuantTable(source, 0)),
    };
}

void validateDimensions(const ImageInfo& info) {
    if (info.width == 0 || info.height == 0) {
        fail("JPEG has invalid zero dimensions.");
    }
    if (info.width > kMaxDimension || info.height > kMaxDimension) {
        fail("Image File Size Error: Cover image dimensions exceed X-Twitter's 4096x4096-pixel limit.");
    }
}

void validateDecodableCover(const jpeg_decompress_struct& source) {
    if (source.num_components != 1 && source.num_components != 3) {
        fail("Cover must be a grayscale or three-component JPEG.");
    }
}

void configureStandardJfif(jpeg_compress_struct& destination) {
    destination.write_JFIF_header = TRUE;
    destination.JFIF_major_version = 1;
    destination.JFIF_minor_version = 1;
    destination.density_unit = 0;
    destination.X_density = 1;
    destination.Y_density = 1;
    destination.write_Adobe_marker = FALSE;
}

} // namespace

ImageInfo inspectJpeg(std::span<const std::uint8_t> jpeg) {
    return jpeg_codec::inspectHeader(jpeg, [](const jpeg_decompress_struct& source) {
        const ImageInfo info = infoFromHeader(source);
        validateDimensions(info);
        return info;
    });
}

namespace {
// No replacement means the fully validated input can be retained as-is.
std::optional<Bytes> prepareSourceQuality(
    std::span<const std::uint8_t> jpeg,
    CoefficientImage* coefficients_out,
    ImageInfo* input_info_out) {
    const ImageInfo input_info = inspectJpeg(jpeg);
    if (input_info_out != nullptr) *input_info_out = input_info;
    if (input_info.components == 3 &&
        input_info.is_ycbcr &&
        input_info.is_420 &&
        input_info.estimated_quality > 0 &&
        input_info.estimated_quality <= kMaxCarrierQuality) {
        // The normal case needs no re-encode at all. Rewriting the source
        // coefficient arrays with a progressive scan script retains both the
        // quantization tables and the decoded image exactly, so the source
        // can stand in for its own progressive rewrite: the one rewrite the
        // carrier needs happens when the payload is written. Reading the
        // coefficients also fully decodes the input under the scan limits.
        CoefficientImage coefficients = readCoefficients(jpeg, /*cover=*/true);
        coefficients.info.progressive = true;
        if (coefficients_out != nullptr) {
            *coefficients_out = std::move(coefficients);
        }
        return std::nullopt;
    }

    // Capture the header tables before progressive decoding can consume later
    // scans. This plain-data object outlives the helper's setjmp frame.
    QuantizationProfile profile;
    return jpeg_codec::transcodeRgb420(jpeg,
        [&](const jpeg_decompress_struct& source) {
            validateDecodableCover(source);
            validateDimensions(infoFromHeader(source));
            profile = sourceQuantizationProfile(source);
        },
        [&](const jpeg_decompress_struct&, jpeg_compress_struct& destination) {
            installQuantizationProfile(destination, profile);
            destination.optimize_coding = TRUE;
            configureStandardJfif(destination);
            jpeg_simple_progression(&destination);
        }, "JPEG error while preparing the carrier: ", JDCT_ISLOW);
}
} // namespace

Bytes prepareProgressiveSourceQuality(
    std::span<const std::uint8_t> jpeg,
    CoefficientImage* coefficients_out,
    ImageInfo* input_info_out) {
    auto replacement = prepareSourceQuality(jpeg, coefficients_out, input_info_out);
    if (replacement) return std::move(*replacement);
    return Bytes(jpeg.begin(), jpeg.end());
}

Bytes prepareProgressiveSourceQuality(
    Bytes&& jpeg,
    CoefficientImage* coefficients_out,
    ImageInfo* input_info_out) {
    auto replacement = prepareSourceQuality(jpeg, coefficients_out, input_info_out);
    if (replacement) return std::move(*replacement);
    return std::move(jpeg);
}

namespace {

// libjpeg's accurate integer inverse DCT (jidctint.c, JDCT_ISLOW), as a
// libjpeg decode with that method computes it (the tests check against one):
// the same fixed-point constants, the same two passes and the same rounding,
// so the samples match it exactly.
// Where libjpeg-turbo's SIMD implementation of this transform narrows --
// dequantized values and a few sums held in 16-bit lanes, the first pass
// saturated to 16 bits, the final range limit saturating -- this does too, so
// the match holds even for coefficients far outside anything an encoder emits
// (checked byte for byte against libjpeg-turbo: qualities 3-100, every
// subsampling, odd sizes, and random full-range coefficients). The result
// only feeds the embedding cost model, never recovery.
constexpr int kIdctConstBits = 13;
constexpr int kIdctPass1Bits = 2;
constexpr std::int32_t
    kFix0_298631336 = 2446,  kFix0_390180644 = 3196,  kFix0_541196100 = 4433,
    kFix0_765366865 = 6270,  kFix0_899976223 = 7373,  kFix1_175875602 = 9633,
    kFix1_501321110 = 12299, kFix1_847759065 = 15137, kFix1_961570560 = 16069,
    kFix2_053119869 = 16819, kFix2_562915447 = 20995, kFix3_072711026 = 25172;

// Wrapping add, then an arithmetic shift: defined for any coefficient.
[[nodiscard]] constexpr std::int32_t idctDescale(std::int32_t value, int bits) noexcept {
    return static_cast<std::int32_t>(
        static_cast<std::uint32_t>(value) + (std::uint32_t{1} << (bits - 1))) >> bits;
}

// One-dimensional pass over eight inputs `in`, shared by the column and row
// passes; `out` receives the eight results before descaling.
struct IdctPass {
    std::int32_t even[4];  // tmp10..tmp13
    std::int32_t odd[4];   // tmp0..tmp3
};

[[nodiscard]] inline IdctPass idctPass(
    std::int32_t in0, std::int32_t in1, std::int32_t in2, std::int32_t in3,
    std::int32_t in4, std::int32_t in5, std::int32_t in6, std::int32_t in7) noexcept {
    // Wrapping arithmetic, as in libjpeg: unsigned so overflow on a hostile
    // coefficient is defined.
    const auto mul = [](std::int32_t a, std::int32_t b) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
    };
    const auto add = [](std::int32_t a, std::int32_t b) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
    };
    const auto sub = [](std::int32_t a, std::int32_t b) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
    };
    const auto shl = [](std::int32_t a, int bits) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) << bits);
    };

    // The SIMD transform keeps these sums in 16-bit lanes.
    const auto word = [](std::int32_t a) { return static_cast<std::int32_t>(static_cast<std::int16_t>(a)); };

    // Even part.
    std::int32_t z1 = add(mul(in2, kFix0_541196100), mul(in6, kFix0_541196100));
    const std::int32_t even2 = add(z1, mul(in6, -kFix1_847759065));
    const std::int32_t even3 = add(z1, mul(in2, kFix0_765366865));
    const std::int32_t even0 = shl(word(add(in0, in4)), kIdctConstBits);
    const std::int32_t even1 = shl(word(sub(in0, in4)), kIdctConstBits);

    // Odd part.
    std::int32_t tmp0 = in7, tmp1 = in5, tmp2 = in3, tmp3 = in1;
    z1 = add(tmp0, tmp3);
    std::int32_t z2 = add(tmp1, tmp2);
    std::int32_t z3 = word(add(tmp0, tmp2));
    std::int32_t z4 = word(add(tmp1, tmp3));
    const std::int32_t z5 = mul(add(z3, z4), kFix1_175875602);
    tmp0 = mul(tmp0, kFix0_298631336);
    tmp1 = mul(tmp1, kFix2_053119869);
    tmp2 = mul(tmp2, kFix3_072711026);
    tmp3 = mul(tmp3, kFix1_501321110);
    z1 = mul(z1, -kFix0_899976223);
    z2 = mul(z2, -kFix2_562915447);
    z3 = add(mul(z3, -kFix1_961570560), z5);
    z4 = add(mul(z4, -kFix0_390180644), z5);

    return IdctPass{
        .even = {add(even0, even3), add(even1, even2), sub(even1, even2), sub(even0, even3)},
        .odd = {add(tmp0, add(z1, z3)), add(tmp1, add(z2, z4)), add(tmp2, add(z2, z3)), add(tmp3, add(z1, z4))},
    };
}

// Inverse DCT of one quantized block into an 8x8 sample block.
void idctBlock(const std::int16_t* block,
               const std::array<std::uint16_t, 64>& quantization,
               std::uint8_t* samples) noexcept {
    const auto wrapAdd = [](std::int32_t a, std::int32_t b) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
    };
    const auto wrapSub = [](std::int32_t a, std::int32_t b) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
    };
    std::int32_t workspace[64];
    // Pass 1: columns, results scaled up by 2^kIdctPass1Bits.
    for (int column = 0; column < 8; ++column) {
        const auto dequantized = [&](int row) {
            return static_cast<std::int32_t>(static_cast<std::int16_t>(
                static_cast<std::int32_t>(block[row * 8 + column]) *
                static_cast<std::int32_t>(quantization[static_cast<std::size_t>(row * 8 + column)])));
        };
        const IdctPass pass = idctPass(
            dequantized(0), dequantized(1), dequantized(2), dequantized(3),
            dequantized(4), dequantized(5), dequantized(6), dequantized(7));
        const int bits = kIdctConstBits - kIdctPass1Bits;
        workspace[0 * 8 + column] = std::clamp(idctDescale(wrapAdd(pass.even[0], pass.odd[3]), bits), -32768, 32767);
        workspace[7 * 8 + column] = std::clamp(idctDescale(wrapSub(pass.even[0], pass.odd[3]), bits), -32768, 32767);
        workspace[1 * 8 + column] = std::clamp(idctDescale(wrapAdd(pass.even[1], pass.odd[2]), bits), -32768, 32767);
        workspace[6 * 8 + column] = std::clamp(idctDescale(wrapSub(pass.even[1], pass.odd[2]), bits), -32768, 32767);
        workspace[2 * 8 + column] = std::clamp(idctDescale(wrapAdd(pass.even[2], pass.odd[1]), bits), -32768, 32767);
        workspace[5 * 8 + column] = std::clamp(idctDescale(wrapSub(pass.even[2], pass.odd[1]), bits), -32768, 32767);
        workspace[3 * 8 + column] = std::clamp(idctDescale(wrapAdd(pass.even[3], pass.odd[0]), bits), -32768, 32767);
        workspace[4 * 8 + column] = std::clamp(idctDescale(wrapSub(pass.even[3], pass.odd[0]), bits), -32768, 32767);
    }
    // Pass 2: rows, descaled by the pass-1 scale and the transform's 1/8,
    // then level-shifted and limited to a sample.
    for (int row = 0; row < 8; ++row) {
        const std::int32_t* const in = workspace + row * 8;
        const IdctPass pass = idctPass(in[0], in[1], in[2], in[3], in[4], in[5], in[6], in[7]);
        const int bits = kIdctConstBits + kIdctPass1Bits + 3;
        const auto sample = [&](std::int32_t value) {
            return static_cast<std::uint8_t>(std::clamp(idctDescale(value, bits), -128, 127) + 128);
        };
        std::uint8_t* const out = samples + row * 8;
        out[0] = sample(wrapAdd(pass.even[0], pass.odd[3]));
        out[7] = sample(wrapSub(pass.even[0], pass.odd[3]));
        out[1] = sample(wrapAdd(pass.even[1], pass.odd[2]));
        out[6] = sample(wrapSub(pass.even[1], pass.odd[2]));
        out[2] = sample(wrapAdd(pass.even[2], pass.odd[1]));
        out[5] = sample(wrapSub(pass.even[2], pass.odd[1]));
        out[3] = sample(wrapAdd(pass.even[3], pass.odd[0]));
        out[4] = sample(wrapSub(pass.even[3], pass.odd[0]));
    }
}

} // namespace

std::vector<std::uint8_t> luminanceFromCoefficients(const CoefficientImage& coefficients) {
    throwIfSignalCancellationRequested();
    const ImageInfo& info = coefficients.info;
    validateDimensions(info);
    const std::size_t width = info.width;
    const std::size_t height = info.height;
    const std::size_t blocks_wide = info.y_blocks_wide;
    const std::size_t blocks_high = info.y_blocks_high;
    // Full-resolution luminance: its block grid covers the image exactly.
    if (blocks_wide != (width + 7U) / 8U || blocks_high != (height + 7U) / 8U ||
        coefficients.luminance.size() != blocks_wide * blocks_high * 64U) {
        fail("Internal luminance coefficient-count mismatch.");
    }

    std::vector<std::uint8_t> pixels(width * height);
    const auto signed_block_rows = static_cast<std::int64_t>(blocks_high);
    carrier_parallel::forEach(0, signed_block_rows, 8, [&](std::int64_t signed_block_row) {
        const auto block_row = static_cast<std::size_t>(signed_block_row);
        const std::size_t rows = std::min<std::size_t>(8, height - block_row * 8U);
        std::uint8_t samples[64];
        for (std::size_t block_column = 0; block_column < blocks_wide; ++block_column) {
            idctBlock(
                coefficients.luminance.data() + (block_row * blocks_wide + block_column) * 64U,
                coefficients.luminance_quantization,
                samples);
            // The last block in a row or column may overhang the image.
            const std::size_t columns = std::min<std::size_t>(8, width - block_column * 8U);
            for (std::size_t row = 0; row < rows; ++row) {
                std::copy_n(
                    samples + row * 8U, columns,
                    pixels.data() + (block_row * 8U + row) * width + block_column * 8U);
            }
        }
    });
    throwIfSignalCancellationRequested();
    return pixels;
}

CoefficientImage readCoefficients(std::span<const std::uint8_t> jpeg, bool cover) {
    throwIfSignalCancellationRequested();
    jpeg_codec::requireMemorySource(jpeg);
    auto error = std::make_unique<jpeg_codec::ErrorManager>();
    auto state = std::make_unique<jpeg_codec::CodecState>();
    // libjpeg reports errors by longjmp'ing back to the setjmp point, and an
    // automatic object modified in between has an indeterminate value
    // afterwards -- yet the image's destructor runs while the exception
    // throwAfterLongjmp raises there unwinds this frame. Holding it, info
    // included, on the heap behind a pointer set before setjmp (and never
    // reassigned) keeps it out of longjmp's reach, as CodecState and
    // ErrorManager already are.
    auto coefficients = std::make_unique<CoefficientImage>();
    jpeg_codec::install(*error);
    if (cover) jpeg_codec::rejectWarnings(*error);
    if (setjmp(error->jump) != 0) {
        jpeg_codec::throwAfterLongjmp(*error, *state, "JPEG coefficient error: ");
    }

    state->openSource(*error, jpeg);
    jpeg_read_header(&state->source, TRUE);
    coefficients->info = infoFromHeader(state->source);
    validateDimensions(coefficients->info);
    if (coefficients->info.components < 1) {
        fail("JPEG has no luminance component.");
    }
    // Greyscale or YCbCr; the carriers read component 0 only, but a CMYK or
    // other layout is not an image they make or accept.
    if (coefficients->info.components > 3) {
        fail("JPEG has an unsupported number of components.");
    }
    if (jpeg_codec::componentQuantTable(state->source, 0) == nullptr) {
        fail("JPEG has no luminance quantization table.");
    }
    // Each component's table as the header left it, before any scan: the one
    // info.estimated_quality was measured from. A DQT later in the stream --
    // even before a component's first scan, so its latched table agrees with
    // the slot -- would otherwise go unseen. Plain arrays, filled after setjmp
    // but never read on the longjmp path.
    std::array<std::array<UINT16, DCTSIZE2>, 3> header_tables{};
    std::array<bool, 3> header_has_table{};
    if (cover) {
        for (int component = 0; component < state->source.num_components; ++component) {
            const JQUANT_TBL* table = jpeg_codec::componentQuantTable(state->source, component);
            if (table == nullptr) continue;
            const auto index = static_cast<std::size_t>(component);
            std::ranges::copy(table->quantval, header_tables[index].begin());
            header_has_table[index] = true;
        }
    }

    jvirt_barray_ptr* arrays = jpeg_read_coefficients(&state->source);
    const jpeg_component_info& y = state->source.comp_info[0];
    // The table libjpeg latched at the component's first scan is the one its
    // coefficients are quantized by. A DQT between scans can leave the slot
    // holding another, which is what writeProgressiveCoefficients would copy.
    const JQUANT_TBL* slot = jpeg_codec::componentQuantTable(state->source, 0);
    const JQUANT_TBL* quant = y.quant_table != nullptr ? y.quant_table : slot;
    if (quant == nullptr) {
        fail("JPEG has no luminance quantization table.");
    }
    if (cover) {
        // Every component, not just luminance, and against both the table
        // its first scan latched and the header's: a slot redefined after
        // the header no longer matches what the cover's quality was judged
        // by, nor, after the first scan, the table its coefficients are
        // quantized by. jpeg_copy_critical_parameters refuses the latter too,
        // but only when writing the carrier, after the costs and STC, and
        // with libjpeg's "multiple use of quantization table" message.
        for (int component = 0; component < state->source.num_components; ++component) {
            const JQUANT_TBL* table = jpeg_codec::componentQuantTable(state->source, component);
            if (table == nullptr) {
                fail("Image Error: JPEG component has no quantization table (corrupt or non-conformant JPG).");
            }
            const auto index = static_cast<std::size_t>(component);
            const JQUANT_TBL* latched = state->source.comp_info[component].quant_table;
            if (!header_has_table[index] ||
                !std::ranges::equal(header_tables[index], table->quantval) ||
                (latched != nullptr && !std::ranges::equal(latched->quantval, table->quantval))) {
                fail("Image Error: JPEG redefines a quantization table between scans.");
            }
            if (std::ranges::contains(table->quantval, UINT16_C(0))) {
                fail("Image Error: JPEG quantization table has a zero entry (corrupt or non-conformant JPG).");
            }
        }
    }
    for (std::size_t index = 0; index < 64; ++index) {
        coefficients->luminance_quantization[index] =
            static_cast<std::uint16_t>(quant->quantval[index]);
    }
    // The quality of the table the coefficients actually use. For a cover
    // the checks above make it the header's; an extracted carrier gets it
    // right as well.
    coefficients->info.estimated_quality = estimateLuminanceQuality(quant);
    const std::uint64_t coefficient_count =
        static_cast<std::uint64_t>(y.width_in_blocks) *
        y.height_in_blocks * DCTSIZE2;
    if (coefficient_count > std::numeric_limits<std::size_t>::max()) {
        fail("JPEG coefficient array is too large.");
    }
    coefficients->luminance.resize(
        static_cast<std::size_t>(coefficient_count));
    for (JDIMENSION block_row = 0; block_row < y.height_in_blocks; ++block_row) {
        throwIfSignalCancellationRequested();
        JBLOCKARRAY row = (*state->source.mem->access_virt_barray)(
            reinterpret_cast<j_common_ptr>(&state->source),
            arrays[0], block_row, 1, FALSE);
        for (JDIMENSION block_column = 0;
             block_column < y.width_in_blocks;
             ++block_column) {
            const std::size_t base =
                (static_cast<std::size_t>(block_row) * y.width_in_blocks +
                 block_column) * DCTSIZE2;
            for (std::size_t coefficient = 0;
                 coefficient < DCTSIZE2;
                 ++coefficient) {
                coefficients->luminance[base + coefficient] =
                    static_cast<std::int16_t>(row[0][block_column][coefficient]);
            }
        }
    }
    jpeg_finish_decompress(&state->source);
    state->cleanup();
    return std::move(*coefficients);
}

// Rewrites source_jpeg as progressive JFIF with its Y coefficients replaced.
Bytes writeProgressiveCoefficients(
    std::span<const std::uint8_t> source_jpeg,
    std::span<const std::int16_t> luminance_coefficients) {
    if (luminance_coefficients.empty()) {
        fail("Internal luminance coefficient-count mismatch.");
    }
    throwIfSignalCancellationRequested();
    jpeg_codec::requireMemorySource(source_jpeg);
    auto error = std::make_unique<jpeg_codec::ErrorManager>();
    auto state = std::make_unique<jpeg_codec::CodecState>();
    jpeg_codec::install(*error);
    if (setjmp(error->jump) != 0) {
        jpeg_codec::throwAfterLongjmp(*error, *state, "JPEG coefficient-write error: ");
    }

    state->openSource(*error, source_jpeg);
    jpeg_read_header(&state->source, TRUE);
    const ImageInfo info = infoFromHeader(state->source);
    validateDimensions(info);
    jvirt_barray_ptr* arrays = jpeg_read_coefficients(&state->source);
    const jpeg_component_info& y = state->source.comp_info[0];
    const std::uint64_t expected =
        static_cast<std::uint64_t>(y.width_in_blocks) *
        y.height_in_blocks * DCTSIZE2;
    if (luminance_coefficients.size() != expected) {
        fail("Internal luminance coefficient-count mismatch.");
    }
    for (JDIMENSION block_row = 0; block_row < y.height_in_blocks; ++block_row) {
        throwIfSignalCancellationRequested();
        JBLOCKARRAY row = (*state->source.mem->access_virt_barray)(
            reinterpret_cast<j_common_ptr>(&state->source),
            arrays[0], block_row, 1, TRUE);
        for (JDIMENSION block_column = 0;
             block_column < y.width_in_blocks;
             ++block_column) {
            const std::size_t base =
                (static_cast<std::size_t>(block_row) * y.width_in_blocks +
                 block_column) * DCTSIZE2;
            for (std::size_t coefficient = 0;
                 coefficient < DCTSIZE2;
                 ++coefficient) {
                row[0][block_column][coefficient] =
                    static_cast<JCOEF>(luminance_coefficients[base + coefficient]);
            }
        }
    }

    state->openDestination(*error);
    jpeg_copy_critical_parameters(&state->source, &state->destination);
    state->destination.optimize_coding = TRUE;
    configureStandardJfif(state->destination);
    jpeg_simple_progression(&state->destination);
    jpeg_write_coefficients(&state->destination, arrays);
    jpeg_finish_compress(&state->destination);
    jpeg_finish_decompress(&state->source);
    // libjpeg is done: nothing can longjmp past here, and the state's
    // destructor releases the codecs.
    return state->takeOutput();
}

} // namespace twitter_steg_internal
