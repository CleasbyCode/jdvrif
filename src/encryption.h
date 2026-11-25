#pragma once

#include "cipher_source.h"
#include "common.h"

#include <functional>
#include <span>

class StagingFile;

// Largest ciphertext buildBlueskySegments can actually pack (EXIF + two
// Photoshop datasets + the base64 XMP overflow), intersected with the program's
// Bluesky cap. Conceal checks the encrypted payload against this so an
// over-large one is refused up front instead of failing deep inside the packer;
// recovery enforces the same bound in validateBlueskyExifCapacity.
[[nodiscard]] std::size_t maxBlueskyEmbeddedCipherCapacity();

// How buildBlueskySegments spreads a ciphertext of this size: the EXIF
// segment first, then the Photoshop segment's two datasets, then the XMP
// overflow (base64). Each share is in ciphertext bytes; they sum to the size.
struct BlueskySegmentSplit {
    std::size_t exif{0};
    std::size_t first_dataset{0};
    std::size_t second_dataset{0};
    std::size_t xmp{0};

    [[nodiscard]] bool usesPhotoshop() const noexcept { return first_dataset != 0; }
    [[nodiscard]] bool usesXmp() const noexcept { return xmp != 0; }
};
[[nodiscard]] BlueskySegmentSplit blueskySegmentSplit(std::size_t encrypted_size);

[[nodiscard]] std::size_t computeStreamEncryptedSizePrefixed(
    std::size_t input_plaintext_size,
    std::size_t prefix_plaintext_size);

[[nodiscard]] RecoveryKey encryptDataFileForBluesky(
    vBytes& segment_vec,
    const fs::path& data_path,
    std::size_t input_size,
    const std::string& data_filename,
    bool is_data_compressed);

// Default mode. The ciphertext is not collected: once the KDF metadata is in
// segment_vec, `begin_output` is called (the template can now be written),
// and the framed ciphertext then goes to `append_encrypted` piece by piece.
// Its total is exactly computeStreamEncryptedSizePrefixed(input_size,
// 1 + data_filename.size()).
[[nodiscard]] RecoveryKey encryptDataFileToSink(
    vBytes& segment_vec,
    const fs::path& data_path,
    std::size_t input_size,
    const std::string& data_filename,
    bool is_data_compressed,
    const std::function<void()>& begin_output,
    const std::function<void(std::span<const Byte>)>& append_encrypted);

// Metadata-stripping carriers (Reddit, X-Twitter) cannot preserve APP/ICC
// metadata, so the 56-byte KDF block comes back separately for their
// DCT-carried envelope. Their small carrier capacity bounds the ciphertext, so
// it is returned in memory rather than staged on disk.
[[nodiscard]] RecoveryKey encryptDataFileForCarrier(
    vBytes& kdf_metadata,
    vBytes& encrypted_vec,
    const fs::path& data_path,
    std::size_t input_size,
    const std::string& data_filename,
    bool is_data_compressed);

// Wipes the recovered filename on destruction, so it is cleared on every
// path -- including the refusals (unsafe filename, empty output) that come
// after decryption succeeds. Callers read it in place rather than move it
// out. Not move-assignable: assignment would free the old name unwiped.
struct DecryptResult {
    DecryptResult() = default;
    ~DecryptResult() { wipeContainerCapacity(filename); }
    DecryptResult(DecryptResult&&) = default;
    DecryptResult& operator=(DecryptResult&&) = delete;
    DecryptResult(const DecryptResult&) = delete;
    DecryptResult& operator=(const DecryptResult&) = delete;

    std::string filename{};
    std::size_t output_size{0};
    // Payload bytes as stored in the image: the zlib stream when compressed,
    // otherwise output_size.
    std::size_t stored_size{0};
    bool failed{false};
};

// Validates KDF metadata (and Bluesky EXIF capacity), prompts for the recovery
// KEY, and derives the secretstream key + header. Intended to run *before*
// extracting ciphertext from the cover image so corrupt/oversized embeddings
// fail without multi-gigabyte staging I/O. on_key_entered runs between the
// prompt and the key derivation (recover starts its progress step there).
void prepareDecryptKeyFromMetadata(
    std::span<const Byte> metadata_vec,
    bool isBlueskyFile,
    Key& out_key,
    StreamHeader& out_stream_header,
    const std::function<void()>& on_key_entered = {});

// Metadata-stripping coefficient carriers carry only the compact KDF block
// rather than an APP/EXIF template. Validates it and derives the existing
// secretstream key/header pair from a KEY the caller already collected. Carrier
// positions derive from that KEY, so recovery must obtain it before it can
// locate anything and must not prompt a second time here.
void prepareDecryptKeyFromKdfMetadata(
    std::span<const Byte> kdf_metadata,
    RecoveryKey& recovery_key,
    Key& out_key,
    StreamHeader& out_stream_header);

// Streams encrypted_input through secretstream decrypt (and inflate when
// is_data_compressed) into stream_stage using a key prepared above.
[[nodiscard]] DecryptResult decryptDataFileWithKey(
    const Key& key,
    const StreamHeader& stream_header,
    CipherSource& encrypted_input,
    const StagingFile& stream_stage,
    bool is_data_compressed);
