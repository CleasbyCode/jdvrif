#include "jpeg_codec_common.h"
#include "jpeg_utils.h"
#include "reddit_steg.h"
#include "signal_utils.h"
#include "twitter_jpeg_codec.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
#include <jpeglib.h>
}

namespace {
int live_codecs = 0;
bool cancel_at_decode = false;
// When set, report SIGINT to the libjpeg progress monitor as soon as the
// active encoder's write position has left the buffer it started in, i.e.
// once the destination has grown its output buffer.
bool cancel_after_growth = false;
j_compress_ptr active_encoder = nullptr;
std::uintptr_t first_buffer_begin = 0;
std::uintptr_t first_buffer_end = 0;

void noteEncoderStart(j_compress_ptr codec) {
    active_encoder = codec;
    first_buffer_begin = reinterpret_cast<std::uintptr_t>(codec->dest->next_output_byte);
    first_buffer_end = first_buffer_begin + codec->dest->free_in_buffer;
}

bool outputGrew() {
    if (active_encoder == nullptr) return false;
    const auto position = reinterpret_cast<std::uintptr_t>(active_encoder->dest->next_output_byte);
    return position < first_buffer_begin || position > first_buffer_end;
}
}

// Observe ownership without allocator-dependent heap-size heuristics. Raising
// SIGINT at the C decode boundary deterministically exercises cancellation
// after the entry point's ordinary C++ cancellation check.
extern "C" {
void __real_jpeg_CreateDecompress(j_decompress_ptr, int, std::size_t);
void __real_jpeg_CreateCompress(j_compress_ptr, int, std::size_t);
void __real_jpeg_destroy_decompress(j_decompress_ptr);
void __real_jpeg_destroy_compress(j_compress_ptr);
jvirt_barray_ptr* __real_jpeg_read_coefficients(j_decompress_ptr);
boolean __real_jpeg_start_decompress(j_decompress_ptr);
void __real_jpeg_start_compress(j_compress_ptr, boolean);
void __real_jpeg_write_coefficients(j_compress_ptr, jvirt_barray_ptr*);
int __real__Z25pendingSignalCancellationv() noexcept;

void __wrap_jpeg_CreateDecompress(j_decompress_ptr codec, int version, std::size_t size) {
    ++live_codecs;
    __real_jpeg_CreateDecompress(codec, version, size);
}
void __wrap_jpeg_CreateCompress(j_compress_ptr codec, int version, std::size_t size) {
    ++live_codecs;
    __real_jpeg_CreateCompress(codec, version, size);
}
void __wrap_jpeg_destroy_decompress(j_decompress_ptr codec) {
    --live_codecs;
    __real_jpeg_destroy_decompress(codec);
}
void __wrap_jpeg_destroy_compress(j_compress_ptr codec) {
    --live_codecs;
    if (codec == active_encoder) active_encoder = nullptr;
    __real_jpeg_destroy_compress(codec);
}
jvirt_barray_ptr* __wrap_jpeg_read_coefficients(j_decompress_ptr codec) {
    if (cancel_at_decode) std::raise(SIGINT);
    return __real_jpeg_read_coefficients(codec);
}
boolean __wrap_jpeg_start_decompress(j_decompress_ptr codec) {
    if (cancel_at_decode) std::raise(SIGINT);
    return __real_jpeg_start_decompress(codec);
}
void __wrap_jpeg_start_compress(j_compress_ptr codec, boolean write_all_tables) {
    __real_jpeg_start_compress(codec, write_all_tables);
    noteEncoderStart(codec);
}
void __wrap_jpeg_write_coefficients(j_compress_ptr codec, jvirt_barray_ptr* arrays) {
    __real_jpeg_write_coefficients(codec, arrays);
    noteEncoderStart(codec);
}
// pendingSignalCancellation(), as called by the libjpeg progress monitor.
int __wrap__Z25pendingSignalCancellationv() noexcept {
    if (cancel_after_growth && outputGrew()) return SIGINT;
    return __real__Z25pendingSignalCancellationv();
}
}

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

// Noise rows make the encoder outgrow its first output buffer within a few
// scanlines; flat rows would compress to almost nothing.
void fillRow(std::span<JSAMPLE> row, bool noise, std::uint32_t& seed) {
    for (JSAMPLE& sample : row) {
        seed = seed * 1664525U + 1013904223U;
        sample = noise ? static_cast<JSAMPLE>(seed >> 24) : JSAMPLE{128};
    }
}

vBytes makeProgressiveJpeg(bool grayscale = false, bool noise = false) {
    jpeg_compress_struct codec{};
    jpeg_error_mgr error{};
    codec.err = jpeg_std_error(&error);
    jpeg_create_compress(&codec);
    unsigned char* buffer = nullptr;
    unsigned long size = 0;
    jpeg_mem_dest(&codec, &buffer, &size);
    codec.image_width = 512;
    codec.image_height = 512;
    codec.input_components = grayscale ? 1 : 3;
    codec.in_color_space = grayscale ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_set_defaults(&codec);
    jpeg_set_quality(&codec, 75, TRUE);
    jpeg_simple_progression(&codec);
    jpeg_start_compress(&codec, TRUE);
    std::array<JSAMPLE, 1536> row{};
    std::uint32_t seed = 1;
    while (codec.next_scanline < codec.image_height) {
        fillRow(row, noise, seed);
        JSAMPROW pointer = row.data();
        jpeg_write_scanlines(&codec, &pointer, 1);
    }
    jpeg_finish_compress(&codec);
    vBytes result(buffer, buffer + size);
    std::free(buffer);
    jpeg_destroy_compress(&codec);
    return result;
}

vBytes repeatLastScan(std::span<const Byte> jpeg) {
    std::size_t last_scan = 0;
    for (std::size_t index = 0; index + 1 < jpeg.size(); ++index) {
        if (jpeg[index] == 0xff && jpeg[index + 1] == 0xda) last_scan = index;
    }
    require(last_scan != 0 && jpeg[jpeg.size() - 1] == 0xd9, "bad test JPEG");
    vBytes result(jpeg.begin(), jpeg.end() - 2);
    for (int repetition = 0; repetition < 600; ++repetition) {
        result.insert(result.end(), jpeg.begin() + static_cast<std::ptrdiff_t>(last_scan), jpeg.end() - 2);
    }
    result.insert(result.end(), {0xff, 0xd9});
    return result;
}

template<class Function>
void expectRejection(Function&& action, std::string_view reason) {
    bool rejected = false;
    try {
        action();
    } catch (const std::runtime_error& error) {
        rejected = std::string_view(error.what()).find(reason) != std::string_view::npos;
        if (!rejected) std::cerr << "Unexpected rejection: " << error.what() << '\n';
    }
    require(rejected, "expected JPEG rejection was absent");
    require(live_codecs == 0, "JPEG codec leaked after rejection");
}

template<class Function>
void expectCancellation(Function&& action, bool after_output_growth = false) {
    const pid_t child = fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        installProcessSignalHandlers();
        cancel_at_decode = !after_output_growth;
        cancel_after_growth = after_output_growth;
        try {
            action();
        } catch (const SignalCancellation& cancellation) {
            std::_Exit(cancellation.signalNumber() == SIGINT && live_codecs == 0 ? 0 : 3);
        } catch (...) {
            std::_Exit(4);
        }
        std::_Exit(5);
    }
    int status = 0;
    require(waitpid(child, &status, 0) == child, "waitpid failed");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "decode cancellation failed or leaked a codec");
}

// An encoder error after the output buffer has grown must release the
// current buffer exactly once. Starting a second compression mid-image is a
// libjpeg state error, raised through error_exit like any encoder failure.
void checkEncoderErrorAfterGrowth() {
    auto error = std::make_unique<jpeg_codec::ErrorManager>();
    auto state = std::make_unique<jpeg_codec::CodecState>();
    auto failed = std::make_unique<bool>(false);
    jpeg_codec::install(*error);
    if (setjmp(error->jump) != 0) {
        state->cleanup();
        *failed = true;
    } else {
        state->openDestination(*error);
        state->destination.image_width = 512;
        state->destination.image_height = 512;
        state->destination.input_components = 3;
        state->destination.in_color_space = JCS_RGB;
        jpeg_set_defaults(&state->destination);
        jpeg_start_compress(&state->destination, TRUE);
        std::array<JSAMPLE, 1536> row{};
        std::uint32_t seed = 7;
        for (int line = 0; line < 256; ++line) {
            fillRow(row, true, seed);
            JSAMPROW pointer = row.data();
            jpeg_write_scanlines(&state->destination, &pointer, 1);
        }
        require(outputGrew(), "test image did not grow the encoder's output buffer");
        jpeg_start_compress(&state->destination, TRUE);
    }
    require(*failed, "encoder state error was not reported");
    require(live_codecs == 0, "JPEG codec leaked after an encoder error");
}
}

int main() {
    try {
        namespace codec = twitter_steg_internal;
        const vBytes image = makeProgressiveJpeg();
        const vBytes grayscale = makeProgressiveJpeg(true);
        const auto coefficients = codec::readCoefficients(image);
        require(codec::decodeLuminance(image).size() == 512U * 512U, "normal progressive decode failed");
        (void)codec::prepareProgressiveSourceQuality(grayscale);
        (void)prepareRedditCover(image);
        (void)optimizeImage(image, true, false);
        require(live_codecs == 0, "normal JPEG processing leaked a codec");

        const vBytes excessive_scans = repeatLastScan(image);
        expectRejection([&] { (void)codec::readCoefficients(excessive_scans); }, "scan");
        expectRejection([&] { (void)codec::decodeLuminance(excessive_scans); }, "scan");
        expectRejection([&] { (void)codec::prepareProgressiveSourceQuality(excessive_scans); }, "scan");
        expectRejection([&] { (void)codec::prepareProgressiveSourceQuality(repeatLastScan(grayscale)); }, "scan");
        expectRejection([&] { (void)codec::writeProgressiveCoefficients(excessive_scans, coefficients.luminance); }, "scan");
        expectRejection([&] { (void)prepareRedditCover(excessive_scans); }, "scan");
        expectRejection([&] { (void)optimizeImage(excessive_scans, true, false); }, "scan");

        vBytes oversize = image;
        bool found_frame = false;
        for (std::size_t index = 0; index + 8 < oversize.size(); ++index) {
            if (oversize[index] == 0xff && oversize[index + 1] == 0xc2) {
                oversize[index + 7] = 0x10;
                oversize[index + 8] = 0x01; // 4097 pixels, rejected after allocation.
                found_frame = true;
                break;
            }
        }
        require(found_frame, "test JPEG has no progressive frame");
        for (int attempt = 0; attempt < 100; ++attempt) {
            expectRejection([&] { (void)codec::inspectJpeg(oversize); }, "dimensions");
            expectRejection([&] { (void)codec::decodeLuminance(oversize); }, "dimensions");
            expectRejection([&] { (void)codec::readCoefficients(oversize); }, "dimensions");
            expectRejection([&] { (void)codec::writeProgressiveCoefficients(image, {}); }, "coefficient-count mismatch");
        }

        // A fake 400x400 SOF placed after SOI must not stand in for the real
        // frame when the size limits are checked. libjpeg skips the bare one
        // as junk and decodes the real frame, here widened past the 4096 cap.
        constexpr std::array<Byte, 18> fake_sof{
            0xc0, 0x00, 0x11, 0x08, 0x01, 0x90, 0x01, 0x90, 0x03,
            0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01};
        vBytes wide = oversize;
        for (std::size_t index = 0; index + 8 < wide.size(); ++index) {
            if (wide[index] == 0xff && wide[index + 1] == 0xc2) {
                wide[index + 7] = 0x13;
                wide[index + 8] = 0x88; // 5000 pixels wide.
                break;
            }
        }
        expectRejection([&] { (void)optimizeImage(wide, true, false); }, "exceed");
        vBytes bare_fake = wide;
        bare_fake.insert(bare_fake.begin() + 2, fake_sof.begin(), fake_sof.end());
        expectRejection([&] { (void)optimizeImage(bare_fake, true, false); }, "header");
        vBytes framed_fake = image;
        framed_fake.insert(framed_fake.begin() + 2, fake_sof.begin(), fake_sof.end());
        framed_fake.insert(framed_fake.begin() + 2, Byte{0xff});
        expectRejection([&] { (void)optimizeImage(framed_fake, true, false); }, "header");

        expectCancellation([&] { (void)codec::readCoefficients(image); });
        expectCancellation([&] { (void)codec::decodeLuminance(image); });
        expectCancellation([&] { (void)codec::prepareProgressiveSourceQuality(grayscale); });
        expectCancellation([&] { (void)codec::writeProgressiveCoefficients(image, coefficients.luminance); });
        expectCancellation([&] { (void)prepareRedditCover(image); });

        // Cancel each encoder after its output buffer has grown: the baseline
        // Reddit transcode, and the Twitter pixel and coefficient writers,
        // which emit everything inside jpeg_finish_compress.
        const vBytes noisy = makeProgressiveJpeg(false, true);
        const vBytes noisy_grayscale = makeProgressiveJpeg(true, true);
        const auto noisy_coefficients = codec::readCoefficients(noisy);
        expectCancellation([&] { (void)prepareRedditCover(noisy); }, true);
        expectCancellation([&] { (void)codec::prepareProgressiveSourceQuality(noisy_grayscale); }, true);
        expectCancellation([&] { (void)codec::writeProgressiveCoefficients(noisy, noisy_coefficients.luminance); }, true);
        checkEncoderErrorAfterGrowth();
        std::cout << "JPEG scan limits, cancellation, and exception cleanup tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
