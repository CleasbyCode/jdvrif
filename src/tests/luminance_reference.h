#pragma once

// Test support: libjpeg's own decode of a JPEG's luminance, kept out of the
// production codec. Production derives the luminance pixels from coefficients
// it already holds (luminanceFromCoefficients); this independent decode is
// what those pixels are checked against, and it also exercises malformed-input
// handling (scan limits, oversized dimensions, cancellation) through the same
// codec plumbing production uses.

#include "jpeg_codec_common.h"
#include "signal_utils.h"
#include "twitter_jpeg_codec.h"

#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace twitter_steg_internal {

// The 8-bit luminance samples libjpeg decodes with its accurate integer
// inverse DCT (JDCT_ISLOW), row by row. Refuses images over the X-Twitter
// dimension limit, as the production entry points do.
[[nodiscard]] inline std::vector<std::uint8_t> decodeLuminance(std::span<const std::uint8_t> jpeg) {
    throwIfSignalCancellationRequested();
    (void)inspectJpeg(jpeg);
    jpeg_codec::requireMemorySource(jpeg);
    auto error = std::make_unique<jpeg_codec::ErrorManager>();
    auto state = std::make_unique<jpeg_codec::CodecState>();
    // On the heap, allocated before setjmp, like every codec entry point's
    // locals: a longjmp must not skip a destructor in this frame.
    auto pixels = std::make_unique<std::vector<std::uint8_t>>();
    jpeg_codec::install(*error);
    if (setjmp(error->jump) != 0) {
        jpeg_codec::throwAfterLongjmp(*error, *state, "JPEG error while decoding luminance: ");
    }

    state->openSource(*error, jpeg);
    jpeg_read_header(&state->source, TRUE);
    state->source.out_color_space = JCS_GRAYSCALE;
    state->source.dct_method = JDCT_ISLOW;
    jpeg_start_decompress(&state->source);
    pixels->resize(static_cast<std::size_t>(state->source.output_width) * state->source.output_height);
    while (state->source.output_scanline < state->source.output_height) {
        JSAMPROW output_row = pixels->data() +
            static_cast<std::size_t>(state->source.output_scanline) * state->source.output_width;
        jpeg_read_scanlines(&state->source, &output_row, 1);
    }
    jpeg_finish_decompress(&state->source);
    state->cleanup();
    return std::move(*pixels);
}

} // namespace twitter_steg_internal
