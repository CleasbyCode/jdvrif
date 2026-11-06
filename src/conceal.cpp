#include "conceal.h"
#include "binary_io.h"
#include "compression.h"
#include "embedded_layout.h"
#include "encryption.h"
#include "encryption_internal.h"
#include "file_utils.h"
#include "jpeg_utils.h"
#include "key_codec.h"
#include "reddit_steg.h"
#include "segmentation.h"
#include "signal_utils.h"
#include "step_output.h"
#include "template_assets.h"
#include "twitter_steg.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>

#include <sys/ioctl.h>
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
    MAX_SIZE_CONCEAL          = 2uz * 1024 * 1024 * 1024, // Finished default-mode image: template, ICC headers, encrypted payload and cover
    OUTPUT_STREAM_BUFFER      = 1 * 1024 * 1024;

struct EncryptionInput {
    fs::path path{};
    std::size_t size{0};
    bool is_compressed{true};
};

// A fully written, fsynced image on a link-free StagingFile inode. It has no
// directory entry until finalizeConcealOutput gives it one.
struct StagedImage {
    fs::path output_path{};
    std::unique_ptr<StagingFile> file{};
};

struct EmbeddedWriteResult {
    StagedImage staged;
    SegmentedEmbedSummary summary{};
};

struct ConcealFinalizeResult {
    RecoveryKey recovery_key{};
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

// Intermediate stages (deflated payload, encrypted payload) go to link-free
// StagingFile inodes rather than named temporaries: deflate is not encryption,
// so the compressed payload is plaintext-equivalent and must never be visible
// in a directory listing, to a backup/sync client, or survive a SIGKILL.
// See StagingFile in file_utils.h.

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

[[nodiscard]] OptimizedCover prepareCoverImage(std::span<const Byte> input, std::size_t source_data_size, Option option) {
    const bool has_no_option = option == Option::None;
    const bool is_progressive = (source_data_size < PROGRESSIVE_SOURCE_LIMIT) && has_no_option;
    return optimizeImage(input, is_progressive, has_no_option);
}

void validateCoverImageLimits(std::size_t jpg_size, Option option) {
    throwIf(
        jpg_size > MAX_OPTIMIZED_IMAGE_SIZE,
        "Image File Error: Cover image exceeds default maximum size limit for all conceal modes.");
    throwIf(
        option == Option::Bluesky && jpg_size > MAX_BLUESKY_IMAGE_SIZE,
        "File Size Error: Cover image exceeds the maximum size limit of 2,000,000 bytes for the Bluesky platform. Use a smaller cover image.");
}

[[nodiscard]] std::string validateDataFilename(const fs::path& data_file_path) {
    std::string data_filename = data_file_path.filename().string();
    throwIf(
        !hasSafeEmbeddedFilename(data_file_path.filename()),
        "Data File Error: Embedded filename is unsafe. "
        "Filenames may not begin with '.' or '-', and must be valid UTF-8 "
        "without control or bidirectional-formatting characters.");
    throwIf(
        data_filename.size() > DATA_FILENAME_MAX_LENGTH,
        "Data File Error: For compatibility requirements, "
        "data filename must not exceed 20 bytes (UTF-8). "
        "Non-ASCII characters use 2-4 bytes each.");
    return data_filename;
}

// Names the format when the data file's leading bytes show one that is already
// entropy-coded, so a zlib pass gains ~0%; empty otherwise. The header, not the
// extension, decides: a renamed archive is still recognized, and a text file
// named ".zip" is still compressed. Only consulted for large default/Bluesky-
// mode inputs (alreadyCompressedFormat), to save the time of a pointless
// multi-megabyte deflate; everywhere else the payload is compressed and the
// smaller of the two forms is kept. Executables are deliberately absent: they
// typically deflate by 30-50%.
constexpr std::size_t FORMAT_HEADER_SIZE = 16;

[[nodiscard]] std::string_view detectCompressedFormat(std::span<const Byte> header) {
    const auto has = [header](std::string_view signature, std::size_t at = 0) {
        return header.size() >= at + signature.size()
            && std::equal(signature.begin(), signature.end(), header.begin() + static_cast<std::ptrdiff_t>(at),
                   [](char a, Byte b) { return static_cast<Byte>(a) == b; });
    };
    using namespace std::string_view_literals;

    // Archives and compressed streams (ZIP also covers jar, apk, docx, odt, epub).
    if (has("PK\x03\x04"sv) || has("PK\x05\x06"sv) || has("PK\x07\x08"sv)) return "ZIP";
    if (has("Rar!\x1a\x07"sv)) return "RAR";
    if (has("7z\xbc\xaf\x27\x1c"sv)) return "7-Zip";
    if (has("\x1f\x8b"sv)) return "gzip";
    if (has("BZh"sv) && header.size() > 3 && header[3] >= '1' && header[3] <= '9') return "bzip2";
    if (has("\xfd" "7zXZ\x00"sv)) return "xz";
    if (has("LZIP"sv)) return "lzip";
    if (has("\x04\x22\x4d\x18"sv)) return "LZ4";
    if (has("\x28\xb5\x2f\xfd"sv)) return "Zstandard";
    if (has("MSCF\x00\x00\x00\x00"sv)) return "CAB";
    if (has("\xed\xab\xee\xdb"sv)) return "RPM";
    if (has("!<arch>\ndebian"sv)) return "Debian package";
    // Images.
    if (has("\xff\xd8\xff"sv)) return "JPEG";
    if (has("\x89PNG\r\n\x1a\n"sv)) return "PNG";
    if (has("GIF87a"sv) || has("GIF89a"sv)) return "GIF";
    if (has("RIFF"sv) && has("WEBP"sv, 8)) return "WebP";
    if (has("\xff\x0a"sv) || has("\x00\x00\x00\x0cJXL \r\n\x87\n"sv)) return "JPEG XL";
    // Audio and video. "ftyp" at offset 4 is ISO media: MP4, MOV, M4A, HEIC, AVIF.
    if (has("ftyp"sv, 4)) return "MP4/ISO media";
    if (has("\x1a\x45\xdf\xa3"sv)) return "Matroska/WebM";
    if (has("OggS"sv)) return "Ogg";
    if (has("fLaC"sv)) return "FLAC";
    if (has("ID3"sv)) return "MP3";
    // MPEG audio frame sync (MP3, ADTS AAC).
    if (header.size() >= 2 && header[0] == 0xFF && (header[1] & 0xE0) == 0xE0) return "MPEG audio";
    return {};
}

// The detected format when a large payload should skip compression; empty
// when it should be compressed.
[[nodiscard]] std::string_view alreadyCompressedFormat(const fs::path& data_file_path, std::size_t source_data_size) {
    if (source_data_size <= COMPRESS_BYPASS_SIZE) return {};
    const std::string open_error = std::format("Failed to open file for compression: {}", displaySafePath(data_file_path));
    InputFile input(data_file_path, open_error);
    std::array<Byte, FORMAT_HEADER_SIZE> header{};
    WipePodGuard<std::array<Byte, FORMAT_HEADER_SIZE>> header_wipe{header};
    const std::size_t header_size = input.readSome(header, open_error);
    return detectCompressedFormat(std::span<const Byte>(header).first(header_size));
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
    // Recovery identifies a Bluesky image by the signature at this fixed offset.
    throwIf(
        template_bytes.size() < BLUESKY_SEGMENT_LAYOUT.exif_segment_data_insert_index ||
            !std::ranges::equal(
                template_bytes.subspan(BLUESKY_JDVRIF_SIGNATURE_INDEX_ABS, JDVRIF_SIGNATURE.size()),
                JDVRIF_SIGNATURE),
        "Internal Error: Corrupt Bluesky segment template.");
    return copyTemplateBytes(template_bytes);
}

[[nodiscard]] vBytes makeSegmentTemplate(bool has_bluesky_option) {
    return has_bluesky_option ? makeBlueskySegmentTemplate() : makeDefaultSegmentTemplate();
}

// ---------------------------------------------------------------------------
// Conceal progress report (step helpers in step_output.h).
// ---------------------------------------------------------------------------

constexpr std::string_view ENCRYPTION_RESULT =
    "XChaCha20-Poly1305, 256-bit key derived by Argon2id from the 128-bit recovery KEY.";

// The compress step for a large data file shows a wait note on its result
// line, on a terminal only (a log gets just the result). When the step ends,
// the cursor goes back to the start of the note, rows included when it
// wrapped, and the note is cleared and replaced by the result.
constexpr std::string_view LARGE_FILE_WAIT =
    " Please wait. Larger data files will take longer to complete this process.";

// No note when compression is skipped: the step then ends at once.
[[nodiscard]] bool beginCompressStep(std::size_t data_size, bool bypass_compression = false) {
    beginStep("Compressing data file");
    if (bypass_compression || data_size <= LARGE_FILE_SIZE || isatty(STDOUT_FILENO) == 0) return false;
    std::print("{}", LARGE_FILE_WAIT);
    (void)std::fflush(stdout);
    return true;
}

void endCompressStep(bool wait_shown, std::string_view result) {
    if (wait_shown) {
        std::size_t columns = 80;
        winsize window{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) == 0 && window.ws_col > 0) {
            columns = window.ws_col;
        }
        const std::size_t rows_up = (LARGE_FILE_WAIT.size() - 1) / columns;
        if (rows_up > 0) std::print("\x1b[{}A", rows_up);
        std::print("\r\x1b[J");
    }
    endStep(result);
}

// Whole percent, rounded down, as "37%"; a nonzero share under 1% is "<1%".
// Sizes here stay far below SIZE_MAX / 100.
[[nodiscard]] std::string percentText(std::size_t part, std::size_t whole) {
    const std::size_t percent = whole == 0 ? 0 : part * 100 / whole;
    return percent == 0 && part > 0 ? std::string("<1%") : std::format("{}%", percent);
}

void printConcealHeader(
    const fs::path& image_file_path,
    std::size_t cover_size,
    const std::string& data_filename,
    std::size_t data_size) {

    std::println("\nCover image: {} ({}).", displaySafeFilename(image_file_path), formatSize(cover_size));
    std::println("Data file:   {} ({}).", data_filename, formatSize(data_size));
    std::print("\n");
}

// "3024x4032, Progressive YCbCr 4:2:0, Source-derived Q92 quantization, EXIF orientation applied, metadata removed."
// The default and Bluesky transforms are lossless, so the quantization tables
// (and the quality estimated from them) are the source image's own.
[[nodiscard]] std::string describeOptimizedCover(const CoverImageInfo& info) {
    return std::format("{}x{}, {} {}, Source-derived Q{} quantization, {}metadata removed.",
        info.width,
        info.height,
        info.progressive ? "Progressive" : "Baseline",
        info.color_layout,
        info.quality,
        info.orientation_applied ? "EXIF orientation applied, " : "");
}

// Whether the payload may be stored uncompressed when compression does not
// shrink it. Bluesky cannot: its layout has no compression marker, and its
// recovery always inflates.
enum class RawStorage { allowed, forbidden };

// Compress the payload (unless bypassed) and keep whichever form is smaller.
// A ratio near 1:1 is common for random or already-compressed data, where the
// zlib framing would otherwise make the stored payload larger than the file.
[[nodiscard]] EncryptionInput preparePayloadInput(
    const fs::path& data_file_path,
    std::size_t source_data_size,
    bool bypass_compression,
    RawStorage raw_storage,
    std::string_view staging_tag,
    std::string_view compression_error,
    std::optional<StagingFile>& compressed_stage) {

    const EncryptionInput raw_input{
        .path = data_file_path,
        .size = source_data_size,
        .is_compressed = false,
    };
    if (bypass_compression) return raw_input;

    const fs::path& compressed_path =
        compressed_stage.emplace(fs::path{}, staging_tag).path();
    zlibCompressFileToPath(data_file_path, compressed_path, source_data_size);
    const std::size_t compressed_size = checkedFileSize(compressed_path, compression_error, true);

    if (raw_storage == RawStorage::allowed && compressed_size >= source_data_size) {
        compressed_stage.reset();
        return raw_input;
    }
    return EncryptionInput{
        .path = compressed_path,
        .size = compressed_size,
        .is_compressed = true,
    };
}

// Result text for the "Compressing data file" step.
[[nodiscard]] std::string describeCompression(
    std::size_t source_data_size,
    const EncryptionInput& input,
    std::string_view bypass_format) {

    if (!bypass_format.empty()) {
        return std::format("skipped (file header shows {} data, already compressed).", bypass_format);
    }
    if (!input.is_compressed) return "no size reduction, stored uncompressed.";
    if (input.size >= source_data_size) {
        // Only Bluesky keeps a compressed form that did not shrink.
        return std::format("{} -> {} (no reduction; Bluesky stores compressed data only).",
            formatSize(source_data_size), formatSize(input.size));
    }
    return std::format("{} -> {} ({} smaller).",
        formatSize(source_data_size),
        formatSize(input.size),
        percentText(source_data_size - input.size, source_data_size));
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
        "Conservative maximum stored payload capacity with a 20-byte filename:",
        CAPACITY_LABEL_WIDTH,
        limits.conservative_compressed,
        limits.conservative_compressed / BYTES_PER_KIBIBYTE);
    std::println(
        "{:<{}}{} bytes (~{}KiB).",
        "Recommended  maximum stored payload capacity with a 20-byte filename:",
        CAPACITY_LABEL_WIDTH,
        limits.recommended_compressed,
        limits.recommended_compressed / BYTES_PER_KIBIBYTE);
}

[[nodiscard]] EncryptionInput prepareEncryptionInput(const fs::path& data_file_path,
                                                     std::size_t source_data_size,
                                                     bool bypass_compression,
                                                     bool is_bluesky,
                                                     vBytes& segment_vec,
                                                     std::optional<StagingFile>& compressed_stage) {
    const EncryptionInput input = preparePayloadInput(
        data_file_path,
        source_data_size,
        bypass_compression,
        is_bluesky ? RawStorage::forbidden : RawStorage::allowed,
        "comp",
        "Zlib Compression Error: Failed to build compressed payload.",
        compressed_stage);
    if (!input.is_compressed) {
        // NO_ZLIB_COMPRESSION_ID_INDEX is an offset into the *ICC* template.
        // concealData refuses Bluesky + bypass and Bluesky never falls back to
        // raw storage, so this only ever runs against that layout; keep the
        // two in step if either changes.
        throwIf(
            is_bluesky || NO_ZLIB_COMPRESSION_ID_INDEX >= segment_vec.size(),
            "Internal Error: Compression marker index out of range.");
        segment_vec[NO_ZLIB_COMPRESSION_ID_INDEX] = NO_ZLIB_COMPRESSION_ID;
    }
    return input;
}

constexpr const char* DEFAULT_SIZE_LIMIT_ERROR =
    "File Size Error: Combined size of image and data file "
    "exceeds maximum default size limit for jdvrif.";

void validateCombinedSizeLimits(
    std::size_t encrypted_payload_size,
    std::size_t template_size,
    std::size_t jpg_size,
    Option option) {

    // Check the packer's real ceiling, not just the program cap: the Bluesky
    // segments hold ~171 KiB, well under MAX_EMBEDDED_CIPHERTEXT_BLUESKY, and
    // anything between the two would otherwise be compressed, encrypted and
    // half-packed before failing inside appendXmpPayload. This runs on the
    // *encrypted* size, which is derived from the compressed payload, so a
    // large but well-compressing file is still accepted on its merits.
    if (option == Option::Bluesky) {
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
    // The cap is on the finished image, so count every byte it will hold --
    // the segment template and the per-segment ICC headers as well as the
    // payload and cover -- before anything is compressed into it.
    throwIf(
        option == Option::None &&
            embeddedJpgSize(template_size, encrypted_payload_size, jpg_size) > MAX_SIZE_CONCEAL,
        DEFAULT_SIZE_LIMIT_ERROR);
}

template<typename WriteFn>
[[nodiscard]] StagedImage writeToStagedOutput(WriteFn&& write_fn) {
    StagedImage staged{
        .output_path = uniqueOutputPath(),
        .file = std::make_unique<StagingFile>(fs::path{}, "img"),
    };
    // OutputFile's internal 1 MiB buffer batches consecutive small appends
    // while letting the bulk payload stream through sendfile(2); see
    // file_utils OutputFile for which writes that actually coalesces.
    OutputFile out(staged.file->path(), OUTPUT_STREAM_BUFFER, OutputCreate::existing_staging);
    write_fn(out);
    // Durable close: the recovery KEY is printed and then discarded, so the
    // image it unlocks must already be on stable storage before it is named and
    // the KEY shown -- a KEY for an image lost to a crash is unrecoverable.
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
    const OptimizedCover& cover) {
    SegmentedEmbedSummary summary;
    StagedImage staged = writeToStagedOutput([&](OutputFile& f) {
        summary = writeEmbeddedJpgFromEncryptedFile(
            f, segment_vec, encrypted_path, cover.leading_segments(), cover.body_view());
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
    beginStep("Encrypting");
    RecoveryKey recovery_key = encryptDataFileToFile(
        segment_vec,
        encryption_input.path,
        encryption_input.size,
        data_filename,
        encrypted_stage.path(),
        encryption_input.is_compressed);
    endStep(ENCRYPTION_RESULT);

    beginStep("Embedding");
    EmbeddedWriteResult embedded = saveEmbeddedJpgFromEncryptedPath(
        segment_vec,
        encrypted_stage.path(),
        cover);
    // A payload that fits the template's own ICC segment reports 0 extra ones.
    const std::size_t icc_segments = std::max<std::size_t>(embedded.summary.total_segments, 1);
    endStep(std::format("{} ICC_PROFILE (APP2) segment{}.",
        icc_segments,
        icc_segments == 1 ? "" : "s"));

    beginStep("Writing output image");
    // Still staged and unnamed, and the KEY not yet shown: refuse rather than
    // publish an image over the cap, however the up-front estimate was missed.
    throwIf(embedded.summary.embedded_image_size > MAX_SIZE_CONCEAL, DEFAULT_SIZE_LIMIT_ERROR);
    endStep(std::format("{}.", formatSizePrecise(embedded.summary.embedded_image_size)));

    finalizePlatformReport(platforms_vec, embedded.summary);

    return ConcealFinalizeResult{
        .recovery_key = std::move(recovery_key),
        .staged = std::move(embedded.staged),
        .embedded_jpg_size = embedded.summary.embedded_image_size,
    };
}

// "EXIF (APP1) + Photoshop (APP13) + XMP (APP1), 145603 of 175104 bytes used (83%)."
[[nodiscard]] std::string describeBlueskySegments(std::size_t encrypted_payload_size) {
    const BlueskySegmentUse use = blueskySegmentUse(encrypted_payload_size);
    const std::size_t max_payload = std::min(
        maxBlueskyEmbeddedCipherCapacity(),
        MAX_EMBEDDED_CIPHERTEXT_BLUESKY);
    return std::format("EXIF (APP1){}{}, {} of {} bytes used ({} of the Bluesky payload limit).",
        use.photoshop ? " + Photoshop (APP13)" : "",
        use.xmp ? " + XMP (APP1)" : "",
        encrypted_payload_size,
        max_payload,
        percentText(encrypted_payload_size, max_payload));
}

[[nodiscard]] ConcealFinalizeResult concealBlueskyPath(
    vBytes& segment_vec,
    const OptimizedCover& cover,
    const EncryptionInput& encryption_input,
    const std::string& data_filename,
    std::size_t encrypted_payload_size,
    vString& platforms_vec) {

    beginStep("Encrypting");
    RecoveryKey recovery_key = encryptDataFileForBluesky(
        segment_vec,
        encryption_input.path,
        encryption_input.size,
        platforms_vec,
        data_filename,
        encryption_input.is_compressed);
    endStep(ENCRYPTION_RESULT);
    // The segments are built during encryption; this step only reports them.
    beginStep("Embedding");
    endStep(describeBlueskySegments(encrypted_payload_size));

    beginStep("Writing output image");
    const std::span<const Byte> cover_view = cover.view();
    StagedImage staged = saveEmbeddedJpg(std::span<const Byte>(segment_vec), cover_view);
    const std::size_t embedded_jpg_size = checkedFileSize(
        staged.file->path(),
        "Write File Error: Failed to verify final Bluesky output image.",
        true);
    throwIf(
        embedded_jpg_size > MAX_BLUESKY_IMAGE_SIZE,
        "File Size Error: Final output image exceeds the 2,000,000-byte limit for the Bluesky platform.\n"
        "                 Use a smaller cover image or reduce the size of the payload (hidden data file).");
    endStep(std::format("{} of {} bytes ({} of the Bluesky image upload limit).",
        embedded_jpg_size,
        MAX_BLUESKY_IMAGE_SIZE,
        percentText(embedded_jpg_size, MAX_BLUESKY_IMAGE_SIZE)));

    return ConcealFinalizeResult{
        .recovery_key = std::move(recovery_key),
        .staged = std::move(staged),
        .embedded_jpg_size = embedded_jpg_size,
    };
}

// ---------------------------------------------------------------------------
// Metadata-stripping DCT carriers (-r Reddit, -x X-Twitter). Both run the same
// conceal and capsize steps; these traits hold everything that differs.
// ---------------------------------------------------------------------------

struct RedditCarrier {
    using Cover = RedditPreparedCover;
    static constexpr std::string_view NAME = "Reddit";
    static constexpr std::string_view OPTION = "-r";
    static constexpr std::string_view STAGING_TAG = "reddit";
    static constexpr std::string_view CAPACITY_KIND = "C3";
    static constexpr std::string_view EMBED_LABEL = "Embedding";
    static constexpr std::string_view EMBED_RESULT =
        "Quantization Index Modulation (QIM) --> "
        "Discrete Cosine Transform coefficients of Color Component 3 (DCT/C3).";
    static constexpr std::size_t UPLOAD_LIMIT = REDDIT_UPLOAD_SIZE_LIMIT;
    static constexpr std::size_t UPLOAD_LIMIT_MIB = 20;
    // Reddit accepts larger square images than the other platforms.
    static constexpr CoverImageLimits COVER_LIMITS = REDDIT_COVER_IMAGE_LIMITS;
    static constexpr auto envelopeSize = redditEnvelopeSize;
    static constexpr auto makeEnvelope = makeRedditEnvelope;
    static constexpr auto embedPayload = embedRedditPayload;

    static void inspectSource(std::span<const Byte>) {}

    // The carrier performs the required pixel transcode to baseline YCbCr
    // 4:2:0 with standard Q75 tables.
    static Cover prepareCover(std::span<const Byte> normalized) {
        return prepareRedditCover(normalized);
    }

    // Conceal's "Preparing cover image" result.
    [[nodiscard]] static std::string describeCover(const Cover& cover, bool orientation_applied) {
        return std::format(
            "{}x{}, Baseline YCbCr 4:2:0, Standard Q75 quantization, {}metadata removed.",
            cover.width,
            cover.height,
            orientation_applied ? "EXIF orientation applied, " : "");
    }

    static void printCoverSummary(const Cover& cover, std::size_t source_cover_size) {
        std::println(
            "Cover Image: {}KiB, {}x{}, Baseline YCbCr 4:2:0, Standard Q75 quantization (C3).\n",
            source_cover_size / BYTES_PER_KIBIBYTE,
            cover.width,
            cover.height);
    }
};

struct TwitterCarrier {
    using Cover = TwitterPreparedCover;
    static constexpr std::string_view NAME = "X-Twitter";
    static constexpr std::string_view OPTION = "-x";
    static constexpr std::string_view STAGING_TAG = "twitter";
    static constexpr std::string_view CAPACITY_KIND = "J-UNIWARD/STC";
    static constexpr std::string_view EMBED_LABEL = "Embedding";
    static constexpr std::string_view EMBED_RESULT =
        "Syndrome-Trellis Codes (STC) and J-UNIWARD --> "
        "Discrete Cosine Transform coefficients (DCT).";
    static constexpr std::size_t UPLOAD_LIMIT = TWITTER_UPLOAD_SIZE_LIMIT;
    static constexpr std::size_t UPLOAD_LIMIT_MIB = 5;
    static constexpr CoverImageLimits COVER_LIMITS = DEFAULT_COVER_IMAGE_LIMITS;
    static constexpr auto envelopeSize = twitterEnvelopeSize;
    static constexpr auto makeEnvelope = makeTwitterEnvelope;
    // Embeds into the cover's own coefficients: the cover is spent.
    static vBytes embedPayload(Cover&& cover, const CarrierKey128& carrier_key, std::span<const Byte> payload) {
        return embedTwitterPayload(std::move(cover), carrier_key, payload);
    }

    // Reject X-Twitter-incompatible dimensions from the input header before
    // doing the lossless orientation transform or pixel transcode.
    static void inspectSource(std::span<const Byte> jpg) {
        (void)inspectTwitterCover(jpg);
    }

    // The adaptive carrier prepares progressive YCbCr 4:2:0 JFIF while
    // retaining source quantization. Existing 4:2:0 covers remain in the
    // coefficient domain; only other layouts require a pixel transcode. Source
    // quality finer than Q97 is capped at Q97.
    static Cover prepareCover(std::span<const Byte> normalized) {
        return prepareTwitterCover(normalized);
    }

    // Conceal's "Preparing cover image" result.
    [[nodiscard]] static std::string describeCover(const Cover& cover, bool orientation_applied) {
        const std::string quality = cover.source_quality > cover.carrier_quality
            ? std::format("Source Q{} capped to Q{}", cover.source_quality, cover.carrier_quality)
            : std::format("Source-derived Q{} quantization", cover.carrier_quality);
        return std::format(
            "{}x{}, Progressive YCbCr 4:2:0, {}, {}metadata removed.",
            cover.width,
            cover.height,
            quality,
            orientation_applied ? "EXIF orientation applied, " : "");
    }

    static void printCoverSummary(const Cover& cover, std::size_t source_cover_size) {
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
};

// "Reddit's 20 MiB upload size limit" / "X-Twitter's 5 MiB upload size limit".
template<typename Carrier>
[[nodiscard]] std::string uploadLimitText() {
    return std::format("{}'s {} MiB upload size limit", Carrier::NAME, Carrier::UPLOAD_LIMIT_MIB);
}

template<typename Carrier>
void validateCarrierPreliminaryLimits(std::size_t cover_size, std::size_t payload_size) {
    const bool cover_too_large = cover_size > Carrier::UPLOAD_LIMIT;
    const bool payload_too_large = payload_size > Carrier::UPLOAD_LIMIT;
    if (cover_too_large && payload_too_large) {
        throw std::runtime_error(std::format(
            "File Size Error: Cover image and payload file exceed {}.", uploadLimitText<Carrier>()));
    }
    if (cover_too_large) {
        throw std::runtime_error(std::format(
            "Image File Size Error: Cover image exceeds {}.", uploadLimitText<Carrier>()));
    }
    if (payload_too_large) {
        throw std::runtime_error(std::format(
            "Data File Size Error: Payload file exceeds {}.", uploadLimitText<Carrier>()));
    }
}

template<typename Carrier>
[[nodiscard]] typename Carrier::Cover prepareCarrier(vBytes& jpg_vec, bool* orientation_applied = nullptr) {
    Carrier::inspectSource(jpg_vec);

    // The carrier decodes the cover itself and writes none of its markers, so
    // a cover with no EXIF orientation to apply goes to it as it is: the
    // lossless rewrite below would change nothing the carrier reads.
    // Otherwise apply jdvrif's lossless EXIF-orientation transform (which also
    // strips the metadata) first, and prepare the carrier from that.
    typename Carrier::Cover cover = [&] {
        if (coverNeedsNoNormalization(jpg_vec, Carrier::COVER_LIMITS)) {
            if (orientation_applied != nullptr) *orientation_applied = false;
            typename Carrier::Cover prepared = Carrier::prepareCover(jpg_vec);
            vBytes{}.swap(jpg_vec);
            return prepared;
        }
        OptimizedCover normalized = optimizeImage(
            std::span<const Byte>(jpg_vec),
            /*isProgressive=*/false,
            /*enforceQualityLimit=*/false,
            Carrier::COVER_LIMITS);
        vBytes{}.swap(jpg_vec);
        if (orientation_applied != nullptr) *orientation_applied = normalized.info().orientation_applied;
        return Carrier::prepareCover(normalized.full_view());
    }();
    if (cover.jpeg.size() > Carrier::UPLOAD_LIMIT) {
        throw std::runtime_error(std::format(
            "Image File Size Error: Transcoded cover image exceeds {}.", uploadLimitText<Carrier>()));
    }
    return cover;
}

template<typename Carrier>
[[nodiscard]] CarrierCapacityLimits carrierCoverCapacityLimits(const typename Carrier::Cover& cover) {
    return carrierCapacityLimits(cover.payload_capacity, Carrier::envelopeSize);
}

template<typename Carrier>
void printCarrierCoverCapacityLimits(const typename Carrier::Cover& cover, const CarrierCapacityLimits& limits) {
    printCarrierCapacityLimits(
        std::format("Theoretical {} capacity limit for this cover image:", Carrier::CAPACITY_KIND),
        cover.payload_capacity,
        limits);
}

// The payload does not fit this cover with this filename: a hard limit, so
// report the real figures for the supplied filename rather than the generic
// 20-byte-filename guidance that capsize prints.
template<typename Carrier>
[[noreturn]] void failCarrierPayloadCapacity(
    const typename Carrier::Cover& cover,
    std::size_t source_data_size,
    const EncryptionInput& encryption_input,
    std::size_t filename_size,
    std::size_t envelope_size) {

    constexpr int LABEL_WIDTH = 57;
    const std::size_t stored_payload_size = encryption_input.size;
    const std::size_t overhead_size = envelope_size - stored_payload_size;
    const std::size_t max_stored_payload = maxCarrierStoredPayload(
        cover.payload_capacity, filename_size, Carrier::envelopeSize);
    const std::size_t bytes_over = stored_payload_size > max_stored_payload
        ? stored_payload_size - max_stored_payload
        : 1;
    const std::size_t recommended_payload = max_stored_payload > BYTES_PER_KIBIBYTE
        ? max_stored_payload - BYTES_PER_KIBIBYTE
        : 0;
    const std::string storage_note = encryption_input.is_compressed
        ? std::format("compressed by jdvrif from {} bytes", source_data_size)
        : std::string("stored uncompressed; compression did not reduce it");

    // The prepare step has already printed the cover summary.
    std::print("\n");
    std::println("{:<{}}{} bytes ({}).", "Stored payload size:", LABEL_WIDTH,
                 stored_payload_size, storage_note);
    std::println("{:<{}}{} bytes ({}-byte filename).", "Encryption and carrier overhead:", LABEL_WIDTH,
                 overhead_size, filename_size);
    std::println("{:<{}}{} bytes.", "Total carrier envelope size:", LABEL_WIDTH, envelope_size);
    std::println("{:<{}}{} bytes.",
                 std::format("Theoretical {} capacity of this cover image:", Carrier::CAPACITY_KIND),
                 LABEL_WIDTH, cover.payload_capacity);
    std::println("{:<{}}{} bytes (~{}KiB).", "Maximum stored payload size for this filename:", LABEL_WIDTH,
                 max_stored_payload, max_stored_payload / BYTES_PER_KIBIBYTE);
    if (recommended_payload > 0) {
        std::println("{:<{}}{} bytes (~{}KiB, keeps 1KiB headroom).", "Recommended stored payload size:", LABEL_WIDTH,
                     recommended_payload, recommended_payload / BYTES_PER_KIBIBYTE);
    }
    (void)std::fflush(stdout);

    throw std::runtime_error(std::format(
        "Data File Size Error: Stored payload of {} bytes exceeds this cover image's capacity "
        "by {} byte{}.\n"
        "                      Use a smaller data file or a larger cover image.",
        stored_payload_size,
        bytes_over,
        bytes_over == 1 ? "" : "s"));
}

template<typename Carrier>
[[nodiscard]] ConcealFinalizeResult concealCarrierPath(
    vBytes& jpg_vec,
    const fs::path& data_file_path,
    std::size_t source_data_size,
    const std::string& data_filename,
    vString& platforms_vec) {

    beginStep("Preparing cover image");
    bool orientation_applied = false;
    typename Carrier::Cover cover = prepareCarrier<Carrier>(jpg_vec, &orientation_applied);
    endStep(Carrier::describeCover(cover, orientation_applied));

    // The carrier budget is small and every byte matters, so always try
    // compression and keep whichever form is smaller (the envelope flag
    // records which). Inputs are capped at the upload limit, so the trial
    // deflate is cheap.
    std::optional<StagingFile> compressed_stage;
    const bool compress_wait = beginCompressStep(source_data_size);
    const EncryptionInput encryption_input = preparePayloadInput(
        data_file_path,
        source_data_size,
        /*bypass_compression=*/false,
        RawStorage::allowed,
        std::format("{}_comp", Carrier::STAGING_TAG),
        std::format("Zlib Compression Error: Failed to build {} compressed payload.", Carrier::NAME),
        compressed_stage);
    endCompressStep(compress_wait, describeCompression(source_data_size, encryption_input, {}));
    const std::size_t stored_payload_size = encryption_input.size;

    if (data_filename.size() > std::numeric_limits<std::size_t>::max() - 1 - stored_payload_size) {
        throw std::runtime_error(std::format("File Size Error: Encrypted {} output overflow.", Carrier::NAME));
    }
    const std::size_t filename_prefix_size = 1 + data_filename.size();
    beginStep("Checking cover capacity");
    const std::size_t encrypted_size = computeStreamEncryptedSizePrefixed(
        stored_payload_size,
        filename_prefix_size);
    const std::size_t envelope_size = Carrier::envelopeSize(encrypted_size);
    if (envelope_size > cover.payload_capacity) {
        failStep("does not fit.");
        failCarrierPayloadCapacity<Carrier>(
            cover, source_data_size, encryption_input, data_filename.size(), envelope_size);
    }
    endStep(std::format("{} of {} bytes ({}).",
        envelope_size, cover.payload_capacity, percentText(envelope_size, cover.payload_capacity)));

    vBytes kdf_metadata;
    WipeBytesGuard kdf_metadata_wipe{kdf_metadata};
    vBytes encrypted_data;
    WipeBytesGuard encrypted_wipe{encrypted_data};
    beginStep("Encrypting");
    RecoveryKey recovery_key = encryptDataFileForCarrier(
        kdf_metadata,
        encrypted_data,
        encryption_input.path,
        stored_payload_size,
        data_filename,
        encryption_input.is_compressed);
    compressed_stage.reset();
    if (encrypted_data.size() != encrypted_size) {
        throw std::runtime_error(std::format(
            "Encryption Error: {} encrypted payload has an unexpected size.", Carrier::NAME));
    }

    vBytes envelope = Carrier::makeEnvelope(
        kdf_metadata,
        encrypted_data,
        encryption_input.is_compressed);
    WipeBytesGuard envelope_wipe{envelope};
    endStep(ENCRYPTION_RESULT);

    beginStep(Carrier::EMBED_LABEL);
    // The carrier's coefficient positions, whitening and dither are keyed by
    // an Argon2id hash of the 128-bit KEY that was just minted, so only the
    // KEY holder can locate or read the payload. (Keying hides where it is,
    // not that it exists.)
    CarrierKey128 carrier_key = deriveCarrierKey128FromRecoveryKey(recovery_key);
    WipePodGuard<CarrierKey128> carrier_key_wipe{carrier_key};
    const vBytes embedded_jpeg = Carrier::embedPayload(
        std::move(cover),
        carrier_key,
        envelope);
    endStep(Carrier::EMBED_RESULT);

    beginStep("Writing output image");
    StagedImage staged = saveEmbeddedJpg({}, embedded_jpeg);
    const std::size_t embedded_jpg_size = checkedFileSize(
        staged.file->path(),
        std::format("Write File Error: Failed to verify final {} output image.", Carrier::NAME),
        true);
    if (embedded_jpg_size > Carrier::UPLOAD_LIMIT) {
        throw std::runtime_error(std::format(
            "File Size Error: Final output image exceeds {}.\n"
            "                 Use a smaller cover image or reduce the size of the payload.",
            uploadLimitText<Carrier>()));
    }
    endStep(std::format("{} ({} of {}).",
        formatSizePrecise(embedded_jpg_size),
        percentText(embedded_jpg_size, Carrier::UPLOAD_LIMIT),
        uploadLimitText<Carrier>()));

    platforms_vec = {
        std::format("{0}. (Only share this \"file-embedded\" JPG image on {0}).", Carrier::NAME)
    };
    return ConcealFinalizeResult{
        .recovery_key = std::move(recovery_key),
        .staged = std::move(staged),
        .embedded_jpg_size = embedded_jpg_size,
    };
}

template<typename Carrier>
void displayCarrierCapacity(vBytes& jpg_vec) {
    const std::size_t source_cover_size = jpg_vec.size();
    const typename Carrier::Cover cover = prepareCarrier<Carrier>(jpg_vec);
    const CarrierCapacityLimits capacity_limits = carrierCoverCapacityLimits<Carrier>(cover);
    const std::size_t minimum_single_frame_overhead = Carrier::envelopeSize(
        computeStreamEncryptedSizePrefixed(
            0,
            /*one-byte filename plus its length byte=*/2));
    const std::size_t maximum_single_frame_overhead = Carrier::envelopeSize(
        computeStreamEncryptedSizePrefixed(
            0,
            /*maximum filename plus its length byte=*/
                1 + DATA_FILENAME_MAX_LENGTH));

    std::println("\n{} capacity check for conceal {} mode only.\n", Carrier::NAME, Carrier::OPTION);
    Carrier::printCoverSummary(cover, source_cover_size);
    printCarrierCoverCapacityLimits<Carrier>(cover, capacity_limits);

    std::println(
        "\nImportant:\n\n"
        "This is total encrypted carrier-envelope capacity, not a raw secret-file limit.\n\n"
        "JDVRIF compresses the input and stores whichever of the compressed or original form is smaller.\n\n"
        "For a payload contained in one encryption frame, filename, encryption and recovery metadata\n"
        "consume {} to {} bytes; larger payloads add framing overhead.\n\n"
        "Do not target the theoretical exact limit. When capacity permits, keep the stored payload at least\n"
        "1KiB below the conservative maximum shown above. The final conceal {} size check is authoritative.\n"
        "The final embedded JPEG size can differ from the cover image and must remain within {}MiB.\n",
        minimum_single_frame_overhead,
        maximum_single_frame_overhead,
        Carrier::OPTION,
        Carrier::UPLOAD_LIMIT_MIB);
}

constexpr const char* KEY_DELIVERY_ERROR = "Output Error: Failed to deliver recovery KEY.";

void flushStdoutOrThrow() {
    throwIf(std::fflush(stdout) != 0 || std::ferror(stdout) != 0, KEY_DELIVERY_ERROR);
}

// std::print / std::format would stage the KEY's text in a heap buffer that is
// freed without being zeroed, undoing the wiping the rest of the KEY path is
// careful about. Render the grouped Base58 form into a stack array, drain
// stdio so ordering is preserved, write it straight to fd 1 between two
// constant fragments, then zero the array (WipePodGuard, on every exit path).
void printRecoveryKeySecurely(const RecoveryKey& recovery_key) {
    // Conceal only ever mints 128-bit KEYs.
    throwIf(recovery_key.kind != RecoveryKey::Kind::base58_128, KEY_DELIVERY_ERROR);

    std::array<char, RECOVERY_KEY_DISPLAY_CHARS> display{};
    WipePodGuard<std::array<char, RECOVERY_KEY_DISPLAY_CHARS>> wipe_display{display};
    encodeRecoveryKeyDisplay(recovery_key.secret, display);

    std::print("\nRecovery KEY: [***");
    flushStdoutOrThrow();
    writeAllToFd(
        STDOUT_FILENO,
        std::span<const Byte>(reinterpret_cast<const Byte*>(display.data()), display.size()),
        KEY_DELIVERY_ERROR);
    std::print("***]\n\nImportant: Keep your KEY safe, so that you can extract the hidden file.\n\n");
    flushStdoutOrThrow();
}

// Give the staged image its final name. The name was free when it was chosen;
// if something has taken it since, nothing has been promised yet, so another is
// picked rather than failing.
[[nodiscard]] fs::path commitStagedImage(const StagedImage& staged) {
    fs::path candidate = staged.output_path;
    for (std::size_t attempt = 0; attempt < MAX_PATH_ATTEMPTS; ++attempt) {
        if (tryCommitStagingFileNoReplace(*staged.file, candidate, "Write File Error: Failed to commit output image")) {
            return candidate;
        }
        candidate = uniqueOutputPath();
    }
    throwError("Write File Error: Could not create a unique output filename.");
}

// Name the image first, then hand out its KEY. Interrupted in between, the
// result is at worst an image nobody can open; the reverse order could deliver a
// KEY for an image that never got a name. If the KEY cannot be delivered, the
// just-named image is removed again: without its KEY it is useless.
void finalizeConcealOutput(const vString& platforms_vec, ConcealFinalizeResult& result) {
    const fs::path output_path = commitStagedImage(result.staged);

    try {
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
        printRecoveryKeySecurely(result.recovery_key);
    } catch (...) {
        cleanupPathNoThrow(output_path);
        throw;
    }
    result.recovery_key.wipe();

    std::println("Saved \"file-embedded\" JPG image: {} ({} bytes).\n\nComplete!\n",
                 output_path.string(),
                 result.embedded_jpg_size);
    flushStdoutOrThrow();
}
} // namespace

void displayRedditCapacity(vBytes& jpg_vec) {
    displayCarrierCapacity<RedditCarrier>(jpg_vec);
}

void displayTwitterCapacity(vBytes& jpg_vec) {
    displayCarrierCapacity<TwitterCarrier>(jpg_vec);
}

void concealData(vBytes& jpg_vec, Option option, const fs::path& image_file_path, const fs::path& data_file_path) {
    vString platforms_vec = platformReportTemplate();
    const std::size_t source_data_size = validateFileForRead(data_file_path);

    if (option == Option::Reddit || option == Option::Twitter) {
        const auto conceal_with = [&]<typename Carrier>() {
            validateCarrierPreliminaryLimits<Carrier>(jpg_vec.size(), source_data_size);
            const std::string data_filename = validateDataFilename(data_file_path);
            printConcealHeader(image_file_path, jpg_vec.size(), data_filename, source_data_size);
            ConcealFinalizeResult result = concealCarrierPath<Carrier>(
                jpg_vec,
                data_file_path,
                source_data_size,
                data_filename,
                platforms_vec);
            finalizeConcealOutput(platforms_vec, result);
        };
        if (option == Option::Reddit) {
            conceal_with.operator()<RedditCarrier>();
        } else {
            conceal_with.operator()<TwitterCarrier>();
        }
        return;
    }

    const std::string data_filename = validateDataFilename(data_file_path);
    printConcealHeader(image_file_path, jpg_vec.size(), data_filename, source_data_size);

    beginStep("Preparing cover image");
    OptimizedCover cover = prepareCoverImage(jpg_vec, source_data_size, option);
    // Caller's input vector is no longer needed; release its storage so it
    // doesn't sit alongside the OptimizedCover for the rest of the run.
    vBytes{}.swap(jpg_vec);

    const std::size_t jpg_size = cover.trimmed_size();
    validateCoverImageLimits(jpg_size, option);
    endStep(describeOptimizedCover(cover.info()));

    const std::string_view bypass_format = alreadyCompressedFormat(data_file_path, source_data_size);
    const bool bypass_compression = !bypass_format.empty();

    // Bluesky recovery hardcodes is_data_compressed (see recoverFromBlueskyPath):
    // its layout carries no compression marker, so a stored-uncompressed payload
    // could never be read back. Reject here rather than letting the marker write
    // in prepareEncryptionInput -- an index that belongs to the ICC template --
    // land in the Bluesky EXIF template on the way to a size failure further on.
    // Bypass needs a >10 MB source, which can never fit the ~171 KiB Bluesky
    // payload limit uncompressed, so nothing valid is turned away.
    throwIf(
        option == Option::Bluesky && bypass_compression,
        "Data File Size Error: Data file is far above the maximum payload size for the Bluesky platform.\n"
        "                      Use a smaller data file, or compress it yourself first.");

    const bool is_bluesky = option == Option::Bluesky;
    vBytes segment_vec = makeSegmentTemplate(is_bluesky);

    std::optional<StagingFile> compressed_stage;
    const bool compress_wait = beginCompressStep(source_data_size, bypass_compression);
    const EncryptionInput encryption_input = prepareEncryptionInput(
        data_file_path,
        source_data_size,
        bypass_compression,
        is_bluesky,
        segment_vec,
        compressed_stage);
    endCompressStep(compress_wait, describeCompression(source_data_size, encryption_input, bypass_format));

    throwIf(
        data_filename.size() > std::numeric_limits<std::size_t>::max() - 1 - encryption_input.size,
        "File Size Error: Encrypted output overflow.");
    const std::size_t filename_prefix_size = 1 + data_filename.size();
    const std::size_t encrypted_payload_size = computeStreamEncryptedSizePrefixed(
        encryption_input.size,
        filename_prefix_size);
    validateCombinedSizeLimits(encrypted_payload_size, segment_vec.size(), jpg_size, option);

    ConcealFinalizeResult result = is_bluesky
        ? concealBlueskyPath(segment_vec, cover, encryption_input, data_filename, encrypted_payload_size, platforms_vec)
        : concealDefaultPath(segment_vec, cover, encryption_input, data_filename, platforms_vec);

    finalizeConcealOutput(platforms_vec, result);
}
