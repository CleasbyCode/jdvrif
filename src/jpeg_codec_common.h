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
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <jpeglib.h>
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

// Owns the libjpeg codec objects and the jpeg_mem_dest buffer. Also
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
    unsigned char* output_buffer{nullptr};
    unsigned long output_size{0};
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
        std::free(output_buffer);
        output_buffer = nullptr;
        output_size = 0;
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

    // Create the compressor writing into output_buffer.
    void openDestination(ErrorManager& error) {
        destination.err = &error.pub;
        destination_created = true;
        jpeg_create_compress(&destination);
        destination.progress = &error.destination_progress.pub;
        jpeg_mem_dest(&destination, &output_buffer, &output_size);
    }

    // Copy the encoder's output out and release its buffer.
    [[nodiscard]] std::vector<std::uint8_t> takeOutput() {
        if (output_buffer == nullptr || output_size == 0) {
            throw std::runtime_error("Image Error: JPEG encoder produced no output.");
        }
        std::vector<std::uint8_t> result(output_buffer, output_buffer + output_size);
        std::free(output_buffer);
        output_buffer = nullptr;
        output_size = 0;
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
