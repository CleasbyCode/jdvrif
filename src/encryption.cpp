#include "encryption.h"
#include "encryption_internal.h"
#include "binary_io.h"
#include "embedded_layout.h"
#include "file_utils.h"
#include "pin_input.h"
#include "segmentation.h"
#include "template_assets.h"

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
        KDF_METADATA_MAGIC_V3,
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
    SecurePin& recovery_pin,
    Key& key,
    StreamHeader& stream_header,
    const char* corrupt_error) {

    requireSpanRange(metadata, kdf_metadata_index + KDF_SALT_OFFSET, Salt{}.size(), corrupt_error);
    requireSpanRange(metadata, kdf_metadata_index + KDF_NONCE_OFFSET, stream_header.size(), corrupt_error);

    Salt salt{};
    std::ranges::copy_n(
        metadata.begin() + static_cast<std::ptrdiff_t>(kdf_metadata_index + KDF_SALT_OFFSET),
        static_cast<std::ptrdiff_t>(salt.size()),
        salt.begin());
    deriveKeyFromPin(key, recovery_pin, salt);
    // PIN is no longer needed after key derivation on the recover path.
    recovery_pin.wipe();

    std::ranges::copy_n(
        metadata.begin() + static_cast<std::ptrdiff_t>(kdf_metadata_index + KDF_NONCE_OFFSET),
        static_cast<std::ptrdiff_t>(stream_header.size()),
        stream_header.begin());
}

// Max ciphertext reconstructable from EXIF + two Photoshop datasets + XMP
// base64 overflow — matches buildBlueskySegments packing limits.
[[nodiscard]] std::size_t maxBlueskyEmbeddedCipherCapacity() {
    const std::size_t xmp_overhead = checkedAdd(
        xmpSegmentTemplateBytes().size(),
        BLUESKY_SEGMENT_LAYOUT.xmp_footer_size,
        "Internal Error: Bluesky XMP layout overflow.");
    if (xmp_overhead >= BLUESKY_SEGMENT_LAYOUT.xmp_segment_size_limit) {
        throw std::runtime_error("Internal Error: Corrupt Bluesky XMP segment template.");
    }
    const std::size_t max_xmp_b64 =
        BLUESKY_SEGMENT_LAYOUT.xmp_segment_size_limit - xmp_overhead;
    // Floor to a whole base64 quartet, then decoded binary length.
    const std::size_t max_xmp_binary = (max_xmp_b64 / 4) * 3;

    return checkedAdd(
        checkedAdd(
            BLUESKY_SEGMENT_LAYOUT.exif_segment_data_size_limit,
            BLUESKY_SEGMENT_LAYOUT.first_dataset_size_limit,
            "Internal Error: Bluesky capacity overflow."),
        checkedAdd(
            BLUESKY_SEGMENT_LAYOUT.last_dataset_size_limit,
            max_xmp_binary,
            "Internal Error: Bluesky capacity overflow."),
        "Internal Error: Bluesky capacity overflow.");
}

void validateBlueskyExifCapacity(
    std::span<const Byte> image,
    std::size_t embedded_file_size,
    const char* corrupt_error) {

    constexpr std::size_t SEARCH_LIMIT = 100;

    auto index_opt = searchSig(image, JPEG_EXIF_MARKER, SEARCH_LIMIT);
    if (!index_opt) throw std::runtime_error(corrupt_error);

    const std::size_t exif_sig_index = *index_opt;
    requireSpanRange(image, exif_sig_index, 4, corrupt_error);

    const std::size_t exif_segment_size = getValue(image, exif_sig_index + 2, 2);
    // JPEG segment length includes the 2-byte length field itself; minimum is 2.
    if (exif_segment_size < 2) {
        throw std::runtime_error(corrupt_error);
    }

    // Exclusive end index of the APP1 segment: marker (2) + length value.
    const std::size_t exif_end = checkedAdd(
        exif_sig_index,
        checkedAdd(std::size_t{2}, exif_segment_size, corrupt_error),
        corrupt_error);

    const std::size_t payload_start = BLUESKY_CIPHER_LAYOUT.encrypted_payload_start_index;
    if (payload_start >= exif_end) {
        throw std::runtime_error(corrupt_error);
    }
    const std::size_t available_exif_payload = exif_end - payload_start;

    // Hard ceiling from the multi-segment packer (and the program Bluesky cap).
    const std::size_t max_capacity = std::min(
        maxBlueskyEmbeddedCipherCapacity(),
        MAX_EMBEDDED_CIPHERTEXT_BLUESKY);
    if (embedded_file_size > max_capacity) {
        throw std::runtime_error(corrupt_error);
    }

    if (embedded_file_size <= BLUESKY_SEGMENT_LAYOUT.exif_segment_data_size_limit) {
        // Single-segment: the entire ciphertext must live inside this APP1.
        if (embedded_file_size > available_exif_payload) {
            throw std::runtime_error(corrupt_error);
        }
    } else {
        // Overflow path: first chunk is always the full EXIF payload limit;
        // APP1 must provide at least that many bytes after payload_start.
        if (available_exif_payload < BLUESKY_SEGMENT_LAYOUT.exif_segment_data_size_limit) {
            throw std::runtime_error(corrupt_error);
        }
    }
}

} // namespace

namespace {
constexpr const char* CORRUPT_FILE_ERROR = "File Extraction Error: Embedded data file is corrupt!";

// Zero the full allocation (capacity, not only size) then release. vector::clear()
// alone leaves residual ciphertext in the heap freelist until the block is reused.
void wipeAndRelease(vBytes& buf) noexcept {
    if (const std::size_t cap = buf.capacity(); cap > 0) {
        // Bring capacity into size so the entire allocation is addressable.
        try {
            buf.resize(cap);
        } catch (...) {
            // If resize fails, still wipe the live size() region.
            if (!buf.empty()) {
                sodium_memzero(buf.data(), buf.size());
            }
            return;
        }
        sodium_memzero(buf.data(), cap);
    }
    buf.clear();
    buf.shrink_to_fit();
}

struct WipeBytesGuard {
    vBytes* buf{nullptr};

    explicit WipeBytesGuard(vBytes& b) noexcept : buf(&b) {}
    WipeBytesGuard(const WipeBytesGuard&) = delete;
    WipeBytesGuard& operator=(const WipeBytesGuard&) = delete;

    ~WipeBytesGuard() {
        if (buf) wipeAndRelease(*buf);
    }
};
} // namespace

SecurePin encryptDataFileForBluesky(
    vBytes& segment_vec,
    const fs::path& data_path,
    std::size_t input_size,
    vString& platforms_vec,
    const std::string& data_filename,
    bool is_data_compressed) {
    constexpr std::size_t kdf_metadata_index = BLUESKY_CIPHER_LAYOUT.template_kdf_metadata_index;

    requireSpanRange(segment_vec, kdf_metadata_index, KDF_METADATA_REGION_BYTES, "Internal Error: Corrupt key metadata.");
    const FilenamePrefix filename_prefix = makeFilenamePrefix(data_filename);

    SecureBuffer<Key> key;
    Salt salt{};
    StreamHeader stream_header{};
    vBytes encrypted_vec;

    SecurePin pin = generateRecoveryPin();
    randombytes_buf(salt.data(), salt.size());
    deriveKeyFromPin(key.buf, pin, salt);
    encryptFileWithSecretStreamPrefixed(
        data_path,
        input_size,
        filename_prefix.view(),
        streamModeByte(is_data_compressed),
        key.buf,
        stream_header,
        encrypted_vec);
    // Always wipe the intermediate ciphertext buffer (success or exception).
    WipeBytesGuard encrypted_wipe{encrypted_vec};

    buildBlueskySegments(segment_vec, encrypted_vec);

    storeKdfMetadata(segment_vec, kdf_metadata_index, salt, stream_header);

    keepOnlyPlatformEntry(platforms_vec, BLUESKY_PLATFORM_INDEX);
    return pin;
}

SecurePin encryptDataFileToFile(
    vBytes& segment_vec,
    const fs::path& data_path,
    std::size_t input_size,
    const std::string& data_filename,
    const fs::path& encrypted_output_path,
    bool is_data_compressed) {

    constexpr std::size_t kdf_metadata_index = ICC_CIPHER_LAYOUT.template_kdf_metadata_index;
    requireSpanRange(segment_vec, kdf_metadata_index, KDF_METADATA_REGION_BYTES, "Internal Error: Corrupt key metadata.");
    const FilenamePrefix filename_prefix = makeFilenamePrefix(data_filename);

    SecureBuffer<Key> key;
    Salt salt{};
    StreamHeader stream_header{};

    SecurePin pin = generateRecoveryPin();
    randombytes_buf(salt.data(), salt.size());
    deriveKeyFromPin(key.buf, pin, salt);
    encryptFileWithSecretStreamPrefixedToFile(
        data_path,
        input_size,
        filename_prefix.view(),
        streamModeByte(is_data_compressed),
        key.buf,
        stream_header,
        encrypted_output_path);

    storeKdfMetadata(segment_vec, kdf_metadata_index, salt, stream_header);
    return pin;
}

KdfMetadataVersion prepareDecryptKeyFromMetadata(
    vBytes& metadata_vec,
    bool isBlueskyFile,
    Key& out_key,
    StreamHeader& out_stream_header) {

    const EmbeddedCipherLayout& cipher_layout = embeddedCipherLayout(isBlueskyFile);
    const std::size_t kdf_metadata_index = cipher_layout.embedded_kdf_metadata_index;

    requireSpanRange(metadata_vec, kdf_metadata_index, KDF_METADATA_REGION_BYTES, CORRUPT_FILE_ERROR);

    // Reject unsupported/legacy metadata before prompting: there is no point
    // asking for a PIN we cannot use, and it avoids holding the entered PIN in a
    // stack local across the throw below (which does not get zeroized).
    const KdfMetadataVersion metadata_version =
        getKdfMetadataVersion(metadata_vec, kdf_metadata_index);
    if (metadata_version != KdfMetadataVersion::v2_secretstream &&
        metadata_version != KdfMetadataVersion::v3_secretstream_authenticated_mode) {
        throw std::runtime_error("File Decryption Error: Unsupported legacy encrypted file format. Use an older jdvrif release to recover this file.");
    }

    if (isBlueskyFile) {
        requireSpanRange(metadata_vec, cipher_layout.file_size_index, 4, CORRUPT_FILE_ERROR);
        validateBlueskyExifCapacity(
            metadata_vec,
            getValue(metadata_vec, cipher_layout.file_size_index, 4),
            CORRUPT_FILE_ERROR);
    }

    SecurePin recovery_pin = getPin();
    deriveStreamKeyMaterial(
        metadata_vec,
        kdf_metadata_index,
        recovery_pin,
        out_key,
        out_stream_header,
        CORRUPT_FILE_ERROR);
    // recovery_pin already wiped inside deriveStreamKeyMaterial.
    return metadata_version;
}

DecryptResult decryptDataFileWithKey(
    const Key& key,
    const StreamHeader& stream_header,
    KdfMetadataVersion metadata_version,
    const fs::path& encrypted_input_path,
    const fs::path& stream_output_path,
    bool is_data_compressed) {

    DecryptResult result;

    constexpr std::size_t min_encrypted_size = STREAM_FRAME_LEN_BYTES + crypto_secretstream_xchacha20poly1305_ABYTES;
    if (checkedFileSize(encrypted_input_path, CORRUPT_FILE_ERROR, true) < min_encrypted_size) {
        result.failed = true;
        return result;
    }

    std::size_t output_size = 0;
    std::string decrypted_filename;
    if (!decryptWithSecretStreamFileInputToFileExtractingFilename(
            encrypted_input_path,
            key,
            stream_header,
            metadata_version,
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

DecryptResult decryptDataFile(
    vBytes& metadata_vec,
    bool isBlueskyFile,
    const fs::path& encrypted_input_path,
    const fs::path& stream_output_path,
    bool is_data_compressed) {

    SecureBuffer<Key> key;
    StreamHeader stream_header{};
    const KdfMetadataVersion metadata_version =
        prepareDecryptKeyFromMetadata(metadata_vec, isBlueskyFile, key.buf, stream_header);
    return decryptDataFileWithKey(
        key.buf,
        stream_header,
        metadata_version,
        encrypted_input_path,
        stream_output_path,
        is_data_compressed);
}
