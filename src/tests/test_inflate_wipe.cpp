// Include the implementation to exercise its private streaming inflater. The
// linker wrapper observes the allocation immediately before its real delete,
// avoiding a use-after-free or dependence on allocator reuse.
#include "../encryption_stream_decrypt.cpp"

#include <cstdio>

namespace {
bool watch_deallocation = false;
bool found_plaintext = false;
std::size_t observed_buffers = 0;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
} // namespace

extern "C" void __real__ZdlPvm(void* pointer, std::size_t size) noexcept;
extern "C" void __wrap__ZdlPvm(void* pointer, std::size_t size) noexcept {
    if (watch_deallocation && pointer != nullptr && size == STREAM_INFLATE_OUT_CHUNK_SIZE) {
        ++observed_buffers;
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
        const vBytes plaintext(64 * 1024, 'S');
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
            const std::size_t previous = observed_buffers;
            bool rejected = false;
            watch_deallocation = true;
            try {
                StreamInflateToFile inflater(output.path());
                inflater.consume(input);
                require(inflater.finish() == plaintext.size(), "unexpected inflated size");
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            watch_deallocation = false;
            require(rejected == corrupt, "unexpected inflater success/failure");
            require(observed_buffers == previous + 1, "did not observe the inflater buffer release");
            require(!found_plaintext, "inflater released an allocation containing plaintext");
        }
        std::puts("Inflater output wiped on success and decompression failure");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
