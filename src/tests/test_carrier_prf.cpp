// Unit tests for the keyed carrier PRF in carrier_common.h:
//   - siphash24 against the SipHash paper's reference vector;
//   - siphash24 against libsodium's crypto_shorthash_siphash24 on the
//     LE64(domain) || LE64(x) messages CarrierPrf::keyed hashes;
//   - CarrierPrf::legacy reproducing the pre-v10 mix64 keying exactly;
//   - KeyedIndexStream / KeyedPrefixShuffle (the RQSTEG3 / JXSTEG4 position
//     order) against a reference written on libsodium, plus a pinned vector.

#include "carrier_common.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <span>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// Fixed-seed generator for the pseudo-random triples (independent of mix64).
struct Xorshift64 {
    std::uint64_t state;
    std::uint64_t next() noexcept {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        return state;
    }
};

void storeLe64(std::uint8_t* out, std::uint64_t value) noexcept {
    for (unsigned int byte = 0; byte < 8; ++byte) {
        out[byte] = static_cast<std::uint8_t>(value >> (byte * 8U));
    }
}

[[nodiscard]] std::uint64_t loadLe64(const std::uint8_t* in) noexcept {
    std::uint64_t value = 0;
    for (unsigned int byte = 0; byte < 8; ++byte) {
        value |= static_cast<std::uint64_t>(in[byte]) << (byte * 8U);
    }
    return value;
}

void testReferenceVector() {
    CarrierKey128 key{};
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i);
    std::array<std::uint8_t, 15> message{};
    for (std::size_t i = 0; i < message.size(); ++i) message[i] = static_cast<std::uint8_t>(i);
    check(carrier_common::siphash24(key, message) == 0xa129ca6149be45e5ULL,
          "siphash24 paper reference vector");

    // Every message length 0..63 against libsodium, covering every tail size.
    std::array<std::uint8_t, 64> long_message{};
    for (std::size_t i = 0; i < long_message.size(); ++i) {
        long_message[i] = static_cast<std::uint8_t>(i);
    }
    bool all_lengths = true;
    for (std::size_t length = 0; length < long_message.size(); ++length) {
        std::array<std::uint8_t, crypto_shorthash_siphash24_BYTES> out{};
        crypto_shorthash_siphash24(out.data(), long_message.data(), length, key.data());
        const std::uint64_t expected = loadLe64(out.data());
        if (carrier_common::siphash24(key, std::span(long_message).first(length)) != expected) {
            all_lengths = false;
        }
    }
    check(all_lengths, "siphash24 agrees with libsodium for message lengths 0..63");
}

void testLibsodiumAgreement() {
    Xorshift64 rng{0x6a64767269667033ULL};
    bool all_equal = true;
    bool prf_equal = true;
    bool offset_equal = true;
    constexpr std::uint64_t OFFSET_X = 0x4f46465345543031ULL; // "OFFSET01"
    for (int trial = 0; trial < 1000; ++trial) {
        CarrierKey128 key{};
        storeLe64(key.data(), rng.next());
        storeLe64(key.data() + 8, rng.next());
        const std::uint64_t domain = rng.next();
        const std::uint64_t x = rng.next();

        std::array<std::uint8_t, 16> message{};
        storeLe64(message.data(), domain);
        storeLe64(message.data() + 8, x);
        std::array<std::uint8_t, crypto_shorthash_siphash24_BYTES> out{};
        crypto_shorthash_siphash24(out.data(), message.data(), message.size(), key.data());
        const std::uint64_t expected = loadLe64(out.data());

        if (carrier_common::siphash24(key, message) != expected) all_equal = false;

        const carrier_common::CarrierPrf prf = carrier_common::CarrierPrf::keyed(key);
        if (prf(domain, x) != expected) prf_equal = false;

        storeLe64(message.data() + 8, OFFSET_X);
        crypto_shorthash_siphash24(out.data(), message.data(), message.size(), key.data());
        if (prf.offset(domain) != loadLe64(out.data())) offset_equal = false;
    }
    check(all_equal, "siphash24 equals crypto_shorthash_siphash24 on 1,000 triples");
    check(prf_equal, "CarrierPrf::keyed(k)(d, x) equals SipHash(k, LE64(d)||LE64(x)) on 1,000 triples");
    check(offset_equal, "CarrierPrf::keyed(k).offset(d) uses x = \"OFFSET01\" on 1,000 triples");
}

void testLegacyInstance() {
    using carrier_common::mix64;
    Xorshift64 rng{0x4c45474143593031ULL};
    bool prf_equal = true;
    bool offset_equal = true;
    for (int trial = 0; trial < 1000; ++trial) {
        const std::uint64_t key = rng.next();
        const std::uint64_t domain = rng.next();
        const std::uint64_t x = rng.next();
        const carrier_common::CarrierPrf prf = carrier_common::CarrierPrf::legacy(key);
        if (prf(domain, x) != mix64(key ^ domain ^ x)) prf_equal = false;
        if (prf.offset(domain) != mix64(key + domain)) offset_equal = false;
    }
    check(prf_equal, "CarrierPrf::legacy(k)(d, x) == mix64(k ^ d ^ x) on 1,000 triples");
    check(offset_equal, "CarrierPrf::legacy(k).offset(d) == mix64(k + d) on 1,000 triples");
}

void testKeyKindsDiffer() {
    CarrierKey128 key{};
    const auto zero_key_keyed = carrier_common::CarrierPrf::keyed(key);
    const auto zero_key_legacy = carrier_common::CarrierPrf::legacy(0);
    check(zero_key_keyed(1, 2) != zero_key_legacy(1, 2),
          "keyed and legacy instances are distinct functions");
}

// SipHash-2-4(key, LE64(domain) || LE64(x)) via libsodium.
[[nodiscard]] std::uint64_t sodiumPrf(const CarrierKey128& key, std::uint64_t domain, std::uint64_t x) {
    std::array<std::uint8_t, 16> message{};
    storeLe64(message.data(), domain);
    storeLe64(message.data() + 8, x);
    std::array<std::uint8_t, crypto_shorthash_siphash24_BYTES> out{};
    crypto_shorthash_siphash24(out.data(), message.data(), message.size(), key.data());
    return loadLe64(out.data());
}

// The shuffle as specified: for i < min(count, n - 1), swap items[i] with
// items[i + (w mod (n - i))], w the next stream word not below
// (2^64 - (n - i)) mod (n - i).
void referenceShuffle(std::vector<std::uint32_t>& items, std::size_t count,
                      const CarrierKey128& key, std::uint64_t domain) {
    std::uint64_t counter = 0;
    for (std::size_t i = 0; i < count && i + 1 < items.size(); ++i) {
        const std::uint64_t bound = items.size() - i;
        const std::uint64_t threshold = (0U - bound) % bound;
        std::uint64_t word = 0;
        do {
            word = sodiumPrf(key, domain, counter++);
        } while (word < threshold);
        std::swap(items[i], items[i + static_cast<std::size_t>(word % bound)]);
    }
}

[[nodiscard]] std::vector<std::uint32_t> iotaVector(std::size_t size) {
    std::vector<std::uint32_t> items(size);
    std::iota(items.begin(), items.end(), std::uint32_t{0});
    return items;
}

void testKeyedShuffle() {
    Xorshift64 rng{0x53485546464c4531ULL};
    bool matches = true;
    bool split_matches = true;
    bool is_permutation = true;
    for (const std::size_t size : {0U, 1U, 2U, 3U, 17U, 1000U, 4099U}) {
        for (int trial = 0; trial < 4; ++trial) {
            CarrierKey128 key{};
            for (std::size_t i = 0; i < key.size(); i += 8) storeLe64(key.data() + i, rng.next());
            const std::uint64_t domain = rng.next();
            const std::size_t count = size == 0 ? 0 : static_cast<std::size_t>(rng.next() % (size + 1));
            const std::size_t first = count / 3;

            std::vector<std::uint32_t> expected = iotaVector(size);
            referenceShuffle(expected, count, key, domain);

            const carrier_common::CarrierPrf prf = carrier_common::CarrierPrf::keyed(key);
            std::vector<std::uint32_t> actual = iotaVector(size);
            carrier_common::KeyedPrefixShuffle<std::uint32_t> one(actual, prf, domain);
            one.extendTo(count, [] {});
            if (actual != expected) matches = false;

            std::vector<std::uint32_t> split = iotaVector(size);
            carrier_common::KeyedPrefixShuffle<std::uint32_t> two(split, prf, domain);
            two.extendTo(first, [] {});
            two.extendTo(count, [] {});
            two.extendTo(first, [] {}); // never goes backwards
            if (split != expected) split_matches = false;

            std::vector<std::uint32_t> sorted = actual;
            std::sort(sorted.begin(), sorted.end());
            if (sorted != iotaVector(size)) is_permutation = false;
        }
    }
    check(matches, "KeyedPrefixShuffle equals the libsodium reference shuffle");
    check(split_matches, "KeyedPrefixShuffle extended in steps equals one call");
    check(is_permutation, "KeyedPrefixShuffle output is a permutation of its input");
}

// A bound just over 2^63 rejects close to half of all words, so this exercises
// the rejection path on nearly every draw.
void testIndexRejection() {
    CarrierKey128 key{};
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(0xa0 + i);
    constexpr std::uint64_t domain = 0x52454a4543543031ULL;
    constexpr std::uint64_t bound = (std::uint64_t{1} << 63U) + 1U;
    constexpr std::uint64_t threshold = (0U - bound) % bound;
    const carrier_common::CarrierPrf prf = carrier_common::CarrierPrf::keyed(key);
    carrier_common::KeyedIndexStream draws(prf, domain);
    std::uint64_t counter = 0;
    bool equal = true;
    int rejected = 0;
    for (int draw = 0; draw < 200; ++draw) {
        std::uint64_t word = sodiumPrf(key, domain, counter++);
        while (word < threshold) {
            ++rejected;
            word = sodiumPrf(key, domain, counter++);
        }
        if (draws.below(bound) != word % bound) equal = false;
    }
    check(equal, "KeyedIndexStream::below skips rejected words exactly as specified");
    check(rejected > 0, "rejection path exercised");
}

// Pinned: the first 8 entries of iota(1000) after KeyedPrefixShuffle under key
// 00..0f, domain 0x4a58535445474c31 (JXSTEG4's LAYOUT_DOMAIN), count 8.
void testPinnedShuffleVector() {
    CarrierKey128 key{};
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i);
    const carrier_common::CarrierPrf prf = carrier_common::CarrierPrf::keyed(key);
    std::vector<std::uint32_t> items = iotaVector(1000);
    carrier_common::KeyedPrefixShuffle<std::uint32_t> shuffle(items, prf, 0x4a58535445474c31ULL);
    shuffle.extendTo(8, [] {});
    constexpr std::array<std::uint32_t, 8> expected{703, 209, 836, 286, 804, 728, 262, 36};
    bool equal = true;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (items[i] != expected[i]) equal = false;
    }
    if (!equal) {
        std::fprintf(stderr, "pinned shuffle prefix:");
        for (std::size_t i = 0; i < expected.size(); ++i) std::fprintf(stderr, " %u", items[i]);
        std::fprintf(stderr, "\n");
    }
    check(equal, "KeyedPrefixShuffle pinned vector");
}

} // namespace

int main() {
    if (sodium_init() < 0) return 2;
    testReferenceVector();
    testLibsodiumAgreement();
    testLegacyInstance();
    testKeyKindsDiffer();
    testKeyedShuffle();
    testIndexRejection();
    testPinnedShuffleVector();
    if (failures != 0) {
        std::fprintf(stderr, "carrier PRF tests: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("carrier PRF tests passed");
    return 0;
}
