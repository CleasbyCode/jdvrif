#pragma once

#include "common.h"

#include <array>
#include <span>
#include <string>

// Layout of the 56-byte KDF metadata region:
//   0..3   magic ("KDF2" / "KDF3" / "KDF4")
//   4      KDF algorithm id
//   5      sentinel
//   6..7   random filler
//   8..23  Argon2id salt
//   24..47 secretstream header
//   48..51 Argon2id opslimit, big-endian   (V4 only; random filler before V4)
//   52..55 Argon2id memlimit, big-endian   (V4 only; random filler before V4)
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

inline constexpr auto KDF_METADATA_MAGIC_V2 = std::to_array<Byte>({'K', 'D', 'F', '2'});
inline constexpr auto KDF_METADATA_MAGIC_V3 = std::to_array<Byte>({'K', 'D', 'F', '3'});
inline constexpr auto KDF_METADATA_MAGIC_V4 = std::to_array<Byte>({'K', 'D', 'F', '4'});

// V3 authenticates the payload interpretation on every secretstream frame.
// The JPEG metadata remains readable for routing, but changing its compression
// marker can no longer make valid ciphertext be decoded with different rules.
inline constexpr Byte STREAM_MODE_ZLIB = 1;
inline constexpr Byte STREAM_MODE_RAW  = 2;

inline constexpr std::size_t STREAM_CHUNK_SIZE = 1 * 1024 * 1024;
inline constexpr std::size_t STREAM_FRAME_LEN_BYTES = 4;

enum class KdfMetadataVersion : Byte {
    none = 0,
    v2_secretstream = 2,
    v3_secretstream_authenticated_mode = 3,
    // V4 is V3 plus an explicit record of the Argon2id cost parameters. V2/V3
    // left them implicit, so retuning them would have turned every existing
    // image into an "invalid PIN" -- with V4 the image states what it was
    // derived with and old costs stay readable.
    v4_recorded_kdf_parameters = 4,
};

// Argon2id cost parameters used to derive a key from the recovery PIN.
struct KdfParams {
    std::uint64_t opslimit{crypto_pwhash_OPSLIMIT_INTERACTIVE};
    std::size_t   memlimit{crypto_pwhash_MEMLIMIT_INTERACTIVE};
};

// Costs jdvrif mints today. V2/V3 images carry no record and are assumed to
// have used exactly these -- which they did, so changing them is safe now.
inline constexpr KdfParams KDF_PARAMS_CURRENT{
    .opslimit = crypto_pwhash_OPSLIMIT_INTERACTIVE,
    .memlimit = crypto_pwhash_MEMLIMIT_INTERACTIVE,
};

// Accepted range when reading costs back out of an image. The floors are
// libsodium's own minimums; the ceilings bound the work a hostile image can
// demand, since Argon2id allocates memlimit bytes and runs opslimit passes
// over them before the PIN is known to be wrong.
inline constexpr std::uint64_t KDF_OPSLIMIT_MIN_ACCEPTED = crypto_pwhash_OPSLIMIT_MIN;
inline constexpr std::uint64_t KDF_OPSLIMIT_MAX_ACCEPTED = 16;
inline constexpr std::size_t   KDF_MEMLIMIT_MIN_ACCEPTED = crypto_pwhash_MEMLIMIT_MIN;
inline constexpr std::size_t   KDF_MEMLIMIT_MAX_ACCEPTED = 512ULL * 1024 * 1024;

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

// True for the versions that bind the payload interpretation into every
// secretstream frame as associated data.
[[nodiscard]] constexpr bool authenticatesStreamMode(KdfMetadataVersion version) noexcept {
    return version == KdfMetadataVersion::v3_secretstream_authenticated_mode ||
           version == KdfMetadataVersion::v4_recorded_kdf_parameters;
}

[[nodiscard]] constexpr bool isSupportedKdfMetadataVersion(KdfMetadataVersion version) noexcept {
    return version == KdfMetadataVersion::v2_secretstream ||
           authenticatesStreamMode(version);
}

[[nodiscard]] std::size_t computeStreamEncryptedSize(std::size_t plaintext_size);

// Reads pin.value for the KDF; does not wipe `pin` (caller may still need it).
// Any stack copy of the integer made inside is zeroed before return.
void deriveKeyFromPin(Key& out_key, const SecurePin& pin, const Salt& salt, const KdfParams& params);
[[nodiscard]] KdfMetadataVersion getKdfMetadataVersion(std::span<const Byte> data, std::size_t base_index);
// Cost parameters this image says its key was derived with. V2/V3 carry no
// record, so they resolve to KDF_PARAMS_CURRENT; V4 reads the stored pair and
// rejects anything outside the accepted range.
[[nodiscard]] KdfParams readKdfParams(
    std::span<const Byte> data,
    std::size_t base_index,
    KdfMetadataVersion version,
    const char* corrupt_error);
[[nodiscard]] SecurePin generateRecoveryPin();

// Key for the Reddit carrier's coefficient permutation and whitening bits.
//
// Deliberately not the Argon2 encryption key: that derivation needs the salt,
// and the salt lives inside the envelope which this key is required to locate.
// So it is a cheap, salt-free hash of the PIN alone, domain-separated from
// everything else. It defends position secrecy, not confidentiality -- the
// payload underneath is still secretstream-encrypted under the Argon2 key -- so
// it does not need to be slow, and being fast keeps a wrong PIN cheap to reject.
[[nodiscard]] std::uint64_t deriveCarrierKeyFromPin(const SecurePin& pin);

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

[[nodiscard]] bool decryptWithSecretStreamFileInputToFileExtractingFilename(
    const fs::path& encrypted_input_path,
    const Key& key,
    const std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header,
    KdfMetadataVersion metadata_version,
    bool is_compressed_payload,
    const fs::path& output_path,
    std::size_t& output_size,
    std::string& decrypted_filename);
