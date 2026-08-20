#pragma once

#include "common.h"

#include <array>
#include <span>
#include <string>

inline constexpr std::size_t
    KDF_METADATA_REGION_BYTES = 56,
    KDF_MAGIC_OFFSET          = 0,
    KDF_ALG_OFFSET            = 4,
    KDF_SENTINEL_OFFSET       = 5,
    KDF_SALT_OFFSET           = 8,
    KDF_NONCE_OFFSET          = 24;

inline constexpr Byte KDF_ALG_ARGON2ID13 = 1;
inline constexpr Byte KDF_SENTINEL = 0xA5;

inline constexpr auto KDF_METADATA_MAGIC_V2 = std::to_array<Byte>({'K', 'D', 'F', '2'});
inline constexpr auto KDF_METADATA_MAGIC_V3 = std::to_array<Byte>({'K', 'D', 'F', '3'});

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
};

[[nodiscard]] constexpr Byte streamModeByte(bool is_compressed_payload) noexcept {
    return is_compressed_payload ? STREAM_MODE_ZLIB : STREAM_MODE_RAW;
}

[[nodiscard]] std::size_t computeStreamEncryptedSize(std::size_t plaintext_size);

// Reads pin.value for the KDF; does not wipe `pin` (caller may still need it).
// Any stack copy of the integer made inside is zeroed before return.
void deriveKeyFromPin(Key& out_key, const SecurePin& pin, const Salt& salt);
[[nodiscard]] KdfMetadataVersion getKdfMetadataVersion(std::span<const Byte> data, std::size_t base_index);
[[nodiscard]] SecurePin generateRecoveryPin();

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
