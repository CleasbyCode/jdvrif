#include "common.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
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
        fail("WipeStringGuard left PIN digits in the string after unwind");
    }

    char ch = '7';
    try {
        WipePodGuard<char> guard(ch);
        throw std::runtime_error("cancel");
    } catch (const std::runtime_error&) {
    }

    if (ch != '\0') {
        fail("WipePodGuard left the last PIN byte after unwind");
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

    return 0;
}
