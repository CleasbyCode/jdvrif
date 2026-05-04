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

std::size_t estimateStreamEncryptedSize(std::size_t plaintext_size) { return computeStreamEncryptedSize(plaintext_size); }

std::size_t estimateStreamEncryptedSizePrefixed(std::size_t input_plaintext_size, std::size_t prefix_plaintext_size) {
    return computeStreamEncryptedSizePrefixed(input_plaintext_size, prefix_plaintext_size);
}

namespace {
constexpr auto KDF_METADATA_MAGIC = std::to_array<Byte>({'K', 'D', 'F', '2'});
struct InternalOffsets { std::size_t sodium_key_index{}, file_size_index{}, encrypted_file_start_index{}; };
struct PayloadInfo { uint16_t total_profile_header_segments{0}; std::size_t encrypted_file_start_index{0}, embedded_file_size{0}; };

[[nodiscard]] vBytes makeFilenamePrefix(const std::string& data_filename) {
    if (data_filename.empty() || data_filename.size() > static_cast<std::size_t>(std::numeric_limits<Byte>::max() - 1)) {
        throw std::runtime_error("Data File Error: Invalid data filename length.");
    }

    vBytes filename_prefix;
    filename_prefix.reserve(1 + data_filename.size());
    filename_prefix.push_back(static_cast<Byte>(data_filename.size()));
    filename_prefix.insert(filename_prefix.end(), data_filename.begin(), data_filename.end());
    return filename_prefix;
}

void storeKdfMetadata(vBytes& segment_vec, std::size_t kdf_metadata_index, const Salt& salt, const std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& stream_header) {
    randombytes_buf(segment_vec.data() + kdf_metadata_index, KDF_METADATA_REGION_BYTES);
    std::ranges::copy(KDF_METADATA_MAGIC, segment_vec.begin() + static_cast<std::ptrdiff_t>(kdf_metadata_index + KDF_MAGIC_OFFSET));
    segment_vec[kdf_metadata_index + KDF_ALG_OFFSET] = KDF_ALG_ARGON2ID13;
    segment_vec[kdf_metadata_index + KDF_SENTINEL_OFFSET] = KDF_SENTINEL;
    std::ranges::copy(salt, segment_vec.begin() + static_cast<std::ptrdiff_t>(kdf_metadata_index + KDF_SALT_OFFSET));
    std::ranges::copy(stream_header, segment_vec.begin() + static_cast<std::ptrdiff_t>(kdf_metadata_index + KDF_NONCE_OFFSET));
}

}

std::uint64_t encryptDataFileFromPath(vBytes& segment_vec, const fs::path& data_path, std::size_t input_size,
    vBytes& jpg_vec, vString& platforms_vec, const std::string& data_filename, bool hasBlueskyOption, bool /*hasRedditOption*/) {
    if (!hasBlueskyOption) throw std::runtime_error("Internal Error: encryptDataFileFromPath is Bluesky-only.");

    const std::size_t kdf_metadata_index = templateKdfMetadataIndex(hasBlueskyOption);

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

std::uint64_t encryptDataFileFromPathToFile(vBytes& segment_vec, const fs::path& data_path, std::size_t input_size, const std::string& data_filename, bool hasBlueskyOption, const fs::path& encrypted_output_path) {
    const std::size_t kdf_metadata_index = templateKdfMetadataIndex(hasBlueskyOption);
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
    const InternalOffsets offsets{
        .sodium_key_index = cipher_layout.embedded_kdf_metadata_index,
        .file_size_index = cipher_layout.file_size_index,
        .encrypted_file_start_index = cipher_layout.encrypted_payload_start_index
    };

    requireSpanRange(jpg_vec, offsets.sodium_key_index, KDF_METADATA_REGION_BYTES, CORRUPT_FILE_ERROR);

    const std::uint64_t recovery_pin = getPin();

    SecureBuffer<Key> key;
    std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES> stream_header{};
    const KdfMetadataVersion kdf_metadata_version = getKdfMetadataVersion(jpg_vec, offsets.sodium_key_index);
    const bool has_external_encrypted_input = (request.encrypted_input_path != nullptr);

    if (kdf_metadata_version != KdfMetadataVersion::v2_secretstream) throw std::runtime_error("File Decryption Error: Unsupported legacy encrypted file format. Use an older jdvrif release to recover this file.");

    auto fail_decryption = [&]() -> DecryptResult {
        std::println(std::cerr, "\nDecryption failed!");
        return DecryptResult{.failed = true};
    };

    auto derive_key_material = [&]() {
        requireSpanRange(jpg_vec, offsets.sodium_key_index + KDF_SALT_OFFSET, Salt{}.size(), CORRUPT_FILE_ERROR);
        requireSpanRange(jpg_vec, offsets.sodium_key_index + KDF_NONCE_OFFSET, stream_header.size(), CORRUPT_FILE_ERROR);

        Salt salt{}; std::ranges::copy_n(jpg_vec.begin() + static_cast<std::ptrdiff_t>(offsets.sodium_key_index + KDF_SALT_OFFSET), salt.size(), salt.begin());
        deriveKeyFromPin(key.buf, recovery_pin, salt);
        std::ranges::copy_n(jpg_vec.begin() + static_cast<std::ptrdiff_t>(offsets.sodium_key_index + KDF_NONCE_OFFSET), stream_header.size(), stream_header.begin());
    };

    auto extract_filename_from_plaintext = [&](vBytes& plain_payload) {
        if (plain_payload.empty()) throw std::runtime_error(CORRUPT_FILE_ERROR);

        const std::size_t filename_len = plain_payload[0];
        if (filename_len == 0 || filename_len > std::numeric_limits<std::size_t>::max() - 1) throw std::runtime_error(CORRUPT_FILE_ERROR);
        const std::size_t prefix_len = 1 + filename_len;
        requireSpanRange(plain_payload, 0, prefix_len, CORRUPT_FILE_ERROR);

        std::string decrypted_filename(reinterpret_cast<const char*>(plain_payload.data() + 1), filename_len);

        std::memmove(plain_payload.data(), plain_payload.data() + static_cast<std::ptrdiff_t>(prefix_len), plain_payload.size() - prefix_len);
        plain_payload.resize(plain_payload.size() - prefix_len);

        return decrypted_filename;
    };

    auto parse_payload_info = [&]() -> PayloadInfo {
        requireSpanRange(jpg_vec, offsets.file_size_index, 4, CORRUPT_FILE_ERROR);

        uint16_t total_profile_header_segments = 0;
        if (!isBlueskyFile) { requireSpanRange(jpg_vec, ICC_SEGMENT_LAYOUT.embedded_total_profile_header_segments_index, 2, CORRUPT_FILE_ERROR); total_profile_header_segments = static_cast<uint16_t>(getValue(jpg_vec, ICC_SEGMENT_LAYOUT.embedded_total_profile_header_segments_index)); }

        const std::size_t embedded_file_size = getValue(jpg_vec, offsets.file_size_index, 4);

        if (total_profile_header_segments && !isBlueskyFile && !has_external_encrypted_input) {
            const auto marker_index = iccTrailingMarkerIndex(total_profile_header_segments);
            if (!marker_index || !spanHasRange(jpg_vec, *marker_index, JPEG_APP2_MARKER.size()) || !std::equal(JPEG_APP2_MARKER.begin(), JPEG_APP2_MARKER.end(), jpg_vec.begin() + static_cast<std::ptrdiff_t>(*marker_index))) {
                throw std::runtime_error("File Extraction Error: Missing segments detected. Embedded data file is corrupt!");
            }
        }

        if (isBlueskyFile) {
            constexpr std::size_t SEARCH_LIMIT = 100, EXIF_MAX_SIZE = 65534;

            auto index_opt = searchSig(jpg_vec, JPEG_EXIF_MARKER, SEARCH_LIMIT);
            if (!index_opt) throw std::runtime_error(CORRUPT_FILE_ERROR);

            const std::size_t exif_sig_index = *index_opt;
            requireSpanRange(jpg_vec, exif_sig_index, 4, CORRUPT_FILE_ERROR);
            const std::size_t exif_segment_size = getValue(jpg_vec, exif_sig_index + 2, 2);

            if (embedded_file_size >= EXIF_MAX_SIZE && EXIF_MAX_SIZE > exif_segment_size) throw std::runtime_error(CORRUPT_FILE_ERROR);
        }

        return PayloadInfo{.total_profile_header_segments = total_profile_header_segments, .encrypted_file_start_index = offsets.encrypted_file_start_index, .embedded_file_size = embedded_file_size};
    };

    auto prepare_encrypted_payload = [&](const PayloadInfo& payload) -> bool {
        const std::size_t min_encrypted_size = STREAM_FRAME_LEN_BYTES + crypto_secretstream_xchacha20poly1305_ABYTES;

        if (has_external_encrypted_input) {
            const std::size_t external_size = checkedFileSize(*request.encrypted_input_path, CORRUPT_FILE_ERROR, true);
            if (external_size < min_encrypted_size) {
                result.failed = true;
                return false;
            }
            jpg_vec.clear();
            return true;
        }

        if (!spanHasRange(jpg_vec, payload.encrypted_file_start_index, payload.embedded_file_size)) throw std::runtime_error(CORRUPT_FILE_ERROR);
        if (payload.embedded_file_size < min_encrypted_size) {
            result.failed = true;
            return false;
        }

        std::memmove(jpg_vec.data(), jpg_vec.data() + payload.encrypted_file_start_index, payload.embedded_file_size);
        jpg_vec.resize(payload.embedded_file_size);

        if (!isBlueskyFile && payload.total_profile_header_segments) {
            const std::size_t limit = jpg_vec.size();

            std::size_t read_pos = 0, write_pos = 0, next_header = ICC_SEGMENT_LAYOUT.profile_header_insert_index;

            while (read_pos < limit) {
                if (read_pos == next_header) {
                    read_pos += std::min(ICC_SEGMENT_LAYOUT.profile_header_length, limit - read_pos);
                    next_header += ICC_SEGMENT_LAYOUT.per_segment_stride;
                    continue;
                }
                jpg_vec[write_pos++] = jpg_vec[read_pos++];
            }
            jpg_vec.resize(write_pos);
        }

        return true;
    };

    derive_key_material();
    const PayloadInfo payload_info = parse_payload_info();
    if (!prepare_encrypted_payload(payload_info)) return result;

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
        if (!ok) return fail_decryption();
        if (!request.is_data_compressed && output_size == 0) throw std::runtime_error("File Extraction Error: Output file is empty.");

        jpg_vec.clear(); result.filename = std::move(decrypted_filename); result.output_size = output_size; result.used_stream_output = true;
        return result;
    }

    if (has_external_encrypted_input) {
        vBytes encrypted_vec = readFile(*request.encrypted_input_path, FileTypeCheck::data_file);
        if (!decryptWithSecretStream(encrypted_vec, key.buf, stream_header)) return fail_decryption();
        result.filename = extract_filename_from_plaintext(encrypted_vec);
        jpg_vec = std::move(encrypted_vec);
    } else {
        if (!decryptWithSecretStream(jpg_vec, key.buf, stream_header)) return fail_decryption();
        result.filename = extract_filename_from_plaintext(jpg_vec);
    }
    return result;
}
