#pragma once

// libjpeg plumbing shared by the coefficient carriers (reddit_steg.cpp and
// twitter_jpeg_codec.cpp): the longjmp error manager with its progress
// monitors, and the codec-object owner both entry-point styles clean up through.
//
// Compiled into the C++ CLI, the web variant and the Rust port's FFI build, so
// keep it byte-identical across the three trees.

#include "jpeg_safety.h"

#include <array>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
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

inline void requireMemorySource(std::span<const std::uint8_t> input) {
    if (input.empty() ||
        input.size() > static_cast<std::size_t>(std::numeric_limits<unsigned long>::max())) {
        throw std::runtime_error("Image Error: JPEG input is empty or too large for the decoder.");
    }
}

} // namespace jpeg_codec
