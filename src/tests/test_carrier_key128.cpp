// Independent C++ confirmation of the RQSTEG3 / JXSTEG4 carrier key vector.
// The Rust port asserts the same value in rust_port/src/crypto.rs.

#include "common.h"
#include "encryption_internal.h"

#include <sodium.h>

#include <array>
#include <cstdio>
#include <cstring>

int main() {
    if (sodium_init() < 0) {
        std::fprintf(stderr, "FAIL: sodium_init\n");
        return 1;
    }
    std::array<Byte, 16> secret{};
    for (std::size_t i = 0; i < secret.size(); ++i) secret[i] = static_cast<Byte>(i);
    const SecurePin pin = SecurePin::modern(secret);

    // Argon2id(PIN bytes 00..0f, "jdvrif-carrierv3", ops 2, mem 64 MiB)[0..16].
    constexpr std::array<Byte, 16> expected{
        0xf3, 0x55, 0x9f, 0x3f, 0x62, 0x1b, 0xca, 0x57,
        0xb9, 0x55, 0xa0, 0xc2, 0xa7, 0x72, 0xc7, 0xe1};

    const CarrierKey128 key = deriveCarrierKey128FromPin(pin);
    if (std::memcmp(key.data(), expected.data(), expected.size()) != 0) {
        std::fprintf(stderr, "FAIL: deriveCarrierKey128FromPin vector mismatch\n");
        return 1;
    }
    std::puts("carrier key128 vector OK");
    return 0;
}
