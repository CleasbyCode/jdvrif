#include "encryption_internal.h"
#include "binary_io.h"
#include "file_utils.h"
#include "signal_utils.h"

#include <algorithm>
#include <array>

void deriveKeyFromRecoveryKey(Key& out_key, const RecoveryKey& recovery_key, const Salt& salt, const KdfParams& params) {
    throwIfSignalCancellationRequested();
    throwIf(!recovery_key.valid, "KDF Error: Failed to encode recovery KEY.");

    // The Argon2id password (D4) is the 16 secret bytes, read in place:
    // crypto_pwhash only reads it.
    const int rc = crypto_pwhash(
        out_key.data(),
        out_key.size(),
        reinterpret_cast<const char*>(recovery_key.secret.data()),
        static_cast<unsigned long long>(recovery_key.secret.size()),
        salt.data(),
        static_cast<unsigned long long>(params.opslimit),
        params.memlimit,
        crypto_pwhash_ALG_ARGON2ID13
    );

    throwIfSignalCancellationRequested();

    throwIf(rc != 0, "KDF Error: Unable to derive encryption key.");
}

[[nodiscard]] bool hasKdfMetadataV5(std::span<const Byte> data, std::size_t base_index) {
    if (!spanHasRange(data, base_index, KDF_METADATA_REGION_BYTES)) return false;
    return data[base_index + KDF_ALG_OFFSET] == KDF_ALG_ARGON2ID13 &&
        data[base_index + KDF_SENTINEL_OFFSET] == KDF_SENTINEL &&
        std::ranges::equal(
            data.subspan(base_index + KDF_MAGIC_OFFSET, KDF_METADATA_MAGIC_V5.size()),
            KDF_METADATA_MAGIC_V5);
}

[[nodiscard]] KdfParams readKdfParams(
    std::span<const Byte> data,
    std::size_t base_index) {

    requireSpanRange(data, base_index + KDF_OPSLIMIT_OFFSET, KDF_COST_FIELD_BYTES, CORRUPT_FILE_ERROR);
    requireSpanRange(data, base_index + KDF_MEMLIMIT_OFFSET, KDF_COST_FIELD_BYTES, CORRUPT_FILE_ERROR);

    const KdfParams params{
        .opslimit = getValue(data, base_index + KDF_OPSLIMIT_OFFSET, KDF_COST_FIELD_BYTES),
        .memlimit = getValue(data, base_index + KDF_MEMLIMIT_OFFSET, KDF_COST_FIELD_BYTES),
    };

    // Tampering with these fields needs no separate integrity check: they feed
    // key derivation, so any change produces a different key and the very first
    // secretstream frame fails to authenticate. The range check exists for a
    // different reason -- these drive an allocation and a work loop that run
    // before the KEY can be shown to be wrong, so a hostile image does not get
    // to name them freely.
    throwIf(
        params.opslimit < KDF_OPSLIMIT_MIN_ACCEPTED || params.opslimit > KDF_OPSLIMIT_MAX_ACCEPTED ||
            params.memlimit < KDF_MEMLIMIT_MIN_ACCEPTED || params.memlimit > KDF_MEMLIMIT_MAX_ACCEPTED,
        "File Extraction Error: Encrypted file declares unsupported key-derivation parameters.");

    // The carrier key is Argon2id over the same KEY with the fixed salt
    // CARRIER_V3_KDF_SALT. A recorded salt equal to it (with the default
    // costs) would make this file's stream key begin with the carrier key.
    // Conceal draws its salt at random, so only a crafted image names it.
    requireSpanRange(data, base_index + KDF_SALT_OFFSET, CARRIER_V3_KDF_SALT.size(), CORRUPT_FILE_ERROR);
    throwIf(
        std::ranges::equal(
            data.subspan(base_index + KDF_SALT_OFFSET, CARRIER_V3_KDF_SALT.size()), CARRIER_V3_KDF_SALT),
        "File Extraction Error: Encrypted file declares a reserved key-derivation salt.");

    return params;
}

void deriveCarrierKey128FromRecoveryKey(const RecoveryKey& recovery_key, CarrierKey128& carrier_key) {
    throwIf(!recovery_key.valid, "Internal Error: Carrier key requested without a KEY.");
    // Password = the 16 KEY bytes (deriveKeyFromRecoveryKey's 128-bit encoding), with
    // the v3 carrier salt and the pinned carrier costs. The Rust port derives
    // this identically in crypto.rs.
    Key derived{};
    WipePodGuard<Key> derived_wipe{derived};
    deriveKeyFromRecoveryKey(derived, recovery_key, CARRIER_V3_KDF_SALT, CARRIER_KDF_PARAMS);
    // Written straight into the caller's storage: a returned value can leave
    // a copy in this frame or in registers spilled to the stack.
    std::ranges::copy_n(derived.begin(), static_cast<std::ptrdiff_t>(carrier_key.size()), carrier_key.begin());
}

[[nodiscard]] RecoveryKey generateRecoveryKey() {
    std::array<Byte, 16> secret{};
    WipePodGuard<std::array<Byte, 16>> secret_wipe{secret};
    while (std::ranges::all_of(secret, [](Byte b) static { return b == 0; })) {
        randombytes_buf(secret.data(), secret.size());
    }
    return RecoveryKey::fromSecret(secret);
}
