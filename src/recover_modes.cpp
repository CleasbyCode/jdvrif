#include "recover_modes.h"
#include "recover_internal.h"
#include "binary_io.h"
#include "embedded_layout.h"
#include "encryption.h"
#include "encryption_internal.h"
#include "file_utils.h"
#include "recover_output.h"
#include "step_output.h"
#include "reddit_steg.h"
#include "twitter_steg.h"

#include <algorithm>
#include <format>
#include <print>
#include <memory>
#include <span>
#include <stdexcept>

namespace {
constexpr std::string_view KEY_DERIVATION_RESULT = "Argon2id from the 128-bit recovery KEY.";

// Smallest secretstream ciphertext: 4-byte frame length + empty-frame ABYTES.
constexpr std::size_t MIN_EMBEDDED_CIPHERTEXT =
    STREAM_FRAME_LEN_BYTES + crypto_secretstream_xchacha20poly1305_ABYTES;

enum class RecoveryFormat : Byte {
    default_icc,
    bluesky,
    reddit,
    twitter,
};

[[nodiscard]] constexpr bool isBlueskyFormat(RecoveryFormat format) noexcept {
    return format == RecoveryFormat::bluesky;
}

[[nodiscard]] constexpr bool isCoefficientCarrierFormat(
    RecoveryFormat format) noexcept {

    return format == RecoveryFormat::reddit ||
        format == RecoveryFormat::twitter;
}

[[nodiscard]] constexpr std::size_t maxDeclaredSpan(RecoveryFormat format) noexcept {
    switch (format) {
        case RecoveryFormat::default_icc:
            return MAX_EMBEDDED_SPAN_RECOVERY_DEFAULT;
        case RecoveryFormat::bluesky:
            return MAX_EMBEDDED_CIPHERTEXT_BLUESKY;
        case RecoveryFormat::reddit:
            return REDDIT_UPLOAD_SIZE_LIMIT;
        case RecoveryFormat::twitter:
            return TWITTER_UPLOAD_SIZE_LIMIT;
    }
    return 0;
}

// Each ICC_PROFILE segment after the first starts with an 18-byte header that
// sits inside the embedded span. Recover does not remove these: the
// ciphertext is read from the image in runs that step over them.
[[nodiscard]] std::size_t skippedProfileHeaderCount(
    std::size_t embedded_file_size,
    std::uint16_t total_profile_header_segments) noexcept {

    const std::size_t first = ICC_SEGMENT_LAYOUT.profile_header_insert_index;
    if (total_profile_header_segments == 0 || embedded_file_size <= first) return 0;
    return 1 + (embedded_file_size - 1 - first) / ICC_SEGMENT_LAYOUT.per_segment_stride;
}

void validateDeclaredCipherSize(std::size_t embedded_file_size, RecoveryFormat format) {
    throwIf(embedded_file_size == 0, "File Extraction Error: Embedded data file is empty.");
    throwIf(embedded_file_size < MIN_EMBEDDED_CIPHERTEXT, "File Extraction Error: Embedded data file is corrupt!");

    const std::size_t max_size = maxDeclaredSpan(format);
    throwIf(
        embedded_file_size > max_size,
        "File Extraction Error: Embedded data size exceeds the maximum allowed for this format.");
}

// The embedded metadata block always sits at a fixed prefix of the image, so
// read exactly that many bytes and close the stream again before any of it is
// interpreted.
[[nodiscard]] vBytes readEmbeddedMetadata(const fs::path& image_file_path, std::size_t offset, std::size_t size) {
    vBytes metadata_vec(size);
    readExactAt(image_file_path, offset, metadata_vec);
    return metadata_vec;
}

// Decryption and inflation run as one streamed pass, so the Decompressing step
// only reports what that pass did.
void reportDecryption(const DecryptResult& result, bool is_data_compressed) {
    endStep("XChaCha20-Poly1305, authenticated.");
    if (!is_data_compressed) return;
    beginStep("Decompressing");
    endStep(std::format("{} -> {}.",
        formatSizePrecise(result.stored_size),
        formatSizePrecise(result.output_size)));
}

void finalizeRecoveredOutput(
    DecryptResult decrypt_result,
    const StagingFile& stream_stage) {

    beginStep("Writing output file");
    // Make the decrypted payload durable before it is given its final name and
    // reported as extracted.
    syncStagingFileOrThrow(stream_stage, WRITE_COMPLETE_ERROR);

    const fs::path output_path = commitRecoveredOutput(
        stream_stage,
        validatedRecoveryPath(std::move(decrypt_result.filename)));
    endStep(std::format("{} ({}).", displaySafePath(output_path), formatSizePrecise(decrypt_result.output_size)));
    printRecoveryComplete();
}

// Order: validate declared size → KEY/KDF → open ciphertext → decrypt.
// The ciphertext is read where it sits (cover image or decoded carrier) and
// never staged: a wrong KEY fails on the first frame instead of after copying
// up to 2 GiB. Opening it runs the cheap structural checks, so a crafted image
// still cannot make this process do any of that work before a KEY is entered.
//
// The decrypted payload goes to a link-free StagingFile inode, so it never
// exists on disk under a real directory entry -- visible to a backup or sync
// client, or surviving a SIGKILL or power loss -- until it has authenticated.
// It reaches its final name via linkat(2) on /proc/self/fd
// (tryCommitStagingFileNoReplace), the one name the user is told about.
template <typename OpenCipherFn>
void recoverFromCipherExtractor(
    vBytes& metadata_vec,
    RecoveryFormat format,
    bool is_data_compressed,
    std::size_t embedded_file_size,
    RecoveryKey* carrier_recovery_key,
    OpenCipherFn&& open_cipher) {

    validateDeclaredCipherSize(embedded_file_size, format);
    const bool is_bluesky_file = isBlueskyFormat(format);
    throwIf(
        isCoefficientCarrierFormat(format) && carrier_recovery_key == nullptr,
        "Internal Error: Coefficient-carrier recovery is missing its KEY.");

    // Created before the KEY prompt, so an unwritable output directory fails
    // before the user has typed anything.
    StagingFile stream_stage({}, "plain");
    SecureBuffer<Key> key;
    StreamHeader stream_header{};
    if (isCoefficientCarrierFormat(format)) {
        // The KEY was entered before the carrier could be located.
        beginStep("Deriving key");
        prepareDecryptKeyFromKdfMetadata(
            metadata_vec,
            *carrier_recovery_key,
            key.buf,
            stream_header);
    } else {
        prepareDecryptKeyFromMetadata(
            metadata_vec,
            is_bluesky_file,
            key.buf,
            stream_header,
            [] {
                std::println("");
                beginStep("Deriving key");
            });
    }
    endStep(KEY_DERIVATION_RESULT);

    const std::unique_ptr<CipherSource> cipher = open_cipher();
    throwIf(cipher->remaining() == 0, "File Extraction Error: Embedded data file is empty.");

    beginStep("Decrypting");
    DecryptResult decrypt_result = decryptDataFileWithKey(
        key.buf,
        stream_header,
        *cipher,
        stream_stage.path(),
        is_data_compressed);
    if (decrypt_result.failed) {
        failStep("authentication failed.");
        throw std::runtime_error("File Decryption Error: Invalid recovery KEY or file is corrupt.");
    }
    reportDecryption(decrypt_result, is_data_compressed);
    finalizeRecoveredOutput(std::move(decrypt_result), stream_stage);
}

template <typename Envelope>
void recoverFromCoefficientCarrierPath(
    Envelope envelope,
    RecoveryKey recovery_key,
    RecoveryFormat format) {

    const std::size_t embedded_file_size = envelope.encrypted_data.size();
    recoverFromCipherExtractor(
        envelope.kdf_metadata,
        format,
        envelope.is_compressed,
        embedded_file_size,
        &recovery_key,
        [&] {
            // Already in memory: the carrier decoder produced it.
            return std::make_unique<MemoryCipherSource>(
                std::span<const Byte>(envelope.encrypted_data));
        });
}
} // namespace

void recoverFromIccPath(
    const fs::path& image_file_path,
    std::size_t image_file_size,
    std::size_t icc_profile_sig_index) {

    throwIf(icc_profile_sig_index < ICC_PROFILE_SIGNATURE_OFFSET, "File Extraction Error: Corrupt ICC metadata.");

    // conceal writes the ICC profile at one fixed offset; an upload pipeline
    // may only push it back by the bounded run of leading segments that
    // findDefaultIccSignatureIndex walks. Anything else is unsupported.
    const std::size_t base_offset = icc_profile_sig_index - ICC_PROFILE_SIGNATURE_OFFSET;
    throwIf(
        base_offset < DEFAULT_TEMPLATE_BASE_OFFSET ||
            base_offset - DEFAULT_TEMPLATE_BASE_OFFSET > MAX_LEADING_SEGMENT_BYTES,
        "File Extraction Error: Unsupported embedded profile layout.");
    throwIf(base_offset > image_file_size ||
        ICC_CIPHER_LAYOUT.encrypted_payload_start_index > image_file_size - base_offset,
        "File Extraction Error: Embedded data file is corrupt!");

    vBytes metadata_vec = readEmbeddedMetadata(
        image_file_path, base_offset, ICC_CIPHER_LAYOUT.encrypted_payload_start_index);

    throwIf(!spanHasRange(metadata_vec, ICC_SEGMENT_LAYOUT.compression_flag_index, 1) ||
        !spanHasRange(metadata_vec, ICC_SEGMENT_LAYOUT.embedded_total_profile_header_segments_index, 2) ||
        !spanHasRange(metadata_vec, ICC_CIPHER_LAYOUT.file_size_index, 4),
        "File Extraction Error: Corrupt metadata.");

    const bool is_data_compressed = (metadata_vec[ICC_SEGMENT_LAYOUT.compression_flag_index] != NO_ZLIB_COMPRESSION_ID);
    const auto total_profile_header_segments = static_cast<std::uint16_t>(
        getValue(metadata_vec, ICC_SEGMENT_LAYOUT.embedded_total_profile_header_segments_index));
    const std::size_t embedded_file_size = getValue(metadata_vec, ICC_CIPHER_LAYOUT.file_size_index, 4);

    // A payload that fits the template's own ICC segment records 0 extra ones.
    const std::size_t icc_segments = std::max<std::size_t>(total_profile_header_segments, 1);
    endStep(std::format("Default mode, {} ICC_PROFILE (APP2) segment{}.",
        icc_segments, icc_segments == 1 ? "" : "s"));

    recoverFromCipherExtractor(metadata_vec, RecoveryFormat::default_icc, is_data_compressed, embedded_file_size, nullptr, [&] {
        const std::size_t headers = skippedProfileHeaderCount(embedded_file_size, total_profile_header_segments);
        if (headers != 0) beginStep("Skipping ICC_PROFILE headers");
        auto cipher = openDefaultCiphertext(
            image_file_path,
            image_file_size,
            base_offset,
            embedded_file_size,
            total_profile_header_segments);
        if (headers != 0) {
            endStep(std::format("{} header{} ({} bytes) left out of the ciphertext read; image unchanged.",
                headers,
                headers == 1 ? "" : "s",
                headers * ICC_SEGMENT_LAYOUT.profile_header_length));
        }
        return cipher;
    });
}

void recoverFromBlueskyPath(
    const fs::path& image_file_path,
    std::size_t image_file_size,
    std::size_t jdvrif_sig_index) {

    throwIf(
        BLUESKY_CIPHER_LAYOUT.encrypted_payload_start_index > image_file_size,
        "Image File Error: Corrupt signature metadata.");

    vBytes metadata_vec = readEmbeddedMetadata(
        image_file_path, 0, BLUESKY_CIPHER_LAYOUT.encrypted_payload_start_index);

    throwIf(jdvrif_sig_index > metadata_vec.size() ||
        JDVRIF_SIGNATURE.size() > metadata_vec.size() - jdvrif_sig_index,
        "Image File Error: Corrupt signature metadata.");
    const auto sig_begin = metadata_vec.begin() + static_cast<std::ptrdiff_t>(jdvrif_sig_index);
    throwIf(
        !std::equal(JDVRIF_SIGNATURE.begin(), JDVRIF_SIGNATURE.end(), sig_begin),
        "Image File Error: Corrupt signature metadata.");
    throwIf(
        !spanHasRange(metadata_vec, BLUESKY_CIPHER_LAYOUT.file_size_index, 4),
        "File Extraction Error: Corrupt metadata.");

    const std::size_t embedded_file_size = getValue(metadata_vec, BLUESKY_CIPHER_LAYOUT.file_size_index, 4);

    const BlueskySegmentUse use = blueskySegmentUse(embedded_file_size);
    endStep(std::format("Bluesky mode, EXIF (APP1){}{} segment{}.",
        use.photoshop ? " + Photoshop (APP13)" : "",
        use.xmp ? " + XMP (APP1)" : "",
        use.photoshop || use.xmp ? "s" : ""));

    // is_data_compressed is hardcoded true for Bluesky. A payload is only stored
    // uncompressed when conceal bypasses compression (source > COMPRESS_BYPASS_SIZE,
    // 10 MB), but such a payload always exceeds the 2 MB Bluesky encrypted cap
    // (MAX_EMBEDDED_CIPHERTEXT_BLUESKY) and is rejected at conceal time — so a valid
    // Bluesky image is always zlib-compressed. Revisit this coupling if those limits change.
    recoverFromCipherExtractor(metadata_vec, RecoveryFormat::bluesky, true, embedded_file_size, nullptr, [&] {
        return openBlueskyCiphertext(image_file_path, image_file_size, embedded_file_size);
    });
}

void recoverFromRedditPath(RedditEncryptedEnvelope envelope, RecoveryKey recovery_key) {
    recoverFromCoefficientCarrierPath(
        std::move(envelope),
        std::move(recovery_key),
        RecoveryFormat::reddit);
}

void recoverFromTwitterPath(TwitterEncryptedEnvelope envelope, RecoveryKey recovery_key) {
    recoverFromCoefficientCarrierPath(
        std::move(envelope),
        std::move(recovery_key),
        RecoveryFormat::twitter);
}
