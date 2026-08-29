#include "twitter_jpeg_codec.h"

#include <algorithm>
#include <array>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

extern "C" {
#include <jpeglib.h>
}

namespace twitter_steg_internal {
namespace {

constexpr int kMaxCarrierQuality = 97;
constexpr std::uint64_t kMaxPixels = 4'096ULL * 4'096ULL;
constexpr std::uint32_t kMaxDimension = 4'096;

constexpr std::array<unsigned int, 64> kStdLumaQ50{
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,
    24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99
};

constexpr std::array<unsigned int, 64> kStdChromaQ50{
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};

using QuantizationTable = std::array<unsigned int, 64>;

struct QuantizationProfile {
    std::array<QuantizationTable, 3> component_tables{};
    int source_quality{};
    int carrier_quality{};
};

struct JpegErrorManager {
    jpeg_error_mgr pub{};
    std::jmp_buf jump{};
    std::array<char, JMSG_LENGTH_MAX> message{};
};

extern "C" void twitterJpegErrorExit(j_common_ptr common) {
    auto* error = reinterpret_cast<JpegErrorManager*>(common->err);
    (*common->err->format_message)(common, error->message.data());
    std::longjmp(error->jump, 1);
}

struct CodecState {
    jpeg_decompress_struct source{};
    jpeg_compress_struct destination{};
    bool source_created{false};
    bool destination_created{false};
    unsigned char* destination_buffer{nullptr};
    unsigned long destination_size{0};
};

// The C++ locals each codec entry point needs across a setjmp/longjmp round trip.
//
// libjpeg reports errors by longjmp'ing back to the setjmp point. An automatic
// object modified between setjmp and longjmp has an indeterminate value
// afterwards, and every object here has a non-trivial destructor that runs
// while fail()'s exception unwinds the setjmp frame -- destroying a container
// whose members are indeterminate is precisely what that rule forbids. Holding
// them on the heap behind a pointer established *before* setjmp puts them out
// of longjmp's reach: the pointer is never reassigned, so its value survives,
// and the objects it owns are untouched by the jump.
//
// CodecState and JpegErrorManager are already heap-allocated for the same
// reason; this extends the treatment to the rest. (ImageInfo and the plain
// scalars stay on the stack: they are trivially destructible and are never
// read on the longjmp path, so no indeterminate value is ever observed.)
//
// Mirrors CodecLocals in reddit_steg.cpp -- keep the two in step.
struct CodecLocals {
    std::vector<JSAMPLE> row;
    std::vector<std::uint8_t> pixels;
    CoefficientImage coefficients;
    QuantizationProfile quantization;
    Bytes output;
};

void cleanup(CodecState& state) noexcept {
    if (state.destination_created) {
        jpeg_destroy_compress(&state.destination);
        state.destination_created = false;
    }
    if (state.source_created) {
        jpeg_destroy_decompress(&state.source);
        state.source_created = false;
    }
    std::free(state.destination_buffer);
    state.destination_buffer = nullptr;
    state.destination_size = 0;
}

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void validateInputSize(std::span<const std::uint8_t> jpeg) {
    if (jpeg.empty()) {
        fail("JPEG input is empty.");
    }
    if (jpeg.size() > std::numeric_limits<unsigned long>::max()) {
        fail("JPEG input is too large for libjpeg.");
    }
}

void setMemorySource(jpeg_decompress_struct& source,
                     std::span<const std::uint8_t> jpeg) {
    validateInputSize(jpeg);
    jpeg_mem_src(&source,
                 const_cast<unsigned char*>(jpeg.data()),
                 static_cast<unsigned long>(jpeg.size()));
}

[[nodiscard]] QuantizationTable scaledQuantTable(
    const QuantizationTable& base,
    int quality) {
    const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    std::array<unsigned int, 64> result{};
    for (std::size_t index = 0; index < base.size(); ++index) {
        result[index] = static_cast<unsigned int>(std::clamp(
            (static_cast<int>(base[index]) * scale + 50) / 100, 1, 255));
    }
    return result;
}

[[nodiscard]] const JQUANT_TBL* componentQuantTable(
    const jpeg_decompress_struct& source,
    int component) {
    if (component < 0 || component >= source.num_components) {
        return nullptr;
    }
    const int table_number = source.comp_info[component].quant_tbl_no;
    if (table_number < 0 || table_number >= NUM_QUANT_TBLS) {
        return nullptr;
    }
    return source.quant_tbl_ptrs[table_number];
}

[[nodiscard]] int estimateLuminanceQuality(const JQUANT_TBL* table) {
    if (table == nullptr) return 0;

    std::uint64_t actual_sum = 0;
    for (std::size_t index = 0; index < DCTSIZE2; ++index) {
        actual_sum += table->quantval[index];
    }

    int best_quality = 1;
    std::uint64_t best_distance = std::numeric_limits<std::uint64_t>::max();
    for (int quality = 1; quality <= 100; ++quality) {
        const QuantizationTable expected =
            scaledQuantTable(kStdLumaQ50, quality);
        std::uint64_t expected_sum = 0;
        for (const unsigned int value : expected) expected_sum += value;
        const std::uint64_t distance = actual_sum > expected_sum
            ? actual_sum - expected_sum
            : expected_sum - actual_sum;
        if (distance < best_distance) {
            best_distance = distance;
            best_quality = quality;
        }
    }
    return best_quality;
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

    QuantizationProfile profile;
    profile.source_quality = estimateLuminanceQuality(
        componentQuantTable(source, 0));
    if (profile.source_quality == 0) {
        fail("JPEG has no luminance quantization table.");
    }

    profile.carrier_quality = std::min(
        profile.source_quality,
        kMaxCarrierQuality);
    if (profile.source_quality > kMaxCarrierQuality) {
        profile.component_tables[0] = scaledQuantTable(
            kStdLumaQ50,
            kMaxCarrierQuality);
        profile.component_tables[1] = scaledQuantTable(
            kStdChromaQ50,
            kMaxCarrierQuality);
        profile.component_tables[2] = profile.component_tables[1];
        return profile;
    }

    profile.component_tables[0] = copyQuantizationTable(
        componentQuantTable(source, 0));
    if (source.num_components == 3) {
        profile.component_tables[1] = copyQuantizationTable(
            componentQuantTable(source, 1));
        profile.component_tables[2] = copyQuantizationTable(
            componentQuantTable(source, 2));
    } else {
        profile.component_tables[1] = scaledQuantTable(
            kStdChromaQ50,
            profile.carrier_quality);
        profile.component_tables[2] = profile.component_tables[1];
    }
    return profile;
}

void installQuantizationProfile(
    jpeg_compress_struct& destination,
    const QuantizationProfile& profile) {

    jpeg_add_quant_table(
        &destination,
        0,
        profile.component_tables[0].data(),
        100,
        TRUE);
    jpeg_add_quant_table(
        &destination,
        1,
        profile.component_tables[1].data(),
        100,
        TRUE);
    destination.comp_info[0].quant_tbl_no = 0;
    destination.comp_info[1].quant_tbl_no = 1;

    if (profile.component_tables[2] == profile.component_tables[1]) {
        destination.comp_info[2].quant_tbl_no = 1;
    } else {
        jpeg_add_quant_table(
            &destination,
            2,
            profile.component_tables[2].data(),
            100,
            TRUE);
        destination.comp_info[2].quant_tbl_no = 2;
    }
}

[[nodiscard]] ImageInfo infoFromHeader(const jpeg_decompress_struct& source) {
    const bool colour = source.num_components == 3;
    const bool is_420 = colour &&
        source.comp_info[0].h_samp_factor == 2 &&
        source.comp_info[0].v_samp_factor == 2 &&
        source.comp_info[1].h_samp_factor == 1 &&
        source.comp_info[1].v_samp_factor == 1 &&
        source.comp_info[2].h_samp_factor == 1 &&
        source.comp_info[2].v_samp_factor == 1;
    const bool is_ycbcr = source.jpeg_color_space == JCS_YCbCr;
    const int estimated_quality = estimateLuminanceQuality(
        componentQuantTable(source, 0));

    return ImageInfo{
        .width = source.image_width,
        .height = source.image_height,
        .y_blocks_wide = source.num_components > 0
            ? source.comp_info[0].width_in_blocks : 0,
        .y_blocks_high = source.num_components > 0
            ? source.comp_info[0].height_in_blocks : 0,
        .components = source.num_components,
        .progressive = source.progressive_mode != 0,
        .is_420 = is_420,
        .is_ycbcr = is_ycbcr,
        .estimated_quality = estimated_quality
    };
}

void validateDimensions(const ImageInfo& info) {
    if (info.width == 0 || info.height == 0) {
        fail("JPEG has invalid zero dimensions.");
    }
    if (info.width > kMaxDimension || info.height > kMaxDimension) {
        fail("Image File Size Error: Cover image dimensions exceed X-Twitter's 4096x4096-pixel limit.");
    }
    if (info.pixelCount() > kMaxPixels) {
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

[[nodiscard]] Bytes takeDestination(CodecState& state) {
    if (state.destination_buffer == nullptr || state.destination_size == 0) {
        fail("libjpeg produced an empty output image.");
    }
    Bytes result(state.destination_buffer,
                 state.destination_buffer + state.destination_size);
    std::free(state.destination_buffer);
    state.destination_buffer = nullptr;
    state.destination_size = 0;
    return result;
}

} // namespace

std::uint64_t ImageInfo::pixelCount() const {
    return static_cast<std::uint64_t>(width) * height;
}

std::uint64_t ImageInfo::yBlockCount() const {
    return static_cast<std::uint64_t>(y_blocks_wide) * y_blocks_high;
}

ImageInfo inspectJpeg(std::span<const std::uint8_t> jpeg) {
    validateInputSize(jpeg);
    auto state = std::make_unique<CodecState>();
    auto error = std::make_unique<JpegErrorManager>();
    jpeg_std_error(&error->pub);
    error->pub.error_exit = twitterJpegErrorExit;
    if (setjmp(error->jump) != 0) {
        const std::string message = error->message.data();
        cleanup(*state);
        fail("JPEG error: " + message);
    }

    state->source.err = &error->pub;
    jpeg_create_decompress(&state->source);
    state->source_created = true;
    setMemorySource(state->source, jpeg);
    jpeg_read_header(&state->source, TRUE);
    const ImageInfo info = infoFromHeader(state->source);
    validateDimensions(info);
    cleanup(*state);
    return info;
}

Bytes prepareProgressiveSourceQuality(std::span<const std::uint8_t> jpeg) {
    const ImageInfo input_info = inspectJpeg(jpeg);
    if (input_info.components == 3 &&
        input_info.is_ycbcr &&
        input_info.is_420 &&
        input_info.estimated_quality > 0 &&
        input_info.estimated_quality <= kMaxCarrierQuality) {
        // The normal case needs no pixel-domain re-encode. Rewriting the
        // original coefficient arrays with a progressive scan script retains
        // both the source quantization tables and the decoded image exactly.
        const CoefficientImage coefficients = readCoefficients(jpeg);
        return writeProgressiveCoefficients(jpeg, coefficients.luminance);
    }

    validateInputSize(jpeg);
    auto state = std::make_unique<CodecState>();
    auto error = std::make_unique<JpegErrorManager>();
    auto locals = std::make_unique<CodecLocals>();
    jpeg_std_error(&error->pub);
    error->pub.error_exit = twitterJpegErrorExit;
    if (setjmp(error->jump) != 0) {
        const std::string message = error->message.data();
        cleanup(*state);
        fail("JPEG error while preparing the carrier: " + message);
    }

    state->source.err = &error->pub;
    jpeg_create_decompress(&state->source);
    state->source_created = true;
    setMemorySource(state->source, jpeg);
    jpeg_read_header(&state->source, TRUE);
    validateDecodableCover(state->source);
    validateDimensions(infoFromHeader(state->source));
    locals->quantization = sourceQuantizationProfile(state->source);
    state->source.out_color_space = JCS_RGB;
    state->source.dct_method = JDCT_ISLOW;
    jpeg_start_decompress(&state->source);

    state->destination.err = &error->pub;
    jpeg_create_compress(&state->destination);
    state->destination_created = true;
    jpeg_mem_dest(&state->destination,
                  &state->destination_buffer,
                  &state->destination_size);
    state->destination.image_width = state->source.output_width;
    state->destination.image_height = state->source.output_height;
    state->destination.input_components = 3;
    state->destination.in_color_space = JCS_RGB;
    jpeg_set_defaults(&state->destination);
    installQuantizationProfile(
        state->destination,
        locals->quantization);
    state->destination.comp_info[0].h_samp_factor = 2;
    state->destination.comp_info[0].v_samp_factor = 2;
    state->destination.comp_info[1].h_samp_factor = 1;
    state->destination.comp_info[1].v_samp_factor = 1;
    state->destination.comp_info[2].h_samp_factor = 1;
    state->destination.comp_info[2].v_samp_factor = 1;
    state->destination.optimize_coding = TRUE;
    configureStandardJfif(state->destination);
    jpeg_simple_progression(&state->destination);
    jpeg_start_compress(&state->destination, TRUE);

    const std::uint64_t row_bytes =
        static_cast<std::uint64_t>(state->source.output_width) * 3U;
    if (row_bytes > std::numeric_limits<std::size_t>::max()) {
        fail("Decoded JPEG row is too large.");
    }
    locals->row.resize(static_cast<std::size_t>(row_bytes));
    while (state->source.output_scanline < state->source.output_height) {
        JSAMPROW input_row = locals->row.data();
        jpeg_read_scanlines(&state->source, &input_row, 1);
        JSAMPROW output_row = locals->row.data();
        jpeg_write_scanlines(&state->destination, &output_row, 1);
    }
    jpeg_finish_compress(&state->destination);
    jpeg_finish_decompress(&state->source);
    locals->output = takeDestination(*state);
    cleanup(*state);
    return std::move(locals->output);
}

std::vector<std::uint8_t> decodeLuminance(
    std::span<const std::uint8_t> jpeg,
    ImageInfo* decoded_info) {
    validateInputSize(jpeg);
    auto state = std::make_unique<CodecState>();
    auto error = std::make_unique<JpegErrorManager>();
    auto locals = std::make_unique<CodecLocals>();
    jpeg_std_error(&error->pub);
    error->pub.error_exit = twitterJpegErrorExit;
    if (setjmp(error->jump) != 0) {
        const std::string message = error->message.data();
        cleanup(*state);
        fail("JPEG error while decoding luminance: " + message);
    }

    state->source.err = &error->pub;
    jpeg_create_decompress(&state->source);
    state->source_created = true;
    setMemorySource(state->source, jpeg);
    jpeg_read_header(&state->source, TRUE);
    const ImageInfo info = infoFromHeader(state->source);
    validateDimensions(info);
    state->source.out_color_space = JCS_GRAYSCALE;
    state->source.dct_method = JDCT_ISLOW;
    jpeg_start_decompress(&state->source);
    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(state->source.output_width) *
        state->source.output_height;
    locals->pixels.resize(static_cast<std::size_t>(pixel_count));
    while (state->source.output_scanline < state->source.output_height) {
        JSAMPROW output_row = locals->pixels.data() +
            static_cast<std::size_t>(state->source.output_scanline) *
            state->source.output_width;
        jpeg_read_scanlines(&state->source, &output_row, 1);
    }
    jpeg_finish_decompress(&state->source);
    cleanup(*state);
    if (decoded_info != nullptr) {
        *decoded_info = info;
    }
    return std::move(locals->pixels);
}

CoefficientImage readCoefficients(std::span<const std::uint8_t> jpeg) {
    validateInputSize(jpeg);
    auto state = std::make_unique<CodecState>();
    auto error = std::make_unique<JpegErrorManager>();
    auto locals = std::make_unique<CodecLocals>();
    jpeg_std_error(&error->pub);
    error->pub.error_exit = twitterJpegErrorExit;
    if (setjmp(error->jump) != 0) {
        const std::string message = error->message.data();
        cleanup(*state);
        fail("JPEG coefficient error: " + message);
    }

    state->source.err = &error->pub;
    jpeg_create_decompress(&state->source);
    state->source_created = true;
    setMemorySource(state->source, jpeg);
    jpeg_read_header(&state->source, TRUE);
    locals->coefficients.info = infoFromHeader(state->source);
    validateDimensions(locals->coefficients.info);
    if (locals->coefficients.info.components < 1) {
        fail("JPEG has no luminance component.");
    }
    const JQUANT_TBL* quant = componentQuantTable(state->source, 0);
    if (quant == nullptr) {
        fail("JPEG has no luminance quantization table.");
    }
    for (std::size_t index = 0; index < 64; ++index) {
        locals->coefficients.luminance_quantization[index] =
            static_cast<std::uint16_t>(quant->quantval[index]);
    }

    jvirt_barray_ptr* arrays = jpeg_read_coefficients(&state->source);
    const jpeg_component_info& y = state->source.comp_info[0];
    const std::uint64_t coefficient_count =
        static_cast<std::uint64_t>(y.width_in_blocks) *
        y.height_in_blocks * DCTSIZE2;
    if (coefficient_count > std::numeric_limits<std::size_t>::max()) {
        fail("JPEG coefficient array is too large.");
    }
    locals->coefficients.luminance.resize(
        static_cast<std::size_t>(coefficient_count));
    for (JDIMENSION block_row = 0; block_row < y.height_in_blocks; ++block_row) {
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
                locals->coefficients.luminance[base + coefficient] =
                    static_cast<std::int16_t>(row[0][block_column][coefficient]);
            }
        }
    }
    jpeg_finish_decompress(&state->source);
    cleanup(*state);
    return std::move(locals->coefficients);
}

Bytes writeProgressiveCoefficients(
    std::span<const std::uint8_t> source_jpeg,
    std::span<const std::int16_t> luminance_coefficients) {
    validateInputSize(source_jpeg);
    auto state = std::make_unique<CodecState>();
    auto error = std::make_unique<JpegErrorManager>();
    auto locals = std::make_unique<CodecLocals>();
    jpeg_std_error(&error->pub);
    error->pub.error_exit = twitterJpegErrorExit;
    if (setjmp(error->jump) != 0) {
        const std::string message = error->message.data();
        cleanup(*state);
        fail("JPEG coefficient-write error: " + message);
    }

    state->source.err = &error->pub;
    jpeg_create_decompress(&state->source);
    state->source_created = true;
    setMemorySource(state->source, source_jpeg);
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

    state->destination.err = &error->pub;
    jpeg_create_compress(&state->destination);
    state->destination_created = true;
    jpeg_mem_dest(&state->destination,
                  &state->destination_buffer,
                  &state->destination_size);
    jpeg_copy_critical_parameters(&state->source, &state->destination);
    state->destination.optimize_coding = TRUE;
    configureStandardJfif(state->destination);
    jpeg_simple_progression(&state->destination);
    jpeg_write_coefficients(&state->destination, arrays);
    jpeg_finish_compress(&state->destination);
    jpeg_finish_decompress(&state->source);
    locals->output = takeDestination(*state);
    cleanup(*state);
    return std::move(locals->output);
}

} // namespace twitter_steg_internal
