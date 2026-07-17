#pragma once

#include "common.h"

enum class KdfMetadataVersion : Byte;

template<typename T>
struct SecureBuffer {
    T buf{};

    SecureBuffer() = default;

    ~SecureBuffer() {
        sodium_memzero(buf.data(), buf.size());
    }

    SecureBuffer(const SecureBuffer&) = delete;
    SecureBuffer& operator=(const SecureBuffer&) = delete;
    SecureBuffer(SecureBuffer&&) = delete;
    SecureBuffer& operator=(SecureBuffer&&) = delete;

    auto data() { return buf.data(); }
    auto size() const { return buf.size(); }
};

void buildBlueskySegments(vBytes& segment_vec, const vBytes& data_vec);

[[nodiscard]] std::size_t computeStreamEncryptedSizePrefixed(
    std::size_t input_plaintext_size,
    std::size_t prefix_plaintext_size);

[[nodiscard]] SecurePin encryptDataFileForBluesky(
    vBytes& segment_vec,
    const fs::path& data_path,
    std::size_t input_size,
    vString& platforms_vec,
    const std::string& data_filename,
    bool is_data_compressed);

[[nodiscard]] SecurePin encryptDataFileToFile(
    vBytes& segment_vec,
    const fs::path& data_path,
    std::size_t input_size,
    const std::string& data_filename,
    const fs::path& encrypted_output_path,
    bool is_data_compressed);

struct DecryptResult {
    std::string filename{};
    std::size_t output_size{0};
    bool failed{false};
};

// Validates KDF metadata (and Bluesky EXIF capacity), prompts for the recovery
// PIN, and derives the secretstream key + header. Intended to run *before*
// extracting ciphertext from the cover image so corrupt/oversized embeddings
// fail without multi-gigabyte staging I/O.
[[nodiscard]] KdfMetadataVersion prepareDecryptKeyFromMetadata(
    vBytes& metadata_vec,
    bool isBlueskyFile,
    Key& out_key,
    StreamHeader& out_stream_header);

// Streams encrypted_input_path through secretstream decrypt (and inflate when
// is_data_compressed) into stream_output_path using a key prepared above.
[[nodiscard]] DecryptResult decryptDataFileWithKey(
    const Key& key,
    const StreamHeader& stream_header,
    KdfMetadataVersion metadata_version,
    const fs::path& encrypted_input_path,
    const fs::path& stream_output_path,
    bool is_data_compressed);

// prepareDecryptKeyFromMetadata + decryptDataFileWithKey (PIN then decrypt).
// Prefer the split path in recover so ciphertext is extracted only after PIN.
[[nodiscard]] DecryptResult decryptDataFile(
    vBytes& metadata_vec,
    bool isBlueskyFile,
    const fs::path& encrypted_input_path,
    const fs::path& stream_output_path,
    bool is_data_compressed);
