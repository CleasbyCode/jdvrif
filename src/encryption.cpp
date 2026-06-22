#include "encryption.h"
#include "encryption_internal.h"
#include "binary_io.h"
#include "embedded_layout.h"
#include "file_utils.h"
#include "pin_input.h"
#include "segmentation.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <print>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
inline constexpr std::size_t MAX_FILENAME_PREFIX_BYTES =
    static_cast<std::size_t>(std::numeric_limits<Byte>::max());

struct FilenamePrefix {
    std::array<Byte, MAX_FILENAME_PREFIX_BYTES> bytes{};
    std::size_t size{0};

    [[nodiscard]] std::span<const Byte> view() const noexcept {
        return std::span<const Byte>(bytes.data(), size);
    }
};

[[nodiscard]] FilenamePrefix makeFilenamePrefix(const std::string& data_filename) {
    if (data_filename.empty() ||
        data_filename.size() > static_cast<std::size_t>(std::numeric_limits<Byte>::max() - 1)) {
        throw std::runtime_error("Data File Error: Invalid data filename length.");
    }

    FilenamePrefix filename_prefix;
    filename_prefix.size = 1 + data_filename.size();
    filename_prefix.bytes[0] = static_cast<Byte>(data_filename.size());
    std::memcpy(filename_prefix.bytes.data() + 1, data_filename.data(), data_filename.size());
    return filename_prefix;
}

void storeKdfMetadata(
    vBytes& segment_vec,
    std::size_t kdf_metadata_index,
    const Salt& salt,
    const std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& stream_header) {

    randombytes_buf(segment_vec.data() + kdf_metadata_index, KDF_METADATA_REGION_BYTES);

    std::ranges::copy(
        KDF_METADATA_MAGIC_V2,
        segment_vec.begin() + static_cast<std::ptrdiff_t>(kdf_metadata_index + KDF_MAGIC_OFFSET));
    segment_vec[kdf_metadata_index + KDF_ALG_OFFSET] = KDF_ALG_ARGON2ID13;
    segment_vec[kdf_metadata_index + KDF_SENTINEL_OFFSET] = KDF_SENTINEL;
    std::ranges::copy(
        salt,
        segment_vec.begin() + static_cast<std::ptrdiff_t>(kdf_metadata_index + KDF_SALT_OFFSET));
    std::ranges::copy(
        stream_header,
        segment_vec.begin() + static_cast<std::ptrdiff_t>(kdf_metadata_index + KDF_NONCE_OFFSET));
}

[[nodiscard]] DecryptResult failDecryption() {
    std::println(std::cerr, "\nDecryption failed!");
    return DecryptResult{.failed = true};
}

void deriveStreamKeyMaterial(
    std::span<const Byte> metadata,
    std::size_t kdf_metadata_index,
    std::uint64_t recovery_pin,
    Key& key,
    std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& stream_header,
    const char* corrupt_error) {

    requireSpanRange(metadata, kdf_metadata_index + KDF_SALT_OFFSET, Salt{}.size(), corrupt_error);
    requireSpanRange(metadata, kdf_metadata_index + KDF_NONCE_OFFSET, stream_header.size(), corrupt_error);

    Salt salt{};
    std::ranges::copy_n(
        metadata.begin() + static_cast<std::ptrdiff_t>(kdf_metadata_index + KDF_SALT_OFFSET),
        static_cast<std::ptrdiff_t>(salt.size()),
        salt.begin());
    deriveKeyFromPin(key, recovery_pin, salt);
    sodium_memzero(&recovery_pin, sizeof(recovery_pin));

    std::ranges::copy_n(
        metadata.begin() + static_cast<std::ptrdiff_t>(kdf_metadata_index + KDF_NONCE_OFFSET),
        static_cast<std::ptrdiff_t>(stream_header.size()),
        stream_header.begin());
}

void validateBlueskyExifCapacity(
    std::span<const Byte> image,
    std::size_t embedded_file_size,
    const char* corrupt_error) {

    constexpr std::size_t SEARCH_LIMIT = 100;
    constexpr std::size_t EXIF_MAX_SIZE = 65534;

    auto index_opt = searchSig(image, JPEG_EXIF_MARKER, SEARCH_LIMIT);
    if (!index_opt) throw std::runtime_error(corrupt_error);

    const std::size_t exif_sig_index = *index_opt;
    requireSpanRange(image, exif_sig_index, 4, corrupt_error);

    const std::size_t exif_segment_size = getValue(image, exif_sig_index + 2, 2);
    if (embedded_file_size >= EXIF_MAX_SIZE && EXIF_MAX_SIZE > exif_segment_size) {
        throw std::runtime_error(corrupt_error);
    }
}

} // namespace

std::uint64_t encryptDataFileForBluesky(
    vBytes& segment_vec,
    const fs::path& data_path,
    std::size_t input_size,
    vString& platforms_vec,
    const std::string& data_filename) {
    constexpr std::size_t kdf_metadata_index = BLUESKY_CIPHER_LAYOUT.template_kdf_metadata_index;

    requireSpanRange(segment_vec, kdf_metadata_index, KDF_METADATA_REGION_BYTES, "Internal Error: Corrupt key metadata.");
    const FilenamePrefix filename_prefix = makeFilenamePrefix(data_filename);

    SecureBuffer<Key> key;
    Salt salt{};
    std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES> stream_header{};
    vBytes encrypted_vec;

    const std::uint64_t pin = generateRecoveryPin();
    randombytes_buf(salt.data(), salt.size());
    deriveKeyFromPin(key.buf, pin, salt);
    encryptFileWithSecretStreamPrefixed(data_path, input_size, filename_prefix.view(), key.buf, stream_header, encrypted_vec);

    buildBlueskySegments(segment_vec, encrypted_vec);
    encrypted_vec.clear();

    storeKdfMetadata(segment_vec, kdf_metadata_index, salt, stream_header);

    keepOnlyPlatformEntry(platforms_vec, BLUESKY_PLATFORM_INDEX);
    return pin;
}

std::uint64_t encryptDataFileToFile(
    vBytes& segment_vec,
    const fs::path& data_path,
    std::size_t input_size,
    const std::string& data_filename,
    const fs::path& encrypted_output_path) {

    constexpr std::size_t kdf_metadata_index = ICC_CIPHER_LAYOUT.template_kdf_metadata_index;
    requireSpanRange(segment_vec, kdf_metadata_index, KDF_METADATA_REGION_BYTES, "Internal Error: Corrupt key metadata.");
    const FilenamePrefix filename_prefix = makeFilenamePrefix(data_filename);

    SecureBuffer<Key> key;
    Salt salt{};
    std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES> stream_header{};

    const std::uint64_t pin = generateRecoveryPin();
    randombytes_buf(salt.data(), salt.size());
    deriveKeyFromPin(key.buf, pin, salt);
    encryptFileWithSecretStreamPrefixedToFile(data_path, input_size, filename_prefix.view(), key.buf, stream_header, encrypted_output_path);

    storeKdfMetadata(segment_vec, kdf_metadata_index, salt, stream_header);
    return pin;
}

DecryptResult decryptDataFile(
    vBytes& metadata_vec,
    bool isBlueskyFile,
    const fs::path& encrypted_input_path,
    const fs::path& stream_output_path,
    bool is_data_compressed) {

    constexpr const char* CORRUPT_FILE_ERROR = "File Extraction Error: Embedded data file is corrupt!";

    DecryptResult result;

    const EmbeddedCipherLayout& cipher_layout = embeddedCipherLayout(isBlueskyFile);
    const std::size_t kdf_metadata_index = cipher_layout.embedded_kdf_metadata_index;

    requireSpanRange(metadata_vec, kdf_metadata_index, KDF_METADATA_REGION_BYTES, CORRUPT_FILE_ERROR);

    // Reject unsupported/legacy metadata before prompting: there is no point
    // asking for a PIN we cannot use, and it avoids holding the entered PIN in a
    // stack local across the throw below (which does not get zeroized).
    if (getKdfMetadataVersion(metadata_vec, kdf_metadata_index) != KdfMetadataVersion::v2_secretstream) {
        throw std::runtime_error("File Decryption Error: Unsupported legacy encrypted file format. Use an older jdvrif release to recover this file.");
    }

    std::uint64_t recovery_pin = getPin();

    SecureBuffer<Key> key;
    std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES> stream_header{};

    deriveStreamKeyMaterial(
        metadata_vec,
        kdf_metadata_index,
        recovery_pin,
        key.buf,
        stream_header,
        CORRUPT_FILE_ERROR);
    sodium_memzero(&recovery_pin, sizeof(recovery_pin));

    if (isBlueskyFile) {
        requireSpanRange(metadata_vec, cipher_layout.file_size_index, 4, CORRUPT_FILE_ERROR);
        validateBlueskyExifCapacity(
            metadata_vec,
            getValue(metadata_vec, cipher_layout.file_size_index, 4),
            CORRUPT_FILE_ERROR);
    }

    constexpr std::size_t min_encrypted_size = STREAM_FRAME_LEN_BYTES + crypto_secretstream_xchacha20poly1305_ABYTES;
    if (checkedFileSize(encrypted_input_path, CORRUPT_FILE_ERROR, true) < min_encrypted_size) {
        result.failed = true;
        return result;
    }

    std::size_t output_size = 0;
    std::string decrypted_filename;
    if (!decryptWithSecretStreamFileInputToFileExtractingFilename(
            encrypted_input_path,
            key.buf,
            stream_header,
            is_data_compressed,
            stream_output_path,
            output_size,
            decrypted_filename)) {
        return failDecryption();
    }
    if (!is_data_compressed && output_size == 0) {
        throw std::runtime_error("File Extraction Error: Output file is empty.");
    }

    result.filename = std::move(decrypted_filename);
    result.output_size = output_size;
    return result;
}
