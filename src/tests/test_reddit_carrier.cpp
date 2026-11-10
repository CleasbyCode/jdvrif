// Unit test for the Reddit (RQSTEG3) coefficient carrier's keying:
// an RQSTEG3 round trip under a 128-bit key and wrong-key rejection.
//
// Usage: test_reddit_carrier <cover.jpg> <payload>

#include "reddit_steg.h"
#include "encryption_internal.h"

#include <sodium.h>

#include <array>
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
        if (argc != 3 || sodium_init() < 0) return 2;
        const vBytes cover_bytes = readFile(argv[1]);
        const vBytes payload = readFile(argv[2]);

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
            embedded, KEY_00_0F);
        require(envelope &&
                    envelope->kdf_metadata == kdf_metadata &&
                    envelope->encrypted_data == payload &&
                    envelope->is_compressed,
                "RQSTEG3 envelope round trip mismatch");

        // Wrong 128-bit key.
        require(!extractRedditEnvelope(
                    embedded, KEY_01_10),
                "wrong 128-bit carrier key was accepted");

        std::cout << "Reddit carrier unit test passed: "
                  << carrier_payload.size() << " framed bytes, capacity "
                  << cover.payload_capacity << " bytes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
