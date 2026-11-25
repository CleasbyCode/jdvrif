#include "common.h"
#include "encryption.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
[[noreturn]] void fail(const char* msg) {
    std::fprintf(stderr, "%s\n", msg);
    std::exit(1);
}

// The wipe helpers size everything by sizeof, and refuse types whose bytes
// are not the whole secret.
static_assert(WipeableValue<std::array<std::uint32_t, 4>>);
static_assert(!WipeableValue<Byte*>, "a pointer's bytes are not what it points at");
static_assert(!WipeableValue<std::string>, "use wipeContainerCapacity for strings");
static_assert(WipeableContainer<vBytes> && WipeableContainer<std::string>);
static_assert(WipeableContainer<std::vector<std::uint16_t>>);
static_assert(!WipeableContainer<std::vector<std::string>>);
static_assert(!WipeableContainer<std::array<Byte, 4>>, "fixed arrays use WipePodGuard");

bool freed_dirty = false;
std::size_t freed_blocks = 0;

// Checks every byte of each block as it is released.
template<typename T>
struct InspectingAllocator {
    using value_type = T;
    InspectingAllocator() = default;
    template<typename U>
    InspectingAllocator(const InspectingAllocator<U>&) noexcept {}
    T* allocate(std::size_t n) { return std::allocator<T>{}.allocate(n); }
    void deallocate(T* p, std::size_t n) noexcept {
        const auto* bytes = reinterpret_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n * sizeof(T); ++i) {
            if (bytes[i] != 0) freed_dirty = true;
        }
        ++freed_blocks;
        std::allocator<T>{}.deallocate(p, n);
    }
    template<typename U>
    bool operator==(const InspectingAllocator<U>&) const noexcept { return true; }
};
} // namespace

int main() {
    std::string input;
    input.reserve(20);
    input = "12345";
    input.push_back('6');
    input.pop_back();

    try {
        WipeStringGuard guard(input);
        throw std::runtime_error("cancel");
    } catch (const std::runtime_error&) {
    }

    if (!input.empty()) {
        fail("WipeStringGuard left KEY characters in the string after unwind");
    }

    char ch = '7';
    try {
        WipePodGuard<char> guard(ch);
        throw std::runtime_error("cancel");
    } catch (const std::runtime_error&) {
    }

    if (ch != '\0') {
        fail("WipePodGuard left the last KEY byte after unwind");
    }

    // Multi-byte elements: every byte, not just the first size() of them.
    std::array<std::uint32_t, 4> words{0xffffffffU, 0xffffffffU, 0xffffffffU, 0xffffffffU};
    {
        WipePodGuard<decltype(words)> guard(words);
    }
    for (const std::uint32_t word : words) {
        if (word != 0) fail("WipePodGuard wiped an element count instead of a byte count");
    }
    {
        SecureBuffer<std::array<std::uint32_t, 4>> owned;
        owned.buf.fill(0xffffffffU);
    } // Destructor wipes; nothing observable after, but it must compile and run.

    {
        std::vector<std::uint16_t, InspectingAllocator<std::uint16_t>> samples;
        // Filled to capacity: zeroing capacity() *bytes* would clear only the
        // first half of these 2-byte elements.
        samples.assign(64, 0xffffU);
        wipeContainerCapacity(samples);
    }
    if (freed_blocks == 0) fail("container allocation was never released");
    if (freed_dirty) {
        fail("wipeContainerCapacity left bytes of a multi-byte container unzeroed");
    }

    // A std::string keeps short contents inline. One that started inline and
    // then grew onto the heap still holds its early characters in the inline
    // buffer, which only the object's own bytes show.
    const auto holdsNameBytes = [](const unsigned char* bytes, std::size_t size) {
        for (std::size_t index = 0; index + 3 <= size; ++index) {
            if (bytes[index] == 'M' && bytes[index + 1] == 'E' && bytes[index + 2] == '.') return true;
        }
        return false;
    };
    {
        alignas(std::string) unsigned char storage[sizeof(std::string)];
        auto* name = new (storage) std::string("SECRETNAME.TXT");  // inline
        name->append(100, 'x');                                     // then heap
        wipeContainerCapacity(*name);
        if (holdsNameBytes(storage, sizeof(storage))) {
            fail("wipeContainerCapacity left a grown string's inline characters behind");
        }
        name->~basic_string();
    }
    // DecryptResult's recovered filename is wiped on every path, so a result
    // dropped by a refusal after decryption leaves no name in its object.
    {
        alignas(DecryptResult) unsigned char storage[sizeof(DecryptResult)];
        auto* result = new (storage) DecryptResult();
        result->filename = "SECRETNAME.TXT";  // inline
        result->~DecryptResult();
        if (holdsNameBytes(storage, sizeof(storage))) {
            fail("DecryptResult left the recovered filename behind");
        }
    }

    return 0;
}
