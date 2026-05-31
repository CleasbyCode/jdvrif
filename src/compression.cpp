#include "compression.h"
#include "file_utils.h"

#include <libdeflate.h>
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

// Files at or below this size are compressed in a single whole-buffer
// libdeflate call. Peak RSS for that path is roughly input + compress_bound
// (~2x input), so the cap keeps the fast path's memory bounded. Larger inputs
// fall back to the zlib streaming deflate below, which holds only fixed chunks.
inline constexpr std::size_t LIBDEFLATE_WHOLE_BUFFER_LIMIT = 256 * 1024 * 1024;

// --------------------------- libdeflate fast path ---------------------------

// Map the zlib level the existing heuristic picks onto a libdeflate level.
// Measured (bench/compress_levels) on real corpora, libdeflate L9 costs ~2.6-2.9x
// the time of L6 for only ~1-2% smaller output; L10-12 are far worse (an "ultra"
// tier with marginal gains). L6 is the ratio/time sweet spot, so the
// "best compression" tier maps to 6 — compression is the dominant cost in a
// conceal, and a 1-2% larger payload practically never changes platform fit.
// Do NOT raise this back toward 9-12 without re-running the level sweep.
[[nodiscard]] int libdeflateLevelFor(std::size_t input_size) {
    switch (selectCompressionLevel(input_size)) {
        case Z_BEST_SPEED:          return 1;
        case Z_DEFAULT_COMPRESSION: return 6;
        case Z_BEST_COMPRESSION:    return 6;
        default:                    return 6;
    }
}

struct CompressorGuard {
    libdeflate_compressor* c{nullptr};
    explicit CompressorGuard(int level) : c(libdeflate_alloc_compressor(level)) {}
    ~CompressorGuard() { if (c) libdeflate_free_compressor(c); }
    CompressorGuard(const CompressorGuard&) = delete;
    CompressorGuard& operator=(const CompressorGuard&) = delete;
};

[[nodiscard]] vBytes readWholeFile(const fs::path& path, std::size_t expected_size) {
    std::ifstream input = openBinaryInputOrThrow(
        path,
        std::format("Failed to open file for compression: {}", path.string()));
    vBytes buffer(expected_size);
    if (expected_size > 0) {
        readExactOrThrow(input, buffer.data(), buffer.size(),
                         "Read Error: Failed while reading input file.");
    }
    requireNoTrailingDataOrThrow(input, "Read Error: Input file changed while compressing.");
    return buffer;
}

void libdeflateCompressFileToPath(const fs::path& input_path, const fs::path& output_path, std::size_t expected_input_size) {
    const vBytes input = readWholeFile(input_path, expected_input_size);

    CompressorGuard compressor(libdeflateLevelFor(expected_input_size));
    if (!compressor.c) {
        throw std::runtime_error("libdeflate: failed to allocate compressor");
    }

    const std::size_t bound = libdeflate_zlib_compress_bound(compressor.c, input.size());
    vBytes output(bound);

    const std::size_t produced = libdeflate_zlib_compress(
        compressor.c,
        input.data(),
        input.size(),
        output.data(),
        output.size());
    if (produced == 0) {
        // Only happens if the bound-sized buffer was somehow insufficient.
        throw std::runtime_error("libdeflate: zlib compression failed");
    }

    std::ofstream out_file = openBinaryOutputForWriteOrThrow(output_path);
    writeBytesOrThrow(out_file, std::span<const Byte>(output.data(), produced), WRITE_COMPLETE_ERROR);
    closeOutputOrThrow(out_file, WRITE_COMPLETE_ERROR);
}

// ------------------------- zlib streaming fallback --------------------------

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

void zlibStreamCompressFileToPath(const fs::path& input_path, const fs::path& output_path, std::size_t expected_input_size) {
    std::ifstream input = openBinaryInputOrThrow(
        input_path,
        std::format("Failed to open file for compression: {}", input_path.string()));
    std::ofstream output = openBinaryOutputForWriteOrThrow(output_path);
    deflateFromInputStream(input, expected_input_size, [&](std::span<const Byte> chunk) {
        writeBytesOrThrow(output, chunk, WRITE_COMPLETE_ERROR);
    });
    closeOutputOrThrow(output, WRITE_COMPLETE_ERROR);
}

} // namespace

void zlibCompressFileToPath(const fs::path& input_path, const fs::path& output_path, std::size_t expected_input_size) {
    // Both paths emit a standard RFC 1950 zlib stream, so the recover-side
    // zlib inflate decodes either one. libdeflate handles the common (smaller)
    // case far faster; the streaming zlib path keeps memory bounded for very
    // large inputs.
    if (expected_input_size <= LIBDEFLATE_WHOLE_BUFFER_LIMIT) {
        libdeflateCompressFileToPath(input_path, output_path, expected_input_size);
    } else {
        zlibStreamCompressFileToPath(input_path, output_path, expected_input_size);
    }
}
