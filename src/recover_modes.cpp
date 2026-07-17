#include "recover_modes.h"
#include "recover_internal.h"
#include "binary_io.h"
#include "embedded_layout.h"
#include "encryption.h"
#include "encryption_internal.h"
#include "file_utils.h"
#include "recover_output.h"

#include <algorithm>
#include <fstream>
#include <span>
#include <stdexcept>

namespace {
// Smallest secretstream ciphertext: 4-byte frame length + empty-frame ABYTES.
constexpr std::size_t MIN_EMBEDDED_CIPHERTEXT =
    STREAM_FRAME_LEN_BYTES + crypto_secretstream_xchacha20poly1305_ABYTES;

enum class RecoveryFormat : Byte {
    default_icc,
    reddit_icc,
    bluesky,
};

[[nodiscard]] constexpr bool isBlueskyFormat(RecoveryFormat format) noexcept {
    return format == RecoveryFormat::bluesky;
}

[[nodiscard]] constexpr std::size_t maxDeclaredSpan(RecoveryFormat format) noexcept {
    switch (format) {
        case RecoveryFormat::default_icc:
            return MAX_EMBEDDED_SPAN_RECOVERY_DEFAULT;
        case RecoveryFormat::reddit_icc:
            return MAX_EMBEDDED_SPAN_RECOVERY_REDDIT;
        case RecoveryFormat::bluesky:
            return MAX_EMBEDDED_CIPHERTEXT_BLUESKY;
    }
    return 0;
}

void validateDeclaredCipherSize(std::size_t embedded_file_size, RecoveryFormat format) {
    if (embedded_file_size == 0) {
        throw std::runtime_error("File Extraction Error: Embedded data file is empty.");
    }
    if (embedded_file_size < MIN_EMBEDDED_CIPHERTEXT) {
        throw std::runtime_error("File Extraction Error: Embedded data file is corrupt!");
    }

    const std::size_t max_size = maxDeclaredSpan(format);
    if (embedded_file_size > max_size) {
        throw std::runtime_error(
            "File Extraction Error: Embedded data size exceeds the maximum allowed for this format.");
    }
}

template <typename WorkFn>
void runWithRecoverStageFiles(WorkFn&& work) {
    TempFileCleanupGuard cipher_stage(tempRecoveryPath(fs::path("jdvrif_cipher.bin")));
    TempFileCleanupGuard stream_stage(tempRecoveryPath(fs::path("jdvrif_recovered.bin")));
    work(cipher_stage, stream_stage);
}

void finalizeRecoveredOutput(
    DecryptResult decrypt_result,
    TempFileCleanupGuard& stream_stage) {

    if (decrypt_result.failed) {
        throw std::runtime_error("File Decryption Error: Invalid recovery PIN or file is corrupt.");
    }

    const fs::path output_path = commitRecoveredOutput(
        stream_stage,
        validatedRecoveryPath(std::move(decrypt_result.filename)));
    printRecoverySuccess(output_path, decrypt_result.output_size);
}

// Order: validate declared size → PIN/KDF → extract ciphertext → decrypt.
// Extracting before PIN would let a crafted image force multi-GB staging I/O
// with no user interaction; size caps also bound work after a wrong PIN.
template <typename ExtractFn>
void recoverFromCipherExtractor(
    vBytes& metadata_vec,
    RecoveryFormat format,
    bool is_data_compressed,
    std::size_t embedded_file_size,
    ExtractFn&& extract_cipher) {

    validateDeclaredCipherSize(embedded_file_size, format);
    const bool is_bluesky_file = isBlueskyFormat(format);

    runWithRecoverStageFiles([&](TempFileCleanupGuard& cipher_stage, TempFileCleanupGuard& stream_stage) {
        SecureBuffer<Key> key;
        StreamHeader stream_header{};
        const KdfMetadataVersion metadata_version =
            prepareDecryptKeyFromMetadata(metadata_vec, is_bluesky_file, key.buf, stream_header);

        if (extract_cipher(cipher_stage.path) == 0) {
            throw std::runtime_error("File Extraction Error: Embedded data file is empty.");
        }

        DecryptResult decrypt_result = decryptDataFileWithKey(
            key.buf,
            stream_header,
            metadata_version,
            cipher_stage.path,
            stream_stage.path,
            is_data_compressed);
        finalizeRecoveredOutput(std::move(decrypt_result), stream_stage);
    });
}
} // namespace

void recoverFromIccPath(
    const fs::path& image_file_path,
    std::size_t image_file_size,
    std::size_t icc_profile_sig_index) {

    if (icc_profile_sig_index < ICC_PROFILE_SIGNATURE_OFFSET) {
        throw std::runtime_error("File Extraction Error: Corrupt ICC metadata.");
    }

    const std::size_t base_offset = icc_profile_sig_index - ICC_PROFILE_SIGNATURE_OFFSET;
    if (base_offset > image_file_size ||
        ICC_CIPHER_LAYOUT.encrypted_payload_start_index > image_file_size - base_offset) {
        throw std::runtime_error("File Extraction Error: Embedded data file is corrupt!");
    }

    vBytes metadata_vec(ICC_CIPHER_LAYOUT.encrypted_payload_start_index);
    {
        std::ifstream input = openBinaryInputOrThrow(image_file_path, "Read Error: Failed to open image file.");
        readExactAt(input, base_offset, std::span<Byte>(metadata_vec));
    }

    if (!spanHasRange(metadata_vec, ICC_SEGMENT_LAYOUT.compression_flag_index, 1) ||
        !spanHasRange(metadata_vec, ICC_SEGMENT_LAYOUT.embedded_total_profile_header_segments_index, 2) ||
        !spanHasRange(metadata_vec, ICC_CIPHER_LAYOUT.file_size_index, 4)) {
        throw std::runtime_error("File Extraction Error: Corrupt metadata.");
    }

    const bool is_data_compressed = (metadata_vec[ICC_SEGMENT_LAYOUT.compression_flag_index] != NO_ZLIB_COMPRESSION_ID);
    const auto total_profile_header_segments = static_cast<std::uint16_t>(
        getValue(metadata_vec, ICC_SEGMENT_LAYOUT.embedded_total_profile_header_segments_index));
    const std::size_t embedded_file_size = getValue(metadata_vec, ICC_CIPHER_LAYOUT.file_size_index, 4);

    const RecoveryFormat format = base_offset == DEFAULT_TEMPLATE_BASE_OFFSET
        ? RecoveryFormat::default_icc
        : RecoveryFormat::reddit_icc;
    recoverFromCipherExtractor(metadata_vec, format, is_data_compressed, embedded_file_size, [&](const fs::path& cipher_path) {
        return extractDefaultCiphertextToFile(
            image_file_path,
            image_file_size,
            base_offset,
            embedded_file_size,
            total_profile_header_segments,
            cipher_path);
    });
}

void recoverFromBlueskyPath(
    const fs::path& image_file_path,
    std::size_t image_file_size,
    std::size_t jdvrif_sig_index) {

    if (BLUESKY_CIPHER_LAYOUT.encrypted_payload_start_index > image_file_size) {
        throw std::runtime_error("Image File Error: Corrupt signature metadata.");
    }

    vBytes metadata_vec(BLUESKY_CIPHER_LAYOUT.encrypted_payload_start_index);
    {
        std::ifstream input = openBinaryInputOrThrow(image_file_path, "Read Error: Failed to open image file.");
        readExactAt(input, 0, std::span<Byte>(metadata_vec));
    }

    if (jdvrif_sig_index > metadata_vec.size() ||
        JDVRIF_SIGNATURE.size() > metadata_vec.size() - jdvrif_sig_index) {
        throw std::runtime_error("Image File Error: Corrupt signature metadata.");
    }
    const auto sig_begin = metadata_vec.begin() + static_cast<std::ptrdiff_t>(jdvrif_sig_index);
    if (!std::equal(JDVRIF_SIGNATURE.begin(), JDVRIF_SIGNATURE.end(), sig_begin)) {
        throw std::runtime_error("Image File Error: Corrupt signature metadata.");
    }
    if (!spanHasRange(metadata_vec, BLUESKY_CIPHER_LAYOUT.file_size_index, 4)) {
        throw std::runtime_error("File Extraction Error: Corrupt metadata.");
    }

    const std::size_t embedded_file_size = getValue(metadata_vec, BLUESKY_CIPHER_LAYOUT.file_size_index, 4);

    // is_data_compressed is hardcoded true for Bluesky. A payload is only stored
    // uncompressed when conceal bypasses compression (source > COMPRESS_BYPASS_SIZE,
    // 10 MB), but such a payload always exceeds the 2 MB Bluesky encrypted cap
    // (MAX_EMBEDDED_CIPHERTEXT_BLUESKY) and is rejected at conceal time — so a valid
    // Bluesky image is always zlib-compressed. Revisit this coupling if those limits change.
    recoverFromCipherExtractor(metadata_vec, RecoveryFormat::bluesky, true, embedded_file_size, [&](const fs::path& cipher_path) {
        return extractBlueskyCiphertextToFile(image_file_path, image_file_size, embedded_file_size, cipher_path);
    });
}
