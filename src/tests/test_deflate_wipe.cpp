// Include the implementation to exercise its private streaming deflater.
// zlib's deflate state (window, hash chains, pending output) holds plaintext
// and must come back through the wiping zfree, zeroed.
#include "compression.cpp"  // found via -I<source tree>
#include "tests/zlib_alloc_observer.h"

#include <cstdio>

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
    deflateFromInputFile(input, plaintext.size(), [&](std::span<const Byte> chunk) {
        produced += chunk.size();
    });
    zlib_alloc_observer::watching = false;

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
    std::puts("zlib deflate state wiped on release");
    return 0;
}
