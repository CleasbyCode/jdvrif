#include "jpeg_utils.h"
#include "reddit_steg.h"
#include "signal_utils.h"
#include "twitter_jpeg_codec.h"

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdlib>
#include <iostream>
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
}

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

vBytes makeProgressiveJpeg(bool grayscale = false) {
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
    row.fill(128);
    while (codec.next_scanline < codec.image_height) {
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
void expectCancellation(Function&& action) {
    const pid_t child = fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        installProcessSignalHandlers();
        cancel_at_decode = true;
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

        expectCancellation([&] { (void)codec::readCoefficients(image); });
        expectCancellation([&] { (void)codec::decodeLuminance(image); });
        expectCancellation([&] { (void)codec::prepareProgressiveSourceQuality(grayscale); });
        expectCancellation([&] { (void)codec::writeProgressiveCoefficients(image, coefficients.luminance); });
        expectCancellation([&] { (void)prepareRedditCover(image); });
        std::cout << "JPEG scan limits, cancellation, and exception cleanup tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
