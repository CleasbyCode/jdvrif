#pragma once

// Helpers shared by the metadata-stripping coefficient carriers (Reddit C3 and
// X-Twitter J-UNIWARD/STC) and by the JPEG quality gate in jpeg_utils.cpp.
//
// This header is compiled into the C++ CLI, the web variant and the Rust port's
// FFI build (C++23 in all three), so it depends on the standard library alone. Keep it
// byte-identical across the three trees.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// The 128-bit carrier key behind RQSTEG3 / JXSTEG4 (128-bit recovery KEYs).
using CarrierKey128 = std::array<std::uint8_t, 16>;


namespace carrier_common {

// ---- Wiping key-derived carrier state ---------------------------------------
//
// A carrier's layout (which coefficients carry which bits, in what order) and
// its whitening and dither schedules all follow from the carrier key. Once the
// key is wiped they would still let anyone reading the process's freed heap,
// a core file or swap find and strip the carrier in the image, so the buffers
// holding them are zeroed before release like the key itself. Standard
// library only (this header's contract), hence no sodium_memzero here.

// memset called through a volatile function pointer: the compiler cannot see
// which function runs, so it cannot drop the store as dead before a free.
inline void wipeMemory(void* data, std::size_t size) noexcept {
    static void* (*const volatile memset_function)(void*, int, std::size_t) = std::memset;
    if (data != nullptr && size != 0) memset_function(data, 0, size);
}

// An allocator that zeroes every block it releases, so a std::vector using it
// leaves nothing behind on destruction, on reallocation as it grows, or when
// an exception or a codec longjmp's follow-up throw unwinds past it.
template <typename T>
struct WipingAllocator {
    using value_type = T;

    WipingAllocator() noexcept = default;
    template <typename U>
    WipingAllocator(const WipingAllocator<U>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t count) { return std::allocator<T>{}.allocate(count); }
    void deallocate(T* pointer, std::size_t count) noexcept {
        wipeMemory(pointer, count * sizeof(T));
        std::allocator<T>{}.deallocate(pointer, count);
    }

    template <typename U>
    bool operator==(const WipingAllocator<U>&) const noexcept { return true; }
};

template <typename T>
using WipedVector = std::vector<T, WipingAllocator<T>>;

// Zeroes a vector's whole allocation, not just its live elements, then
// releases it. For buffers that must keep a plain std::vector type.
// The vector is first resized to its capacity, so every byte wiped is a live
// element (as wipeContainerCapacity does): a container-overflow checker
// (libc++, _GLIBCXX_SANITIZE_VECTOR) would flag a write past size(). Growing
// within capacity never reallocates and value-initialises trivially, so it
// cannot throw.
template <typename T, typename Allocator>
void wipeAndRelease(std::vector<T, Allocator>& vector) noexcept {
    static_assert(std::is_trivially_copyable_v<T> && std::is_nothrow_default_constructible_v<T>);
    vector.resize(vector.capacity());
    wipeMemory(vector.data(), vector.size() * sizeof(T));
    vector.clear();
    std::vector<T, Allocator>{}.swap(vector);
}

// Wipes a plain std::vector when it goes out of scope, however that happens.
template <typename Vector>
class WipeOnExit {
public:
    explicit WipeOnExit(Vector& vector) noexcept : vector_(vector) {}
    WipeOnExit(const WipeOnExit&) = delete;
    WipeOnExit& operator=(const WipeOnExit&) = delete;
    ~WipeOnExit() { wipeAndRelease(vector_); }

private:
    Vector& vector_;
};

// Wipes a vector being built for the caller only if its scope is left by an
// exception (an error or a SignalCancellation partway through filling it):
// on success the caller takes it, and its own guard. Declared right after
// the vector, so it also covers a return by NRVO.
template <typename Vector>
class WipeOnThrow {
public:
    explicit WipeOnThrow(Vector& vector) noexcept
        : vector_(vector), exceptions_(std::uncaught_exceptions()) {}
    WipeOnThrow(const WipeOnThrow&) = delete;
    WipeOnThrow& operator=(const WipeOnThrow&) = delete;
    ~WipeOnThrow() {
        if (std::uncaught_exceptions() > exceptions_) wipeAndRelease(vector_);
    }

private:
    Vector& vector_;
    int exceptions_;
};

// SplitMix64 finaliser: J-UNIWARD's parity-change direction mixer.
[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

namespace detail {

[[nodiscard]] constexpr std::uint64_t loadLe64(const std::uint8_t* bytes, std::size_t count) noexcept {
    std::uint64_t value = 0;
    for (std::size_t byte = 0; byte < count; ++byte) {
        value |= static_cast<std::uint64_t>(bytes[byte]) << (byte * 8U);
    }
    return value;
}

struct SipState {
    std::uint64_t v0, v1, v2, v3;

    constexpr SipState(std::uint64_t k0, std::uint64_t k1) noexcept
        : v0(k0 ^ 0x736f6d6570736575ULL),
          v1(k1 ^ 0x646f72616e646f6dULL),
          v2(k0 ^ 0x6c7967656e657261ULL),
          v3(k1 ^ 0x7465646279746573ULL) {}

    constexpr void round() noexcept {
        v0 += v1; v1 = std::rotl(v1, 13); v1 ^= v0; v0 = std::rotl(v0, 32);
        v2 += v3; v3 = std::rotl(v3, 16); v3 ^= v2;
        v0 += v3; v3 = std::rotl(v3, 21); v3 ^= v0;
        v2 += v1; v1 = std::rotl(v1, 17); v1 ^= v2; v2 = std::rotl(v2, 32);
    }

    // SipHash-2-4: two compression rounds per message word ...
    constexpr void compress(std::uint64_t word) noexcept {
        v3 ^= word;
        round();
        round();
        v0 ^= word;
    }

    // ... and four finalisation rounds.
    [[nodiscard]] constexpr std::uint64_t finish() noexcept {
        v2 ^= 0xffU;
        round();
        round();
        round();
        round();
        return v0 ^ v1 ^ v2 ^ v3;
    }
};

// SipHash-2-4 of the 16-byte message LE64(m0) || LE64(m1) under key words
// (k0, k1): the per-coefficient hot path of CarrierPrf::keyed.
[[nodiscard]] constexpr std::uint64_t siphash24Words(
    std::uint64_t k0, std::uint64_t k1, std::uint64_t m0, std::uint64_t m1) noexcept {
    SipState state(k0, k1);
    state.compress(m0);
    state.compress(m1);
    state.compress(std::uint64_t{16} << 56U); // empty tail, length 16
    return state.finish();
}

} // namespace detail

// The keyed hash behind every carrier permutation, whitening stream, dither
// offset and parity-change direction: (d, x) -> SipHash-2-4(k, LE64(d) || LE64(x)).
//
// Holds a copy of the key and zeroes it on destruction. Neither copyable nor
// movable (a moved-from copy would leave a second, unwiped key behind):
// construct it in place from keyed() and pass it by const reference.
class CarrierPrf {
public:
    [[nodiscard]] static CarrierPrf keyed(const CarrierKey128& key) noexcept {
        return CarrierPrf(detail::loadLe64(key.data(), 8), detail::loadLe64(key.data() + 8, 8));
    }

    CarrierPrf(const CarrierPrf&) = delete;
    CarrierPrf& operator=(const CarrierPrf&) = delete;
    CarrierPrf(CarrierPrf&&) = delete;
    CarrierPrf& operator=(CarrierPrf&&) = delete;

    ~CarrierPrf() {
        volatile std::uint64_t* words[] = {&k0_, &k1_};
        for (volatile std::uint64_t* word : words) *word = 0;
        // Keeps the compiler from moving the stores past the end of the
        // object's life.
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }

    [[nodiscard]] std::uint64_t operator()(std::uint64_t domain, std::uint64_t x) const noexcept {
        return detail::siphash24Words(k0_, k1_, domain, x);
    }

private:
    CarrierPrf(std::uint64_t k0, std::uint64_t k1) noexcept
        : k0_(k0), k1_(k1) {}

    std::uint64_t k0_; // key bytes 0..7, LE
    std::uint64_t k1_; // key bytes 8..15, LE
};

// Uniform draws in [0, bound) for the RQSTEG3 / JXSTEG4 position shuffles.
//
// Draw n reads word n of the stream prf(domain, 0), prf(domain, 1), ...;
// a rejected word still advances the stream. A word w is accepted when
// w >= (2^64 - bound) mod bound -- the accepted range is then a whole number of
// bound-sized intervals -- and the draw is w mod bound. Each draw therefore
// depends on the full 128-bit key through its own SipHash call, rather than
// every position following from one 64-bit seed.
class KeyedIndexStream {
public:
    KeyedIndexStream(const CarrierPrf& prf, std::uint64_t domain) noexcept
        : prf_(prf), domain_(domain) {}

    KeyedIndexStream(const KeyedIndexStream&) = delete;
    KeyedIndexStream& operator=(const KeyedIndexStream&) = delete;

    // bound must be at least 1.
    [[nodiscard]] std::uint64_t below(std::uint64_t bound) noexcept {
        for (;;) {
            const std::uint64_t word = prf_(domain_, counter_++);
            // The threshold is below bound, so a word at or above bound is
            // accepted without computing it -- nearly every word, as bounds
            // stay under 2^32 -- saving a 64-bit division. Same acceptance
            // rule, so the same draws.
            if (word >= bound || word >= (0U - bound) % bound) return word % bound;
        }
    }

private:
    const CarrierPrf& prf_;
    std::uint64_t domain_;
    std::uint64_t counter_{0};
};

// The first entries of a keyed Fisher-Yates permutation of 0, 1, ..., size - 1,
// front to back: for i = 0, 1, ... while i + 1 < size,
//     j = i + below(size - i);   swap(items[i], items[j])
// with below() drawn from one KeyedIndexStream(prf, domain). Only the positions
// a carrier actually uses are ever drawn, and a later extendTo() continues the
// same stream, so the prefix is the same whether fixed in one call or several.
// No array of all `size` values is kept while the prefix is short. An index the shuffle has not touched still holds its own
// value, so only the displaced entries are kept, in a small hash table: the
// memory follows the prefix length, not the domain. Once the prefix passes
// size / kSparseDivisor the table would cost about as much as the array, so
// the remaining entries are materialised and the swap continues in place.
//
// `poll` is called every 2^20 positions so callers can honour cancellation.
class KeyedIndexPrefix {
public:
    KeyedIndexPrefix(std::uint64_t size, const CarrierPrf& prf, std::uint64_t domain)
        : size_(size), draws_(prf, domain) {
        if (size > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("Internal Error: keyed permutation domain exceeds 32 bits.");
        }
    }

    KeyedIndexPrefix(const KeyedIndexPrefix&) = delete;
    KeyedIndexPrefix& operator=(const KeyedIndexPrefix&) = delete;

    // Fixes the first min(count, size) entries.
    template <typename Poll>
    void extendTo(std::size_t count, Poll&& poll) {
        const std::size_t target = static_cast<std::size_t>(
            std::min<std::uint64_t>(count, size_));
        if (target <= visible_) return;
        if (!dense_ && target > size_ / kSparseDivisor) materialise();
        // The last position needs no draw.
        const std::size_t swaps = std::min<std::size_t>(
            target, static_cast<std::size_t>(size_ == 0 ? 0 : size_ - 1));
        constexpr std::size_t kPollMask = (std::size_t{1} << 20U) - 1U;
        if (dense_) {
            // The swap targets are spread over the whole array, so each one is
            // a cache miss, and that -- not the keyed draw -- is what a large
            // shuffle costs. A draw depends only on the stream and the
            // position, never on the array, so a batch of them is drawn first
            // and their targets prefetched; the swaps are then applied in the
            // same order as ever. The permutation is unchanged (measured 2-3x
            // faster at the carriers' largest sizes).
            // Keyed swap targets: wiped however the loop ends, including a
            // SignalCancellation from poll().
            std::array<std::size_t, kDrawBatch> others;
            const struct WipeBatch {
                std::array<std::size_t, kDrawBatch>& batch;
                ~WipeBatch() { wipeMemory(batch.data(), sizeof(batch)); }
            } others_wipe{others};
            while (fixed_ < swaps) {
                if ((fixed_ & kPollMask) == 0) poll();
                // A batch never crosses a poll boundary, so nothing is drawn
                // ahead of a poll that may throw.
                const std::size_t batch = std::min({
                    kDrawBatch, swaps - fixed_, kPollMask + 1U - (fixed_ & kPollMask)});
                for (std::size_t ahead = 0; ahead < batch; ++ahead) {
                    const std::size_t position = fixed_ + ahead;
                    others[ahead] =
                        position + static_cast<std::size_t>(draws_.below(size_ - position));
                    __builtin_prefetch(&items_[others[ahead]], 1);
                }
                for (std::size_t ahead = 0; ahead < batch; ++ahead) {
                    std::swap(items_[fixed_ + ahead], items_[others[ahead]]);
                }
                fixed_ += batch;
            }
        } else {
            for (; fixed_ < swaps; ++fixed_) {
                if ((fixed_ & kPollMask) == 0) poll();
                const std::size_t other =
                    fixed_ + static_cast<std::size_t>(draws_.below(size_ - fixed_));
                const std::uint32_t here = valueAt(fixed_);
                items_.push_back(valueAt(other));
                if (other != fixed_) store(static_cast<std::uint32_t>(other), here);
            }
        }
        visible_ = target;
    }

    // The fixed entries, in order.
    [[nodiscard]] std::span<const std::uint32_t> prefix() const noexcept {
        return std::span<const std::uint32_t>(items_).first(visible_);
    }

private:
    static constexpr std::uint64_t kSparseDivisor = 16;
    // Draws computed ahead of their swaps in the dense shuffle.
    static constexpr std::size_t kDrawBatch = 32;
    static constexpr std::uint32_t kEmpty = std::numeric_limits<std::uint32_t>::max();

    struct Slot {
        std::uint32_t index;
        std::uint32_t value;
    };

    [[nodiscard]] std::size_t home(std::uint32_t index) const noexcept {
        return static_cast<std::size_t>(
            (static_cast<std::uint64_t>(index) * 0x9e3779b97f4a7c15ULL) >> shift_);
    }

    // The value at an unfixed position: displaced, or still its own index.
    [[nodiscard]] std::uint32_t valueAt(std::size_t position) const noexcept {
        const auto index = static_cast<std::uint32_t>(position);
        if (table_.empty()) return index;
        const std::size_t mask = table_.size() - 1U;
        for (std::size_t slot = home(index);; slot = (slot + 1U) & mask) {
            if (table_[slot].index == index) return table_[slot].value;
            if (table_[slot].index == kEmpty) return index;
        }
    }

    void store(std::uint32_t index, std::uint32_t value) {
        if ((used_ + 1U) * 2U > table_.size()) grow();
        const std::size_t mask = table_.size() - 1U;
        for (std::size_t slot = home(index);; slot = (slot + 1U) & mask) {
            if (table_[slot].index == index) {
                table_[slot].value = value;
                return;
            }
            if (table_[slot].index == kEmpty) {
                table_[slot] = Slot{index, value};
                ++used_;
                return;
            }
        }
    }

    // Doubles the table (open addressing, load at most 1/2). Positions the
    // shuffle has fixed may linger as stale entries; they are never read.
    void grow() {
        WipedVector<Slot> old = std::move(table_);
        const std::size_t capacity = old.empty() ? 64U : old.size() * 2U;
        table_.assign(capacity, Slot{kEmpty, 0});
        shift_ = 64U - static_cast<unsigned int>(std::countr_zero(capacity));
        used_ = 0;
        for (const Slot& entry : old) {
            if (entry.index != kEmpty) store(entry.index, entry.value);
        }
    }

    // Switches to the dense form: the whole domain in shuffled-so-far order.
    void materialise() {
        WipedVector<std::uint32_t> items(static_cast<std::size_t>(size_));
        for (std::size_t position = 0; position < items.size(); ++position) {
            items[position] = static_cast<std::uint32_t>(position);
        }
        for (const Slot& entry : table_) {
            if (entry.index != kEmpty && entry.index >= fixed_) items[entry.index] = entry.value;
        }
        std::copy(items_.begin(), items_.end(), items.begin());
        items_ = std::move(items);
        WipedVector<Slot>{}.swap(table_);
        used_ = 0;
        dense_ = true;
    }

    std::uint64_t size_;
    KeyedIndexStream draws_;
    // Sparse: the fixed prefix. Dense: the whole domain. Key-derived, so both
    // are wiped whenever they release memory (WipingAllocator).
    WipedVector<std::uint32_t> items_;
    WipedVector<Slot> table_;
    std::size_t used_{0};
    unsigned int shift_{64};
    std::size_t fixed_{0};
    std::size_t visible_{0};
    bool dense_{false};
};

namespace crc32_detail {
// Slicing-by-8 tables: tables[0] is the classic byte table; tables[k][i] is
// the CRC of byte i followed by k zero bytes, so eight table lookups advance
// the CRC by eight input bytes. 8 KiB, built at compile time. Measured on
// 5 MiB: 2.3 ms against 49 ms for the bit-at-a-time loop.
[[nodiscard]] consteval std::array<std::array<std::uint32_t, 256>, 8> makeTables() {
    std::array<std::array<std::uint32_t, 256>, 8> tables{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
        tables[0][i] = crc;
    }
    for (std::size_t slice = 1; slice < tables.size(); ++slice) {
        for (std::size_t i = 0; i < 256; ++i) {
            const std::uint32_t previous = tables[slice - 1][i];
            tables[slice][i] = (previous >> 8U) ^ tables[0][previous & 0xffU];
        }
    }
    return tables;
}

inline constexpr auto TABLES = makeTables();

[[nodiscard]] inline std::uint32_t loadU32Le(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}
} // namespace crc32_detail

// CRC-32 (IEEE 802.3, reflected). Integrity only -- never authentication; the
// payload it covers is secretstream-authenticated separately.
[[nodiscard]] inline std::uint32_t crc32(std::span<const std::uint8_t> data) noexcept {
    const auto& t = crc32_detail::TABLES;
    std::uint32_t crc = 0xffffffffU;
    const std::uint8_t* next = data.data();
    std::size_t left = data.size();
    for (; left >= 8; left -= 8, next += 8) {
        const std::uint32_t low = crc ^ crc32_detail::loadU32Le(next);
        const std::uint32_t high = crc32_detail::loadU32Le(next + 4);
        crc = t[7][low & 0xffU] ^ t[6][(low >> 8U) & 0xffU] ^
              t[5][(low >> 16U) & 0xffU] ^ t[4][low >> 24U] ^
              t[3][high & 0xffU] ^ t[2][(high >> 8U) & 0xffU] ^
              t[1][(high >> 16U) & 0xffU] ^ t[0][high >> 24U];
    }
    for (; left > 0; --left, ++next) {
        crc = (crc >> 8U) ^ t[0][(crc ^ *next) & 0xffU];
    }
    return ~crc;
}

inline void appendU32Le(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

// The little-endian u32 at `offset`. Callers check the range first.
[[nodiscard]] inline std::uint32_t readU32Le(
    std::span<const std::uint8_t> input,
    std::size_t offset) {
    return crc32_detail::loadU32Le(input.subspan(offset, 4).data());
}

using QuantizationTable = std::array<unsigned int, 64>;

// IJG (Annex K) quality-50 base tables, natural order.
inline constexpr QuantizationTable STD_LUMA_Q50{
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,
    24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99
};

inline constexpr QuantizationTable STD_CHROMA_Q50{
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};

// libjpeg's jpeg_quality_scaling + baseline clamp, applied to `base`.
[[nodiscard]] constexpr QuantizationTable scaledQuantTable(
    const QuantizationTable& base,
    int quality) noexcept {

    // libjpeg's jpeg_quality_scaling clamps the same way; 0 would divide by zero.
    quality = std::clamp(quality, 1, 100);
    const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    QuantizationTable result{};
    for (std::size_t i = 0; i < base.size(); ++i) {
        int value = (static_cast<int>(base[i]) * scale + 50) / 100;
        if (value < 1) value = 1;
        if (value > 255) value = 255;
        result[i] = static_cast<unsigned int>(value);
    }
    return result;
}

// Sum of the 64 entries of the standard scaled luminance table at each quality,
// indexed by quality (entry 0 unused). Computed at compile time from the same
// tables, so it cannot drift from them; the Rust port's STD_LUMINANCE_SUMS
// (jpeg_preprocess.rs) holds the same values.
inline constexpr auto STD_LUMA_QUALITY_SUMS = [] {
    std::array<std::uint64_t, 101> sums{};
    for (int quality = 1; quality <= 100; ++quality) {
        sums[static_cast<std::size_t>(quality)] = std::ranges::fold_left(
            scaledQuantTable(STD_LUMA_Q50, quality), std::uint64_t{0}, std::plus{});
    }
    return sums;
}();

// Estimated IJG quality (1..100) of a luminance table, from the sum of its 64
// entries: the quality whose standard scaled table has the nearest sum.
//
// This is the one estimator for the whole program. It decides both the default
// conceal path's Q97 ceiling and which X-Twitter images count as carriers, so an
// exact tie between two qualities resolves to the LOWER quality -- the rule the
// X-Twitter carrier format was defined with. Scanning upward and replacing only
// on a strictly smaller distance is what implements it.
[[nodiscard]] constexpr int estimateQualityFromLumaSum(std::uint64_t table_sum) noexcept {
    int best_quality = 1;
    std::uint64_t best_distance = std::numeric_limits<std::uint64_t>::max();
    for (int quality = 1; quality <= 100; ++quality) {
        const std::uint64_t expected_sum = STD_LUMA_QUALITY_SUMS[static_cast<std::size_t>(quality)];
        const std::uint64_t distance = table_sum > expected_sum
            ? table_sum - expected_sum
            : expected_sum - table_sum;
        if (distance < best_distance) {
            best_distance = distance;
            best_quality = quality;
        }
    }
    return best_quality;
}

[[noreturn]] inline void failNamed(
    std::string_view prefix, std::string_view name, std::string_view suffix) {
    std::string message;
    message.reserve(prefix.size() + name.size() + suffix.size());
    message.append(prefix).append(name).append(suffix);
    throw std::runtime_error(message);
}

// ---------------------------------------------------------------------------
// Keyed carrier header, shared by RQSTEG and JXSTEG. 24 bytes:
//   0..7 magic | 8..11 format parameters | 12..15 payload size, u32 LE |
//   16..19 payload CRC-32, u32 LE | 20..23 CRC-32 of bytes 0..19, u32 LE
// ---------------------------------------------------------------------------

inline constexpr std::size_t CARRIER_HEADER_BYTES = 24;

struct CarrierHeaderFormat {
    std::string_view name;                  // "Reddit" / "X-Twitter", for messages
    std::array<std::uint8_t, 8> magic;
    std::array<std::uint8_t, 4> parameters; // version + carrier-specific settings
};

struct CarrierHeader {
    std::uint32_t payload_size{0};
    std::uint32_t payload_crc{0};
};

[[nodiscard]] inline std::vector<std::uint8_t> makeCarrierHeader(
    const CarrierHeaderFormat& format,
    std::uint32_t payload_size,
    std::uint32_t payload_crc) {

    std::vector<std::uint8_t> header;
    header.reserve(CARRIER_HEADER_BYTES);
    header.insert(header.end(), format.magic.begin(), format.magic.end());
    header.insert(header.end(), format.parameters.begin(), format.parameters.end());
    appendU32Le(header, payload_size);
    appendU32Le(header, payload_crc);
    appendU32Le(header, crc32(header));
    return header;
}

// nullopt when the magic does not match: under a wrong key (or no carrier at
// all) the whitened header is noise. Once the magic matches, anything else that
// does not check out is corruption. The CRC is checked before the parameters:
// until the header has vouched for itself, a garbled parameter byte says
// nothing about the format and everything about the bits being wrong.
[[nodiscard]] inline std::optional<CarrierHeader> parseCarrierHeader(
    const CarrierHeaderFormat& format,
    std::span<const std::uint8_t> header) {

    if (header.size() != CARRIER_HEADER_BYTES) {
        failNamed("File Extraction Error: ", format.name, " carrier header has an invalid size.");
    }
    if (!std::ranges::equal(header.first(format.magic.size()), format.magic)) {
        return std::nullopt;
    }
    if (crc32(header.first(20)) != readU32Le(header, 20)) {
        failNamed("File Extraction Error: ", format.name, " carrier header failed its integrity check.");
    }
    if (!std::ranges::equal(header.subspan(8, 4), format.parameters)) {
        failNamed("File Extraction Error: Unsupported ", format.name, " carrier format.");
    }
    return CarrierHeader{
        .payload_size = readU32Le(header, 12),
        .payload_crc = readU32Le(header, 16),
    };
}

// ---------------------------------------------------------------------------
// Compact jdvrif envelope carried inside both coefficient carriers:
//   0..7 magic | 8 flags (bit 0 = compressed) | 9..11 reserved, zero |
//   12..15 encrypted size, u32 LE | KDF metadata | secretstream ciphertext
// ---------------------------------------------------------------------------

inline constexpr std::size_t ENVELOPE_PREFIX_BYTES = 16;
inline constexpr std::uint8_t ENVELOPE_FLAG_COMPRESSED = 1;

struct EnvelopeFormat {
    std::string_view name;               // "Reddit" / "X-Twitter", for messages
    std::array<std::uint8_t, 8> magic;   // "JDVRIFR1" / "JDVRIFX2"
    std::size_t kdf_metadata_bytes;
};

struct EncryptedEnvelope {
    std::vector<std::uint8_t> kdf_metadata{};
    std::vector<std::uint8_t> encrypted_data{};
    bool is_compressed{true};
};

[[nodiscard]] inline std::size_t envelopeSize(const EnvelopeFormat& format, std::size_t encrypted_size) {
    const std::size_t header_bytes = ENVELOPE_PREFIX_BYTES + format.kdf_metadata_bytes;
    if (encrypted_size > std::numeric_limits<std::size_t>::max() - header_bytes) {
        failNamed("File Size Error: ", format.name, " encrypted-envelope size overflow.");
    }
    return header_bytes + encrypted_size;
}

[[nodiscard]] inline std::vector<std::uint8_t> makeEnvelope(
    const EnvelopeFormat& format,
    std::span<const std::uint8_t> kdf_metadata,
    std::span<const std::uint8_t> encrypted_data,
    bool is_compressed) {

    if (kdf_metadata.size() != format.kdf_metadata_bytes) {
        failNamed("Internal Error: ", format.name, " KDF metadata has an invalid size.");
    }
    if (encrypted_data.size() > std::numeric_limits<std::uint32_t>::max()) {
        failNamed("File Size Error: ", format.name, " encrypted payload exceeds its format limit.");
    }

    // Built at its final size and filled in place: 9..11 (reserved) stay zero.
    std::vector<std::uint8_t> envelope(envelopeSize(format, encrypted_data.size()), 0);
    std::ranges::copy(format.magic, envelope.begin());
    envelope[8] = is_compressed ? ENVELOPE_FLAG_COMPRESSED : 0;
    const auto encrypted_size = static_cast<std::uint32_t>(encrypted_data.size());
    for (unsigned int byte = 0; byte < 4; ++byte) {
        envelope[12 + byte] = static_cast<std::uint8_t>((encrypted_size >> (byte * 8U)) & 0xffU);
    }
    const auto kdf_at = envelope.begin() + static_cast<std::ptrdiff_t>(ENVELOPE_PREFIX_BYTES);
    std::ranges::copy(kdf_metadata, kdf_at);
    std::ranges::copy(encrypted_data, kdf_at + static_cast<std::ptrdiff_t>(kdf_metadata.size()));
    return envelope;
}

// nullopt when the payload is not a jdvrif envelope of this format (the carrier
// may hold other rqsteg/jxsteg data); once the magic matches, malformed
// lengths or flags are corruption.
[[nodiscard]] inline std::optional<EncryptedEnvelope> parseEnvelope(
    const EnvelopeFormat& format,
    std::span<const std::uint8_t> payload) {

    if (payload.size() < format.magic.size() ||
        !std::ranges::equal(payload.first(format.magic.size()), format.magic)) {
        return std::nullopt;
    }
    const std::size_t header_bytes = ENVELOPE_PREFIX_BYTES + format.kdf_metadata_bytes;
    if (payload.size() < header_bytes) {
        failNamed("File Extraction Error: ", format.name, " embedded data is corrupt!");
    }

    const std::uint8_t flags = payload[8];
    if ((flags & static_cast<std::uint8_t>(~ENVELOPE_FLAG_COMPRESSED)) != 0 ||
        payload[9] != 0 || payload[10] != 0 || payload[11] != 0) {
        failNamed("File Extraction Error: ", format.name, " envelope contains unsupported flags.");
    }
    // Compared as the length after the header (payload.size() >= header_bytes
    // was checked above), so no sum can wrap even where size_t is 32 bits.
    if (payload.size() - header_bytes != readU32Le(payload, 12)) {
        failNamed("File Extraction Error: ", format.name, " embedded data is corrupt!");
    }

    return EncryptedEnvelope{
        .kdf_metadata = std::vector<std::uint8_t>(
            payload.begin() + static_cast<std::ptrdiff_t>(ENVELOPE_PREFIX_BYTES),
            payload.begin() + static_cast<std::ptrdiff_t>(header_bytes)),
        .encrypted_data = std::vector<std::uint8_t>(
            payload.begin() + static_cast<std::ptrdiff_t>(header_bytes), payload.end()),
        .is_compressed = (flags & ENVELOPE_FLAG_COMPRESSED) != 0,
    };
}

// 186 is equidistant from the Q97 (221) and Q98 (151) table sums.
static_assert(estimateQualityFromLumaSum(186) == 97 &&
              estimateQualityFromLumaSum(221) == 97 &&
              estimateQualityFromLumaSum(64) == 100 &&
              estimateQualityFromLumaSum(16320) == 1,
              "quality estimator tie rule or table drifted");

} // namespace carrier_common
