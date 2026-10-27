#include "encryption_internal.h"
#include "binary_io.h"
#include "file_utils.h"
#include "signal_utils.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <ranges>
#include <stdexcept>

void deriveKeyFromPin(Key& out_key, const SecurePin& pin, const Salt& salt, const KdfParams& params) {
    throwIfSignalCancellationRequested();
    throwIf(pin.kind == SecurePin::Kind::none, "KDF Error: Failed to encode recovery KEY/PIN.");

    // The Argon2id password (D4): the decimal string for a legacy PIN, exactly
    // as jdvrif v9.x derived it, or the 16 secret bytes for a 128-bit PIN.
    // Staged in a stack buffer so the caller's SecurePin can outlive the KDF
    // (encrypt still needs the PIN for the user); wiped on every exit path.
    std::array<char, 32> pin_buf{};
    WipePodGuard<std::array<char, 32>> pin_buf_wipe{pin_buf};
    std::size_t pin_len = 0;
    if (pin.kind == SecurePin::Kind::legacy_decimal) {
        std::uint64_t pin_value = pin.legacy_value;
        WipePodGuard<std::uint64_t> pin_value_wipe{pin_value};
        auto [ptr, ec] = std::to_chars(pin_buf.data(), pin_buf.data() + pin_buf.size(), pin_value);
        throwIf(ec != std::errc{}, "KDF Error: Failed to encode recovery PIN.");
        pin_len = static_cast<std::size_t>(ptr - pin_buf.data());
    } else {
        static_assert(sizeof(pin.secret) <= sizeof(pin_buf));
        std::memcpy(pin_buf.data(), pin.secret.data(), pin.secret.size());
        pin_len = pin.secret.size();
    }

    const int rc = crypto_pwhash(
        out_key.data(),
        out_key.size(),
        pin_buf.data(),
        static_cast<unsigned long long>(pin_len),
        salt.data(),
        static_cast<unsigned long long>(params.opslimit),
        params.memlimit,
        crypto_pwhash_ALG_ARGON2ID13
    );

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
    if (std::ranges::equal(header, KDF_METADATA_MAGIC_V5)) {
        return KdfMetadataVersion::v5_pin128;
    }
    return KdfMetadataVersion::none;
}

[[nodiscard]] KdfParams readKdfParams(
    std::span<const Byte> data,
    std::size_t base_index,
    KdfMetadataVersion version,
    const char* corrupt_error) {

    // V2/V3 predate the recorded fields; those bytes are random filler there,
    // so the costs are the fixed ones those formats were always made with.
    if (!recordsKdfParameters(version)) {
        return KDF_PARAMS_V2_V3;
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

namespace {
[[nodiscard]] std::uint64_t firstEightBytesBigEndian(std::span<const Byte> bytes) {
    return std::ranges::fold_left(
        bytes.first(sizeof(std::uint64_t)),
        std::uint64_t{0},
        [](std::uint64_t value, Byte byte) static { return (value << 8) | byte; });
}
} // namespace

void requirePinMatchesKdfVersion(const SecurePin& pin, KdfMetadataVersion version) {
    const bool image_needs_pin128 = (version == KdfMetadataVersion::v5_pin128);
    if (image_needs_pin128 && pin.kind != SecurePin::Kind::base58_128) {
        throw std::runtime_error(
            "File Extraction Error: This image was made by jdvrif v10 or later and needs its "
            "24-character recovery KEY.");
    }
    if (!image_needs_pin128 && pin.kind != SecurePin::Kind::legacy_decimal) {
        throw std::runtime_error(
            "File Extraction Error: This image was made before jdvrif v10 and needs the numeric "
            "recovery PIN it was created with.");
    }
}

namespace {
constexpr const char* CARRIER_KEY_KIND_ERROR = "Internal Error: Carrier key requested for the wrong KEY/PIN kind.";
} // namespace

std::uint64_t deriveCarrierKeyFromPin(const SecurePin& pin) {
    throwIf(pin.kind != SecurePin::Kind::legacy_decimal, CARRIER_KEY_KIND_ERROR);
    // Same password encoding (decimal PIN) and Argon2id primitive as the
    // envelope key, with the carrier's own fixed salt and pinned costs. The
    // Rust port derives this identically in crypto.rs.
    Key derived{};
    WipePodGuard<Key> derived_wipe{derived};
    deriveKeyFromPin(derived, pin, CARRIER_KDF_SALT, CARRIER_KDF_PARAMS);
    return firstEightBytesBigEndian(derived);
}

std::uint64_t deriveLegacyCarrierKeyFromPin(const SecurePin& pin) {
    throwIf(pin.kind != SecurePin::Kind::legacy_decimal, CARRIER_KEY_KIND_ERROR);
    // The v9.0 derivation, byte-for-byte. Domain separation, so it can never
    // collide with any other use of the PIN.
    static constexpr unsigned char DOMAIN[crypto_generichash_KEYBYTES] = {
        'j','d','v','r','i','f',' ','c','a','r','r','i','e','r',' ','v',
        '9',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
    };

    std::array<Byte, sizeof(std::uint64_t)> pin_bytes{};
    WipePodGuard<decltype(pin_bytes)> pin_bytes_wipe{pin_bytes};
    for (std::size_t i = 0; i < pin_bytes.size(); ++i) {
        pin_bytes[i] = static_cast<Byte>(pin.legacy_value >> (8 * (pin_bytes.size() - 1 - i)));
    }

    // A full-length digest, with the first 8 bytes taken as the key. Not an
    // 8-byte digest: BLAKE2b binds the output length into its parameter block,
    // so hashing to 8 bytes and truncating a 32-byte hash give different values,
    // and libsodium accepts an out-of-range 8 without complaint while stricter
    // wrappers reject it. Fixing the length keeps every implementation -- the
    // Rust port mirrors this in crypto.rs -- in agreement.
    std::array<Byte, crypto_generichash_BYTES> digest{};
    WipePodGuard<decltype(digest)> digest_wipe{digest};
    throwIf(
        crypto_generichash(
            digest.data(), digest.size(),
            pin_bytes.data(), pin_bytes.size(),
            DOMAIN, sizeof(DOMAIN)) != 0,
        "KDF Error: Unable to derive legacy carrier key.");

    return firstEightBytesBigEndian(digest);
}

CarrierKey128 deriveCarrierKey128FromPin(const SecurePin& pin) {
    throwIf(pin.kind != SecurePin::Kind::base58_128, CARRIER_KEY_KIND_ERROR);
    // Password = the 16 PIN bytes (deriveKeyFromPin's 128-bit encoding), with
    // the v3 carrier salt and the pinned carrier costs. The Rust port derives
    // this identically in crypto.rs.
    Key derived{};
    WipePodGuard<Key> derived_wipe{derived};
    deriveKeyFromPin(derived, pin, CARRIER_V3_KDF_SALT, CARRIER_KDF_PARAMS);
    CarrierKey128 key{};
    std::ranges::copy_n(derived.begin(), static_cast<std::ptrdiff_t>(key.size()), key.begin());
    return key;
}

CarrierKeys deriveCarrierKeysFromPin(const SecurePin& pin) {
    // D7: a 128-bit PIN can only belong to RQSTEG3 / JXSTEG4, a legacy PIN only
    // to the older formats, so exactly one Argon2id run is made either way.
    if (pin.kind == SecurePin::Kind::base58_128) {
        return CarrierKeys{.v3 = deriveCarrierKey128FromPin(pin), .has_v3 = true};
    }
    return CarrierKeys{
        .current = deriveCarrierKeyFromPin(pin),
        .legacy = deriveLegacyCarrierKeyFromPin(pin),
    };
}

[[nodiscard]] SecurePin generateRecoveryPin() {
    std::array<Byte, 16> secret{};
    WipePodGuard<std::array<Byte, 16>> secret_wipe{secret};
    while (std::ranges::all_of(secret, [](Byte b) static { return b == 0; })) {
        randombytes_buf(secret.data(), secret.size());
    }
    return SecurePin::modern(secret);
}
