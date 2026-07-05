#include "conceal.h"
#include "binary_io.h"
#include "compression.h"
#include "embedded_layout.h"
#include "encryption.h"
#include "file_utils.h"
#include "jpeg_utils.h"
#include "segmentation.h"
#include "template_assets.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <limits>
#include <print>
#include <span>
#include <stdexcept>
#include <string>

namespace {
constexpr std::size_t
    MAX_PATH_ATTEMPTS         = 1024,
    PROGRESSIVE_SOURCE_LIMIT  = 2 * 1024 * 1024,
    MAX_OPTIMIZED_IMAGE_SIZE  = 4 * 1024 * 1024,
    MAX_OPTIMIZED_BLUESKY_IMG = 805 * 1024,
    DATA_FILENAME_MAX_LENGTH  = 20,
    LARGE_FILE_SIZE           = 300 * 1024 * 1024,
    COMPRESS_BYPASS_SIZE      = 10 * 1024 * 1024,
    MAX_SIZE_CONCEAL          = 2ULL * 1024 * 1024 * 1024,
    MAX_SIZE_REDDIT           = 20 * 1024 * 1024,
    MAX_DATA_SIZE_BLUESKY     = 2 * 1024 * 1024,
    OUTPUT_STREAM_BUFFER      = 1 * 1024 * 1024;

struct ConcealFlags {
    bool has_no_option{false};
    bool has_bluesky_option{false};
    bool has_reddit_option{false};
};

struct EncryptionInput {
    fs::path path{};
    std::size_t size{0};
};

struct EmbeddedWriteResult {
    fs::path output_path{};
    SegmentedEmbedSummary summary{};
};

struct ConcealFinalizeResult {
    std::uint64_t recovery_pin{0};
    fs::path output_path{};
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

[[nodiscard]] fs::path tempStagePath(const std::string& base_name, std::string_view tag) {
    return randomizedPath(
        {},
        std::format(".{}.jdvrif_{}_", base_name.empty() ? "payload" : fs::path(base_name).stem().string(), tag),
        ".bin",
        std::format("Write File Error: Could not create a temporary {} filename.", tag));
}

[[nodiscard]] ConcealFlags concealFlags(Option option) {
    return {
        .has_no_option = option == Option::None,
        .has_bluesky_option = option == Option::Bluesky,
        .has_reddit_option = option == Option::Reddit,
    };
}

[[nodiscard]] vString platformReportTemplate() {
    return vString{
        "X-Twitter", "Tumblr",
        "Bluesky. (Only share this \"file-embedded\" JPG image on Bluesky).\n\n "
        "You must use the Python script \"bsky_post.py\" (found in the repo src folder)\n "
        "to post the image to Bluesky.",
        "Mastodon", "Pixelfed",
        "Reddit. (Only share this \"file-embedded\" JPG image on Reddit).",
        "PostImage", "ImgBB", "ImgPile", "Flickr"
    };
}

[[nodiscard]] OptimizedCover prepareCoverImage(std::span<const Byte> input, std::size_t source_data_size, const ConcealFlags& flags) {
    const bool is_progressive = (source_data_size < PROGRESSIVE_SOURCE_LIMIT) && flags.has_no_option;
    return optimizeImage(input, is_progressive);
}

void validateCoverImageLimits(std::size_t jpg_size, const ConcealFlags& flags) {
    if (jpg_size > MAX_OPTIMIZED_IMAGE_SIZE) {
        throw std::runtime_error("Image File Error: Cover image file exceeds maximum size limit.");
    }
    if (flags.has_bluesky_option && jpg_size > MAX_OPTIMIZED_BLUESKY_IMG) {
        throw std::runtime_error("File Size Error: Image file exceeds maximum size limit for the Bluesky platform.");
    }
}

[[nodiscard]] std::string validateDataFilename(const fs::path& data_file_path) {
    std::string data_filename = data_file_path.filename().string();
    if (!hasSafeEmbeddedFilename(data_file_path.filename())) {
        throw std::runtime_error(
            "Data File Error: Embedded filename is unsafe. "
            "Filenames may not begin with '.' or '-'.");
    }
    if (data_filename.size() > DATA_FILENAME_MAX_LENGTH) {
        throw std::runtime_error(
            "Data File Error: For compatibility requirements, "
            "length of data filename must not exceed 20 characters.");
    }
    return data_filename;
}

[[nodiscard]] bool shouldBypassCompression(const fs::path& data_file_path, std::size_t source_data_size) {
    return source_data_size > COMPRESS_BYPASS_SIZE && hasFileExtension(data_file_path, {
        ".zip", ".jar", ".rar", ".7z", ".bz2", ".gz", ".xz", ".lz", ".lz4", ".cab", ".rpm", ".deb",
        ".mp4", ".mp3", ".exe", ".jpg", ".jpeg", ".jfif", ".png", ".webp", ".gif", ".ogg", ".flac"
    });
}

[[nodiscard]] vBytes copyTemplateBytes(std::span<const Byte> template_bytes) {
    return vBytes(template_bytes.begin(), template_bytes.end());
}

[[nodiscard]] vBytes makeDefaultSegmentTemplate() {
    std::span<const Byte> template_bytes = defaultIccTemplateBytes();
    if (template_bytes.size() != DEFAULT_METADATA_PREFIX_BYTES) {
        throw std::runtime_error("Internal Error: Corrupt default ICC segment template.");
    }

    vBytes out = copyTemplateBytes(template_bytes);
    if (!spanHasRange(out, DEFAULT_ICC_SIGNATURE_INDEX_ABS, ICC_PROFILE_SIGNATURE.size()) ||
        !spanHasRange(out, DEFAULT_JDVRIF_SIGNATURE_INDEX_ABS, JDVRIF_SIGNATURE.size())) {
        throw std::runtime_error("Internal Error: Corrupt default ICC segment template.");
    }

    std::copy(ICC_PROFILE_SIGNATURE.begin(), ICC_PROFILE_SIGNATURE.end(),
              out.begin() + static_cast<std::ptrdiff_t>(DEFAULT_ICC_SIGNATURE_INDEX_ABS));
    std::copy(JDVRIF_SIGNATURE.begin(), JDVRIF_SIGNATURE.end(),
              out.begin() + static_cast<std::ptrdiff_t>(DEFAULT_JDVRIF_SIGNATURE_INDEX_ABS));
    return out;
}

[[nodiscard]] vBytes makeBlueskySegmentTemplate() {
    std::span<const Byte> template_bytes = blueskyExifTemplateBytes();
    if (template_bytes.size() < BLUESKY_SEGMENT_LAYOUT.exif_segment_data_insert_index) {
        throw std::runtime_error("Internal Error: Corrupt Bluesky segment template.");
    }
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

[[nodiscard]] EncryptionInput prepareEncryptionInput(const fs::path& data_file_path,
                                                     const std::string& data_filename,
                                                     std::size_t source_data_size,
                                                     bool bypass_compression,
                                                     vBytes& segment_vec,
                                                     TempFileCleanupGuard& compressed_guard) {
    if (bypass_compression) {
        if (NO_ZLIB_COMPRESSION_ID_INDEX >= segment_vec.size()) {
            throw std::runtime_error("Internal Error: Compression marker index out of range.");
        }
        segment_vec[NO_ZLIB_COMPRESSION_ID_INDEX] = NO_ZLIB_COMPRESSION_ID;
        return EncryptionInput{data_file_path, source_data_size};
    }

    fs::path compressed_path = tempStagePath(data_filename, "comp");
    zlibCompressFileToPath(data_file_path, compressed_path, source_data_size);
    compressed_guard.set(compressed_path);

    return EncryptionInput{
        .path = compressed_path,
        .size = checkedFileSize(
            compressed_path,
            "Zlib Compression Error: Failed to build compressed payload.",
            true)
    };
}

void validateCombinedSizeLimits(std::size_t encrypted_payload_size, std::size_t jpg_size, const ConcealFlags& flags) {
    if (encrypted_payload_size > std::numeric_limits<std::size_t>::max() - jpg_size) {
        throw std::runtime_error("File Size Error: Combined file size overflow.");
    }
    const std::size_t combined_size = encrypted_payload_size + jpg_size;

    if (flags.has_bluesky_option && encrypted_payload_size > MAX_DATA_SIZE_BLUESKY) {
        throw std::runtime_error("Data File Size Error: File exceeds maximum size limit for the Bluesky platform.");
    }
    if (flags.has_reddit_option && combined_size > MAX_SIZE_REDDIT) {
        throw std::runtime_error("File Size Error: Combined size of image and data file "
                                 "exceeds maximum size limit for the Reddit platform.");
    }
    if (flags.has_no_option && combined_size > MAX_SIZE_CONCEAL) {
        throw std::runtime_error("File Size Error: Combined size of image and data file "
                                 "exceeds maximum default size limit for jdvrif.");
    }
}

template<typename WriteFn>
[[nodiscard]] fs::path writeToStagedOutput(WriteFn&& write_fn) {
    const fs::path output_path = uniqueOutputPath();
    TempFileCleanupGuard temp_output(tempOutputPath(output_path));
    // OutputFile's internal 1 MiB buffer coalesces the small segment-header
    // writes (replacing the old ofstream pubsetbuf) while letting the bulk
    // payload stream through sendfile(2); see file_utils OutputFile.
    OutputFile out(temp_output.path, OUTPUT_STREAM_BUFFER);
    write_fn(out);
    out.close(WRITE_COMPLETE_ERROR);
    commitStagedFileNoReplaceOrThrow(
        temp_output.path,
        output_path,
        "Write File Error: Failed to commit output image");
    temp_output.dismiss();
    return output_path;
}

[[nodiscard]] fs::path saveEmbeddedJpg(std::span<const Byte> segment_vec, std::span<const Byte> jpg_vec) {
    return writeToStagedOutput([&](OutputFile& f) {
        if (!segment_vec.empty()) {
            f.write(segment_vec, "Write File Error: Output data too large to write.");
        }
        f.write(jpg_vec, "Write File Error: Output data too large to write.");
    });
}

[[nodiscard]] EmbeddedWriteResult saveEmbeddedJpgFromEncryptedPath(
    vBytes& segment_vec, const fs::path& encrypted_path,
    std::span<const Byte> jpg_vec, bool has_reddit_option) {
    SegmentedEmbedSummary summary;
    const fs::path output_path = writeToStagedOutput([&](OutputFile& f) {
        summary = writeEmbeddedJpgFromEncryptedFile(f, segment_vec, encrypted_path, jpg_vec, has_reddit_option);
    });
    return EmbeddedWriteResult{output_path, summary};
}

void finalizePlatformReport(vString& platforms_vec, bool has_reddit_option, const SegmentedEmbedSummary& summary) {
    if (has_reddit_option) keepOnlyPlatformEntry(platforms_vec, REDDIT_PLATFORM_INDEX);
    else {
        removeOptionalPlatformEntries(platforms_vec);
        filterPlatforms(platforms_vec, summary.embedded_image_size, summary.first_segment_size, summary.total_segments);
    }
}

[[nodiscard]] ConcealFinalizeResult concealDefaultPath(
    vBytes& segment_vec,
    const OptimizedCover& cover,
    const EncryptionInput& encryption_input,
    const std::string& data_filename,
    bool has_reddit_option,
    vString& platforms_vec) {

    TempFileCleanupGuard encrypted_guard(tempStagePath(data_filename, "enc"));
    const std::uint64_t recovery_pin = encryptDataFileToFile(
        segment_vec,
        encryption_input.path,
        encryption_input.size,
        data_filename,
        encrypted_guard.path);

    const EmbeddedWriteResult embedded = saveEmbeddedJpgFromEncryptedPath(
        segment_vec,
        encrypted_guard.path,
        cover.view(),
        has_reddit_option);

    finalizePlatformReport(platforms_vec, has_reddit_option, embedded.summary);

    return ConcealFinalizeResult{
        .recovery_pin = recovery_pin,
        .output_path = embedded.output_path,
        .embedded_jpg_size = embedded.summary.embedded_image_size,
    };
}

[[nodiscard]] ConcealFinalizeResult concealBlueskyPath(
    vBytes& segment_vec,
    const OptimizedCover& cover,
    const EncryptionInput& encryption_input,
    const std::string& data_filename,
    vString& platforms_vec) {

    const std::uint64_t recovery_pin = encryptDataFileForBluesky(
        segment_vec,
        encryption_input.path,
        encryption_input.size,
        platforms_vec,
        data_filename);

    const std::span<const Byte> cover_view = cover.view();
    const fs::path output_path = saveEmbeddedJpg(
        std::span<const Byte>(segment_vec),
        cover_view);

    return ConcealFinalizeResult{
        .recovery_pin = recovery_pin,
        .output_path = output_path,
        .embedded_jpg_size = segment_vec.size() + cover_view.size(),
    };
}

void printConcealSummary(const vString& platforms_vec,
                         const fs::path& output_path,
                         std::size_t embedded_jpg_size,
                         std::uint64_t& recovery_pin) {
    std::print("\nPlatform compatibility for output image:-\n\n");
    for (const auto& s : platforms_vec) {
        std::println(" ✓ {}", s);
    }

    std::println("\nSaved \"file-embedded\" JPG image: {} ({} bytes).", output_path.string(), embedded_jpg_size);
    std::println("\nRecovery PIN: [***{}***]\n\n"
                 "Important: Keep your PIN safe, so that you can extract the hidden file.\n\n"
                 "Complete!\n", recovery_pin);
    sodium_memzero(&recovery_pin, sizeof(recovery_pin));
}
} // namespace

void concealData(vBytes& jpg_vec, Option option, const fs::path& data_file_path) {
    vString platforms_vec = platformReportTemplate();
    const ConcealFlags flags = concealFlags(option);

    const std::size_t source_data_size = validateFileForRead(data_file_path);

    OptimizedCover cover = prepareCoverImage(jpg_vec, source_data_size, flags);
    // Caller's input vector is no longer needed; release its storage so it
    // doesn't sit alongside the OptimizedCover for the rest of the run.
    vBytes{}.swap(jpg_vec);

    const std::size_t jpg_size = cover.trimmed_size();
    validateCoverImageLimits(jpg_size, flags);

    const std::string data_filename = validateDataFilename(data_file_path);
    const bool bypass_compression = shouldBypassCompression(data_file_path, source_data_size);

    vBytes segment_vec = makeSegmentTemplate(flags.has_bluesky_option);
    maybePrintLargeFileNotice(source_data_size);

    TempFileCleanupGuard compressed_guard;
    const EncryptionInput encryption_input = prepareEncryptionInput(
        data_file_path,
        data_filename,
        source_data_size,
        bypass_compression,
        segment_vec,
        compressed_guard);

    if (data_filename.size() > std::numeric_limits<std::size_t>::max() - 1 - encryption_input.size) {
        throw std::runtime_error("File Size Error: Encrypted output overflow.");
    }
    const std::size_t filename_prefix_size = 1 + data_filename.size();
    const std::size_t encrypted_payload_size = computeStreamEncryptedSizePrefixed(
        encryption_input.size,
        filename_prefix_size);
    validateCombinedSizeLimits(encrypted_payload_size, jpg_size, flags);

    ConcealFinalizeResult result = flags.has_bluesky_option
        ? concealBlueskyPath(segment_vec, cover, encryption_input, data_filename, platforms_vec)
        : concealDefaultPath(segment_vec, cover, encryption_input, data_filename, flags.has_reddit_option, platforms_vec);

    printConcealSummary(platforms_vec, result.output_path, result.embedded_jpg_size, result.recovery_pin);
}
