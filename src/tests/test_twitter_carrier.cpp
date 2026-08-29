#include "twitter_steg.h"

#include <sodium.h>

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

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3 || sodium_init() < 0) return 2;
        const vBytes cover_bytes = readFile(argv[1]);
        const vBytes payload = readFile(argv[2]);
        constexpr std::uint64_t carrier_key = 0x0123456789abcdefULL;
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
        if (carrier_payload.size() > cover.payload_capacity) {
            throw std::runtime_error("test payload exceeds carrier capacity");
        }
        const vBytes embedded = embedTwitterPayload(
            cover,
            carrier_key,
            carrier_payload);
        const auto recovered = extractTwitterPayload(embedded, carrier_key);
        if (!recovered || *recovered != carrier_payload) {
            throw std::runtime_error("carrier round trip mismatch");
        }
        const auto envelope = extractTwitterEnvelope(embedded, carrier_key);
        if (!envelope ||
            envelope->kdf_metadata != kdf_metadata ||
            envelope->encrypted_data != payload ||
            !envelope->is_compressed) {
            throw std::runtime_error("carrier envelope round trip mismatch");
        }
        if (extractTwitterPayload(embedded, carrier_key + 1U)) {
            throw std::runtime_error("wrong carrier key was accepted");
        }

        std::cout << "Twitter carrier unit test passed: "
                  << carrier_payload.size() << " framed bytes, capacity "
                  << cover.payload_capacity << " bytes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
