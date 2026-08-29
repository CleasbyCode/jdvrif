#include "conceal.h"
#include "binary_io.h"
#include "compression.h"
#include "embedded_layout.h"
#include "encryption.h"
#include "encryption_internal.h"
#include "file_utils.h"
#include "jpeg_utils.h"
#include "reddit_steg.h"
#include "segmentation.h"
#include "signal_utils.h"
#include "template_assets.h"
#include "twitter_steg.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>

#include <unistd.h>

namespace {
constexpr std::size_t
    MAX_PATH_ATTEMPTS         = 1024,
    BYTES_PER_KIBIBYTE        = 1024,
    PROGRESSIVE_SOURCE_LIMIT  = 2 * 1024 * 1024,
    MAX_OPTIMIZED_IMAGE_SIZE  = 4 * 1024 * 1024,
    MAX_BLUESKY_IMAGE_SIZE    = 2'000'000,
    DATA_FILENAME_MAX_LENGTH  = 20,
    LARGE_FILE_SIZE           = 300 * 1024 * 1024,
    COMPRESS_BYPASS_SIZE      = 10 * 1024 * 1024,
    MAX_SIZE_CONCEAL          = 2ULL * 1024 * 1024 * 1024, // Cover image size + embedded (compressed) hidden file size (payload)
    OUTPUT_STREAM_BUFFER      = 1 * 1024 * 1024;

struct ConcealFlags {
    bool has_no_option{false};
    bool has_bluesky_option{false};
    bool has_reddit_option{false};
    bool has_twitter_option{false};
};

struct EncryptionInput {
    fs::path path{};
    std::size_t size{0};
    bool is_compressed{true};
};

struct StagedImage {
    fs::path output_path{};
    TempFileCleanupGuard temp_output{};

    StagedImage(fs::path final_path, fs::path temporary_path)
        : output_path(std::move(final_path)),
          temp_output(std::move(temporary_path)) {}

    StagedImage(const StagedImage&) = delete;
    StagedImage& operator=(const StagedImage&) = delete;
    StagedImage(StagedImage&&) noexcept = default;
    StagedImage& operator=(StagedImage&&) noexcept = default;
};

struct EmbeddedWriteResult {
    StagedImage staged;
    SegmentedEmbedSummary summary{};
};

struct ConcealFinalizeResult {
    SecurePin recovery_pin{};
    StagedImage staged;
    std::size_t embedded_jpg_size{0};
};

[[nodiscard]] fs::path randomizedPath(
    const fs::path& parent,
    std::string_view prefix,
    std::string_view suffix,
    std::string_view error_message,
    std::size_t token_hex_chars = 16) {
    return uniqueRandomizedPathOrThrow(parent, prefix, suffix, MAX_PATH_ATTEMPTS, error_message, token_hex_chars);
}

[[nodiscard]] fs::path uniqueOutputPath() {
    return randomizedPath({}, "jrif_", ".jpg", "Write File Error: Could not create a unique output filename.", 9);
}

[[nodiscard]] fs::path tempOutputPath(const fs::path& output_path) {
    return randomizedPath(
        output_path.parent_path(),
        std::format(".{}.jdvrif_tmp_", output_path.filename().string()),
        "",
        "Write File Error: Could not create a temporary output filename.");
}

// Intermediate stages (deflated payload, encrypted payload) go to link-free
// StagingFile inodes rather than named temporaries: deflate is not encryption,
// so the compressed payload is plaintext-equivalent and must never be visible
// in a directory listing, to a backup/sync client, or survive a SIGKILL.
// See StagingFile in file_utils.h.

[[nodiscard]] ConcealFlags concealFlags(Option option) {
    return {
        .has_no_option = option == Option::None,
        .has_bluesky_option = option == Option::Bluesky,
        .has_reddit_option = option == Option::Reddit,
        .has_twitter_option = option == Option::Twitter,
    };
}

[[nodiscard]] vString platformReportTemplate() {
    return vString{
        "X-Twitter", "Tumblr",
        "Bluesky. (Only share this \"file-embedded\" JPG image on Bluesky).\n\n "
        "You must use the Python script \"create_bsky_post.py\" (found in the repo src/bsky folder)\n "
        "to post the image to Bluesky.",
        "Mastodon", "Pixelfed",
        "PostImage", "ImgBB", "ImgPile", "Flickr"
    };
}

[[nodiscard]] OptimizedCover prepareCoverImage(std::span<const Byte> input, std::size_t source_data_size, const ConcealFlags& flags) {
    const bool is_progressive = (source_data_size < PROGRESSIVE_SOURCE_LIMIT) && flags.has_no_option;
    return optimizeImage(input, is_progressive, flags.has_no_option);
}

void validateCoverImageLimits(std::size_t jpg_size, const ConcealFlags& flags) {
    throwIf(
        jpg_size > MAX_OPTIMIZED_IMAGE_SIZE,
        "Image File Error: Cover image exceeds default maximum size limit for all conceal modes.");
    throwIf(
        flags.has_bluesky_option && jpg_size > MAX_BLUESKY_IMAGE_SIZE,
        "File Size Error: Cover image exceeds the maximum size limit of 2,000,000 bytes for the Bluesky platform. Use a smaller cover image.");
}

[[nodiscard]] std::string validateDataFilename(const fs::path& data_file_path) {
    std::string data_filename = data_file_path.filename().string();
    throwIf(
        !hasSafeEmbeddedFilename(data_file_path.filename()),
        "Data File Error: Embedded filename is unsafe. "
        "Filenames may not begin with '.' or '-'.");
    throwIf(
        data_filename.size() > DATA_FILENAME_MAX_LENGTH,
        "Data File Error: For compatibility requirements, "
        "length of data filename must not exceed 20 characters.");
    return data_filename;
}

[[nodiscard]] bool isAlreadyCompressedFileType(const fs::path& data_file_path) {
    return hasFileExtension(data_file_path, {
        ".zip", ".jar", ".rar", ".7z", ".bz2", ".gz", ".xz", ".lz", ".lz4", ".cab", ".rpm", ".deb",
        ".mp4", ".mp3", ".exe", ".jpg", ".jpeg", ".jfif", ".png", ".webp", ".gif", ".ogg", ".flac"
    });
}

[[nodiscard]] bool shouldBypassCompression(const fs::path& data_file_path, std::size_t source_data_size) {
    return source_data_size > COMPRESS_BYPASS_SIZE && isAlreadyCompressedFileType(data_file_path);
}

[[nodiscard]] vBytes makeDefaultSegmentTemplate() {
    std::span<const Byte> template_bytes = defaultIccTemplateBytes();
    throwIf(
        template_bytes.size() != DEFAULT_METADATA_PREFIX_BYTES,
        "Internal Error: Corrupt default ICC segment template.");

    vBytes out = copyTemplateBytes(template_bytes);
    throwIf(!spanHasRange(out, DEFAULT_ICC_SIGNATURE_INDEX_ABS, ICC_PROFILE_SIGNATURE.size()) ||
        !spanHasRange(out, DEFAULT_JDVRIF_SIGNATURE_INDEX_ABS, JDVRIF_SIGNATURE.size()),
        "Internal Error: Corrupt default ICC segment template.");

    std::copy(ICC_PROFILE_SIGNATURE.begin(), ICC_PROFILE_SIGNATURE.end(),
              out.begin() + static_cast<std::ptrdiff_t>(DEFAULT_ICC_SIGNATURE_INDEX_ABS));
    std::copy(JDVRIF_SIGNATURE.begin(), JDVRIF_SIGNATURE.end(),
              out.begin() + static_cast<std::ptrdiff_t>(DEFAULT_JDVRIF_SIGNATURE_INDEX_ABS));
    return out;
}

[[nodiscard]] vBytes makeBlueskySegmentTemplate() {
    std::span<const Byte> template_bytes = blueskyExifTemplateBytes();
    throwIf(
        template_bytes.size() < BLUESKY_SEGMENT_LAYOUT.exif_segment_data_insert_index,
        "Internal Error: Corrupt Bluesky segment template.");
    return copyTemplateBytes(template_bytes);
}

[[nodiscard]] vBytes makeSegmentTemplate(bool has_bluesky_option) {
    return has_bluesky_option ? makeBlueskySegmentTemplate() : makeDefaultSegmentTemplate();
}

void maybePrintLargeFileNotice(std::size_t source_data_size) {
    if (source_data_size > LARGE_FILE_SIZE) {
        std::println("\nPlease wait. Larger files will take longer to complete this process.");
    }
}

[[nodiscard]] EncryptionInput preparePayloadInput(
    const fs::path& data_file_path,
    std::size_t source_data_size,
    bool bypass_compression,
    std::string_view staging_tag,
    std::string_view compression_error,
    std::optional<StagingFile>& compressed_stage) {

    if (bypass_compression) {
        return EncryptionInput{
            .path = data_file_path,
            .size = source_data_size,
            .is_compressed = false,
        };
    }

    const fs::path& compressed_path =
        compressed_stage.emplace(fs::path{}, staging_tag).path();
    zlibCompressFileToPath(data_file_path, compressed_path, source_data_size);

    return EncryptionInput{
        .path = compressed_path,
        .size = checkedFileSize(compressed_path, compression_error, true),
        .is_compressed = true,
    };
}

struct CarrierCapacityLimits {
    std::size_t conservative_compressed{0};
    std::size_t recommended_compressed{0};
};

using CarrierEnvelopeSizeFunction = std::size_t (*)(std::size_t);

[[nodiscard]] std::size_t maxCarrierStoredPayload(
    std::size_t carrier_capacity,
    std::size_t filename_length,
    CarrierEnvelopeSizeFunction envelope_size) {

    const std::size_t filename_prefix_size = 1 + filename_length;
    const auto fits = [&](std::size_t stored_payload_size) {
        const std::size_t encrypted_size = computeStreamEncryptedSizePrefixed(
            stored_payload_size,
            filename_prefix_size);
        return envelope_size(encrypted_size) <= carrier_capacity;
    };

    if (!fits(0)) return 0;
    std::size_t lower = 0;
    std::size_t upper = carrier_capacity;
    while (lower < upper) {
        const std::size_t middle = lower + (upper - lower + 1) / 2;
        if (fits(middle)) {
            lower = middle;
        } else {
            upper = middle - 1;
        }
    }
    return lower;
}

[[nodiscard]] CarrierCapacityLimits carrierCapacityLimits(
    std::size_t carrier_capacity,
    CarrierEnvelopeSizeFunction envelope_size) {

    const std::size_t conservative_compressed = maxCarrierStoredPayload(
        carrier_capacity,
        DATA_FILENAME_MAX_LENGTH,
        envelope_size);
    return CarrierCapacityLimits{
        .conservative_compressed = conservative_compressed,
        .recommended_compressed = conservative_compressed > BYTES_PER_KIBIBYTE
            ? conservative_compressed - BYTES_PER_KIBIBYTE
            : 0,
    };
}

void printCarrierCapacityLimits(
    std::string_view theoretical_label,
    std::size_t theoretical_capacity,
    const CarrierCapacityLimits& limits) {

    constexpr int CAPACITY_LABEL_WIDTH = 71;
    std::println(
        "{:<{}}{} bytes (~{}KiB).",
        theoretical_label,
        CAPACITY_LABEL_WIDTH,
        theoretical_capacity,
        theoretical_capacity / BYTES_PER_KIBIBYTE);
    std::println(
        "{:<{}}{} bytes (~{}KiB).",
        "Conservative maximum compressed capacity with a 20-character filename:",
        CAPACITY_LABEL_WIDTH,
        limits.conservative_compressed,
        limits.conservative_compressed / BYTES_PER_KIBIBYTE);
    std::println(
        "{:<{}}{} bytes (~{}KiB).",
        "Recommended  maximum compressed capacity with a 20-character filename:",
        CAPACITY_LABEL_WIDTH,
        limits.recommended_compressed,
        limits.recommended_compressed / BYTES_PER_KIBIBYTE);
}

[[nodiscard]] EncryptionInput prepareEncryptionInput(const fs::path& data_file_path,
                                                     std::size_t source_data_size,
                                                     bool bypass_compression,
                                                     vBytes& segment_vec,
                                                     std::optional<StagingFile>& compressed_stage) {
    if (bypass_compression) {
        // NO_ZLIB_COMPRESSION_ID_INDEX is an offset into the *ICC* template.
        // concealData refuses Bluesky + bypass so this only ever runs against
        // that layout; keep the two in step if either changes.
        throwIf(
            NO_ZLIB_COMPRESSION_ID_INDEX >= segment_vec.size(),
            "Internal Error: Compression marker index out of range.");
        segment_vec[NO_ZLIB_COMPRESSION_ID_INDEX] = NO_ZLIB_COMPRESSION_ID;
    }
    return preparePayloadInput(
        data_file_path,
        source_data_size,
        bypass_compression,
        "comp",
        "Zlib Compression Error: Failed to build compressed payload.",
        compressed_stage);
}

void validateCombinedSizeLimits(std::size_t encrypted_payload_size, std::size_t jpg_size, const ConcealFlags& flags) {
    throwIf(
        encrypted_payload_size > std::numeric_limits<std::size_t>::max() - jpg_size,
        "File Size Error: Combined file size overflow.");
    const std::size_t combined_size = encrypted_payload_size + jpg_size;

    // Check the packer's real ceiling, not just the program cap: the Bluesky
    // segments hold ~171 KiB, well under MAX_EMBEDDED_CIPHERTEXT_BLUESKY, and
    // anything between the two would otherwise be compressed, encrypted and
    // half-packed before failing inside appendXmpPayload. This runs on the
    // *encrypted* size, which is derived from the compressed payload, so a
    // large but well-compressing file is still accepted on its merits.
    if (flags.has_bluesky_option) {
        const std::size_t max_bluesky_payload = std::min(
            maxBlueskyEmbeddedCipherCapacity(),
            MAX_EMBEDDED_CIPHERTEXT_BLUESKY);
        if (encrypted_payload_size > max_bluesky_payload) {
            throw std::runtime_error(std::format(
                "Data File Size Error: Encrypted payload is {} bytes, above the {}-byte "
                "limit for the Bluesky platform.\n"
                "                      Use a smaller data file, or compress it yourself first.",
                encrypted_payload_size, max_bluesky_payload));
        }
    }
    throwIf(
        flags.has_no_option && combined_size > MAX_SIZE_CONCEAL,
        "File Size Error: Combined size of image and data file "
        "exceeds maximum default size limit for jdvrif.");
}

template<typename WriteFn>
[[nodiscard]] StagedImage writeToStagedOutput(WriteFn&& write_fn) {
    const fs::path output_path = uniqueOutputPath();
    StagedImage staged(output_path, tempOutputPath(output_path));
    // OutputFile's internal 1 MiB buffer batches consecutive small appends
    // (replacing the old ofstream pubsetbuf) while letting the bulk payload
    // stream through sendfile(2); see file_utils OutputFile for which writes
    // that actually coalesces.
    OutputFile out(staged.temp_output.path, OUTPUT_STREAM_BUFFER);
    write_fn(out);
    // Durable close: the recovery PIN is printed and then discarded, so the
    // image it unlocks must already be on stable storage -- a PIN for an image
    // lost to a crash is unrecoverable.
    out.close(WRITE_COMPLETE_ERROR, /*durable=*/true);
    return staged;
}

[[nodiscard]] StagedImage saveEmbeddedJpg(std::span<const Byte> segment_vec, std::span<const Byte> jpg_vec) {
    return writeToStagedOutput([&](OutputFile& f) {
        if (!segment_vec.empty()) {
            f.write(segment_vec, "Write File Error: Output data too large to write.");
        }
        f.write(jpg_vec, "Write File Error: Output data too large to write.");
    });
}

[[nodiscard]] EmbeddedWriteResult saveEmbeddedJpgFromEncryptedPath(
    vBytes& segment_vec, const fs::path& encrypted_path,
    std::span<const Byte> jpg_vec) {
    SegmentedEmbedSummary summary;
    StagedImage staged = writeToStagedOutput([&](OutputFile& f) {
        summary = writeEmbeddedJpgFromEncryptedFile(f, segment_vec, encrypted_path, jpg_vec);
    });
    return EmbeddedWriteResult{std::move(staged), summary};
}

void finalizePlatformReport(vString& platforms_vec, const SegmentedEmbedSummary& summary) {
    removeOptionalPlatformEntries(platforms_vec);
    filterPlatforms(platforms_vec, summary.embedded_image_size, summary.first_segment_size, summary.total_segments);
}

[[nodiscard]] ConcealFinalizeResult concealDefaultPath(
    vBytes& segment_vec,
    const OptimizedCover& cover,
    const EncryptionInput& encryption_input,
    const std::string& data_filename,
    vString& platforms_vec) {

    StagingFile encrypted_stage({}, "enc");
    SecurePin recovery_pin = encryptDataFileToFile(
        segment_vec,
        encryption_input.path,
        encryption_input.size,
        data_filename,
        encrypted_stage.path(),
        encryption_input.is_compressed);

    EmbeddedWriteResult embedded = saveEmbeddedJpgFromEncryptedPath(
        segment_vec,
        encrypted_stage.path(),
        cover.view());

    finalizePlatformReport(platforms_vec, embedded.summary);

    return ConcealFinalizeResult{
        .recovery_pin = std::move(recovery_pin),
        .staged = std::move(embedded.staged),
        .embedded_jpg_size = embedded.summary.embedded_image_size,
    };
}

[[nodiscard]] ConcealFinalizeResult concealBlueskyPath(
    vBytes& segment_vec,
    const OptimizedCover& cover,
    const EncryptionInput& encryption_input,
    const std::string& data_filename,
    vString& platforms_vec) {

    SecurePin recovery_pin = encryptDataFileForBluesky(
        segment_vec,
        encryption_input.path,
        encryption_input.size,
        platforms_vec,
        data_filename,
        encryption_input.is_compressed);

    const std::span<const Byte> cover_view = cover.view();
    StagedImage staged = saveEmbeddedJpg(std::span<const Byte>(segment_vec), cover_view);
    const std::size_t embedded_jpg_size = checkedFileSize(
        staged.temp_output.path,
        "Write File Error: Failed to verify final Bluesky output image.",
        true);
    throwIf(
        embedded_jpg_size > MAX_BLUESKY_IMAGE_SIZE,
        "File Size Error: Final output image exceeds the 2,000,000-byte limit for the Bluesky platform.\n"
        "                 Use a smaller cover image or reduce the size of the payload (hidden data file).");

    return ConcealFinalizeResult{
        .recovery_pin = std::move(recovery_pin),
        .staged = std::move(staged),
        .embedded_jpg_size = embedded_jpg_size,
    };
}

void validateRedditPreliminaryLimits(
    std::size_t cover_size,
    std::size_t payload_size) {

    const bool cover_too_large = cover_size > REDDIT_UPLOAD_SIZE_LIMIT;
    const bool payload_too_large = payload_size > REDDIT_UPLOAD_SIZE_LIMIT;
    if (cover_too_large && payload_too_large) {
        throw std::runtime_error(
            "File Size Error: Cover image and payload file exceed Reddit's 20 MiB upload size limit.");
    }
    if (cover_too_large) {
        throw std::runtime_error(
            "Image File Size Error: Cover image exceeds Reddit's 20 MiB upload size limit.");
    }
    if (payload_too_large) {
        throw std::runtime_error(
            "Data File Size Error: Payload file exceeds Reddit's 20 MiB upload size limit.");
    }
}

[[nodiscard]] RedditPreparedCover prepareRedditCarrier(vBytes& jpg_vec) {
    // Apply the existing lossless EXIF-orientation transform and strip source
    // metadata first. The Reddit carrier then performs the required pixel
    // transcode to baseline YCbCr 4:2:0 with standard Q75 tables.
    OptimizedCover normalized = optimizeImage(
        std::span<const Byte>(jpg_vec),
        /*isProgressive=*/false,
        /*enforceQualityLimit=*/false,
        REDDIT_COVER_IMAGE_LIMITS);
    vBytes{}.swap(jpg_vec);

    RedditPreparedCover cover = prepareRedditCover(normalized.full_view());
    if (cover.jpeg.size() > REDDIT_UPLOAD_SIZE_LIMIT) {
        throw std::runtime_error(
            "Image File Size Error: Transcoded cover image exceeds Reddit's 20 MiB upload size limit.");
    }
    return cover;
}

void printRedditCoverImageSummary(
    const RedditPreparedCover& cover,
    std::size_t source_cover_size) {

    std::println(
        "Cover Image: {}KiB, {}x{}, Baseline YCbCr 4:2:0, Standard Q75 quantization (C3).\n",
        source_cover_size / BYTES_PER_KIBIBYTE,
        cover.width,
        cover.height);
}

[[nodiscard]] CarrierCapacityLimits redditCapacityLimits(
    const RedditPreparedCover& cover) {

    return carrierCapacityLimits(
        cover.payload_capacity,
        redditEnvelopeSize);
}

void printRedditCapacityLimits(
    const RedditPreparedCover& cover,
    const CarrierCapacityLimits& limits) {

    printCarrierCapacityLimits(
        "Theoretical C3 capacity limit for this cover image:",
        cover.payload_capacity,
        limits);
}

[[noreturn]] void failRedditPayloadCapacity(
    const RedditPreparedCover& cover,
    std::size_t source_cover_size,
    std::size_t stored_payload_size) {

    const CarrierCapacityLimits limits = redditCapacityLimits(cover);
    std::print("\n");
    printRedditCoverImageSummary(cover, source_cover_size);
    std::println(
        "Compressed data file (payload) size: {} bytes ({}KiB).\n",
        stored_payload_size,
        stored_payload_size / BYTES_PER_KIBIBYTE);
    printRedditCapacityLimits(cover, limits);
    (void)std::fflush(stdout);

    throw std::runtime_error(std::format(
        "Data File Size Error: \n\n"
        "Already-compressed payload file size of {} bytes ({}KiB) exceeds the "
        "recommended maximum limit of {} bytes (~{}KiB) for this cover image.",
        stored_payload_size,
        stored_payload_size / BYTES_PER_KIBIBYTE,
        limits.recommended_compressed,
        limits.recommended_compressed / BYTES_PER_KIBIBYTE));
}

[[nodiscard]] ConcealFinalizeResult concealRedditPath(
    vBytes& jpg_vec,
    const fs::path& data_file_path,
    std::size_t source_data_size,
    const std::string& data_filename,
    vString& platforms_vec) {

    const std::size_t source_cover_size = jpg_vec.size();
    RedditPreparedCover cover = prepareRedditCarrier(jpg_vec);

    // Avoid adding a second compression layer to formats that are already
    // compressed. Unlike the default metadata path, Reddit does this at every
    // size because its carrier budget is small and every byte matters.
    std::optional<StagingFile> compressed_stage;
    const EncryptionInput encryption_input = preparePayloadInput(
        data_file_path,
        source_data_size,
        isAlreadyCompressedFileType(data_file_path),
        "reddit_comp",
        "Zlib Compression Error: Failed to build Reddit compressed payload.",
        compressed_stage);
    const std::size_t stored_payload_size = encryption_input.size;

    throwIf(
        data_filename.size() >
            std::numeric_limits<std::size_t>::max() - 1 - stored_payload_size,
        "File Size Error: Encrypted Reddit output overflow.");
    const std::size_t filename_prefix_size = 1 + data_filename.size();
    const std::size_t encrypted_size = computeStreamEncryptedSizePrefixed(
        stored_payload_size,
        filename_prefix_size);
    const std::size_t envelope_size = redditEnvelopeSize(encrypted_size);
    if (envelope_size > cover.payload_capacity) {
        failRedditPayloadCapacity(
            cover,
            source_cover_size,
            stored_payload_size);
    }

    StagingFile encrypted_stage({}, "reddit_enc");
    vBytes kdf_metadata;
    WipeBytesGuard kdf_metadata_wipe{kdf_metadata};
    SecurePin recovery_pin = encryptDataFileForReddit(
        kdf_metadata,
        encryption_input.path,
        stored_payload_size,
        data_filename,
        encrypted_stage.path(),
        encryption_input.is_compressed);

    const std::size_t actual_encrypted_size = checkedFileSize(
        encrypted_stage.path(),
        "Encryption Error: Failed to build complete Reddit encrypted payload.",
        true);
    throwIf(
        actual_encrypted_size != encrypted_size,
        "Encryption Error: Reddit encrypted payload has an unexpected size.");

    vBytes encrypted_data(actual_encrypted_size);
    WipeBytesGuard encrypted_wipe{encrypted_data};
    preadExactFromFd(
        encrypted_stage.fd(),
        std::span<Byte>(encrypted_data),
        0,
        "Encryption Error: Failed to read complete Reddit encrypted payload.");

    vBytes envelope = makeRedditEnvelope(
        kdf_metadata,
        encrypted_data,
        encryption_input.is_compressed);
    WipeBytesGuard envelope_wipe{envelope};
    // The carrier's coefficient positions are keyed by the PIN that was just
    // minted, so an image only reveals that it carries anything to whoever
    // holds it.
    vBytes embedded_jpeg =
        embedRedditPayload(cover, deriveCarrierKeyFromPin(recovery_pin), envelope);

    StagedImage staged = saveEmbeddedJpg({}, embedded_jpeg);
    const std::size_t embedded_jpg_size = checkedFileSize(
        staged.temp_output.path,
        "Write File Error: Failed to verify final Reddit output image.",
        true);
    if (embedded_jpg_size > REDDIT_UPLOAD_SIZE_LIMIT) {
        throw std::runtime_error(
            "File Size Error: Final output image exceeds Reddit's 20 MiB upload size limit.\n"
            "                 Use a smaller cover image or reduce the size of the payload.");
    }

    platforms_vec = {
        "Reddit. (Only share this \"file-embedded\" JPG image on Reddit)."
    };
    return ConcealFinalizeResult{
        .recovery_pin = std::move(recovery_pin),
        .staged = std::move(staged),
        .embedded_jpg_size = embedded_jpg_size,
    };
}

void validateTwitterPreliminaryLimits(
    std::size_t cover_size,
    std::size_t payload_size) {

    const bool cover_too_large = cover_size > TWITTER_UPLOAD_SIZE_LIMIT;
    const bool payload_too_large = payload_size > TWITTER_UPLOAD_SIZE_LIMIT;
    if (cover_too_large && payload_too_large) {
        throw std::runtime_error(
            "File Size Error: Cover image and payload file exceed X-Twitter's 5 MiB upload size limit.");
    }
    if (cover_too_large) {
        throw std::runtime_error(
            "Image File Size Error: Cover image exceeds X-Twitter's 5 MiB upload size limit.");
    }
    if (payload_too_large) {
        throw std::runtime_error(
            "Data File Size Error: Payload file exceeds X-Twitter's 5 MiB upload size limit.");
    }
}

[[nodiscard]] TwitterPreparedCover prepareTwitterCarrier(vBytes& jpg_vec) {
    // Reject X-Twitter-incompatible dimensions from the input header before
    // doing the lossless orientation transform or pixel transcode.
    (void)inspectTwitterCover(jpg_vec);

    // Apply jdvrif's existing lossless EXIF-orientation transform and strip
    // source metadata. The adaptive carrier then prepares progressive YCbCr
    // 4:2:0 JFIF while retaining source quantization. Existing 4:2:0 covers
    // remain in the coefficient domain; only other layouts require a pixel
    // transcode. Source quality finer than Q97 is capped at Q97.
    OptimizedCover normalized = optimizeImage(
        std::span<const Byte>(jpg_vec),
        /*isProgressive=*/false,
        /*enforceQualityLimit=*/false);
    vBytes{}.swap(jpg_vec);

    TwitterPreparedCover cover = prepareTwitterCover(normalized.full_view());
    if (cover.jpeg.size() > TWITTER_UPLOAD_SIZE_LIMIT) {
        throw std::runtime_error(
            "Image File Size Error: Transcoded cover image exceeds X-Twitter's 5 MiB upload size limit.");
    }
    return cover;
}

void printTwitterCoverImageSummary(
    const TwitterPreparedCover& cover,
    std::size_t source_cover_size) {

    if (cover.source_quality > cover.carrier_quality) {
        std::println(
            "Cover Image: {}KiB, {}x{}, Progressive YCbCr 4:2:0, Source Q{} capped to Q{} (J-UNIWARD/STC).\n",
            source_cover_size / BYTES_PER_KIBIBYTE,
            cover.width,
            cover.height,
            cover.source_quality,
            cover.carrier_quality);
    } else {
        std::println(
            "Cover Image: {}KiB, {}x{}, Progressive YCbCr 4:2:0, Source-derived Q{} quantization (J-UNIWARD/STC).\n",
            source_cover_size / BYTES_PER_KIBIBYTE,
            cover.width,
            cover.height,
            cover.carrier_quality);
    }
}

[[nodiscard]] CarrierCapacityLimits twitterCapacityLimits(
    const TwitterPreparedCover& cover) {

    return carrierCapacityLimits(
        cover.payload_capacity,
        twitterEnvelopeSize);
}

void printTwitterCapacityLimits(
    const TwitterPreparedCover& cover,
    const CarrierCapacityLimits& limits) {

    printCarrierCapacityLimits(
        "Theoretical J-UNIWARD/STC capacity limit for this cover image:",
        cover.payload_capacity,
        limits);
}

[[noreturn]] void failTwitterPayloadCapacity(
    const TwitterPreparedCover& cover,
    std::size_t source_cover_size,
    std::size_t stored_payload_size) {

    const CarrierCapacityLimits limits = twitterCapacityLimits(cover);
    std::print("\n");
    printTwitterCoverImageSummary(cover, source_cover_size);
    std::println(
        "Compressed data file (payload) size: {} bytes ({}KiB).\n",
        stored_payload_size,
        stored_payload_size / BYTES_PER_KIBIBYTE);
    printTwitterCapacityLimits(cover, limits);
    (void)std::fflush(stdout);

    throw std::runtime_error(std::format(
        "Data File Size Error: \n\n"
        "Already-compressed payload file size of {} bytes ({}KiB) exceeds the "
        "recommended maximum limit of {} bytes (~{}KiB) for this cover image.",
        stored_payload_size,
        stored_payload_size / BYTES_PER_KIBIBYTE,
        limits.recommended_compressed,
        limits.recommended_compressed / BYTES_PER_KIBIBYTE));
}

[[nodiscard]] ConcealFinalizeResult concealTwitterPath(
    vBytes& jpg_vec,
    const fs::path& data_file_path,
    std::size_t source_data_size,
    const std::string& data_filename,
    vString& platforms_vec) {

    const std::size_t source_cover_size = jpg_vec.size();
    TwitterPreparedCover cover = prepareTwitterCarrier(jpg_vec);

    // Avoid adding a second compression layer to formats that are already
    // compressed. Unlike the default metadata path, X-Twitter does this at
    // every size because its content-dependent carrier budget is small.
    std::optional<StagingFile> compressed_stage;
    const EncryptionInput encryption_input = preparePayloadInput(
        data_file_path,
        source_data_size,
        isAlreadyCompressedFileType(data_file_path),
        "twitter_comp",
        "Zlib Compression Error: Failed to build X-Twitter compressed payload.",
        compressed_stage);
    const std::size_t stored_payload_size = encryption_input.size;

    throwIf(
        data_filename.size() >
            std::numeric_limits<std::size_t>::max() - 1 - stored_payload_size,
        "File Size Error: Encrypted X-Twitter output overflow.");
    const std::size_t filename_prefix_size = 1 + data_filename.size();
    const std::size_t encrypted_size = computeStreamEncryptedSizePrefixed(
        stored_payload_size,
        filename_prefix_size);
    const std::size_t envelope_size = twitterEnvelopeSize(encrypted_size);
    if (envelope_size > cover.payload_capacity) {
        failTwitterPayloadCapacity(
            cover,
            source_cover_size,
            stored_payload_size);
    }

    StagingFile encrypted_stage({}, "twitter_enc");
    vBytes kdf_metadata;
    WipeBytesGuard kdf_metadata_wipe{kdf_metadata};
    // This compact KDF + secretstream routine is carrier-agnostic despite its
    // legacy Reddit name; the outer envelope below identifies X-Twitter.
    SecurePin recovery_pin = encryptDataFileForReddit(
        kdf_metadata,
        encryption_input.path,
        stored_payload_size,
        data_filename,
        encrypted_stage.path(),
        encryption_input.is_compressed);

    const std::size_t actual_encrypted_size = checkedFileSize(
        encrypted_stage.path(),
        "Encryption Error: Failed to build complete X-Twitter encrypted payload.",
        true);
    throwIf(
        actual_encrypted_size != encrypted_size,
        "Encryption Error: X-Twitter encrypted payload has an unexpected size.");

    vBytes encrypted_data(actual_encrypted_size);
    WipeBytesGuard encrypted_wipe{encrypted_data};
    preadExactFromFd(
        encrypted_stage.fd(),
        std::span<Byte>(encrypted_data),
        0,
        "Encryption Error: Failed to read complete X-Twitter encrypted payload.");

    vBytes envelope = makeTwitterEnvelope(
        kdf_metadata,
        encrypted_data,
        encryption_input.is_compressed);
    WipeBytesGuard envelope_wipe{envelope};

    std::println(
        "\nPlease wait. Larger cover images and payloads will take longer to complete the adaptive embedding process.");
    vBytes embedded_jpeg = embedTwitterPayload(
        cover,
        deriveCarrierKeyFromPin(recovery_pin),
        envelope);

    StagedImage staged = saveEmbeddedJpg({}, embedded_jpeg);
    const std::size_t embedded_jpg_size = checkedFileSize(
        staged.temp_output.path,
        "Write File Error: Failed to verify final X-Twitter output image.",
        true);
    if (embedded_jpg_size > TWITTER_UPLOAD_SIZE_LIMIT) {
        throw std::runtime_error(
            "File Size Error: Final output image exceeds X-Twitter's 5 MiB upload size limit.\n"
            "                 Use a smaller cover image or reduce the size of the payload.");
    }

    platforms_vec = {
        "X-Twitter. (Only share this \"file-embedded\" JPG image on X-Twitter)."
    };
    return ConcealFinalizeResult{
        .recovery_pin = std::move(recovery_pin),
        .staged = std::move(staged),
        .embedded_jpg_size = embedded_jpg_size,
    };
}

constexpr const char* PIN_DELIVERY_ERROR = "Output Error: Failed to deliver recovery PIN.";

void flushStdoutOrThrow() {
    throwIf(std::fflush(stdout) != 0 || std::ferror(stdout) != 0, PIN_DELIVERY_ERROR);
}

// std::print / std::format would stage the PIN's decimal form in a heap buffer
// that is freed without being zeroed, undoing the wiping the rest of the PIN
// path is careful about. Render the digits into a stack array, drain stdio so
// ordering is preserved, write them straight to fd 1 between two constant
// fragments, then zero the array (WipePodGuard, on every exit path).
void printRecoveryPinSecurely(const SecurePin& recovery_pin) {
    std::array<char, 24> digits{};
    WipePodGuard<std::array<char, 24>> wipe_digits{digits};

    const auto [end, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), recovery_pin.value);
    throwIf(ec != std::errc{}, PIN_DELIVERY_ERROR);

    std::print("\nRecovery PIN: [***");
    flushStdoutOrThrow();
    writeAllToFd(
        STDOUT_FILENO,
        std::span<const Byte>(
            reinterpret_cast<const Byte*>(digits.data()),
            static_cast<std::size_t>(end - digits.data())),
        PIN_DELIVERY_ERROR);
    std::print("***]\n\nImportant: Keep your PIN safe, so that you can extract the hidden file.\n\n");
    flushStdoutOrThrow();
}

void finalizeConcealOutput(const vString& platforms_vec, ConcealFinalizeResult& result) {
    std::print("\nPlatform compatibility for output image:-\n\n");
    if (platforms_vec.empty()) {
        std::println("Unknown!\n\n Due to the large file size of the output JPG image, "
                     "I'm unaware of any\n compatible platforms that this image can be "
                     "posted on. Local use only?");
    } else {
        for (const auto& s : platforms_vec) {
            std::println(" ✓ {}", s);
        }
    }

    throwIfSignalCancellationRequested();
    printRecoveryPinSecurely(result.recovery_pin);

    // PIN is now in the user's hands. The fsynced temp is the only durable
    // copy that PIN can open, so later failures must not delete it.
    const fs::path leftover_path = result.staged.temp_output.path;
    result.staged.temp_output.dismiss();
    result.recovery_pin.wipe();

    try {
        commitStagedFileNoReplaceOrThrow(
            leftover_path,
            result.staged.output_path,
            "Write File Error: Failed to commit output image");
    } catch (const SignalCancellation&) {
        std::println(stderr,
                     "\nInterrupted after PIN delivery. The output image is still at: {}\n",
                     leftover_path.string());
        throw;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::format(
            "{}\nThe output image is still at: {}",
            e.what(), leftover_path.string()));
    }

    std::println("\nSaved \"file-embedded\" JPG image: {} ({} bytes).\n\nComplete!\n",
                 result.staged.output_path.string(),
                 result.embedded_jpg_size);
    flushStdoutOrThrow();
}
} // namespace

void displayRedditCapacity(vBytes& jpg_vec) {
    const std::size_t source_cover_size = jpg_vec.size();
    RedditPreparedCover cover = prepareRedditCarrier(jpg_vec);
    const CarrierCapacityLimits capacity_limits = redditCapacityLimits(cover);
    const std::size_t minimum_single_frame_overhead = redditEnvelopeSize(
        computeStreamEncryptedSizePrefixed(
            0,
            /*one-byte filename plus its length byte=*/2));
    const std::size_t maximum_single_frame_overhead = redditEnvelopeSize(
        computeStreamEncryptedSizePrefixed(
            0,
            /*maximum filename plus its length byte=*/
                1 + DATA_FILENAME_MAX_LENGTH));

    std::println("\nReddit capacity check for conceal -r mode only.\n");
    printRedditCoverImageSummary(cover, source_cover_size);
    printRedditCapacityLimits(cover, capacity_limits);

    std::println(
        "\nImportant:\n\n"
        "This is total encrypted carrier-envelope capacity, not a raw secret-file limit.\n\n"
        "JDVRIF compresses the input first unless its file extension identifies an already-compressed type;\n"
        "compression may shrink or slightly expand other inputs.\n\n"
        "For a payload contained in one encryption frame, filename, encryption and recovery metadata\n"
        "consume {} to {} bytes; larger payloads add framing overhead.\n\n"
        "Do not target the theoretical exact limit. When capacity permits, keep the compressed payload at least\n"
        "1KiB below the conservative maximum shown above. The final conceal -r size check is authoritative.\n"
        "The final embedded JPEG size can differ from the cover image and must remain within 20MB.\n",
        minimum_single_frame_overhead,
        maximum_single_frame_overhead);
}

void displayTwitterCapacity(vBytes& jpg_vec) {
    const std::size_t source_cover_size = jpg_vec.size();
    TwitterPreparedCover cover = prepareTwitterCarrier(jpg_vec);
    const CarrierCapacityLimits capacity_limits = twitterCapacityLimits(cover);
    const std::size_t minimum_single_frame_overhead = twitterEnvelopeSize(
        computeStreamEncryptedSizePrefixed(
            0,
            /*one-byte filename plus its length byte=*/2));
    const std::size_t maximum_single_frame_overhead = twitterEnvelopeSize(
        computeStreamEncryptedSizePrefixed(
            0,
            /*maximum filename plus its length byte=*/
                1 + DATA_FILENAME_MAX_LENGTH));

    std::println("\nX-Twitter capacity check for conceal -x mode only.\n");
    printTwitterCoverImageSummary(cover, source_cover_size);
    printTwitterCapacityLimits(cover, capacity_limits);

    std::println(
        "\nImportant:\n\n"
        "This is total encrypted carrier-envelope capacity, not a raw secret-file limit.\n\n"
        "JDVRIF compresses the input first unless its file extension identifies an already-compressed type;\n"
        "compression may shrink or slightly expand other inputs.\n\n"
        "For a payload contained in one encryption frame, filename, encryption and recovery metadata\n"
        "consume {} to {} bytes; larger payloads add framing overhead.\n\n"
        "Do not target the theoretical exact limit. When capacity permits, keep the compressed payload at least\n"
        "1KiB below the conservative maximum shown above. The final conceal -x size check is authoritative.\n"
        "The final embedded JPEG size can differ from the cover image and must remain within 5MB.\n",
        minimum_single_frame_overhead,
        maximum_single_frame_overhead);
}

void concealData(vBytes& jpg_vec, Option option, const fs::path& data_file_path) {
    vString platforms_vec = platformReportTemplate();
    const ConcealFlags flags = concealFlags(option);

    const std::size_t source_data_size = validateFileForRead(data_file_path);
    if (flags.has_reddit_option) {
        validateRedditPreliminaryLimits(jpg_vec.size(), source_data_size);
        const std::string data_filename = validateDataFilename(data_file_path);
        ConcealFinalizeResult result = concealRedditPath(
            jpg_vec,
            data_file_path,
            source_data_size,
            data_filename,
            platforms_vec);
        finalizeConcealOutput(platforms_vec, result);
        return;
    }
    if (flags.has_twitter_option) {
        validateTwitterPreliminaryLimits(jpg_vec.size(), source_data_size);
        const std::string data_filename = validateDataFilename(data_file_path);
        ConcealFinalizeResult result = concealTwitterPath(
            jpg_vec,
            data_file_path,
            source_data_size,
            data_filename,
            platforms_vec);
        finalizeConcealOutput(platforms_vec, result);
        return;
    }

    OptimizedCover cover = prepareCoverImage(jpg_vec, source_data_size, flags);
    // Caller's input vector is no longer needed; release its storage so it
    // doesn't sit alongside the OptimizedCover for the rest of the run.
    vBytes{}.swap(jpg_vec);

    const std::size_t jpg_size = cover.trimmed_size();
    validateCoverImageLimits(jpg_size, flags);

    const std::string data_filename = validateDataFilename(data_file_path);
    const bool bypass_compression = shouldBypassCompression(data_file_path, source_data_size);

    // Bluesky recovery hardcodes is_data_compressed (see recoverFromBlueskyPath):
    // its layout carries no compression marker, so a stored-uncompressed payload
    // could never be read back. Reject here rather than letting the marker write
    // in prepareEncryptionInput -- an index that belongs to the ICC template --
    // land in the Bluesky EXIF template on the way to a size failure further on.
    // Bypass needs a >10 MB source, which can never fit the ~171 KiB Bluesky
    // payload limit uncompressed, so nothing valid is turned away.
    throwIf(
        flags.has_bluesky_option && bypass_compression,
        "Data File Size Error: Data file is far above the maximum payload size for the Bluesky platform.\n"
        "                      Use a smaller data file, or compress it yourself first.");

    vBytes segment_vec = makeSegmentTemplate(flags.has_bluesky_option);
    maybePrintLargeFileNotice(source_data_size);

    std::optional<StagingFile> compressed_stage;
    const EncryptionInput encryption_input = prepareEncryptionInput(
        data_file_path,
        source_data_size,
        bypass_compression,
        segment_vec,
        compressed_stage);

    throwIf(
        data_filename.size() > std::numeric_limits<std::size_t>::max() - 1 - encryption_input.size,
        "File Size Error: Encrypted output overflow.");
    const std::size_t filename_prefix_size = 1 + data_filename.size();
    const std::size_t encrypted_payload_size = computeStreamEncryptedSizePrefixed(
        encryption_input.size,
        filename_prefix_size);
    validateCombinedSizeLimits(encrypted_payload_size, jpg_size, flags);

    ConcealFinalizeResult result = flags.has_bluesky_option
        ? concealBlueskyPath(segment_vec, cover, encryption_input, data_filename, platforms_vec)
        : concealDefaultPath(segment_vec, cover, encryption_input, data_filename, platforms_vec);

    finalizeConcealOutput(platforms_vec, result);
}
