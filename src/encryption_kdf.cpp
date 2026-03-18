#include "encryption_internal.h"
#include "file_utils.h"

#include <array>
#include <charconv>
#include <ranges>
#include <stdexcept>

namespace {
constexpr auto KDF_METADATA_MAGIC_V2 = std::to_array<Byte>({'K', 'D', 'F', '2'});
}

void deriveKeyFromPin(Key& out_key, std::size_t pin, const Salt& salt) {
    std::array<char, 32> pin_buf{};
    auto [ptr, ec] = std::to_chars(pin_buf.data(), pin_buf.data() + pin_buf.size(), pin);
    if (ec != std::errc{}) {
        sodium_memzero(pin_buf.data(), pin_buf.size());
        throw std::runtime_error("KDF Error: Failed to encode recovery PIN.");
    }

    const auto pin_len = static_cast<unsigned long long>(ptr - pin_buf.data());
    const int rc = crypto_pwhash(
        out_key.data(),
        out_key.size(),
        pin_buf.data(),
        pin_len,
        salt.data(),
        crypto_pwhash_OPSLIMIT_INTERACTIVE,
        crypto_pwhash_MEMLIMIT_INTERACTIVE,
        crypto_pwhash_ALG_ARGON2ID13
    );

    sodium_memzero(pin_buf.data(), pin_buf.size());

    if (rc != 0) {
        throw std::runtime_error("KDF Error: Unable to derive encryption key.");
    }
}

[[nodiscard]] KdfMetadataVersion getKdfMetadataVersion(std::span<const Byte> data, std::size_t base_index) {
    if (!spanHasRange(data, base_index, KDF_METADATA_REGION_BYTES)) {
        return KdfMetadataVersion::none;
    }

    const auto header = std::span<const Byte>(
        data.data() + base_index + KDF_MAGIC_OFFSET,
        KDF_METADATA_MAGIC_V2.size()
    );

    const bool has_common_fields =
        data[base_index + KDF_ALG_OFFSET] == KDF_ALG_ARGON2ID13 &&
        data[base_index + KDF_SENTINEL_OFFSET] == KDF_SENTINEL;
    if (!has_common_fields) {
        return KdfMetadataVersion::none;
    }
    if (std::ranges::equal(header, KDF_METADATA_MAGIC_V2)) {
        return KdfMetadataVersion::v2_secretstream;
    }
    return KdfMetadataVersion::none;
}

[[nodiscard]] std::size_t generateRecoveryPin() {
    std::size_t pin = 0;
    while (pin == 0) {
        randombytes_buf(&pin, sizeof(pin));
    }
    return pin;
}
