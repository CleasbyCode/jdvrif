#pragma once

#include "carrier_common.h"
#include "cipher_source.h"
#include "common.h"

#include <array>
#include <span>
#include <string>

// Layout of the 56-byte KDF metadata region:
//   0..3   magic ("KDF5")
//   4      KDF algorithm id
//   5      sentinel
//   6..7   random filler
//   8..23  Argon2id salt
//   24..47 secretstream header
//   48..51 Argon2id opslimit, big-endian
//   52..55 Argon2id memlimit, big-endian
inline constexpr std::size_t
    KDF_METADATA_REGION_BYTES = 56,
    KDF_MAGIC_OFFSET          = 0,
    KDF_ALG_OFFSET            = 4,
    KDF_SENTINEL_OFFSET       = 5,
    KDF_SALT_OFFSET           = 8,
    KDF_NONCE_OFFSET          = 24,
    KDF_OPSLIMIT_OFFSET       = 48,
    KDF_MEMLIMIT_OFFSET       = 52,
    KDF_COST_FIELD_BYTES      = 4;

inline constexpr Byte KDF_ALG_ARGON2ID13 = 1;
inline constexpr Byte KDF_SENTINEL = 0xA5;

inline constexpr auto KDF_METADATA_MAGIC_V5 = std::to_array<Byte>({'K', 'D', 'F', '5'});

// The payload interpretation is authenticated on every secretstream frame.
inline constexpr Byte STREAM_MODE_ZLIB = 1;
inline constexpr Byte STREAM_MODE_RAW  = 2;

inline constexpr std::size_t STREAM_CHUNK_SIZE = 1 * 1024 * 1024;
inline constexpr std::size_t STREAM_FRAME_LEN_BYTES = 4;


// Argon2id cost parameters used to derive a key from the recovery KEY.
struct KdfParams {
    std::uint64_t opslimit{crypto_pwhash_OPSLIMIT_INTERACTIVE};
    std::size_t   memlimit{crypto_pwhash_MEMLIMIT_INTERACTIVE};
};

// Costs jdvrif mints today. Every image records them, so they can be retuned.
inline constexpr KdfParams KDF_PARAMS_CURRENT{
    .opslimit = crypto_pwhash_OPSLIMIT_INTERACTIVE,
    .memlimit = crypto_pwhash_MEMLIMIT_INTERACTIVE,
};

// Accepted range when reading costs back out of an image. The floors are
// libsodium's own minimums; the ceilings bound the work a hostile image can
// demand, since Argon2id allocates memlimit bytes and runs opslimit passes
// over them before the KEY is known to be wrong.
inline constexpr std::uint64_t KDF_OPSLIMIT_MIN_ACCEPTED = crypto_pwhash_OPSLIMIT_MIN;
inline constexpr std::uint64_t KDF_OPSLIMIT_MAX_ACCEPTED = 16;
inline constexpr std::size_t   KDF_MEMLIMIT_MIN_ACCEPTED = crypto_pwhash_MEMLIMIT_MIN;
inline constexpr std::size_t   KDF_MEMLIMIT_MAX_ACCEPTED = 512uz * 1024 * 1024;

static_assert(KDF_PARAMS_CURRENT.opslimit >= KDF_OPSLIMIT_MIN_ACCEPTED &&
              KDF_PARAMS_CURRENT.opslimit <= KDF_OPSLIMIT_MAX_ACCEPTED &&
              KDF_PARAMS_CURRENT.memlimit >= KDF_MEMLIMIT_MIN_ACCEPTED &&
              KDF_PARAMS_CURRENT.memlimit <= KDF_MEMLIMIT_MAX_ACCEPTED,
              "jdvrif must be able to read back the KDF costs it writes.");
static_assert(KDF_PARAMS_CURRENT.memlimit <= 0xFFFFFFFFULL,
              "KDF cost fields are 32-bit.");

[[nodiscard]] constexpr Byte streamModeByte(bool is_compressed_payload) noexcept {
    return is_compressed_payload ? STREAM_MODE_ZLIB : STREAM_MODE_RAW;
}

[[nodiscard]] std::size_t computeStreamEncryptedSize(std::size_t plaintext_size);

// The Argon2id password is the KEY's 16 raw secret bytes (D4). Does not wipe `recovery_key` (caller may still
// need it); any stack copy made inside is zeroed before return.
void deriveKeyFromRecoveryKey(Key& out_key, const RecoveryKey& recovery_key, const Salt& salt, const KdfParams& params);
[[nodiscard]] bool hasKdfMetadataV5(std::span<const Byte> data, std::size_t base_index);
// Cost parameters this image says its key was derived with; rejects anything
// outside the accepted range.
[[nodiscard]] KdfParams readKdfParams(
    std::span<const Byte> data,
    std::size_t base_index,
    const char* corrupt_error);
// A fresh 128-bit recovery KEY: 16 random bytes, never all zero (D1).
[[nodiscard]] RecoveryKey generateRecoveryKey();

// Argon2id costs for the carrier key. Part of the RQSTEG3 / JXSTEG4 formats:
// nothing in the image records them (the key is needed to read anything), so
// they are pinned here rather than tracking KDF_PARAMS_CURRENT. Changing them
// requires a new carrier format version. Numerically equal to libsodium's
// INTERACTIVE preset today, written out so a libsodium change cannot move them.
inline constexpr KdfParams CARRIER_KDF_PARAMS{
    .opslimit = 2,
    .memlimit = 64uz * 1024 * 1024,
};

// Fixed Argon2id salt for the 128-bit carrier key (RQSTEG3 / JXSTEG4; see
// deriveCarrierKey128FromRecoveryKey), with CARRIER_KDF_PARAMS.
inline constexpr Salt CARRIER_V3_KDF_SALT{
    'j', 'd', 'v', 'r', 'i', 'f', '-', 'c', 'a', 'r', 'r', 'i', 'e', 'r', 'v', '3'
};

// The 128-bit carrier key of RQSTEG3 / JXSTEG4 (D5): the first 16 bytes of
// Argon2id(password = the KEY's 16 secret bytes, salt CARRIER_V3_KDF_SALT,
// CARRIER_KDF_PARAMS, 32-byte output). 128-bit KEYs only; throws otherwise.
[[nodiscard]] CarrierKey128 deriveCarrierKey128FromRecoveryKey(const RecoveryKey& recovery_key);

void encryptFileWithSecretStreamPrefixed(
    const fs::path& data_path,
    std::size_t input_size,
    std::span<const Byte> prefix_plaintext,
    Byte authenticated_mode,
    const Key& key,
    std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header,
    vBytes& output_vec);

void encryptFileWithSecretStreamPrefixedToFile(
    const fs::path& data_path,
    std::size_t input_size,
    std::span<const Byte> prefix_plaintext,
    Byte authenticated_mode,
    const Key& key,
    std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header,
    const fs::path& output_path);

[[nodiscard]] bool decryptSecretStreamToFileExtractingFilename(
    CipherSource& encrypted_input,
    const Key& key,
    const std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header,
    bool is_compressed_payload,
    const fs::path& output_path,
    std::size_t& output_size,
    std::size_t& stored_size,
    std::string& decrypted_filename);
