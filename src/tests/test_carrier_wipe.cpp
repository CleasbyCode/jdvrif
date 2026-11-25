// Key-derived carrier state is zeroed before its memory is released.
//
// A replacement global operator new/delete records every block allocated while
// a check is watching and, when such a block is freed, whether any byte of it
// is still non-zero. KeyedIndexPrefix (the carriers' keyed shuffle) must free
// only zeroed blocks: through the sparse table's growth, the switch to the
// dense array, the prefix's own growth and its destruction.
#include "carrier_common.h"

#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

namespace {
bool watching = false;
std::size_t observed = 0;
std::size_t dirty = 0;

struct Block {
    void* pointer;
    std::size_t size;
};
Block blocks[4096]{};

void record(void* pointer, std::size_t size) noexcept {
    if (!watching || pointer == nullptr) return;
    for (Block& block : blocks) {
        if (block.pointer == nullptr) {
            block = {pointer, size};
            return;
        }
    }
}

void inspect(void* pointer) noexcept {
    if (pointer == nullptr) return;
    for (Block& block : blocks) {
        if (block.pointer != pointer) continue;
        ++observed;
        const auto* bytes = static_cast<const unsigned char*>(pointer);
        for (std::size_t index = 0; index < block.size; ++index) {
            if (bytes[index] != 0) {
                ++dirty;
                break;
            }
        }
        block = {};
        return;
    }
}
} // namespace

void* operator new(std::size_t size) {
    void* pointer = std::malloc(size == 0 ? 1 : size);
    if (pointer == nullptr) throw std::bad_alloc();
    record(pointer, size);
    return pointer;
}
void operator delete(void* pointer) noexcept {
    inspect(pointer);
    std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
    inspect(pointer);
    std::free(pointer);
}

namespace {
int failures = 0;

// Thrown where a SignalCancellation would be; allocates nothing, so every
// block the checker sees is the code under test's.
struct Cancelled {};

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// Runs `body` while watching, then checks it freed at least `minimum` blocks
// and that every one was zero.
template <typename Body>
void checkWiped(const char* what, std::size_t minimum, Body body) {
    for (Block& block : blocks) block = {};
    observed = 0;
    dirty = 0;
    watching = true;
    body();
    watching = false;
    if (observed < minimum || dirty != 0) {
        std::fprintf(stderr, "FAIL: %s: %zu blocks freed, %zu not zeroed\n", what, observed, dirty);
        ++failures;
    }
}
} // namespace

int main() {
    CarrierKey128 key{};
    for (std::size_t index = 0; index < key.size(); ++index) key[index] = static_cast<std::uint8_t>(index * 37U + 1U);
    const auto prf = carrier_common::CarrierPrf::keyed(key);

    // Sparse: a short prefix of a large domain grows the displaced-entry table
    // and the prefix several times over.
    checkWiped("sparse shuffle", 4, [&] {
        carrier_common::KeyedIndexPrefix shuffle(1U << 20U, prf, 7);
        shuffle.extendTo(5000, [] {});
        expect(shuffle.prefix().size() == 5000, "sparse prefix length");
    });
    // Sparse, then dense: the table is dropped and the whole domain materialised.
    checkWiped("dense shuffle", 3, [&] {
        carrier_common::KeyedIndexPrefix shuffle(1U << 16U, prf, 7);
        shuffle.extendTo(1000, [] {});
        shuffle.extendTo(1U << 15U, [] {});
        expect(shuffle.prefix().size() == (1U << 15U), "dense prefix length");
    });
    // The building blocks directly: growth, explicit release and scope exit.
    checkWiped("WipedVector growth", 3, [] {
        carrier_common::WipedVector<std::uint32_t> values;
        for (std::uint32_t value = 1; value <= 10000; ++value) values.push_back(value);
    });
    checkWiped("wipeAndRelease and WipeOnExit", 2, [] {
        std::vector<std::uint8_t> released(4096, 0xA5);
        carrier_common::wipeAndRelease(released);
        expect(released.capacity() == 0, "wipeAndRelease releases the allocation");
        std::vector<std::uint8_t> scoped(4096, 0x5A);
        const carrier_common::WipeOnExit scoped_wipe{scoped};
    });
    // A shrunk vector: the spare capacity past size() still holds old bytes,
    // and must be zeroed too. (Built again under ASan with
    // _GLIBCXX_SANITIZE_VECTOR, where writing it without first resizing to
    // capacity is a container overflow.)
    checkWiped("wipeAndRelease spare capacity", 1, [] {
        std::vector<std::uint8_t> shrunk(4096, 0xA5);
        shrunk.resize(100);
        carrier_common::wipeAndRelease(shrunk);
    });
    // WipeOnThrow: an error or cancellation while a result is being filled
    // frees it zeroed; a normal exit leaves it to the caller, intact.
    checkWiped("WipeOnThrow on a throw", 1, [] {
        try {
            std::vector<std::uint8_t> partial(4096);
            const carrier_common::WipeOnThrow partial_wipe{partial};
            for (std::size_t index = 0; index < 2048; ++index) partial[index] = 0x3C;
            throw Cancelled{};
        } catch (const Cancelled&) {
        }
    });
    {
        const auto build = [] {
            std::vector<std::uint8_t> result(64);
            const carrier_common::WipeOnThrow result_wipe{result};
            for (auto& value : result) value = 0x42;
            return result;
        };
        const std::vector<std::uint8_t> kept = build();
        expect(kept.size() == 64 && kept[0] == 0x42 && kept[63] == 0x42,
               "WipeOnThrow keeps a result that was returned normally");
    }
    // A dense shuffle cancelled by its poll still frees only zeroed blocks.
    checkWiped("dense shuffle cancelled by poll", 1, [&] {
        try {
            carrier_common::KeyedIndexPrefix shuffle(1U << 22U, prf, 7);
            int polls = 0;
            shuffle.extendTo(1U << 22U, [&] {
                if (++polls == 2) throw Cancelled{};
            });
        } catch (const Cancelled&) {
        }
    });
    // The checker itself: a plain vector freed with its contents is caught.
    for (Block& block : blocks) block = {};
    observed = 0;
    dirty = 0;
    watching = true;
    { std::vector<std::uint8_t> plain(64, 0x77); }
    watching = false;
    expect(observed == 1 && dirty == 1, "the checker sees an unwiped block");

    if (failures != 0) return EXIT_FAILURE;
    std::puts("Key-derived carrier state is zeroed before release.");
    return EXIT_SUCCESS;
}
