#include "encryption_internal.h"
#include "binary_io.h"
#include "file_utils.h"
#include "signal_utils.h"

#include <array>
#include <charconv>
#include <ranges>
#include <stdexcept>

void deriveKeyFromPin(Key& out_key, const SecurePin& pin, const Salt& salt, const KdfParams& params) {
    throwIfSignalCancellationRequested();
    // Local copy so the caller's SecurePin can outlive the KDF (encrypt still
    // needs the PIN for the user). Wipe this copy on every exit path.
    std::uint64_t pin_value = pin.value;

    std::array<char, 32> pin_buf{};
    auto [ptr, ec] = std::to_chars(pin_buf.data(), pin_buf.data() + pin_buf.size(), pin_value);
    if (ec != std::errc{}) {
        sodium_memzero(pin_buf.data(), pin_buf.size());
        sodium_memzero(&pin_value, sizeof(pin_value));
        throw std::runtime_error("KDF Error: Failed to encode recovery PIN.");
    }

    const auto pin_len = static_cast<unsigned long long>(ptr - pin_buf.data());
    const int rc = crypto_pwhash(
        out_key.data(),
        out_key.size(),
        pin_buf.data(),
        pin_len,
        salt.data(),
        static_cast<unsigned long long>(params.opslimit),
        params.memlimit,
        crypto_pwhash_ALG_ARGON2ID13
    );

    sodium_memzero(pin_buf.data(), pin_buf.size());
    sodium_memzero(&pin_value, sizeof(pin_value));

    throwIfSignalCancellationRequested();

    throwIf(rc != 0, "KDF Error: Unable to derive encryption key.");
}

[[nodiscard]] KdfMetadataVersion getKdfMetadataVersion(std::span<const Byte> data, std::size_t base_index) {
    if (!spanHasRange(data, base_index, KDF_METADATA_REGION_BYTES)) return KdfMetadataVersion::none;

    const auto header = std::span<const Byte>(
        data.data() + base_index + KDF_MAGIC_OFFSET,
        KDF_METADATA_MAGIC_V2.size()
    );

    const bool has_common_fields =
        data[base_index + KDF_ALG_OFFSET] == KDF_ALG_ARGON2ID13 &&
        data[base_index + KDF_SENTINEL_OFFSET] == KDF_SENTINEL;
    if (!has_common_fields) return KdfMetadataVersion::none;
    if (std::ranges::equal(header, KDF_METADATA_MAGIC_V2)) return KdfMetadataVersion::v2_secretstream;
    if (std::ranges::equal(header, KDF_METADATA_MAGIC_V3)) {
        return KdfMetadataVersion::v3_secretstream_authenticated_mode;
    }
    if (std::ranges::equal(header, KDF_METADATA_MAGIC_V4)) {
        return KdfMetadataVersion::v4_recorded_kdf_parameters;
    }
    return KdfMetadataVersion::none;
}

[[nodiscard]] KdfParams readKdfParams(
    std::span<const Byte> data,
    std::size_t base_index,
    KdfMetadataVersion version,
    const char* corrupt_error) {

    // V2/V3 predate the recorded fields; those bytes are random filler there,
    // so the costs are the ones jdvrif always used.
    if (version != KdfMetadataVersion::v4_recorded_kdf_parameters) {
        return KDF_PARAMS_CURRENT;
    }

    requireSpanRange(data, base_index + KDF_OPSLIMIT_OFFSET, KDF_COST_FIELD_BYTES, corrupt_error);
    requireSpanRange(data, base_index + KDF_MEMLIMIT_OFFSET, KDF_COST_FIELD_BYTES, corrupt_error);

    const KdfParams params{
        .opslimit = getValue(data, base_index + KDF_OPSLIMIT_OFFSET, KDF_COST_FIELD_BYTES),
        .memlimit = getValue(data, base_index + KDF_MEMLIMIT_OFFSET, KDF_COST_FIELD_BYTES),
    };

    // Tampering with these fields needs no separate integrity check: they feed
    // key derivation, so any change produces a different key and the very first
    // secretstream frame fails to authenticate. The range check exists for a
    // different reason -- these drive an allocation and a work loop that run
    // before the PIN can be shown to be wrong, so a hostile image does not get
    // to name them freely.
    throwIf(
        params.opslimit < KDF_OPSLIMIT_MIN_ACCEPTED || params.opslimit > KDF_OPSLIMIT_MAX_ACCEPTED ||
            params.memlimit < KDF_MEMLIMIT_MIN_ACCEPTED || params.memlimit > KDF_MEMLIMIT_MAX_ACCEPTED,
        "File Extraction Error: Encrypted file declares unsupported key-derivation parameters.");

    return params;
}

[[nodiscard]] SecurePin generateRecoveryPin() {
    SecurePin pin;
    while (pin.value == 0) {
        randombytes_buf(&pin.value, sizeof(pin.value));
    }
    return pin;
}
