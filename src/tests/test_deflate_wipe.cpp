// Include the implementation to exercise its private streaming deflater.
// zlib's deflate state (window, hash chains, pending output) holds plaintext
// and must come back through the wiping zfree, zeroed.
#include "compression.cpp"  // found via -I<source tree>
#include "tests/zlib_alloc_observer.h"

#include <cstdio>
#include <fstream>
#include <iterator>

namespace {
[[nodiscard]] std::vector<Byte> readAll(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

// libdeflate's compressor state (match-finder tables, sequence buffers) is
// built from the plaintext: both libdeflate users must allocate it through the
// wiping callbacks, so it is observed here and comes back zeroed. Through
// libdeflate's default allocator its malloc/free calls come from libdeflate
// itself, are not wrapped, and nothing is observed.
[[nodiscard]] bool checkLibdeflateWipe() {
    std::string plaintext;
    for (std::size_t line = 0; plaintext.size() < 5 * 1024 * 1024; ++line) {
        plaintext += std::format("libdeflate wipe test line {} value {}\n", line, line * 40503U);
    }
    const fs::path path = "libdeflate_wipe_input.bin";
    {
        std::ofstream file(path, std::ios::binary);
        file.write(plaintext.data(), static_cast<std::streamsize>(plaintext.size()));
    }
    struct Remove {
        const fs::path& path;
        ~Remove() { std::remove(path.c_str()); }
    } remove{path};
    const auto observed_wiped = [](const char* what) {
        // A level-6 compressor's state is several hundred KiB in one block.
        if (zlib_alloc_observer::largest_block < 64 * 1024 || zlib_alloc_observer::found_plaintext) {
            std::fprintf(stderr, "%s released libdeflate state unobserved or unwiped\n", what);
            return false;
        }
        return true;
    };

    StagingFile out({}, "z");
    zlib_alloc_observer::reset();
    zlib_alloc_observer::watching = true;
    const bool completed = libdeflateCompressFileToPath(
        path, out, plaintext.size(), UnshrunkOutput::discard);
    zlib_alloc_observer::watching = false;
    if (!completed) {
        std::fputs("libdeflate discarded a stream that shrank\n", stderr);
        return false;
    }
    if (!observed_wiped("whole-buffer libdeflate")) return false;
    const std::vector<Byte> stream = readAll(out.path());
    std::vector<Byte> inflated(plaintext.size());
    uLongf inflated_size = static_cast<uLongf>(inflated.size());
    if (uncompress(inflated.data(), &inflated_size, stream.data(), static_cast<uLong>(stream.size())) != Z_OK ||
        inflated_size != plaintext.size() ||
        !std::equal(inflated.begin(), inflated.end(), plaintext.begin())) {
        std::fputs("libdeflate stream does not inflate back to the input\n", stderr);
        return false;
    }

    zlib_alloc_observer::reset();
    zlib_alloc_observer::watching = true;
    const bool incompressible = sampledDataLooksIncompressible(path, plaintext.size());
    zlib_alloc_observer::watching = false;
    if (incompressible) {
        std::fputs("sampling judged repetitive text incompressible\n", stderr);
        return false;
    }
    return observed_wiped("compressibility sampling");
}

// The sliced parallel deflater: its zlib state must be wiped like the
// streaming one's, and its stream must inflate back to the input and must not
// depend on how many threads built it.
[[nodiscard]] bool checkParallelDeflate() {
    // Three and a bit slices of text with a little structure, so slices are
    // primed from their predecessors and the last one is short.
    std::string plaintext;
    plaintext.reserve(3 * PARALLEL_SLICE_SIZE + 4096);
    for (std::size_t line = 0; plaintext.size() < 3 * PARALLEL_SLICE_SIZE + 1000; ++line) {
        plaintext += std::format("record {} of the parallel deflate test input, value {}\n", line, line * 2654435761U);
    }
    const fs::path path = "parallel_deflate_input.bin";
    {
        std::ofstream file(path, std::ios::binary);
        file.write(plaintext.data(), static_cast<std::streamsize>(plaintext.size()));
    }
    struct Remove {
        const fs::path& path;
        ~Remove() { std::remove(path.c_str()); }
    } remove{path};

    std::vector<Byte> reference;
    for (const unsigned threads : {1U, 2U, 5U}) {
        StagingFile out({}, "z");
        // Only the single-threaded run is observed: it deflates on this
        // thread, and the observer is not made for concurrent use.
        if (threads == 1) {
            zlib_alloc_observer::reset();
            zlib_alloc_observer::watching = true;
        }
        const bool completed = parallelDeflateFileToPath(
            path, out, plaintext.size(), UnshrunkOutput::discard, threads);
        zlib_alloc_observer::watching = false;
        if (!completed) {
            std::fputs("parallel deflate discarded a stream that shrank\n", stderr);
            return false;
        }
        if (threads == 1 &&
            (zlib_alloc_observer::largest_block < 64 * 1024 || zlib_alloc_observer::found_plaintext)) {
            std::fputs("parallel deflate released zlib state unobserved or unwiped\n", stderr);
            return false;
        }
        const std::vector<Byte> stream = readAll(out.path());
        if (threads == 1) {
            reference = stream;
            std::vector<Byte> inflated(plaintext.size() + 1);
            uLongf inflated_size = static_cast<uLongf>(inflated.size());
            if (uncompress(inflated.data(), &inflated_size, stream.data(), static_cast<uLong>(stream.size())) != Z_OK ||
                inflated_size != plaintext.size() ||
                !std::equal(plaintext.begin(), plaintext.end(), inflated.begin(),
                    [](char a, Byte b) { return static_cast<Byte>(a) == b; })) {
                std::fputs("parallel deflate stream does not inflate back to its input\n", stderr);
                return false;
            }
        } else if (stream != reference) {
            std::fputs("parallel deflate output depends on the thread count\n", stderr);
            return false;
        }
    }

    // A stream that cannot shrink is abandoned, not written out. A fixed
    // xorshift sequence stands in for encrypted or already-compressed data.
    std::string noise(PARALLEL_SLICE_SIZE + 4096, '\0');
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;
    for (char& byte : noise) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        byte = static_cast<char>(state >> 24);
    }
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(noise.data(), static_cast<std::streamsize>(noise.size()));
    }
    StagingFile out({}, "z");
    if (parallelDeflateFileToPath(path, out, noise.size(), UnshrunkOutput::discard, 2)) {
        std::fputs("parallel deflate kept a stream no smaller than its input\n", stderr);
        return false;
    }
    if (!parallelDeflateFileToPath(path, out, noise.size(), UnshrunkOutput::keep, 2) ||
        readAll(out.path()).size() < noise.size()) {
        std::fputs("parallel deflate did not keep an unshrunk stream when asked to\n", stderr);
        return false;
    }
    return true;
}
} // namespace

int main() {
    if (sodium_init() < 0) {
        std::fputs("sodium_init failed\n", stderr);
        return 1;
    }
    const std::string plaintext(256 * 1024, 'S');
    const fs::path path = "deflate_wipe_input.bin";
    {
        std::FILE* file = std::fopen(path.c_str(), "wb");
        if (file == nullptr || std::fwrite(plaintext.data(), 1, plaintext.size(), file) != plaintext.size()) {
            std::fputs("could not write the test input\n", stderr);
            return 1;
        }
        std::fclose(file);
    }
    InputFile input(path, "could not open the test input");
    std::remove(path.c_str());
    std::size_t produced = 0;

    zlib_alloc_observer::reset();
    zlib_alloc_observer::watching = true;
    const bool completed = deflateFromInputFile(input, plaintext.size(), [&](std::span<const Byte> chunk) {
        produced += chunk.size();
        return true;
    });
    zlib_alloc_observer::watching = false;

    if (!completed) {
        std::fputs("deflate stopped before the end of the input\n", stderr);
        return 1;
    }

    if (produced == 0) {
        std::fputs("deflate produced no output\n", stderr);
        return 1;
    }
    // deflate's sliding window alone is 2 x 32 KiB.
    if (zlib_alloc_observer::largest_block < 64 * 1024) {
        std::fputs("did not observe zlib's deflate window release\n", stderr);
        return 1;
    }
    if (zlib_alloc_observer::found_plaintext) {
        std::fputs("zlib released a deflate allocation containing plaintext\n", stderr);
        return 1;
    }
    if (!checkParallelDeflate()) return 1;
    if (!checkLibdeflateWipe()) return 1;
    std::puts("zlib and libdeflate compressor state wiped on release; "
              "parallel deflate inflates and is thread-count independent");
    return 0;
}
