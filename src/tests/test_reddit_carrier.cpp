// Unit test for the Reddit (RQSTEG) coefficient carrier's keying and format
// routing: an RQSTEG3 round trip under a 128-bit key, wrong-key and
// wrong-key-kind rejection, and the v9.1 RQSTEG2 fixture staying readable on
// the u64 route only.
//
// Usage: test_reddit_carrier <cover.jpg> <payload> <v9.1 reddit.jpg> <v9.1 reddit.pin>

#include "reddit_steg.h"
#include "encryption_internal.h"

#include <sodium.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {

[[nodiscard]] vBytes readFile(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("failed to open test input");
    return vBytes(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
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

[[nodiscard]] constexpr CarrierKey128 sequentialKey(Byte first) {
    CarrierKey128 key{};
    for (std::size_t index = 0; index < key.size(); ++index) {
        key[index] = static_cast<Byte>(first + index);
    }
    return key;
}

constexpr CarrierKey128 KEY_00_0F = sequentialKey(0x00);
constexpr CarrierKey128 KEY_01_10 = sequentialKey(0x01);

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 5 || sodium_init() < 0) return 2;
        const vBytes cover_bytes = readFile(argv[1]);
        const vBytes payload = readFile(argv[2]);
        const vBytes v9_1_fixture = readFile(argv[3]);
        const SecurePin v9_1_pin = SecurePin::legacy(readDecimalPin(argv[4]));

        vBytes kdf_metadata(KDF_METADATA_REGION_BYTES);
        for (std::size_t index = 0; index < kdf_metadata.size(); ++index) {
            kdf_metadata[index] = static_cast<Byte>(index ^ 0x5aU);
        }
        const vBytes carrier_payload = makeRedditEnvelope(
            kdf_metadata, payload, /*is_compressed=*/true);

        const RedditPreparedCover cover = prepareRedditCover(cover_bytes);
        require(carrier_payload.size() <= cover.payload_capacity,
                "test payload exceeds carrier capacity");
        const vBytes embedded = embedRedditPayload(cover, KEY_00_0F, carrier_payload);

        // Round trip under the 128-bit key.
        const auto envelope = extractRedditEnvelope(
            embedded, CarrierKeys{.v3 = KEY_00_0F, .has_v3 = true});
        require(envelope &&
                    envelope->kdf_metadata == kdf_metadata &&
                    envelope->encrypted_data == payload &&
                    envelope->is_compressed,
                "RQSTEG3 envelope round trip mismatch");

        // Wrong 128-bit key.
        require(!extractRedditEnvelope(
                    embedded, CarrierKeys{.v3 = KEY_01_10, .has_v3 = true}),
                "wrong 128-bit carrier key was accepted");

        // Wrong key kind: the u64 (RQSTEG2 / RQSTEG1) route never reads an
        // RQSTEG3 image -- not under any u64 key (including the interim
        // big-endian embedding of a u64 in the 128-bit key), and not when the
        // right 128-bit key is present but has_v3 is false.
        for (const std::uint64_t legacy_key : {
                 std::uint64_t{0},
                 std::uint64_t{0x0001020304050607ULL},
                 std::uint64_t{0x0123456789abcdefULL},
                 std::uint64_t{0xffffffffffffffffULL}}) {
            require(!extractRedditEnvelope(
                        embedded,
                        CarrierKeys{.current = legacy_key, .legacy = legacy_key,
                                    .v3 = KEY_00_0F, .has_v3 = false}),
                    "RQSTEG3 carrier was accepted on the u64 route");
        }

        // The v9.1 RQSTEG2 fixture: still readable on the u64 route under its
        // Argon2id key (the legacy CarrierPrf instance is bit-identical), not
        // under a neighbouring key, not when its key is offered only as the
        // v9.0 (RQSTEG1) key, and never under a 128-bit key.
        const std::uint64_t v9_1_key = deriveCarrierKeyFromPin(v9_1_pin);
        require(extractRedditEnvelope(
                    v9_1_fixture, CarrierKeys{.current = v9_1_key, .legacy = 0}).has_value(),
                "v9.1 RQSTEG2 carrier no longer parses under its key");
        require(!extractRedditEnvelope(
                    v9_1_fixture, CarrierKeys{.current = v9_1_key + 1U, .legacy = 0}),
                "wrong u64 carrier key was accepted");
        require(!extractRedditEnvelope(
                    v9_1_fixture, CarrierKeys{.current = v9_1_key + 1U, .legacy = v9_1_key}),
                "RQSTEG2 carrier was accepted under the RQSTEG1 format");
        for (const CarrierKey128& v3_key : {KEY_00_0F, KEY_01_10, CarrierKey128{}}) {
            require(!extractRedditEnvelope(
                        v9_1_fixture,
                        CarrierKeys{.current = v9_1_key, .legacy = v9_1_key,
                                    .v3 = v3_key, .has_v3 = true}),
                    "v9.1 carrier was accepted under a 128-bit key");
        }

        std::cout << "Reddit carrier unit test passed: "
                  << carrier_payload.size() << " framed bytes, capacity "
                  << cover.payload_capacity << " bytes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
