#include "compression.h"

#include <algorithm>
#include <format>
#include <limits>
#include <stdexcept>
#include <string>

void zlibFunc(vBytes& data_vec, Mode mode) {
    constexpr std::size_t BUFSIZE = 2 * 1024 * 1024;

    vBytes buffer_vec(BUFSIZE);
    vBytes output_vec;

    const std::size_t input_size = data_vec.size();
    output_vec.reserve(input_size + BUFSIZE);

    z_stream strm{};
    strm.next_in   = data_vec.data();
    strm.next_out  = buffer_vec.data();
    strm.avail_out = static_cast<uInt>(BUFSIZE);
    strm.avail_in  = 0;

    std::size_t input_left = input_size;

    // RAII cleanup for the zlib stream.
    auto stream_guard = [&](auto end_fn) {
        struct Guard {
            z_stream* s;
            decltype(end_fn) fn;
            ~Guard() { fn(s); }
        };
        return Guard{&strm, end_fn};
    };

    auto flush_buffer = [&]() {
        const std::size_t written = BUFSIZE - strm.avail_out;
        if (written > 0) {
            output_vec.insert(output_vec.end(), buffer_vec.begin(), buffer_vec.begin() + written);
            strm.next_out  = buffer_vec.data();
            strm.avail_out = static_cast<uInt>(BUFSIZE);
        }
    };

    auto feed_input = [&]() {
        if (strm.avail_in == 0 && input_left > 0) {
            const std::size_t chunk = std::min(input_left, static_cast<std::size_t>(std::numeric_limits<uInt>::max()));
            strm.avail_in = static_cast<uInt>(chunk);
            input_left -= chunk;
        }
    };

    if (mode == Mode::conceal) {
        constexpr std::size_t
            THRESHOLD_BEST_SPEED = 500 * 1024 * 1024,
            THRESHOLD_DEFAULT    = 250 * 1024 * 1024;

        const int compression_level =
            (input_size > THRESHOLD_BEST_SPEED) ? Z_BEST_SPEED :
            (input_size > THRESHOLD_DEFAULT)    ? Z_DEFAULT_COMPRESSION :
                                                  Z_BEST_COMPRESSION;

        if (deflateInit(&strm, compression_level) != Z_OK) {
            throw std::runtime_error("zlib: deflateInit failed");
        }
        auto guard = stream_guard([](z_stream* s) { deflateEnd(s); });

        int ret;
        do {
            feed_input();
            ret = deflate(&strm, input_left > 0 ? Z_NO_FLUSH : Z_FINISH);
            if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
                throw std::runtime_error(std::format("zlib deflate error: {}", strm.msg ? strm.msg : std::to_string(ret)));
            }
            if (strm.avail_out == 0) {
                flush_buffer();
            }
        } while (ret != Z_STREAM_END);
        flush_buffer();

    } else {
        if (inflateInit(&strm) != Z_OK) {
            throw std::runtime_error("zlib: inflateInit failed");
        }
        auto guard = stream_guard([](z_stream* s) { inflateEnd(s); });

        int ret;
        while (true) {
            feed_input();
            if (strm.avail_out == 0) {
                flush_buffer();
            }
            ret = inflate(&strm, input_left > 0 ? Z_NO_FLUSH : Z_FINISH);
            if (ret == Z_STREAM_END) {
                flush_buffer();
                break;
            }
            if (ret == Z_BUF_ERROR) {
                if (strm.avail_out == 0) flush_buffer();
                continue;
            }
            if (ret != Z_OK) {
                throw std::runtime_error(std::format("zlib inflate error: {}", strm.msg ? strm.msg : std::to_string(ret)));
            }
        }
    }
    data_vec = std::move(output_vec);
}
