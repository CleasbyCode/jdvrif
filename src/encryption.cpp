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

std::size_t estimateStreamEncryptedSizePrefixed(std::size_t input_plaintext_size, std::size_t prefix_plaintext_size) {
    return computeStreamEncryptedSizePrefixed(input_plaintext_size, prefix_plaintext_size);
}

namespace {
struct InternalOffsets {
    std::size_t sodium_key_index{};
    std::size_t file_size_index{};
    std::size_t encrypted_file_start_index{};
};

struct PayloadInfo {
    uint16_t total_profile_header_segments{0};
    std::size_t encrypted_file_start_index{0};
    std::size_t embedded_file_size{0};
};

[[nodiscard]] vBytes makeFilenamePrefix(const std::string& data_filename) {
    if (data_filename.empty() ||
        data_filename.size() > static_cast<std::size_t>(std::numeric_limits<Byte>::max() - 1)) {
        throw std::runtime_error("Data File Error: Invalid data filename length.");
    }

    vBytes filename_prefix;
    filename_prefix.reserve(1 + data_filename.size());
    filename_prefix.push_back(static_cast<Byte>(data_filename.size()));
    filename_prefix.insert(filename_prefix.end(), data_filename.begin(), data_filename.end());
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

[[nodiscard]] InternalOffsets internalOffsetsFor(const EmbeddedCipherLayout& cipher_layout) {
    return InternalOffsets{
        .sodium_key_index = cipher_layout.embedded_kdf_metadata_index,
        .file_size_index = cipher_layout.file_size_index,
        .encrypted_file_start_index = cipher_layout.encrypted_payload_start_index
    };
}

[[nodiscard]] DecryptResult failDecryption() {
    std::println(std::cerr, "\nDecryption failed!");
    return DecryptResult{.failed = true};
}

void deriveStreamKeyMaterial(
    std::span<const Byte> metadata,
    const InternalOffsets& offsets,
    std::uint64_t recovery_pin,
    Key& key,
    std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& stream_header,
    const char* corrupt_error) {

    requireSpanRange(metadata, offsets.sodium_key_index + KDF_SALT_OFFSET, Salt{}.size(), corrupt_error);
    requireSpanRange(metadata, offsets.sodium_key_index + KDF_NONCE_OFFSET, stream_header.size(), corrupt_error);

    Salt salt{};
    std::ranges::copy_n(
        metadata.begin() + static_cast<std::ptrdiff_t>(offsets.sodium_key_index + KDF_SALT_OFFSET),
        salt.size(),
        salt.begin());
    deriveKeyFromPin(key, recovery_pin, salt);
    sodium_memzero(&recovery_pin, sizeof(recovery_pin));

    std::ranges::copy_n(
        metadata.begin() + static_cast<std::ptrdiff_t>(offsets.sodium_key_index + KDF_NONCE_OFFSET),
        stream_header.size(),
        stream_header.begin());
}

[[nodiscard]] std::string extractFilenameFromPlaintext(vBytes& plain_payload, const char* corrupt_error) {
    if (plain_payload.empty()) throw std::runtime_error(corrupt_error);

    const std::size_t filename_len = plain_payload[0];
    if (filename_len == 0) {
        throw std::runtime_error(corrupt_error);
    }

    const std::size_t prefix_len = 1 + filename_len;
    requireSpanRange(plain_payload, 0, prefix_len, corrupt_error);

    std::string decrypted_filename(
        reinterpret_cast<const char*>(plain_payload.data() + 1),
        filename_len);

    const std::size_t old_payload_size = plain_payload.size();
    const std::size_t new_payload_size = old_payload_size - prefix_len;
    if (new_payload_size > 0) {
        std::memmove(
            plain_payload.data(),
            plain_payload.data() + static_cast<std::ptrdiff_t>(prefix_len),
            new_payload_size);
        sodium_memzero(
            plain_payload.data() + static_cast<std::ptrdiff_t>(new_payload_size),
            prefix_len);
    } else {
        sodium_memzero(plain_payload.data(), old_payload_size);
    }
    plain_payload.resize(new_payload_size);

    return decrypted_filename;
}

void validateInlineIccSegmentMarkers(
    std::span<const Byte> image,
    std::uint16_t total_profile_header_segments) {

    if (total_profile_header_segments == 0) return;

    const auto marker_index = iccTrailingMarkerIndex(total_profile_header_segments);
    if (!marker_index ||
        !spanHasRange(image, *marker_index, JPEG_APP2_MARKER.size()) ||
        !std::equal(
            JPEG_APP2_MARKER.begin(),
            JPEG_APP2_MARKER.end(),
            image.begin() + static_cast<std::ptrdiff_t>(*marker_index))) {
        throw std::runtime_error("File Extraction Error: Missing segments detected. Embedded data file is corrupt!");
    }
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

[[nodiscard]] PayloadInfo parsePayloadInfo(
    std::span<const Byte> image,
    bool is_bluesky_file,
    const InternalOffsets& offsets,
    bool has_external_encrypted_input,
    const char* corrupt_error) {

    requireSpanRange(image, offsets.file_size_index, 4, corrupt_error);

    uint16_t total_profile_header_segments = 0;
    if (!is_bluesky_file) {
        requireSpanRange(image, ICC_SEGMENT_LAYOUT.embedded_total_profile_header_segments_index, 2, corrupt_error);
        total_profile_header_segments = static_cast<uint16_t>(
            getValue(image, ICC_SEGMENT_LAYOUT.embedded_total_profile_header_segments_index));
    }

    const std::size_t embedded_file_size = getValue(image, offsets.file_size_index, 4);

    if (!is_bluesky_file && !has_external_encrypted_input) {
        validateInlineIccSegmentMarkers(image, total_profile_header_segments);
    }
    if (is_bluesky_file) {
        validateBlueskyExifCapacity(image, embedded_file_size, corrupt_error);
    }

    return PayloadInfo{
        .total_profile_header_segments = total_profile_header_segments,
        .encrypted_file_start_index = offsets.encrypted_file_start_index,
        .embedded_file_size = embedded_file_size
    };
}

void compactIccPayload(vBytes& image, const PayloadInfo& payload, const char* corrupt_error) {
    const std::size_t src_base = payload.encrypted_file_start_index;
    const std::size_t total = payload.embedded_file_size;
    std::size_t read_rel = 0;
    std::size_t write_pos = 0;
    std::size_t next_header = ICC_SEGMENT_LAYOUT.profile_header_insert_index;

    while (read_rel < total) {
        if (read_rel == next_header) {
            read_rel += std::min(ICC_SEGMENT_LAYOUT.profile_header_length, total - read_rel);
            next_header = checkedAdd(
                next_header,
                ICC_SEGMENT_LAYOUT.per_segment_stride,
                corrupt_error);
            continue;
        }

        const std::size_t run_size = std::min(total, next_header) - read_rel;
        std::memmove(image.data() + write_pos, image.data() + src_base + read_rel, run_size);
        read_rel += run_size;
        write_pos += run_size;
    }

    image.resize(write_pos);
}

[[nodiscard]] bool prepareEncryptedPayload(
    vBytes& image,
    const PayloadInfo& payload,
    bool is_bluesky_file,
    bool has_external_encrypted_input,
    const fs::path* encrypted_input_path,
    DecryptResult& result,
    const char* corrupt_error) {

    const std::size_t min_encrypted_size = STREAM_FRAME_LEN_BYTES + crypto_secretstream_xchacha20poly1305_ABYTES;

    if (has_external_encrypted_input) {
        const std::size_t external_size = checkedFileSize(*encrypted_input_path, corrupt_error, true);
        if (external_size < min_encrypted_size) {
            result.failed = true;
            return false;
        }
        image.clear();
        return true;
    }

    if (!spanHasRange(image, payload.encrypted_file_start_index, payload.embedded_file_size)) {
        throw std::runtime_error(corrupt_error);
    }
    if (payload.embedded_file_size < min_encrypted_size) {
        result.failed = true;
        return false;
    }

    if (!is_bluesky_file && payload.total_profile_header_segments) {
        compactIccPayload(image, payload, corrupt_error);
    } else {
        std::memmove(
            image.data(),
            image.data() + payload.encrypted_file_start_index,
            payload.embedded_file_size);
        image.resize(payload.embedded_file_size);
    }

    return true;
}

} // namespace

std::uint64_t encryptDataFileForBluesky(
    vBytes& segment_vec,
    const fs::path& data_path,
    std::size_t input_size,
    vBytes& jpg_vec,
    vString& platforms_vec,
    const std::string& data_filename) {
    constexpr std::size_t kdf_metadata_index = BLUESKY_CIPHER_LAYOUT.template_kdf_metadata_index;

    requireSpanRange(segment_vec, kdf_metadata_index, KDF_METADATA_REGION_BYTES, "Internal Error: Corrupt key metadata.");
    const vBytes filename_prefix = makeFilenamePrefix(data_filename);

    SecureBuffer<Key> key;
    Salt salt{};
    std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES> stream_header{};
    vBytes encrypted_vec;

    const std::uint64_t pin = generateRecoveryPin();
    randombytes_buf(salt.data(), salt.size());
    deriveKeyFromPin(key.buf, pin, salt);
    encryptFileWithSecretStreamPrefixed(data_path, input_size, std::span<const Byte>(filename_prefix), key.buf, stream_header, encrypted_vec);

    segment_vec.reserve(checkedAdd(segment_vec.size(), encrypted_vec.size(), "File Size Error: Segment buffer overflow."));
    buildBlueskySegments(segment_vec, encrypted_vec);
    encrypted_vec.clear();

    storeKdfMetadata(segment_vec, kdf_metadata_index, salt, stream_header);

    jpg_vec.reserve(checkedAdd(jpg_vec.size(), segment_vec.size(), "File Size Error: Embedded image size overflow."));
    jpg_vec.insert(jpg_vec.begin(), segment_vec.begin(), segment_vec.end());

    segment_vec.clear();
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
    const vBytes filename_prefix = makeFilenamePrefix(data_filename);

    SecureBuffer<Key> key;
    Salt salt{};
    std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES> stream_header{};

    const std::uint64_t pin = generateRecoveryPin();
    randombytes_buf(salt.data(), salt.size());
    deriveKeyFromPin(key.buf, pin, salt);
    encryptFileWithSecretStreamPrefixedToFile(data_path, input_size, std::span<const Byte>(filename_prefix), key.buf, stream_header, encrypted_output_path);

    storeKdfMetadata(segment_vec, kdf_metadata_index, salt, stream_header);
    return pin;
}

DecryptResult decryptDataFile(vBytes& jpg_vec, bool isBlueskyFile, const DecryptRequest& request) {
    constexpr const char* CORRUPT_FILE_ERROR = "File Extraction Error: Embedded data file is corrupt!";

    DecryptResult result;

    const EmbeddedCipherLayout& cipher_layout = embeddedCipherLayout(isBlueskyFile);
    const InternalOffsets offsets = internalOffsetsFor(cipher_layout);

    requireSpanRange(jpg_vec, offsets.sodium_key_index, KDF_METADATA_REGION_BYTES, CORRUPT_FILE_ERROR);

    std::uint64_t recovery_pin = getPin();

    SecureBuffer<Key> key;
    std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES> stream_header{};
    const KdfMetadataVersion kdf_metadata_version = getKdfMetadataVersion(jpg_vec, offsets.sodium_key_index);
    const bool has_external_encrypted_input = (request.encrypted_input_path != nullptr);

    if (kdf_metadata_version != KdfMetadataVersion::v2_secretstream) {
        throw std::runtime_error("File Decryption Error: Unsupported legacy encrypted file format. Use an older jdvrif release to recover this file.");
    }

    deriveStreamKeyMaterial(
        jpg_vec,
        offsets,
        recovery_pin,
        key.buf,
        stream_header,
        CORRUPT_FILE_ERROR);
    sodium_memzero(&recovery_pin, sizeof(recovery_pin));

    const PayloadInfo payload_info = parsePayloadInfo(
        jpg_vec,
        isBlueskyFile,
        offsets,
        has_external_encrypted_input,
        CORRUPT_FILE_ERROR);
    if (!prepareEncryptedPayload(
            jpg_vec,
            payload_info,
            isBlueskyFile,
            has_external_encrypted_input,
            request.encrypted_input_path,
            result,
            CORRUPT_FILE_ERROR)) {
        return result;
    }

    if (request.stream_output_path) {
        std::size_t output_size = 0;
        std::string decrypted_filename;

        const bool ok = has_external_encrypted_input
            ? decryptWithSecretStreamFileInputToFileExtractingFilename(
                *request.encrypted_input_path,
                key.buf,
                stream_header,
                request.is_data_compressed,
                *request.stream_output_path,
                output_size,
                decrypted_filename)
            : decryptWithSecretStreamToFileExtractingFilename(
                std::span<const Byte>(jpg_vec),
                key.buf,
                stream_header,
                request.is_data_compressed,
                *request.stream_output_path,
                output_size,
                decrypted_filename);
        if (!ok) return failDecryption();
        if (!request.is_data_compressed && output_size == 0) {
            throw std::runtime_error("File Extraction Error: Output file is empty.");
        }

        jpg_vec.clear();
        result.filename = std::move(decrypted_filename);
        result.output_size = output_size;
        result.used_stream_output = true;
        return result;
    }

    if (has_external_encrypted_input) {
        vBytes encrypted_vec = readFile(*request.encrypted_input_path, FileTypeCheck::data_file);
        if (!decryptWithSecretStream(encrypted_vec, key.buf, stream_header)) return failDecryption();
        result.filename = extractFilenameFromPlaintext(encrypted_vec, CORRUPT_FILE_ERROR);
        jpg_vec = std::move(encrypted_vec);
    } else {
        if (!decryptWithSecretStream(jpg_vec, key.buf, stream_header)) return failDecryption();
        result.filename = extractFilenameFromPlaintext(jpg_vec, CORRUPT_FILE_ERROR);
    }
    return result;
}
