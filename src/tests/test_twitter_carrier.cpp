#include "twitter_steg.h"
#include "twitter_juniward.h"
#include "twitter_stc.h"
#include "twitter_jpeg_codec.h"
#include "signal_utils.h"
#include "encryption_internal.h"

#include <jpeglib.h>
#include <sodium.h>

#include <array>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

namespace {

[[nodiscard]] vBytes readFile(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("failed to open test input");
    return vBytes(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

[[nodiscard]] vBytes makeSmallCover() {
    jpeg_compress_struct encoder{};
    jpeg_error_mgr errors{};
    encoder.err = jpeg_std_error(&errors);
    jpeg_create_compress(&encoder);
    unsigned char* output = nullptr;
    unsigned long output_size = 0;
    jpeg_mem_dest(&encoder, &output, &output_size);
    encoder.image_width = 400;
    encoder.image_height = 400;
    encoder.input_components = 3;
    encoder.in_color_space = JCS_RGB;
    jpeg_set_defaults(&encoder);
    jpeg_set_quality(&encoder, 90, TRUE);
    jpeg_start_compress(&encoder, TRUE);
    std::array<JSAMPLE, 400 * 3> row;
    row.fill(128);
    while (encoder.next_scanline < encoder.image_height) {
        JSAMPROW scanline = row.data();
        jpeg_write_scanlines(&encoder, &scanline, 1);
    }
    jpeg_finish_compress(&encoder);
    jpeg_destroy_compress(&encoder);
    const std::unique_ptr<unsigned char, decltype(&std::free)> guard(
        output, &std::free);
    return vBytes(output, output + output_size);
}

[[nodiscard]] std::uint64_t readDecimalPin(const char* path) {
    const vBytes text = readFile(path);
    std::uint64_t value = 0;
    const char* begin = reinterpret_cast<const char*>(text.data());
    const auto [end, ec] = std::from_chars(begin, begin + text.size(), value);
    if (ec != std::errc{} || value == 0) throw std::runtime_error("bad fixture PIN file");
    (void)end;
    return value;
}

// Independent format helpers let this test write a valid keyed header whose
// declared length is impossible, without the embedding API rejecting it first.
[[nodiscard]] std::uint64_t mix64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] std::uint32_t crc32(std::span<const Byte> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const Byte byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

// LE64(domain) || LE64(x) under SipHash-2-4, computed with libsodium so the
// JXSTEG4 keying is checked against an implementation independent of
// carrier_common::siphash24.
[[nodiscard]] std::uint64_t sodiumSipHash(
    const CarrierKey128& key, std::uint64_t domain, std::uint64_t x) {
    std::array<Byte, 16> message{};
    for (unsigned int byte = 0; byte < 8; ++byte) {
        message[byte] = static_cast<Byte>(domain >> (byte * 8U));
        message[8U + byte] = static_cast<Byte>(x >> (byte * 8U));
    }
    std::array<Byte, crypto_shorthash_siphash24_BYTES> out{};
    crypto_shorthash_siphash24(out.data(), message.data(), message.size(), key.data());
    std::uint64_t value = 0;
    for (unsigned int byte = 0; byte < 8; ++byte) {
        value |= static_cast<std::uint64_t>(out[byte]) << (byte * 8U);
    }
    return value;
}

// JXSTEG4's position order, written out from its definition (carrier_common.h,
// KeyedPrefixShuffle) on top of libsodium: front-to-back Fisher-Yates whose
// i-th swap partner is i + (w mod (n - i)), w the next SipHash(domain, counter)
// word not below (2^64 - (n - i)) mod (n - i).
void referenceKeyedShuffle(
    std::vector<std::uint32_t>& items,
    std::size_t count,
    const CarrierKey128& key,
    std::uint64_t domain) {
    std::uint64_t counter = 0;
    for (std::size_t i = 0; i < count && i + 1 < items.size(); ++i) {
        const std::uint64_t bound = items.size() - i;
        const std::uint64_t threshold = (0U - bound) % bound;
        std::uint64_t word = 0;
        do {
            word = sodiumSipHash(key, domain, counter++);
        } while (word < threshold);
        std::swap(items[i], items[i + static_cast<std::size_t>(word % bound)]);
    }
}

// The key a malformed-header case is written under: `v3` for JXSTEG4, `legacy`
// (a u64) for JXSTEG3 / JXSTEG2.
struct TestKey {
    std::uint64_t legacy{0};
    CarrierKey128 v3{};
};

// `version` selects the format written: 4 (current JXSTEG4, read under
// CarrierKeys::v3), 3 (v9.1 JXSTEG3, read under CarrierKeys::current) or
// 2 (v9.0 JXSTEG2, read under CarrierKeys::legacy).
void checkOversizedHeader(
    const TwitterPreparedCover& cover,
    const TestKey& key,
    std::uint32_t declared_size,
    Byte version = 4) {

    using namespace twitter_steg_internal;
    const auto prf = [&](std::uint64_t domain, std::uint64_t x) {
        return version == 4
            ? sodiumSipHash(key.v3, domain, x)
            : mix64(key.legacy ^ domain ^ x);
    };
    auto coefficients = cover.coefficients;
    constexpr std::uint64_t layout_domain = 0x4a58535445474c31ULL;
    const std::size_t header_symbols =
        static_cast<std::size_t>(requiredCoverSymbols(24U * 8U));
    CarrierLayout layout;
    if (version == 4) {
        layout = makeUnshuffledCarrierLayout(coefficients);
        referenceKeyedShuffle(layout.coefficient_offsets, header_symbols, key.v3, layout_domain);
    } else {
        layout = makeCarrierLayout(coefficients, prf(layout_domain, 0));
    }
    std::array<Byte, 24> header{
        'J', 'X', 'S', 'T', 'E', 'G', static_cast<Byte>('0' + version), 0,
        version, 7, 2, 5};
    for (unsigned int byte = 0; byte < 4; ++byte) {
        header[12 + byte] = static_cast<Byte>(declared_size >> (byte * 8U));
    }
    const auto checksum = crc32(std::span<const Byte>(header).first(20));
    for (unsigned int byte = 0; byte < 4; ++byte) {
        header[20 + byte] = static_cast<Byte>(checksum >> (byte * 8U));
    }
    vBytes header_bits(header.size() * 8U);
    for (std::size_t bit = 0; bit < header_bits.size(); ++bit) {
        const auto stream = prf(0x4a58535445474831ULL, bit / 64U);
        header_bits[bit] = static_cast<Byte>(
            ((header[bit / 8U] >> (bit % 8U)) ^
             (stream >> (bit % 64U))) & 1U);
    }
    const auto offsets = std::span<const std::uint32_t>(layout.coefficient_offsets)
        .first(header_symbols);
    const auto parity = coefficientParityBits(coefficients.luminance, offsets);
    const auto embedded = stcEmbed(
        parity, std::vector<float>(offsets.size(), 1), header_bits);
    (void)applyParityBits(coefficients.luminance, offsets, embedded.stego_bits, 0);
    const auto malformed = writeProgressiveCoefficients(
        cover.jpeg, coefficients.luminance);
    try {
        const CarrierKeys keys = version == 4
            ? CarrierKeys{.v3 = key.v3, .has_v3 = true}
            : version == 3
                ? CarrierKeys{.current = key.legacy, .legacy = ~key.legacy}
                : CarrierKeys{.current = ~key.legacy, .legacy = key.legacy};
        (void)extractTwitterPayload(malformed, keys);
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()).find("larger than its coefficient capacity") !=
            std::string_view::npos) {
            return;
        }
        throw;
    }
    throw std::runtime_error("oversized carrier header was accepted");
}

[[nodiscard]] constexpr CarrierKey128 sequentialKey(Byte first) {
    CarrierKey128 key{};
    for (std::size_t index = 0; index < key.size(); ++index) {
        key[index] = static_cast<Byte>(first + index);
    }
    return key;
}

constexpr CarrierKey128 KEY_00_0F = sequentialKey(0x00);
constexpr CarrierKey128 KEY_01_10 = sequentialKey(0x01);

void checkLowAmplitudeCapacity() {
    using namespace twitter_steg_internal;
    const vBytes base = makeSmallCover();
    auto coefficients = readCoefficients(base);
    for (std::size_t offset = 0; offset < coefficients.luminance.size(); ++offset) {
        if (offset % 64U != 0) coefficients.luminance[offset] = 1;
    }
    const auto cover = prepareTwitterCover(
        writeProgressiveCoefficients(base, coefficients.luminance));
    constexpr CarrierKey128 carrier_key = sequentialKey(0x42);
    const vBytes payload(cover.payload_capacity, 0x42);
    const auto embedded = embedTwitterPayload(cover, carrier_key, payload);
    const auto after = countCarrierCandidates(readCoefficients(embedded));
    if (payload.empty() || after.candidate_count != cover.candidate_count ||
        after.nonzero_ac_count >= cover.nonzero_ac_count) {
        throw std::runtime_error("low-amplitude fixture did not reduce the nonzero AC count");
    }
    const auto recovered = extractTwitterPayload(
        embedded, CarrierKeys{.v3 = carrier_key, .has_v3 = true});
    if (!recovered || *recovered != payload) {
        throw std::runtime_error("full-capacity low-amplitude carrier round trip mismatch");
    }
    // All eligible AC coefficients were nonzero before embedding, so the
    // advertised limit here is also the invariant coefficient-position limit.
    const TestKey key{.legacy = 42, .v3 = carrier_key};
    checkOversizedHeader(cover, key,
        static_cast<std::uint32_t>(cover.payload_capacity + 1U));
    checkOversizedHeader(cover, key,
        std::numeric_limits<std::uint32_t>::max());
    // The same bound must hold on the v9.1 (JXSTEG3) read path ...
    checkOversizedHeader(cover, key,
        static_cast<std::uint32_t>(cover.payload_capacity + 1U), /*version=*/3);
    checkOversizedHeader(cover, key,
        std::numeric_limits<std::uint32_t>::max(), /*version=*/3);
    // ... and on the v9.0 (JXSTEG2) one.
    checkOversizedHeader(cover, key,
        std::numeric_limits<std::uint32_t>::max(), /*version=*/2);
}

// With SIGINT already pending, each expensive carrier stage must stop with
// SignalCancellation instead of computing to completion. Runs in a child so
// the pending signal and its handlers never touch the rest of the test.
void checkStagesHonourCancellation(const TwitterPreparedCover& cover) {
    using namespace twitter_steg_internal;
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error("fork failed");
    if (child == 0) {
        const auto luminance = decodeLuminance(cover.jpeg);
        const auto layout = makeCarrierLayout(cover.coefficients, 7);
        const std::vector<std::uint8_t> message(4096, 1);
        const auto cover_size = static_cast<std::size_t>(requiredCoverSymbols(message.size()));
        const std::vector<std::uint8_t> cover_bits(cover_size, 0);
        const std::vector<float> costs(cover_size, 1);

        installProcessSignalHandlers();
        std::raise(SIGINT);
        int stopped = 0;
        const auto expect_stop = [&](auto&& stage) {
            try {
                stage();
            } catch (const SignalCancellation&) {
                ++stopped;
            }
        };
        expect_stop([&] { (void)stcEmbed(cover_bits, costs, message); });
        expect_stop([&] { (void)makeCarrierLayout(cover.coefficients, 7); });
        expect_stop([&] {
            (void)computeJUniwardCosts(luminance, cover.coefficients, layout.coefficient_offsets);
        });
        std::_Exit(stopped == 3 ? 0 : 1);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error("an X-Twitter carrier stage ignored a pending cancellation");
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 5 || sodium_init() < 0) return 2;
        const vBytes cover_bytes = readFile(argv[1]);
        const vBytes payload = readFile(argv[2]);
        const vBytes v9_1_fixture = readFile(argv[3]);
        const SecurePin v9_1_pin = SecurePin::legacy(readDecimalPin(argv[4]));
        constexpr CarrierKey128 carrier_key = KEY_00_0F;
        constexpr std::size_t kdf_metadata_size = 56;

        vBytes kdf_metadata(kdf_metadata_size);
        for (std::size_t index = 0; index < kdf_metadata.size(); ++index) {
            kdf_metadata[index] = static_cast<Byte>(index ^ 0x5aU);
        }
        const vBytes carrier_payload = makeTwitterEnvelope(
            kdf_metadata,
            payload,
            /*is_compressed=*/true);

        const TwitterPreparedCover cover = prepareTwitterCover(cover_bytes);
        {
            // The coefficient-domain fast path hands back its coefficients
            // instead of decoding the prepared image; they must be exactly
            // what that image decodes to.
            const auto fresh = twitter_steg_internal::readCoefficients(cover.jpeg);
            const auto& reused = cover.coefficients;
            if (reused.luminance != fresh.luminance ||
                reused.luminance_quantization != fresh.luminance_quantization ||
                reused.info.width != fresh.info.width ||
                reused.info.height != fresh.info.height ||
                reused.info.y_blocks_wide != fresh.info.y_blocks_wide ||
                reused.info.y_blocks_high != fresh.info.y_blocks_high ||
                reused.info.components != fresh.info.components ||
                reused.info.progressive != fresh.info.progressive ||
                reused.info.is_420 != fresh.info.is_420 ||
                reused.info.is_ycbcr != fresh.info.is_ycbcr ||
                reused.info.estimated_quality != fresh.info.estimated_quality) {
                throw std::runtime_error("prepared cover coefficients differ from its decode");
            }
        }
        if (carrier_payload.size() > cover.payload_capacity) {
            throw std::runtime_error("test payload exceeds carrier capacity");
        }
        const vBytes embedded = embedTwitterPayload(
            cover,
            carrier_key,
            carrier_payload);
        const CarrierKeys keys{.v3 = carrier_key, .has_v3 = true};
        const auto recovered = extractTwitterPayload(embedded, keys);
        if (!recovered || *recovered != carrier_payload) {
            throw std::runtime_error("carrier round trip mismatch");
        }
        const auto envelope = extractTwitterEnvelope(embedded, keys);
        if (!envelope ||
            envelope->kdf_metadata != kdf_metadata ||
            envelope->encrypted_data != payload ||
            !envelope->is_compressed) {
            throw std::runtime_error("carrier envelope round trip mismatch");
        }
        if (extractTwitterPayload(
                embedded, CarrierKeys{.v3 = KEY_01_10, .has_v3 = true})) {
            throw std::runtime_error("wrong carrier key was accepted");
        }
        // A JXSTEG4 image must not parse through the u64 (JXSTEG3 / JXSTEG2)
        // route under any u64 key -- including the interim big-endian embedding
        // of a u64 in the 128-bit key -- nor when the right 128-bit key is
        // present but has_v3 is false.
        for (const std::uint64_t legacy_key : {
                 std::uint64_t{0},
                 std::uint64_t{0x0001020304050607ULL},
                 std::uint64_t{0x0123456789abcdefULL},
                 std::uint64_t{0xffffffffffffffffULL}}) {
            if (extractTwitterPayload(
                    embedded,
                    CarrierKeys{.current = legacy_key, .legacy = legacy_key,
                                .v3 = carrier_key, .has_v3 = false})) {
                throw std::runtime_error("JXSTEG4 carrier was accepted on the u64 route");
            }
        }
        // The v9.1 JXSTEG3 fixture still parses on the u64 route under its
        // Argon2id key (the legacy CarrierPrf instance is bit-identical), but
        // not under a neighbouring key, and not when its own key is offered
        // only as the v9.0 (JXSTEG2) key: the format version is bound into
        // the keyed header.
        const std::uint64_t v9_1_key = deriveCarrierKeyFromPin(v9_1_pin);
        if (!extractTwitterEnvelope(
                v9_1_fixture, CarrierKeys{.current = v9_1_key, .legacy = 0})) {
            throw std::runtime_error("v9.1 JXSTEG3 carrier no longer parses under its key");
        }
        if (extractTwitterPayload(
                v9_1_fixture, CarrierKeys{.current = v9_1_key + 1U, .legacy = 0})) {
            throw std::runtime_error("wrong carrier key was accepted");
        }
        if (extractTwitterPayload(
                v9_1_fixture, CarrierKeys{.current = v9_1_key + 1U, .legacy = v9_1_key})) {
            throw std::runtime_error("current-format carrier was accepted under the legacy format");
        }
        // A v9.1 JXSTEG3 image never parses under a 128-bit key.
        for (const CarrierKey128& v3_key : {KEY_00_0F, KEY_01_10, CarrierKey128{}}) {
            if (extractTwitterPayload(
                    v9_1_fixture,
                    CarrierKeys{.current = v9_1_key, .legacy = v9_1_key,
                                .v3 = v3_key, .has_v3 = true})) {
                throw std::runtime_error("v9.1 carrier was accepted under a 128-bit key");
            }
        }
        checkLowAmplitudeCapacity();
        checkStagesHonourCancellation(cover);

        std::cout << "Twitter carrier unit test passed: "
                  << carrier_payload.size() << " framed bytes, capacity "
                  << cover.payload_capacity << " bytes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
