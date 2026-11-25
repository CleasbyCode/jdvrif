#pragma once
// Include from exactly one translation unit: it defines the wrap symbols.

// Observes the blocks zlib allocates through zlib_wipe_alloc. The test is
// linked with --wrap=malloc,--wrap=free; only calls made from this program's
// own objects (which is where the wiping callbacks live) are wrapped, so a
// stream still using zlib's internal default allocator records nothing and
// the "observed" check fails.

#include <cstddef>
#include <cstdlib>

extern "C" void* __real_malloc(std::size_t size);
extern "C" void __real_free(void* pointer);

namespace zlib_alloc_observer {
inline bool watching = false;
inline bool found_plaintext = false;
inline std::size_t observed_blocks = 0;
inline std::size_t largest_block = 0;

struct Record {
    void* pointer;
    std::size_t size;
};
inline Record records[64]{};

inline void reset() {
    for (Record& record : records) record = {};
    found_plaintext = false;
    observed_blocks = 0;
    largest_block = 0;
}
} // namespace zlib_alloc_observer

extern "C" void* __wrap_malloc(std::size_t size) {
    void* pointer = __real_malloc(size);
    if (zlib_alloc_observer::watching && pointer != nullptr) {
        for (auto& record : zlib_alloc_observer::records) {
            if (record.pointer == nullptr) {
                record = {pointer, size};
                break;
            }
        }
    }
    return pointer;
}

extern "C" void __wrap_free(void* pointer) {
    if (pointer != nullptr) {
        for (auto& record : zlib_alloc_observer::records) {
            if (record.pointer != pointer) continue;
            ++zlib_alloc_observer::observed_blocks;
            if (record.size > zlib_alloc_observer::largest_block) {
                zlib_alloc_observer::largest_block = record.size;
            }
            const auto* bytes = static_cast<const unsigned char*>(pointer);
            for (std::size_t index = 0; index < record.size; ++index) {
                if (bytes[index] != 0) zlib_alloc_observer::found_plaintext = true;
            }
            record = {};
            break;
        }
    }
    __real_free(pointer);
}
