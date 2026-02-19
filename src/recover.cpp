#include "recover.h"
#include "binary_io.h"
#include "compression.h"
#include "encryption.h"

#include <fstream>
#include <print>
#include <stdexcept>
#include <string>

void recoverData(vBytes& jpg_vec, Mode mode, const fs::path& image_file_path) {
    constexpr std::size_t
        SIG_LENGTH = 7,
        INDEX_DIFF = 8;

    constexpr auto
        JDVRIF_SIG      = std::to_array<Byte>({ 0xB4, 0x6A, 0x3E, 0xEA, 0x5E, 0x9D, 0xF9 }),
        ICC_PROFILE_SIG = std::to_array<Byte>({ 0x6D, 0x6E, 0x74, 0x72, 0x52, 0x47, 0x42 });

    const auto index_opt = searchSig(jpg_vec, JDVRIF_SIG);

    if (!index_opt) {
        throw std::runtime_error(
            "Image File Error: Signature check failure. "
            "This is not a valid jdvrif \"file-embedded\" image.");
    }

    const std::size_t jdvrif_sig_index = *index_opt;
    const std::streamoff pin_attempts_offset = static_cast<std::streamoff>(jdvrif_sig_index + INDEX_DIFF - 1);

    Byte pin_attempts_val = jpg_vec[jdvrif_sig_index + INDEX_DIFF - 1];

    bool
        isBlueskyFile    = true,
        isDataCompressed = true;

    auto icc_opt = searchSig(jpg_vec, ICC_PROFILE_SIG);

    if (icc_opt) {
        constexpr std::size_t NO_ZLIB_COMPRESSION_ID_INDEX_DIFF = 24;
        const std::size_t icc_profile_sig_index = *icc_opt;

        jpg_vec.erase(jpg_vec.begin(), jpg_vec.begin() + (icc_profile_sig_index - INDEX_DIFF));
        isDataCompressed = (jpg_vec[NO_ZLIB_COMPRESSION_ID_INDEX - NO_ZLIB_COMPRESSION_ID_INDEX_DIFF] != NO_ZLIB_COMPRESSION_ID);
        isBlueskyFile = false;
    }

    if (isBlueskyFile) {
        reassembleBlueskyData(jpg_vec, SIG_LENGTH);
    }

    bool hasDecryptionFailed = false;
    std::string decrypted_filename = decryptDataFile(jpg_vec, isBlueskyFile, hasDecryptionFailed);

    if (hasDecryptionFailed) {
        if (pin_attempts_val == PIN_ATTEMPTS_RESET) {
            pin_attempts_val = 0;
        } else {
            pin_attempts_val++;
        }

        if (pin_attempts_val > 2) {
            destroyImageFile(image_file_path);
        } else {
            writePinAttempts(image_file_path, pin_attempts_offset, pin_attempts_val);
        }
        throw std::runtime_error("File Decryption Error: Invalid recovery PIN or file is corrupt.");
    }

    if (isDataCompressed) {
        zlibFunc(jpg_vec, mode);
    }

    if (jpg_vec.empty()) {
        throw std::runtime_error("Zlib Compression Error: Output file is empty. Inflating file failed.");
    }

    // Reset PIN attempts counter on successful decryption.
    if (pin_attempts_val != PIN_ATTEMPTS_RESET) {
        writePinAttempts(image_file_path, pin_attempts_offset, PIN_ATTEMPTS_RESET);
    }

    {
        std::ofstream file_ofs(decrypted_filename, std::ios::binary);
        if (!file_ofs) {
            throw std::runtime_error(
                "Write Error: Unable to write to file. "
                "Make sure you have WRITE permissions for this location.");
        }
        file_ofs.write(reinterpret_cast<const char*>(jpg_vec.data()), jpg_vec.size());
        file_ofs.close();
        if (!file_ofs) {
            throw std::runtime_error("Write Error: Failed to write complete output file.");
        }
    }

    std::println("\nExtracted hidden file: {} ({} bytes).\n\nComplete! Please check your file.\n",
                decrypted_filename, jpg_vec.size());
}
