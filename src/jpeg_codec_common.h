#pragma once

// libjpeg plumbing shared by the coefficient carriers (reddit_steg.cpp and
// twitter_jpeg_codec.cpp): the longjmp error manager with its progress
// monitors, and the codec-object owner both entry-point styles clean up through.
//
// Compiled into the C++ CLI, the web variant and the Rust port's FFI build, so
// keep it byte-identical across the three trees.

#include "jpeg_header_info.h"
#include "jpeg_safety.h"

#include <array>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

extern "C" {
#include <jpeglib.h>
#include <jerror.h>
}

namespace jpeg_codec {

// Heap-allocate this (std::make_unique) before setjmp: libjpeg updates the
// progress counters after setjmp, and the stop reason must survive longjmp
// unchanged.
struct ErrorManager {
    jpeg_error_mgr pub{};
    std::jmp_buf jump{};
    std::array<char, JMSG_LENGTH_MAX> message{};
    jpeg_safety::ProgressMonitor source_progress{jump};
    jpeg_safety::ProgressMonitor destination_progress{jump};

    void throwIfStopped() const {
        source_progress.throwIfStopped();
        destination_progress.throwIfStopped();
    }
};

extern "C" inline void errorExit(j_common_ptr common) {
    auto* error = reinterpret_cast<ErrorManager*>(common->err);
    (*common->err->format_message)(common, error->message.data());
    std::longjmp(error->jump, 1);
}

inline void install(ErrorManager& error) {
    jpeg_std_error(&error.pub);
    error.pub.error_exit = errorExit;
}

// libjpeg only reports a warning (truncated data, a bad Huffman code, stray
// bytes before a marker) and decodes on; TurboJPEG's tjTransform, which
// normalizes covers, refuses the image instead. Cover preparation calls this
// after install() so a cover the carrier decodes straight from the input is
// refused exactly when normalization would have refused it. Recovery keeps
// libjpeg's lenient default.
extern "C" inline void emitMessageRejectingWarnings(j_common_ptr common, int msg_level) {
    if (msg_level < 0) errorExit(common);
}

inline void rejectWarnings(ErrorManager& error) {
    error.pub.emit_message = emitMessageRejectingWarnings;
}

// Growable in-memory encoder output. Not jpeg_mem_dest: that one swaps in a
// larger buffer as output grows but reports it to the caller only when
// compression completes, so a cancel or encoder error after the first growth
// left the caller holding a pointer that was already freed. Here `buffer`
// names the live allocation at every moment -- the callbacks change it only
// once the replacement exists -- and it lives in the heap-allocated
// CodecState, so whatever longjmp interrupts, cleanup frees exactly once.
struct MemoryDestination {
    jpeg_destination_mgr pub{}; // First member: libjpeg hands back &pub.
    unsigned char* buffer{nullptr};
    std::size_t capacity{0};
    std::size_t size{0};        // Set when compression completes.
};

inline constexpr std::size_t INITIAL_OUTPUT_CAPACITY = 64 * 1024;

extern "C" inline void initOutput(j_compress_ptr codec) {
    auto* output = reinterpret_cast<MemoryDestination*>(codec->dest);
    if (output->buffer == nullptr) {
        output->buffer = static_cast<unsigned char*>(std::malloc(INITIAL_OUTPUT_CAPACITY));
        if (output->buffer == nullptr) ERREXIT1(codec, JERR_OUT_OF_MEMORY, 0);
        output->capacity = INITIAL_OUTPUT_CAPACITY;
    }
    output->size = 0;
    output->pub.next_output_byte = output->buffer;
    output->pub.free_in_buffer = output->capacity;
}

// libjpeg calls this only when the buffer is full: double it.
extern "C" inline boolean growOutput(j_compress_ptr codec) {
    auto* output = reinterpret_cast<MemoryDestination*>(codec->dest);
    if (output->capacity > std::numeric_limits<std::size_t>::max() / 2) {
        ERREXIT1(codec, JERR_OUT_OF_MEMORY, 0);
    }
    const std::size_t grown = output->capacity * 2;
    auto* next = static_cast<unsigned char*>(std::realloc(output->buffer, grown));
    // On failure the old buffer is untouched and still owned.
    if (next == nullptr) ERREXIT1(codec, JERR_OUT_OF_MEMORY, 0);
    output->buffer = next;
    output->pub.next_output_byte = next + output->capacity;
    output->pub.free_in_buffer = grown - output->capacity;
    output->capacity = grown;
    return TRUE;
}

extern "C" inline void finishOutput(j_compress_ptr codec) {
    auto* output = reinterpret_cast<MemoryDestination*>(codec->dest);
    output->size = output->capacity - output->pub.free_in_buffer;
}

// Owns the libjpeg codec objects and the encoder's output buffer. Also
// heap-allocated before setjmp; validation and allocation can throw without
// going through error_exit, so the destructor owns cleanup as well as the
// explicit longjmp path.
struct CodecState {
    CodecState() = default;
    ~CodecState() noexcept { cleanup(); }
    CodecState(const CodecState&) = delete;
    CodecState& operator=(const CodecState&) = delete;

    jpeg_decompress_struct source{};
    jpeg_compress_struct destination{};
    MemoryDestination output{};
    bool source_created{false};
    bool destination_created{false};

    void cleanup() noexcept {
        if (destination_created) {
            jpeg_destroy_compress(&destination);
            destination_created = false;
        }
        if (source_created) {
            jpeg_destroy_decompress(&source);
            source_created = false;
        }
        std::free(output.buffer);
        output.buffer = nullptr;
        output.capacity = 0;
        output.size = 0;
    }

    // Create the decompressor, attach the error manager and scan-limit
    // monitor, and point it at `input`. The caller has already checked the
    // size with requireMemorySource().
    void openSource(ErrorManager& error, std::span<const std::uint8_t> input) {
        source.err = &error.pub;
        source_created = true;
        jpeg_create_decompress(&source);
        source.progress = &error.source_progress.pub;
        jpeg_mem_src(
            &source,
            const_cast<unsigned char*>(input.data()),
            static_cast<unsigned long>(input.size()));
    }

    // Create the compressor writing into `output`.
    void openDestination(ErrorManager& error) {
        destination.err = &error.pub;
        destination_created = true;
        jpeg_create_compress(&destination);
        destination.progress = &error.destination_progress.pub;
        output.pub.init_destination = initOutput;
        output.pub.empty_output_buffer = growOutput;
        output.pub.term_destination = finishOutput;
        destination.dest = &output.pub;
    }

    // Copy the encoder's output out and release its buffer.
    [[nodiscard]] std::vector<std::uint8_t> takeOutput() {
        if (output.buffer == nullptr || output.size == 0) {
            throw std::runtime_error("Image Error: JPEG encoder produced no output.");
        }
        std::vector<std::uint8_t> result(output.buffer, output.buffer + output.size);
        std::free(output.buffer);
        output.buffer = nullptr;
        output.capacity = 0;
        output.size = 0;
        return result;
    }
};

// The body of every entry point's `if (setjmp(error->jump) != 0)` branch:
// where libjpeg lands after errorExit or a progress monitor stops the codec.
// The order matters. The codec objects are destroyed before anything is
// thrown, and a cancellation or scan-limit stop is reported as itself rather
// than as a generic JPEG error. Nothing is allocated before that check, so an
// allocation failure cannot hide a cancellation; the libjpeg message lives in
// the ErrorManager, which cleanup() leaves alone. Only the setjmp call has to
// stay in the caller's frame.
[[noreturn]] inline void throwAfterLongjmp(
    const ErrorManager& error,
    CodecState& state,
    std::string_view message_prefix) {
    state.cleanup();
    error.throwIfStopped();
    std::string message(message_prefix);
    message += error.message.data();
    throw std::runtime_error(message);
}

inline void requireMemorySource(std::span<const std::uint8_t> input) {
    if (input.empty() ||
        input.size() > static_cast<std::size_t>(std::numeric_limits<unsigned long>::max())) {
        throw std::runtime_error("Image Error: JPEG input is empty or too large for the decoder.");
    }
}

[[nodiscard]] inline HeaderInfo headerInfo(const jpeg_decompress_struct& source) {
    const bool is_420 = source.num_components == 3 &&
        source.comp_info[0].h_samp_factor == 2 && source.comp_info[0].v_samp_factor == 2 &&
        source.comp_info[1].h_samp_factor == 1 && source.comp_info[1].v_samp_factor == 1 &&
        source.comp_info[2].h_samp_factor == 1 && source.comp_info[2].v_samp_factor == 1;
    return {
        source.image_width, source.image_height,
        source.num_components > 0 ? source.comp_info[0].width_in_blocks : 0,
        source.num_components > 0 ? source.comp_info[0].height_in_blocks : 0,
        source.num_components, source.progressive_mode != 0, is_420,
        source.jpeg_color_space == JCS_YCbCr,
    };
}

// The quantization table a component is bound to (its DQT slot), or null if
// the binding is out of range or the slot was never defined. Always resolve
// tables this way: slot numbers are the encoder's choice, not a convention.
[[nodiscard]] inline const JQUANT_TBL* componentQuantTable(
    const jpeg_decompress_struct& source, int component) {
    if (component < 0 || component >= source.num_components) return nullptr;
    const int table_number = source.comp_info[component].quant_tbl_no;
    if (table_number < 0 || table_number >= NUM_QUANT_TBLS) return nullptr;
    return source.quant_tbl_ptrs[table_number];
}

// Map format-specific header facts while the quantization tables are alive.
// The result is plain data; no destructor may be skipped by a codec longjmp.
template<class ReadInfo>
[[nodiscard]] auto inspectHeader(std::span<const std::uint8_t> input, ReadInfo read_info) {
    throwIfSignalCancellationRequested();
    requireMemorySource(input);
    const auto error = std::make_unique<ErrorManager>();
    const auto state = std::make_unique<CodecState>();
    install(*error);
    if (setjmp(error->jump) != 0) throwAfterLongjmp(*error, *state, "JPEG error: ");
    state->openSource(*error, input);
    jpeg_read_header(&state->source, TRUE);
    const auto info = read_info(state->source);
    static_assert(std::is_trivially_copyable_v<decltype(info)>);
    return info;
}

inline void configure420(jpeg_compress_struct& destination) {
    destination.comp_info[0].h_samp_factor = 2;
    destination.comp_info[0].v_samp_factor = 2;
    destination.comp_info[1].h_samp_factor = 1;
    destination.comp_info[1].v_samp_factor = 1;
    destination.comp_info[2].h_samp_factor = 1;
    destination.comp_info[2].v_samp_factor = 1;
}

// The caller owns row on the heap before its setjmp. This routine owns no
// destructible locals across libjpeg calls. A null destination just validates
// the decoded scanlines (the reduced-resolution capacity measurement path).
inline void scanRows(jpeg_decompress_struct& source, std::vector<JSAMPLE>& row,
                     jpeg_compress_struct* destination = nullptr) {
    const std::uint64_t row_size =
        static_cast<std::uint64_t>(source.output_width) *
        static_cast<std::uint64_t>(source.output_components);
    if (row_size > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("Image Error: Decoded JPEG row is too large.");
    }
    row.resize(static_cast<std::size_t>(row_size));
    while (source.output_scanline < source.output_height) {
        throwIfSignalCancellationRequested();
        JSAMPROW scanline = row.data();
        jpeg_read_scanlines(&source, &scanline, 1);
        if (destination != nullptr) jpeg_write_scanlines(destination, &scanline, 1);
    }
}

// Format callbacks validate the header and set quantization/markers/scan mode.
// They must not keep destructible local objects live across libjpeg calls.
// All mutable owners, including the scanline, precede this single setjmp.
template<class Validate, class Configure>
[[nodiscard]] std::vector<std::uint8_t> transcodeRgb420(
    std::span<const std::uint8_t> input, Validate validate, Configure configure,
    std::string_view error_prefix, J_DCT_METHOD dct_method = JDCT_DEFAULT) {
    throwIfSignalCancellationRequested();
    requireMemorySource(input);
    const auto error = std::make_unique<ErrorManager>();
    const auto state = std::make_unique<CodecState>();
    const auto row = std::make_unique<std::vector<JSAMPLE>>();
    install(*error);
    rejectWarnings(*error);
    if (setjmp(error->jump) != 0) throwAfterLongjmp(*error, *state, error_prefix);
    state->openSource(*error, input);
    jpeg_read_header(&state->source, TRUE);
    validate(state->source);
    state->source.out_color_space = JCS_RGB;
    state->source.dct_method = dct_method;
    jpeg_start_decompress(&state->source);
    state->openDestination(*error);
    state->destination.image_width = state->source.output_width;
    state->destination.image_height = state->source.output_height;
    state->destination.input_components = 3;
    state->destination.in_color_space = JCS_RGB;
    jpeg_set_defaults(&state->destination);
    configure420(state->destination);
    configure(state->source, state->destination);
    jpeg_start_compress(&state->destination, TRUE);
    scanRows(state->source, *row, &state->destination);
    jpeg_finish_compress(&state->destination);
    jpeg_finish_decompress(&state->source);
    return state->takeOutput();
}

} // namespace jpeg_codec
