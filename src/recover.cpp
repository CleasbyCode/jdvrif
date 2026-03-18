#include "recover.h"
#include "recover_internal.h"
#include "binary_io.h"
#include "compression.h"
#include "embedded_layout.h"
#include "encryption.h"
#include "file_utils.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {
constexpr const char* WRITE_COMPLETE_ERROR = "Write Error: Failed to write complete output file.";

void finalizeRecoverSuccess(
    const fs::path& cipher_stage_path,
    const fs::path& stream_stage_path,
    std::string decrypted_filename,
    std::size_t output_size);

[[nodiscard]] fs::path safeRecoveryPath(std::string decrypted_filename) {
    if (decrypted_filename.empty()) {
        throw std::runtime_error("File Extraction Error: Recovered filename is unsafe.");
    }

    fs::path parsed(std::move(decrypted_filename));
    if (
        parsed.has_root_path() ||
        parsed.has_parent_path() ||
        parsed != parsed.filename() ||
        !hasSafeEmbeddedFilename(parsed)) {
        throw std::runtime_error("File Extraction Error: Recovered filename is unsafe.");
    }

    fs::path candidate = parsed.filename();
    std::error_code ec;
    const bool candidate_exists = fs::exists(candidate, ec);
    if (ec) {
        throw std::runtime_error(std::format(
            "Write Error: Failed to inspect recovery output path: {}",
            ec.message()));
    }
    if (!candidate_exists) {
        return candidate;
    }

    std::string stem = candidate.stem().string();
    if (stem.empty()) {
        stem = "recovered";
    }
    const std::string ext = candidate.extension().string();
    std::string next_name;
    next_name.reserve(stem.size() + 1 + 20 + ext.size());

    for (std::size_t i = 1; i <= 10000; ++i) {
        std::array<char, 32> i_buf{};
        const auto [ptr, ec_to_chars] = std::to_chars(i_buf.data(), i_buf.data() + i_buf.size(), i);
        if (ec_to_chars != std::errc{}) {
            throw std::runtime_error("Write Error: Unable to create a unique output filename.");
        }

        next_name.clear();
        next_name.append(stem);
        next_name.push_back('_');
        next_name.append(i_buf.data(), ptr);
        next_name.append(ext);

        fs::path next(next_name);
        const bool next_exists = fs::exists(next, ec);
        if (ec) {
            throw std::runtime_error(std::format(
                "Write Error: Failed to inspect recovery output path: {}",
                ec.message()));
        }
        if (!next_exists) {
            return next;
        }
    }
    throw std::runtime_error("Write Error: Unable to create a unique output filename.");
}

[[nodiscard]] fs::path tempRecoveryPath(const fs::path& output_path) {
    constexpr std::size_t MAX_ATTEMPTS = 1024;
    const fs::path parent = output_path.parent_path();
    const std::string base = output_path.filename().string();

    const std::string prefix = std::format(".{}.jdvrif_tmp_", base);
    return uniqueRandomizedPathOrThrow(
        parent,
        prefix,
        "",
        MAX_ATTEMPTS,
        "Write Error: Unable to allocate a temporary output filename.");
}

void cleanupRecoverStageFiles(const fs::path& cipher_stage_path, const fs::path& stream_stage_path) noexcept {
    cleanupPathNoThrow(cipher_stage_path);
    cleanupPathNoThrow(stream_stage_path);
}

[[noreturn]] void failDecryptionAndCleanup(
    const fs::path& cipher_stage_path,
    const fs::path& stream_stage_path) {

    cleanupRecoverStageFiles(cipher_stage_path, stream_stage_path);
    throw std::runtime_error("File Decryption Error: Invalid recovery PIN or file is corrupt.");
}

void commitRecoveredOutput(const fs::path& staged_path, const fs::path& output_path) {
    commitStagedFileNoReplaceOrThrow(
        staged_path,
        output_path,
        "Write Error: Failed to commit recovered file");
}

void printRecoverySuccess(const fs::path& output_path, std::size_t output_size) {
    std::println("\nExtracted hidden file: {} ({} bytes).\n\nComplete! Please check your file.\n",
                output_path.string(), output_size);
}

template <typename WorkFn>
void runWithRecoverStageFiles(WorkFn&& work) {
    const fs::path cipher_stage_path = tempRecoveryPath(fs::path("jdvrif_cipher.bin"));
    const fs::path stream_stage_path = tempRecoveryPath(fs::path("jdvrif_recovered.bin"));

    try {
        work(cipher_stage_path, stream_stage_path);
    } catch (...) {
        cleanupRecoverStageFiles(cipher_stage_path, stream_stage_path);
        throw;
    }
}

void writeRecoveredPlainPayload(const vBytes& payload, const fs::path& stream_stage_path) {
    if (payload.empty()) {
        throw std::runtime_error("File Extraction Error: Output file is empty.");
    }
    std::ofstream file_ofs = openBinaryOutputForWriteOrThrow(stream_stage_path);
    writeBytesOrThrow(file_ofs, std::span<const Byte>(payload), WRITE_COMPLETE_ERROR);
    closeOutputOrThrow(file_ofs, WRITE_COMPLETE_ERROR);
}

[[nodiscard]] std::size_t materializeRecoveredOutput(
    vBytes& payload,
    const fs::path& stream_stage_path,
    bool used_stream_output,
    bool is_data_compressed,
    std::size_t output_size) {

    if (used_stream_output) {
        return output_size;
    }

    if (is_data_compressed) {
        zlibInflateToFile(payload, stream_stage_path);
        return checkedFileSize(
            stream_stage_path,
            "Zlib Compression Error: Output file is empty. Inflating file failed.",
            true);
    }

    writeRecoveredPlainPayload(payload, stream_stage_path);
    return payload.size();
}

void decryptAndFinalizeFromCipherStage(
    vBytes& metadata_vec,
    bool is_bluesky_file,
    bool is_data_compressed,
    const fs::path& cipher_stage_path,
    const fs::path& stream_stage_path) {

    DecryptResult decrypt_result = decryptDataFile(
        metadata_vec,
        is_bluesky_file,
        {.stream_output_path = &stream_stage_path,
         .encrypted_input_path = &cipher_stage_path,
         .is_data_compressed = is_data_compressed}
    );

    if (decrypt_result.failed) {
        failDecryptionAndCleanup(cipher_stage_path, stream_stage_path);
    }

    const std::size_t output_size = materializeRecoveredOutput(
        metadata_vec,
        stream_stage_path,
        decrypt_result.used_stream_output,
        is_data_compressed,
        decrypt_result.output_size);

    finalizeRecoverSuccess(
        cipher_stage_path,
        stream_stage_path,
        std::move(decrypt_result.filename),
        output_size);
}

void finalizeRecoverSuccess(
    const fs::path& cipher_stage_path,
    const fs::path& stream_stage_path,
    std::string decrypted_filename,
    std::size_t output_size) {

    const fs::path output_path = safeRecoveryPath(std::move(decrypted_filename));
    commitRecoveredOutput(stream_stage_path, output_path);
    cleanupPathNoThrow(cipher_stage_path);
    printRecoverySuccess(output_path, output_size);
}

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

    runWithRecoverStageFiles([&](const fs::path& cipher_stage_path, const fs::path& stream_stage_path) {
        const std::size_t extracted_cipher_size = extractDefaultCiphertextToFile(
            image_file_path,
            image_file_size,
            base_offset,
            embedded_file_size,
            total_profile_header_segments,
            cipher_stage_path
        );
        if (extracted_cipher_size == 0) {
            throw std::runtime_error("File Extraction Error: Embedded data file is empty.");
        }

        decryptAndFinalizeFromCipherStage(
            metadata_vec,
            false,
            is_data_compressed,
            cipher_stage_path,
            stream_stage_path);
    });
}

void recoverFromBlueskyPath(
    const fs::path& image_file_path,
    std::size_t image_file_size,
    std::size_t jdvrif_sig_index,
    std::span<const Byte> jdvrif_sig) {

    if (BLUESKY_CIPHER_LAYOUT.encrypted_payload_start_index > image_file_size) {
        throw std::runtime_error("Image File Error: Corrupt signature metadata.");
    }

    vBytes metadata_vec(BLUESKY_CIPHER_LAYOUT.encrypted_payload_start_index);
    {
        std::ifstream input = openBinaryInputOrThrow(image_file_path, "Read Error: Failed to open image file.");
        readExactAt(input, 0, std::span<Byte>(metadata_vec));
    }
    if (jdvrif_sig_index > metadata_vec.size() ||
        jdvrif_sig.size() > metadata_vec.size() - jdvrif_sig_index) {
        throw std::runtime_error("Image File Error: Corrupt signature metadata.");
    }
    const auto sig_begin = metadata_vec.begin() + static_cast<std::ptrdiff_t>(jdvrif_sig_index);
    if (!std::equal(jdvrif_sig.begin(), jdvrif_sig.end(), sig_begin)) {
        throw std::runtime_error("Image File Error: Corrupt signature metadata.");
    }
    if (!spanHasRange(metadata_vec, BLUESKY_CIPHER_LAYOUT.file_size_index, 4)) {
        throw std::runtime_error("File Extraction Error: Corrupt metadata.");
    }

    const std::size_t embedded_file_size = getValue(metadata_vec, BLUESKY_CIPHER_LAYOUT.file_size_index, 4);

    runWithRecoverStageFiles([&](const fs::path& cipher_stage_path, const fs::path& stream_stage_path) {
        const std::size_t extracted_cipher_size = extractBlueskyCiphertextToFile(
            image_file_path,
            image_file_size,
            embedded_file_size,
            cipher_stage_path
        );
        if (extracted_cipher_size == 0) {
            throw std::runtime_error("File Extraction Error: Embedded data file is empty.");
        }

        decryptAndFinalizeFromCipherStage(
            metadata_vec,
            true,
            true,
            cipher_stage_path,
            stream_stage_path);
    });
}
}

void recoverData(const fs::path& image_file_path) {
    const std::size_t image_file_size = validateFileForRead(image_file_path, FileTypeCheck::embedded_image);
    const auto jdvrif_sig_opt = findSignatureInFile(image_file_path, JDVRIF_SIGNATURE);
    if (!jdvrif_sig_opt) {
        throw std::runtime_error(
            "Image File Error: Signature check failure. "
            "This is not a valid jdvrif \"file-embedded\" image.");
    }

    const std::size_t jdvrif_sig_index = *jdvrif_sig_opt;
    if (jdvrif_sig_index > image_file_size ||
        ICC_PROFILE_SIGNATURE_OFFSET > image_file_size - jdvrif_sig_index) {
        throw std::runtime_error("Image File Error: Corrupt signature metadata.");
    }

    std::ifstream input = openBinaryInputOrThrow(image_file_path, "Read Error: Failed to open image file.");

    std::optional<std::size_t> icc_opt{};
    if (jdvrif_sig_index >= JDVRIF_TO_ICC_SIGNATURE_OFFSET) {
        const std::size_t icc_candidate = jdvrif_sig_index - JDVRIF_TO_ICC_SIGNATURE_OFFSET;
        if (hasSignatureAt(input, image_file_size, icc_candidate, ICC_PROFILE_SIGNATURE)) {
            icc_opt = icc_candidate;
        } else {
            const std::size_t icc_scan_start = (jdvrif_sig_index > ICC_SCAN_BACKWARD_WINDOW)
                ? jdvrif_sig_index - ICC_SCAN_BACKWARD_WINDOW
                : 0;
            const std::size_t icc_scan_end = jdvrif_sig_index + 1;
            icc_opt = findSignatureInFile(
                image_file_path,
                ICC_PROFILE_SIGNATURE,
                icc_scan_end,
                icc_scan_start);
        }
    }

    if (icc_opt) {
        recoverFromIccPath(
            image_file_path,
            image_file_size,
            *icc_opt);
        return;
    }

    recoverFromBlueskyPath(
        image_file_path,
        image_file_size,
        jdvrif_sig_index,
        JDVRIF_SIGNATURE);
}
