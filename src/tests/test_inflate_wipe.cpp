// Include the implementation to exercise its private streaming inflater. The
// linker wrapper observes the allocation immediately before its real delete,
// avoiding a use-after-free or dependence on allocator reuse.
#include "encryption_stream_decrypt.cpp"  // found via -I<source tree>
#include "tests/zlib_alloc_observer.h"

#include <cstdio>

namespace {
bool watch_deallocation = false;
bool found_plaintext = false;
std::size_t observed_inflate_buffers = 0;
std::size_t observed_output_buffers = 0;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
} // namespace

extern "C" void __real__ZdlPvm(void* pointer, std::size_t size) noexcept;
extern "C" void __wrap__ZdlPvm(void* pointer, std::size_t size) noexcept {
    // The inflater's own chunk, and the OutputFile buffer that short plaintext
    // tails pass through on their way to the staging file.
    const bool is_inflate_buffer = size == STREAM_INFLATE_OUT_CHUNK_SIZE;
    const bool is_output_buffer = size == PLAINTEXT_OUTPUT_BUFFER_SIZE;
    if (watch_deallocation && pointer != nullptr && (is_inflate_buffer || is_output_buffer)) {
        ++(is_inflate_buffer ? observed_inflate_buffers : observed_output_buffers);
        const auto* bytes = static_cast<const Byte*>(pointer);
        for (std::size_t index = 0; index < size; ++index) {
            if (bytes[index] != 0) found_plaintext = true;
        }
    }
    __real__ZdlPvm(pointer, size);
}

int main() {
    try {
        require(sodium_init() >= 0, "sodium_init failed");
        // 64 KiB fills whole output writes, which bypass the OutputFile buffer;
        // 100 bytes is a short tail that goes through it.
        for (const std::size_t plaintext_size : {std::size_t{64 * 1024}, std::size_t{100}}) {
            const vBytes plaintext(plaintext_size, 'S');
            uLongf compressed_size = compressBound(static_cast<uLong>(plaintext.size()));
            vBytes compressed(static_cast<std::size_t>(compressed_size));
            require(compress2(compressed.data(), &compressed_size, plaintext.data(),
                              static_cast<uLong>(plaintext.size()), Z_DEFAULT_COMPRESSION) == Z_OK,
                    "test compression failed");
            compressed.resize(static_cast<std::size_t>(compressed_size));

            for (const bool corrupt : {false, true}) {
                vBytes input = compressed;
                if (corrupt) input.back() ^= 1; // Adler checksum fails after producing plaintext.
                StagingFile output({}, "wipe_test");
                const std::size_t previous_inflate = observed_inflate_buffers;
                const std::size_t previous_output = observed_output_buffers;
                bool rejected = false;
                zlib_alloc_observer::reset();
                zlib_alloc_observer::watching = true;
                watch_deallocation = true;
                try {
                    StreamInflateToFile inflater(output.path());
                    // Small pieces span several inflate calls, which is when
                    // zlib allocates (and fills) its 32 KiB history window.
                    for (std::size_t offset = 0; offset < input.size(); offset += 4) {
                        inflater.consume(std::span<const Byte>(input).subspan(
                            offset, std::min<std::size_t>(4, input.size() - offset)));
                    }
                    require(inflater.finish() == plaintext.size(), "unexpected inflated size");
                } catch (const std::runtime_error&) {
                    rejected = true;
                }
                watch_deallocation = false;
                zlib_alloc_observer::watching = false;
                // inflate's history window is 32 KiB; it must come back
                // through the wiping zfree, zeroed.
                require(zlib_alloc_observer::largest_block >= 32 * 1024,
                        "did not observe zlib's history window release");
                require(!zlib_alloc_observer::found_plaintext,
                        "zlib released an internal allocation containing plaintext");
                require(rejected == corrupt, "unexpected inflater success/failure");
                require(observed_inflate_buffers == previous_inflate + 1,
                        "did not observe the inflater buffer release");
                require(observed_output_buffers == previous_output + 1,
                        "did not observe the output buffer release");
                require(!found_plaintext, "inflater released an allocation containing plaintext");
            }
        }
        std::puts("Inflater, zlib window and output buffers wiped on success and decompression failure");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
