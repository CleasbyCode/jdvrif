#pragma once

// Test-only reference implementations behind the carrier PRF tests. jdvrif
// itself uses carrier_common::CarrierPrf (16-byte messages only) and
// carrier_common::KeyedIndexPrefix; the tests compare those against these
// plainer forms, and these against libsodium.

#include "carrier_common.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace carrier_reference {

using carrier_common::CarrierPrf;
using carrier_common::KeyedIndexStream;
namespace detail = carrier_common::detail;

// Standard SipHash-2-4 with a 128-bit key read as two little-endian 64-bit
// words; the result is the 64-bit output word (read as LE64 it equals the
// 8 bytes libsodium's crypto_shorthash_siphash24 writes).
[[nodiscard]] constexpr std::uint64_t siphash24(
    const CarrierKey128& key, std::span<const std::uint8_t> message) noexcept {
    detail::SipState state(detail::loadLe64(key.data(), 8), detail::loadLe64(key.data() + 8, 8));
    const std::size_t full_words = message.size() / 8U;
    for (std::size_t word = 0; word < full_words; ++word) {
        state.compress(detail::loadLe64(message.data() + (word * 8U), 8));
    }
    const std::size_t tail = message.size() % 8U;
    const std::uint64_t last =
        (static_cast<std::uint64_t>(message.size() & 0xffU) << 56U) |
        detail::loadLe64(message.data() + (full_words * 8U), tail);
    state.compress(last);
    return state.finish();
}

// Keyed Fisher-Yates, front to back, that can be extended in steps.
//
// For i = 0, 1, ... while i < count and i + 1 < items.size():
//     j = i + below(items.size() - i);   swap(items[i], items[j])
// with below() drawn from one KeyedIndexStream(prf, domain). Afterwards
// items[0, count) are the first `count` entries of that keyed permutation of
// the input order, whether they were fixed in one call or several: a later
// extendTo() simply continues the same stream. Only the positions a carrier
// actually uses are ever drawn.
//
// `poll` is called every 2^20 positions so callers can honour cancellation.
template <typename T>
class KeyedPrefixShuffle {
public:
    KeyedPrefixShuffle(std::span<T> items, const CarrierPrf& prf, std::uint64_t domain) noexcept
        : items_(items), draws_(prf, domain) {}

    template <typename Poll>
    void extendTo(std::size_t count, Poll&& poll) {
        const std::size_t size = items_.size();
        const std::size_t end = std::min(count, size == 0 ? 0 : size - 1);
        for (; fixed_ < end; ++fixed_) {
            if ((fixed_ & ((std::size_t{1} << 20U) - 1U)) == 0) poll();
            const std::size_t other =
                fixed_ + static_cast<std::size_t>(draws_.below(size - fixed_));
            std::swap(items_[fixed_], items_[other]);
        }
    }

private:
    std::span<T> items_;
    KeyedIndexStream draws_;
    std::size_t fixed_{0};
};

} // namespace carrier_reference
