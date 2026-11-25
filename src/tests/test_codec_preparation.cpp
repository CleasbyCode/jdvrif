#include "reddit_steg.h"
#include "twitter_steg.h"
#include "twitter_jpeg_codec.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

extern "C" {
#include <jpeglib.h>
}

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

vBytes makeJpeg(int layout, int quality, bool progressive) {
    jpeg_compress_struct codec{};
    jpeg_error_mgr error{};
    codec.err = jpeg_std_error(&error);
    jpeg_create_compress(&codec);
    unsigned char* buffer = nullptr;
    unsigned long size = 0;
    jpeg_mem_dest(&codec, &buffer, &size);
    codec.image_width = 513;
    codec.image_height = 529;
    codec.input_components = layout == 0 ? 1 : 3;
    codec.in_color_space = layout == 0 ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_set_defaults(&codec);
    if (layout != 0) {
        codec.comp_info[0].h_samp_factor = layout == 1 ? 1 : 2;
        codec.comp_info[0].v_samp_factor = layout == 3 ? 2 : 1;
    }
    jpeg_set_quality(&codec, quality, TRUE);
    if (progressive) jpeg_simple_progression(&codec);
    jpeg_start_compress(&codec, TRUE);
    std::array<JSAMPLE, 513 * 3> row{};
    std::uint32_t seed = 1;
    while (codec.next_scanline < codec.image_height) {
        for (auto& value : row) {
            seed = seed * 1664525U + 1013904223U;
            value = static_cast<JSAMPLE>(seed >> 24);
        }
        JSAMPROW scanline = row.data();
        jpeg_write_scanlines(&codec, &scanline, 1);
    }
    jpeg_finish_compress(&codec);
    vBytes bytes(buffer, buffer + size);
    std::free(buffer);
    jpeg_destroy_compress(&codec);
    return bytes;
}

std::uint64_t hash(std::span<const Byte> bytes) {
    std::uint64_t value = 0xcbf29ce484222325ULL;
    for (Byte byte : bytes) value = (value ^ byte) * 0x100000001b3ULL;
    return value;
}

template<class Work>
void rejects(Work work) {
    bool rejected = false;
    try { work(); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "truncated cover accepted");
}
}

int main(int argc, char** argv) {
    try {
        const bool reference = argc > 1 && std::string_view(argv[1]) == "--reference";
        for (int layout : {0, 1, 2, 3}) {
            for (int quality : {75, 97, 100}) {
                for (bool progressive : {false, true}) {
                    const vBytes input = makeJpeg(layout, quality, progressive);
                    const auto borrowed = prepareTwitterCover(input);
                    vBytes owned_input = input;
                    const Byte* allocation = owned_input.data();
                    const auto owned = prepareTwitterCover(std::move(owned_input));
                    const bool fast = layout == 3 && quality <= 97;
                    if (!reference && fast) {
                        require(owned.jpeg.data() == allocation, "fast path copied the JPEG allocation");
                        require(owned_input.empty(), "fast path did not transfer ownership");
                    }
                    require(owned.jpeg == borrowed.jpeg, "owned/borrowed prepared JPEG mismatch");
                    require(owned.coefficients.luminance == borrowed.coefficients.luminance,
                            "owned/borrowed coefficient mismatch");
                    require(owned.payload_capacity == borrowed.payload_capacity, "capacity changed");
                    const auto reddit = prepareRedditCover(input);
                    const auto measured = measureRedditCover(input);
                    require(measured.payload_capacity == reddit.payload_capacity &&
                                measured.width == reddit.width && measured.height == reddit.height,
                            "Reddit measurement differs from transcode");
                    const auto info = twitter_steg_internal::inspectJpeg(reddit.jpeg);
                    require(!info.progressive && info.is_420 && info.estimated_quality == 75,
                            "Reddit carrier profile changed");
                    const auto twitter_info = twitter_steg_internal::inspectJpeg(owned.jpeg);
                    require(twitter_info.is_420 && (fast || twitter_info.progressive),
                            "Twitter carrier profile changed");
                    std::cout << layout << ' ' << quality << ' ' << progressive << ' '
                              << hash(owned.jpeg) << ' ' << hash(reddit.jpeg) << ' '
                              << owned.payload_capacity << ' ' << reddit.payload_capacity << '\n';
                    vBytes damaged = input;
                    damaged.resize(damaged.size() - 2);
                    rejects([&] { (void)prepareTwitterCover(damaged); });
                    rejects([&] { (void)prepareTwitterCover(vBytes(damaged)); });
                    rejects([&] { (void)prepareRedditCover(damaged); });
                    rejects([&] { (void)measureRedditCover(damaged); });
                }
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
