#include "compression.h"
#include "file_utils.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {
[[nodiscard]] int selectCompressionLevel(std::size_t input_size) {
    constexpr std::size_t THRESHOLD_BEST_SPEED = 500 * 1024 * 1024, THRESHOLD_DEFAULT = 250 * 1024 * 1024;

    return (input_size > THRESHOLD_BEST_SPEED) ? Z_BEST_SPEED :
           (input_size > THRESHOLD_DEFAULT)    ? Z_DEFAULT_COMPRESSION :
                                                 Z_BEST_COMPRESSION;
}

inline constexpr std::size_t ZLIB_IN_CHUNK_SIZE = 2 * 1024 * 1024, ZLIB_OUT_CHUNK_SIZE = 2 * 1024 * 1024, ZLIB_MAX_INFLATED = 3ULL * 1024 * 1024 * 1024;
constexpr const char* WRITE_COMPLETE_ERROR = "Write Error: Failed to write complete output file.";

struct CompressionInputHints { std::size_t size_hint{0}; bool has_size{false}; };

[[nodiscard]] CompressionInputHints compressionInputHints(const fs::path& input_path) {
    std::error_code ec;
    const std::uintmax_t raw_input_size = fs::file_size(input_path, ec);

    CompressionInputHints hints{};
    if (!ec && raw_input_size <= static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        hints.size_hint = static_cast<std::size_t>(raw_input_size);
        hints.has_size = true;
    }
    return hints;
}

template<typename WriteChunkFn>
void deflateFromInputStream(std::istream& input, std::size_t input_size_hint, WriteChunkFn&& write_chunk) {
    std::array<Byte, ZLIB_IN_CHUNK_SIZE> in_chunk{};
    std::array<Byte, ZLIB_OUT_CHUNK_SIZE> out_chunk{};

    z_stream strm{};
    if (deflateInit(&strm, selectCompressionLevel(input_size_hint)) != Z_OK) throw std::runtime_error("zlib: deflateInit failed");
    struct DeflateGuard { z_stream* stream; ~DeflateGuard() { deflateEnd(stream); } } guard{&strm};

    bool finished = false;
    while (!finished) {
        input.read(reinterpret_cast<char*>(in_chunk.data()), static_cast<std::streamsize>(in_chunk.size()));
        const std::streamsize read_count = input.gcount();
        if (read_count < 0) throw std::runtime_error("Read Error: Failed while compressing input file.");
        if (!input && !input.eof()) throw std::runtime_error("Read Error: Failed while reading input file.");

        strm.next_in  = in_chunk.data();
        strm.avail_in = static_cast<uInt>(read_count);

        const int flush = input.eof() ? Z_FINISH : Z_NO_FLUSH;
        do {
            strm.next_out  = out_chunk.data();
            strm.avail_out = static_cast<uInt>(out_chunk.size());

            const int ret = deflate(&strm, flush);
            if (ret != Z_OK && ret != Z_STREAM_END) throw std::runtime_error(std::format("zlib deflate error: {}", strm.msg ? strm.msg : std::to_string(ret)));

            const std::size_t produced = out_chunk.size() - strm.avail_out;
            if (produced > 0) write_chunk(std::span<const Byte>(out_chunk.data(), produced));

            if (ret == Z_STREAM_END) {
                finished = true;
                break;
            }
        } while (strm.avail_out == 0);
    }
}

template<typename FeedInputFn, typename WriteChunkFn>
[[nodiscard]] std::size_t inflateWithInputFeed(
    std::size_t input_left,
    std::size_t output_limit,
    FeedInputFn&& feed_input,
    WriteChunkFn&& write_chunk) {

    std::array<Byte, ZLIB_OUT_CHUNK_SIZE> out_chunk{};
    std::size_t total_written = 0;

    z_stream strm{};
    if (inflateInit(&strm) != Z_OK) throw std::runtime_error("zlib: inflateInit failed");
    struct InflateGuard { z_stream* stream; ~InflateGuard() { inflateEnd(stream); } } guard{&strm};

    while (true) {
        feed_input(strm, input_left);

        strm.next_out = out_chunk.data();
        strm.avail_out = static_cast<uInt>(out_chunk.size());

        const int ret = inflate(&strm, input_left > 0 ? Z_NO_FLUSH : Z_FINISH);

        const std::size_t produced = out_chunk.size() - strm.avail_out;
        if (produced > 0) {
            if (produced > output_limit || total_written > output_limit - produced) throw std::runtime_error("zlib inflate error: output exceeds safe size limit");
            write_chunk(std::span<const Byte>(out_chunk.data(), produced));
            total_written += produced;
        }

        if (ret == Z_STREAM_END) {
            if (strm.avail_in != 0 || input_left != 0) throw std::runtime_error("zlib inflate error: trailing compressed data");
            break;
        }
        if (ret == Z_BUF_ERROR) {
            if (strm.avail_out == 0) continue;
            if (strm.avail_in == 0 && input_left > 0) continue;
            throw std::runtime_error("zlib inflate error: truncated or corrupt input");
        }
        if (ret != Z_OK) throw std::runtime_error(std::format("zlib inflate error: {}", strm.msg ? strm.msg : std::to_string(ret)));
    }

    return total_written;
}

}

void zlibCompressFileToPath(const fs::path& input_path, const fs::path& output_path) {
    std::ifstream input = openBinaryInputOrThrow(input_path, std::format("Failed to open file for compression: {}", input_path.string()));
    std::ofstream output = openBinaryOutputForWriteOrThrow(output_path);
    const CompressionInputHints hints = compressionInputHints(input_path);
    deflateFromInputStream(input, hints.size_hint, [&](std::span<const Byte> chunk) { writeBytesOrThrow(output, chunk, WRITE_COMPLETE_ERROR); });
    flushOutputOrThrow(output, WRITE_COMPLETE_ERROR);
}

void zlibInflateToFile(std::span<const Byte> compressed_data, const fs::path& output_path) {
    std::ofstream output = openBinaryOutputForWriteOrThrow(output_path);

    std::size_t input_left = compressed_data.size();
    const Byte* cursor = compressed_data.data();
    const std::size_t total_written = inflateWithInputFeed(
        input_left,
        ZLIB_MAX_INFLATED,
        [&](z_stream& strm, std::size_t& left) {
            if (strm.avail_in == 0 && left > 0) {
                const std::size_t chunk = std::min(left, static_cast<std::size_t>(std::numeric_limits<uInt>::max()));
                strm.next_in = const_cast<Byte*>(cursor);
                strm.avail_in = static_cast<uInt>(chunk);
                cursor += chunk;
                left -= chunk;
            }
        },
        [&](std::span<const Byte> chunk) { writeBytesOrThrow(output, chunk, WRITE_COMPLETE_ERROR); });
    flushOutputOrThrow(output, WRITE_COMPLETE_ERROR);

    if (total_written == 0) throw std::runtime_error("Zlib Compression Error: Output file is empty. Inflating file failed.");
}
