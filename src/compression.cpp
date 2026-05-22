#include "compression.h"
#include "file_utils.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>

namespace {
[[nodiscard]] int selectCompressionLevel(std::size_t input_size) {
    constexpr std::size_t THRESHOLD_BEST_SPEED = 500 * 1024 * 1024;
    constexpr std::size_t THRESHOLD_DEFAULT    = 250 * 1024 * 1024;

    return (input_size > THRESHOLD_BEST_SPEED) ? Z_BEST_SPEED :
           (input_size > THRESHOLD_DEFAULT)    ? Z_DEFAULT_COMPRESSION :
                                                 Z_BEST_COMPRESSION;
}

inline constexpr std::size_t
    ZLIB_IN_CHUNK_SIZE  = 2 * 1024 * 1024,
    ZLIB_OUT_CHUNK_SIZE = 2 * 1024 * 1024;
constexpr const char* WRITE_COMPLETE_ERROR = "Write Error: Failed to write complete output file.";

template<typename WriteChunkFn>
void deflateFromInputStream(std::istream& input, std::size_t expected_input_size, WriteChunkFn&& write_chunk) {
    std::array<Byte, ZLIB_IN_CHUNK_SIZE> in_chunk;
    std::array<Byte, ZLIB_OUT_CHUNK_SIZE> out_chunk;

    z_stream strm{};
    if (deflateInit(&strm, selectCompressionLevel(expected_input_size)) != Z_OK) {
        throw std::runtime_error("zlib: deflateInit failed");
    }
    struct DeflateGuard {
        z_stream* stream;
        ~DeflateGuard() { deflateEnd(stream); }
    } guard{&strm};

    std::size_t input_left = expected_input_size;
    bool finished = false;
    while (!finished) {
        const std::size_t to_read = std::min(input_left, in_chunk.size());
        const std::streamsize read_count = readSomeOrThrow(
            input,
            in_chunk.data(),
            to_read,
            "Read Error: Failed while reading input file.");
        if (read_count != static_cast<std::streamsize>(to_read)) {
            throw std::runtime_error("Read Error: Input file changed while compressing.");
        }
        input_left -= to_read;

        strm.next_in  = in_chunk.data();
        strm.avail_in = static_cast<uInt>(read_count);

        const int flush = (input_left == 0) ? Z_FINISH : Z_NO_FLUSH;
        do {
            strm.next_out  = out_chunk.data();
            strm.avail_out = static_cast<uInt>(out_chunk.size());

            const int ret = deflate(&strm, flush);
            if (ret != Z_OK && ret != Z_STREAM_END) {
                throw std::runtime_error(std::format(
                    "zlib deflate error: {}",
                    strm.msg ? strm.msg : std::to_string(ret)));
            }

            const std::size_t produced = out_chunk.size() - strm.avail_out;
            if (produced > 0) {
                write_chunk(std::span<const Byte>(out_chunk.data(), produced));
            }

            if (ret == Z_STREAM_END) {
                finished = true;
                break;
            }
        } while (strm.avail_out == 0);
    }

    requireNoTrailingDataOrThrow(input, "Read Error: Input file changed while compressing.");
}

} // namespace

void zlibCompressFileToPath(const fs::path& input_path, const fs::path& output_path, std::size_t expected_input_size) {
    std::ifstream input = openBinaryInputOrThrow(
        input_path,
        std::format("Failed to open file for compression: {}", input_path.string()));
    std::ofstream output = openBinaryOutputForWriteOrThrow(output_path);
    deflateFromInputStream(input, expected_input_size, [&](std::span<const Byte> chunk) {
        writeBytesOrThrow(output, chunk, WRITE_COMPLETE_ERROR);
    });
    closeOutputOrThrow(output, WRITE_COMPLETE_ERROR);
}
