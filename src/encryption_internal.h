#pragma once

#include "common.h"

#include <array>
#include <span>
#include <string>

inline constexpr std::size_t KDF_METADATA_REGION_BYTES = 56, KDF_MAGIC_OFFSET = 0, KDF_ALG_OFFSET = 4, KDF_SENTINEL_OFFSET = 5, KDF_SALT_OFFSET = 8, KDF_NONCE_OFFSET = 24;

inline constexpr Byte KDF_ALG_ARGON2ID13 = 1, KDF_SENTINEL = 0xA5;

inline constexpr std::size_t STREAM_CHUNK_SIZE = 1 * 1024 * 1024, STREAM_FRAME_LEN_BYTES = 4;

enum class KdfMetadataVersion : Byte { none = 0, v2_secretstream = 2 };

[[nodiscard]] std::size_t computeStreamEncryptedSize(std::size_t plaintext_size);
[[nodiscard]] std::size_t computeStreamEncryptedSizePrefixed(std::size_t input_plaintext_size, std::size_t prefix_plaintext_size);
void deriveKeyFromPin(Key& out_key, std::uint64_t pin, const Salt& salt);
[[nodiscard]] KdfMetadataVersion getKdfMetadataVersion(std::span<const Byte> data, std::size_t base_index);
[[nodiscard]] std::uint64_t generateRecoveryPin();
void encryptWithSecretStream(vBytes& data_vec, const Key& key, std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header);
void encryptFileWithSecretStreamPrefixed(const fs::path& data_path, std::size_t input_size, std::span<const Byte> prefix_plaintext, const Key& key, std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header, vBytes& output_vec);
void encryptFileWithSecretStreamPrefixedToFile(const fs::path& data_path, std::size_t input_size, std::span<const Byte> prefix_plaintext, const Key& key, std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header, const fs::path& output_path);
[[nodiscard]] bool decryptWithSecretStream(vBytes& data_vec, const Key& key, const std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header);
[[nodiscard]] bool decryptWithSecretStreamToFile(std::span<const Byte> encrypted_data, const Key& key, const std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header, const fs::path& output_path, std::size_t& output_size);
[[nodiscard]] bool decryptWithSecretStreamFileInputToFile(const fs::path& encrypted_input_path, const Key& key, const std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header, const fs::path& output_path, std::size_t& output_size);
[[nodiscard]] bool decryptWithSecretStreamToFileExtractingFilename(std::span<const Byte> encrypted_data, const Key& key, const std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header, bool is_compressed_payload, const fs::path& output_path, std::size_t& output_size, std::string& decrypted_filename);
[[nodiscard]] bool decryptWithSecretStreamFileInputToFileExtractingFilename(const fs::path& encrypted_input_path, const Key& key, const std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header, bool is_compressed_payload, const fs::path& output_path, std::size_t& output_size, std::string& decrypted_filename);
