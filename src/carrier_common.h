#pragma once

// Helpers shared by the metadata-stripping coefficient carriers (Reddit C3 and
// X-Twitter J-UNIWARD/STC) and by the JPEG quality gate in jpeg_utils.cpp.
//
// This header is compiled into the C++ CLI, the web variant and the Rust port's
// FFI build (C++23 in all three), so it depends on the standard library alone. Keep it
// byte-identical across the three trees.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The 128-bit carrier key behind RQSTEG3 / JXSTEG4 (128-bit recovery PINs).
using CarrierKey128 = std::array<std::uint8_t, 16>;

// Every key a carrier recovery may need.
//
// `current` is the legacy (pre-v10, decimal-PIN) Argon2id-derived key that
// locates RQSTEG2 / JXSTEG3 carriers (deriveCarrierKeyFromPin). Deriving it costs one Argon2id run, so a PIN guess
// cannot be tested against a carrier header any faster than against the
// encrypted envelope itself.
//
// `legacy` is the cheap BLAKE2b key used by the jdvrif v9.0 formats (RQSTEG1 /
// JXSTEG2). Recovery still reads those images, but nothing writes them.
//
// `v3` is the 128-bit key of the RQSTEG3 / JXSTEG4 formats, derived from a
// 128-bit PIN. When `has_v3` is set, recovery tries only those formats; the
// u64 keys are then unused. Otherwise it tries only the u64 formats.
struct CarrierKeys {
    std::uint64_t current{0};    // legacy PIN: RQSTEG2 / JXSTEG3
    std::uint64_t legacy{0};     // legacy PIN: RQSTEG1 / JXSTEG2
    CarrierKey128 v3{};          // 128-bit PIN: RQSTEG3 / JXSTEG4
    bool has_v3{false};          // true -> try only the v3 formats
};

namespace carrier_common {

// SplitMix64 finaliser: the keyed hash of the u64-keyed carrier formats (see
// CarrierPrf::legacy) and the mixer of the SplitMix64 generator J-UNIWARD's
// layout seed feeds.
[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

namespace detail {

[[nodiscard]] constexpr std::uint64_t rotl64(std::uint64_t value, unsigned int bits) noexcept {
    return (value << bits) | (value >> (64U - bits));
}

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
        v0 += v1; v1 = rotl64(v1, 13U); v1 ^= v0; v0 = rotl64(v0, 32U);
        v2 += v3; v3 = rotl64(v3, 16U); v3 ^= v2;
        v0 += v3; v3 = rotl64(v3, 21U); v3 ^= v0;
        v2 += v1; v1 = rotl64(v1, 17U); v1 ^= v2; v2 = rotl64(v2, 32U);
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

// The keyed hash behind every carrier permutation, whitening stream, dither
// offset and parity-change direction.
//
//   legacy(k)  RQSTEG1/2, JXSTEG2/3: (d, x) -> mix64(k ^ d ^ x) and
//              offset(d) -> mix64(k + d), exactly as those formats were
//              defined, so their images read unchanged.
//   keyed(k)   RQSTEG3, JXSTEG4: (d, x) -> SipHash-2-4(k, LE64(d) || LE64(x))
//              and offset(d) -> the same with x = "OFFSET01".
//
// Holds a copy of the key and zeroes it on destruction. Neither copyable nor
// movable (a moved-from copy would leave a second, unwiped key behind):
// construct it in place from legacy()/keyed() and pass it by const reference.
class CarrierPrf {
public:
    static constexpr std::uint64_t OFFSET_X = 0x4f46465345543031ULL; // "OFFSET01"

    [[nodiscard]] static CarrierPrf legacy(std::uint64_t key) noexcept {
        return CarrierPrf(false, key, 0);
    }

    [[nodiscard]] static CarrierPrf keyed(const CarrierKey128& key) noexcept {
        return CarrierPrf(
            true, detail::loadLe64(key.data(), 8), detail::loadLe64(key.data() + 8, 8));
    }

    CarrierPrf(const CarrierPrf&) = delete;
    CarrierPrf& operator=(const CarrierPrf&) = delete;
    CarrierPrf(CarrierPrf&&) = delete;
    CarrierPrf& operator=(CarrierPrf&&) = delete;

    ~CarrierPrf() {
        volatile std::uint64_t* words[] = {&k0_, &k1_};
        for (volatile std::uint64_t* word : words) *word = 0;
    }

    [[nodiscard]] std::uint64_t operator()(std::uint64_t domain, std::uint64_t x) const noexcept {
        return keyed_ ? detail::siphash24Words(k0_, k1_, domain, x)
                      : mix64(k0_ ^ domain ^ x);
    }

    [[nodiscard]] std::uint64_t offset(std::uint64_t domain) const noexcept {
        return keyed_ ? detail::siphash24Words(k0_, k1_, domain, OFFSET_X)
                      : mix64(k0_ + domain);
    }

private:
    CarrierPrf(bool keyed, std::uint64_t k0, std::uint64_t k1) noexcept
        : keyed_(keyed), k0_(k0), k1_(k1) {}

    bool keyed_;
    std::uint64_t k0_; // legacy: the u64 key; keyed: key bytes 0..7, LE
    std::uint64_t k1_; // keyed: key bytes 8..15, LE; legacy: zero
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
        const std::uint64_t threshold = (0U - bound) % bound;
        for (;;) {
            const std::uint64_t word = prf_(domain_, counter_++);
            if (word >= threshold) return word % bound;
        }
    }

private:
    const CarrierPrf& prf_;
    std::uint64_t domain_;
    std::uint64_t counter_{0};
};

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

// CRC-32 (IEEE 802.3, reflected). Integrity only -- never authentication; the
// payload it covers is secretstream-authenticated separately.
[[nodiscard]] inline std::uint32_t crc32(std::span<const std::uint8_t> data) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

inline void appendU32Le(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

[[nodiscard]] inline std::uint32_t readU32Le(
    std::span<const std::uint8_t> input,
    std::size_t offset,
    const char* truncated_error) {

    if (offset > input.size() || input.size() - offset < 4U) {
        throw std::runtime_error(truncated_error);
    }
    std::uint32_t value = 0;
    for (unsigned int byte = 0; byte < 4; ++byte) {
        value |= static_cast<std::uint32_t>(input[offset + byte]) << (byte * 8U);
    }
    return value;
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
    if (crc32(header.first(20)) != readU32Le(header, 20, "")) {
        failNamed("File Extraction Error: ", format.name, " carrier header failed its integrity check.");
    }
    if (!std::ranges::equal(header.subspan(8, 4), format.parameters)) {
        failNamed("File Extraction Error: Unsupported ", format.name, " carrier format.");
    }
    return CarrierHeader{
        .payload_size = readU32Le(header, 12, ""),
        .payload_crc = readU32Le(header, 16, ""),
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

    std::vector<std::uint8_t> envelope;
    envelope.reserve(envelopeSize(format, encrypted_data.size()));
    envelope.insert(envelope.end(), format.magic.begin(), format.magic.end());
    envelope.push_back(is_compressed ? ENVELOPE_FLAG_COMPRESSED : 0);
    envelope.insert(envelope.end(), 3, 0); // reserved
    appendU32Le(envelope, static_cast<std::uint32_t>(encrypted_data.size()));
    envelope.insert(envelope.end(), kdf_metadata.begin(), kdf_metadata.end());
    envelope.insert(envelope.end(), encrypted_data.begin(), encrypted_data.end());
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
    // A u32 plus a small header cannot overflow a 64-bit size_t.
    if (payload.size() != header_bytes + readU32Le(payload, 12, "")) {
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
