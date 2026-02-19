#include "encryption.h"
#include "binary_io.h"
#include "base64.h"
#include "pin_input.h"
#include "segmentation.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

    constexpr auto PHOTOSHOP_SEGMENT = std::to_array<Byte>({
        0xFF, 0xED, 0xFF, 0xFF, 0x50, 0x68, 0x6F, 0x74, 0x6F, 0x73, 0x68, 0x6F, 0x70, 0x20, 0x33, 0x2E,
        0x30, 0x00, 0x38, 0x42, 0x49, 0x4D, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xE3, 0x1C, 0x08,
        0x0A, 0x7F, 0xFF
    });

    constexpr auto XMP_SEGMENT = std::to_array<Byte>({
        0xFF, 0xE1, 0x01, 0x93, 0x68, 0x74, 0x74, 0x70, 0x3A, 0x2F, 0x2F, 0x6E, 0x73, 0x2E, 0x61, 0x64,
        0x6F, 0x62, 0x65, 0x2E, 0x63, 0x6F, 0x6D, 0x2F, 0x78, 0x61, 0x70, 0x2F, 0x31, 0x2E, 0x30, 0x2F,
        0x00, 0x3C, 0x3F, 0x78, 0x70, 0x61, 0x63, 0x6B, 0x65, 0x74, 0x20, 0x62, 0x65, 0x67, 0x69, 0x6E,
        0x3D, 0x22, 0x22, 0x20, 0x69, 0x64, 0x3D, 0x22, 0x57, 0x35, 0x4D, 0x30, 0x4D, 0x70, 0x43, 0x65,
        0x68, 0x69, 0x48, 0x7A, 0x72, 0x65, 0x53, 0x7A, 0x4E, 0x54, 0x63, 0x7A, 0x6B, 0x63, 0x39, 0x64,
        0x22, 0x3F, 0x3E, 0x0A, 0x3C, 0x78, 0x3A, 0x78, 0x6D, 0x70, 0x6D, 0x65, 0x74, 0x61, 0x20, 0x78,
        0x6D, 0x6C, 0x6E, 0x73, 0x3A, 0x78, 0x3D, 0x22, 0x61, 0x64, 0x6F, 0x62, 0x65, 0x3A, 0x6E, 0x73,
        0x3A, 0x6D, 0x65, 0x74, 0x61, 0x2F, 0x22, 0x20, 0x78, 0x3A, 0x78, 0x6D, 0x70, 0x74, 0x6B, 0x3D,
        0x22, 0x47, 0x6F, 0x20, 0x58, 0x4D, 0x50, 0x20, 0x53, 0x44, 0x4B, 0x20, 0x31, 0x2E, 0x30, 0x22,
        0x3E, 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x52, 0x44, 0x46, 0x20, 0x78, 0x6D, 0x6C, 0x6E, 0x73, 0x3A,
        0x72, 0x64, 0x66, 0x3D, 0x22, 0x68, 0x74, 0x74, 0x70, 0x3A, 0x2F, 0x2F, 0x77, 0x77, 0x77, 0x2E,
        0x77, 0x33, 0x2E, 0x6F, 0x72, 0x67, 0x2F, 0x31, 0x39, 0x39, 0x39, 0x2F, 0x30, 0x32, 0x2F, 0x32,
        0x32, 0x2D, 0x72, 0x64, 0x66, 0x2D, 0x73, 0x79, 0x6E, 0x74, 0x61, 0x78, 0x2D, 0x6E, 0x73, 0x23,
        0x22, 0x3E, 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x44, 0x65, 0x73, 0x63, 0x72, 0x69, 0x70, 0x74, 0x69,
        0x6F, 0x6E, 0x20, 0x78, 0x6D, 0x6C, 0x6E, 0x73, 0x3A, 0x64, 0x63, 0x3D, 0x22, 0x68, 0x74, 0x74,
        0x70, 0x3A, 0x2F, 0x2F, 0x70, 0x75, 0x72, 0x6C, 0x2E, 0x6F, 0x72, 0x67, 0x2F, 0x64, 0x63, 0x2F,
        0x65, 0x6C, 0x65, 0x6D, 0x65, 0x6E, 0x74, 0x73, 0x2F, 0x31, 0x2E, 0x31, 0x2F, 0x22, 0x20, 0x72,
        0x64, 0x66, 0x3A, 0x61, 0x62, 0x6F, 0x75, 0x74, 0x3D, 0x22, 0x22, 0x3E, 0x3C, 0x64, 0x63, 0x3A,
        0x63, 0x72, 0x65, 0x61, 0x74, 0x6F, 0x72, 0x3E, 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x53, 0x65, 0x71,
        0x3E, 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x6C, 0x69, 0x3E
    });

}

void buildBlueskySegments(vBytes& segment_vec, const vBytes& data_vec) {
    constexpr std::size_t
        COMPRESSED_FILE_SIZE_INDEX     = 0x1CD,
        EXIF_SEGMENT_DATA_SIZE_LIMIT   = 65027,
        EXIF_SEGMENT_DATA_INSERT_INDEX = 0x1D1,
        EXIF_SEGMENT_SIZE_INDEX        = 0x04,
        ARTIST_FIELD_SIZE_INDEX        = 0x4A,
        ARTIST_FIELD_SIZE_DIFF         = 140,
        FIRST_MARKER_BYTES_SIZE        = 4,
        VALUE_BYTE_LENGTH              = 4;

    const std::size_t
        encrypted_vec_size     = data_vec.size(),
        segment_vec_data_size  = segment_vec.size() - FIRST_MARKER_BYTES_SIZE,
        exif_segment_data_size = encrypted_vec_size > EXIF_SEGMENT_DATA_SIZE_LIMIT
            ? EXIF_SEGMENT_DATA_SIZE_LIMIT + segment_vec_data_size
            : encrypted_vec_size + segment_vec_data_size,
        artist_field_size      = exif_segment_data_size - ARTIST_FIELD_SIZE_DIFF;

    bool hasXmpSegment = false;

    updateValue(segment_vec, COMPRESSED_FILE_SIZE_INDEX, encrypted_vec_size, VALUE_BYTE_LENGTH);

    if (encrypted_vec_size <= EXIF_SEGMENT_DATA_SIZE_LIMIT) {
        updateValue(segment_vec, ARTIST_FIELD_SIZE_INDEX, artist_field_size, VALUE_BYTE_LENGTH);
        updateValue(segment_vec, EXIF_SEGMENT_SIZE_INDEX, exif_segment_data_size);
        segment_vec.insert(segment_vec.begin() + EXIF_SEGMENT_DATA_INSERT_INDEX, data_vec.begin(), data_vec.end());
        return;
    }

    // Data exceeds single EXIF segment — split across IPTC/XMP segments.
    segment_vec.insert(segment_vec.begin() + EXIF_SEGMENT_DATA_INSERT_INDEX, data_vec.begin(), data_vec.begin() + EXIF_SEGMENT_DATA_SIZE_LIMIT);

    constexpr std::size_t
        FIRST_DATASET_SIZE_LIMIT = 32767,
        LAST_DATASET_SIZE_LIMIT  = 32730,
        FIRST_DATASET_SIZE_INDEX = 0x21;


    vBytes pshop_vec(
        PHOTOSHOP_SEGMENT.begin(), PHOTOSHOP_SEGMENT.end()
	);

    std::size_t
        remaining_data_size = encrypted_vec_size - EXIF_SEGMENT_DATA_SIZE_LIMIT,
        data_file_index     = EXIF_SEGMENT_DATA_SIZE_LIMIT;

    pshop_vec.reserve(pshop_vec.size() + remaining_data_size);

    const std::size_t first_copy_size = std::min(FIRST_DATASET_SIZE_LIMIT, remaining_data_size);

    if (FIRST_DATASET_SIZE_LIMIT > first_copy_size) {
        updateValue(pshop_vec, FIRST_DATASET_SIZE_INDEX, first_copy_size);
    }

    pshop_vec.insert(pshop_vec.end(), data_vec.begin() + data_file_index, data_vec.begin() + data_file_index + first_copy_size);

    vBytes xmp_vec (XMP_SEGMENT.begin(), XMP_SEGMENT.end());

    if (remaining_data_size > FIRST_DATASET_SIZE_LIMIT) {
        remaining_data_size -= FIRST_DATASET_SIZE_LIMIT;
        data_file_index += FIRST_DATASET_SIZE_LIMIT;

        const std::size_t last_copy_size = std::min(LAST_DATASET_SIZE_LIMIT, remaining_data_size);

        constexpr auto DATASET_MARKER_BASE = std::to_array<Byte>({ 0x1C, 0x08, 0x0A });

        pshop_vec.insert(pshop_vec.end(), DATASET_MARKER_BASE.begin(), DATASET_MARKER_BASE.end());
        pshop_vec.emplace_back(static_cast<Byte>((last_copy_size >> 8) & 0xFF));
        pshop_vec.emplace_back(static_cast<Byte>(last_copy_size & 0xFF));
        pshop_vec.insert(pshop_vec.end(), data_vec.begin() + data_file_index, data_vec.begin() + data_file_index + last_copy_size);

        if (remaining_data_size > LAST_DATASET_SIZE_LIMIT) {
            hasXmpSegment = true;

            remaining_data_size -= LAST_DATASET_SIZE_LIMIT;
            data_file_index += LAST_DATASET_SIZE_LIMIT;

          //  xmp_vec (
          //      XMP_SEGMENT.begin(), XMP_SEGMENT.end()
          //  );

            constexpr std::size_t
                XMP_SEGMENT_SIZE_LIMIT = 60033,
                XMP_FOOTER_SIZE        = 92;

            const std::size_t base64_size = ((remaining_data_size + 2) / 3) * 4;
            xmp_vec.reserve(xmp_vec.size() + base64_size + XMP_FOOTER_SIZE);

            std::span<const Byte> remaining_data(data_vec.data() + data_file_index, remaining_data_size);
            binaryToBase64(remaining_data, xmp_vec);

            constexpr auto XMP_FOOTER = std::to_array<Byte>({
                0x3C, 0x2F, 0x72, 0x64, 0x66, 0x3A, 0x6C, 0x69, 0x3E, 0x3C, 0x2F, 0x72, 0x64, 0x66, 0x3A, 0x53,
                0x65, 0x71, 0x3E, 0x3C, 0x2F, 0x64, 0x63, 0x3A, 0x63, 0x72, 0x65, 0x61, 0x74, 0x6F, 0x72, 0x3E,
                0x3C, 0x2F, 0x72, 0x64, 0x66, 0x3A, 0x44, 0x65, 0x73, 0x63, 0x72, 0x69, 0x70, 0x74, 0x69, 0x6F,
                0x6E, 0x3E, 0x3C, 0x2F, 0x72, 0x64, 0x66, 0x3A, 0x52, 0x44, 0x46, 0x3E, 0x3C, 0x2F, 0x78, 0x3A,
                0x78, 0x6D, 0x70, 0x6D, 0x65, 0x74, 0x61, 0x3E, 0x0A, 0x3C, 0x3F, 0x78, 0x70, 0x61, 0x63, 0x6B,
                0x65, 0x74, 0x20, 0x65, 0x6E, 0x64, 0x3D, 0x22, 0x77, 0x22, 0x3F, 0x3E
            });
            xmp_vec.insert(xmp_vec.end(), XMP_FOOTER.begin(), XMP_FOOTER.end());

            if (xmp_vec.size() > XMP_SEGMENT_SIZE_LIMIT) {
                throw std::runtime_error("File Size Error: Data file exceeds segment size limit for Bluesky.");
            }
        }
    }

    // Finalize segment sizes and append to segment_vec.
    constexpr std::size_t
        PSHOP_VEC_DEFAULT_SIZE    = 35,
        SEGMENT_MARKER_BYTES_SIZE = 2,
        SEGMENT_SIZE_INDEX        = 0x2,
        BIM_SECTION_SIZE_INDEX    = 0x1C,
        BIM_SECTION_SIZE_DIFF     = 28;

    if (hasXmpSegment) {
        updateValue(xmp_vec, SEGMENT_SIZE_INDEX, xmp_vec.size() - SEGMENT_MARKER_BYTES_SIZE);
        segment_vec.insert(segment_vec.end(), xmp_vec.begin(), xmp_vec.end());
    }

    if (pshop_vec.size() > PSHOP_VEC_DEFAULT_SIZE) {
        const std::size_t
            pshop_segment_data_size = pshop_vec.size() - SEGMENT_MARKER_BYTES_SIZE,
            bim_section_size        = pshop_segment_data_size - BIM_SECTION_SIZE_DIFF;

        if (!hasXmpSegment) {
            updateValue(pshop_vec, SEGMENT_SIZE_INDEX, pshop_segment_data_size);
            updateValue(pshop_vec, BIM_SECTION_SIZE_INDEX, bim_section_size);
        }
        segment_vec.insert(segment_vec.end(), pshop_vec.begin(), pshop_vec.end());
    }
}

std::size_t encryptDataFile(vBytes& segment_vec, vBytes& data_vec, vBytes& jpg_vec,
    vString& platforms_vec, const std::string& data_filename, bool hasBlueskyOption, bool hasRedditOption) {

    const std::size_t
        data_filename_xor_key_index = hasBlueskyOption ? 0x175 : 0x2FB,
        data_filename_index         = hasBlueskyOption ? 0x161 : 0x2E7,
        sodium_key_index            = hasBlueskyOption ? 0x18D : 0x313,
        nonce_key_index             = hasBlueskyOption ? 0x1AD : 0x333;

    const Byte data_filename_length = segment_vec[data_filename_index - 1];

    randombytes_buf(segment_vec.data() + data_filename_xor_key_index, data_filename_length);

    std::ranges::transform(
        data_filename | std::views::take(data_filename_length),
        segment_vec | std::views::drop(data_filename_xor_key_index) | std::views::take(data_filename_length),
        segment_vec.begin() + data_filename_index,
        [](char a, Byte b) { return static_cast<Byte>(a) ^ b; }
    );

    SecureBuffer<Key>   key;
    SecureBuffer<Nonce> nonce;

    crypto_secretbox_keygen(key.data());
    randombytes_buf(nonce.data(), nonce.size());

    std::ranges::copy_n(key.data(),   key.size(),   segment_vec.begin() + sodium_key_index);
    std::ranges::copy_n(nonce.data(), nonce.size(), segment_vec.begin() + nonce_key_index);

    const std::size_t data_length = data_vec.size();
    data_vec.resize(data_length + TAG_BYTES);

    if (crypto_secretbox_easy(data_vec.data(), data_vec.data(), data_length, nonce.data(), key.data()) != 0) {
        throw std::runtime_error("crypto_secretbox_easy failed");
    }

    segment_vec.reserve(segment_vec.size() + data_vec.size());

    if (hasBlueskyOption) {
        buildBlueskySegments(segment_vec, data_vec);
    } else {
        segment_vec.insert(segment_vec.end(), data_vec.begin(), data_vec.end());
    }

    data_vec.clear();
    data_vec.shrink_to_fit();

    // XOR-obfuscate (Libsodium XOR) the stored key+nonce with the first 8 bytes at SODIUM_KEY_INDEX.
    // The PIN is derived from these 8 bytes (64bits) before obfuscation. After the XOR,
    // the 8 key bytes are overwritten with random data — the PIN exists only in the return value and cannot be recovered from the file.
    constexpr std::size_t
        SODIUM_XOR_KEY_LENGTH = 8,
        VALUE_BYTE_LENGTH     = 8;

    std::size_t
        pin                = getValue(segment_vec, sodium_key_index, VALUE_BYTE_LENGTH),
        sodium_keys_length = 48,
        sodium_xor_key_pos = sodium_key_index,
        sodium_key_pos     = sodium_key_index + SODIUM_XOR_KEY_LENGTH;

    while (sodium_keys_length--) {
        segment_vec[sodium_key_pos++] ^= segment_vec[sodium_xor_key_pos++];
        if (sodium_xor_key_pos >= sodium_key_index + SODIUM_XOR_KEY_LENGTH) {
            sodium_xor_key_pos = sodium_key_index;
        }
    }

    std::size_t random_val;
    randombytes_buf(&random_val, sizeof random_val);
    updateValue(segment_vec, sodium_key_index, random_val, VALUE_BYTE_LENGTH);

    if (hasBlueskyOption) {
        jpg_vec.reserve(jpg_vec.size() + segment_vec.size());
        jpg_vec.insert(jpg_vec.begin(), segment_vec.begin(), segment_vec.end());

        segment_vec.clear();
        segment_vec.shrink_to_fit();

        platforms_vec[0] = std::move(platforms_vec[2]);
        platforms_vec.resize(1);
    } else {
        segmentDataFile(segment_vec, data_vec, jpg_vec, platforms_vec, hasRedditOption);
    }
    return pin;
}

std::string decryptDataFile(vBytes& jpg_vec, bool isBlueskyFile, bool& hasDecryptionFailed) {
    constexpr std::size_t SODIUM_XOR_KEY_LENGTH = 8;

    const std::size_t
        sodium_key_index         = isBlueskyFile ? 0x18D : 0x2FB,
        nonce_key_index          = isBlueskyFile ? 0x1AD : 0x31B,
        encrypted_filename_index = isBlueskyFile ? 0x161 : 0x2CF,
        filename_xor_key_index   = isBlueskyFile ? 0x175 : 0x2E3,
        file_size_index          = isBlueskyFile ? 0x1CD : 0x2CA,
        filename_length_index    = encrypted_filename_index - 1;

    const std::size_t recovery_pin = getPin();

    updateValue(jpg_vec, sodium_key_index, recovery_pin, SODIUM_XOR_KEY_LENGTH);

    std::size_t
        sodium_keys_length = 48,
        sodium_xor_key_pos = sodium_key_index,
        sodium_key_pos     = sodium_key_index + SODIUM_XOR_KEY_LENGTH;

    while (sodium_keys_length--) {
        jpg_vec[sodium_key_pos++] ^= jpg_vec[sodium_xor_key_pos++];
        if (sodium_xor_key_pos >= sodium_key_index + SODIUM_XOR_KEY_LENGTH) {
            sodium_xor_key_pos = sodium_key_index;
        }
    }

    // RAII: key and nonce are always zeroed, even on exception paths.
    SecureBuffer<Key>   key;
    SecureBuffer<Nonce> nonce;

    std::ranges::copy_n(jpg_vec.begin() + sodium_key_index, key.size(), key.data());
    std::ranges::copy_n(jpg_vec.begin() + nonce_key_index, nonce.size(), nonce.data());

    // Decrypt the original filename.
    const Byte filename_length = jpg_vec[filename_length_index];

    std::string decrypted_filename(filename_length, '\0');

    std::ranges::transform(
        jpg_vec | std::views::drop(encrypted_filename_index) | std::views::take(filename_length),
        jpg_vec | std::views::drop(filename_xor_key_index) | std::views::take(filename_length),
        decrypted_filename.begin(),
        [](Byte a, Byte b) { return static_cast<char>(a ^ b); }
    );

    // Validate segment integrity and extract embedded data.
    constexpr std::size_t
        TOTAL_PROFILE_HEADER_SEGMENTS_INDEX = 0x2C8,
        COMMON_DIFF_VAL                     = 65537;

    const uint16_t total_profile_header_segments = static_cast<uint16_t>(getValue(jpg_vec, TOTAL_PROFILE_HEADER_SEGMENTS_INDEX));

    const std::size_t
       encrypted_file_start_index  = isBlueskyFile ? 0x1D1 : 0x33B,
       embedded_file_size          = getValue(jpg_vec, file_size_index, 4);

    if (total_profile_header_segments && !isBlueskyFile) {
        const std::size_t last_segment_index = (static_cast<std::size_t>(total_profile_header_segments) - 1) * COMMON_DIFF_VAL - 0x16;

        if (last_segment_index >= jpg_vec.size() || jpg_vec[last_segment_index] != 0xFF || jpg_vec[last_segment_index + 1] != 0xE2) {
            throw std::runtime_error("File Extraction Error: Missing segments detected. Embedded data file is corrupt!");
        }
    }

    if (isBlueskyFile) {
        constexpr auto EXIF_SIG = std::to_array<Byte>({0xFF, 0xE1});
        constexpr std::size_t
            SEARCH_LIMIT  = 100,
            EXIF_MAX_SIZE = 65534;

        auto index_opt = searchSig(jpg_vec, EXIF_SIG, SEARCH_LIMIT);

        if (!index_opt) {
            throw std::runtime_error("File Extraction Error: Expected segment marker not found. Embedded data file is corrupt!");
        }

        const std::size_t
            exif_sig_index    = *index_opt,
            exif_segment_size = getValue(jpg_vec, exif_sig_index + 2, 2);

        if (embedded_file_size >= EXIF_MAX_SIZE && EXIF_MAX_SIZE > exif_segment_size) {
            throw std::runtime_error("File Extraction Error: Invalid segment size. Embedded data file is corrupt!");
        }
    }

    // Isolate the encrypted data.
    std::memmove(jpg_vec.data(), jpg_vec.data() + encrypted_file_start_index, embedded_file_size);
    jpg_vec.resize(embedded_file_size);

    // Strip ICC profile headers from multi-segment data before decryption.
    const bool has_zero_profile_headers = (isBlueskyFile || !total_profile_header_segments);

    if (!has_zero_profile_headers) {
        constexpr std::size_t
            PROFILE_HEADER_LENGTH = 18,
            HEADER_INDEX          = 0xFCB0;

        const std::size_t limit = jpg_vec.size();

        std::size_t
            read_pos    = 0,
            write_pos   = 0,
            next_header = HEADER_INDEX;

        while (read_pos < limit) {
            if (read_pos == next_header) {
                read_pos    += std::min(PROFILE_HEADER_LENGTH, limit - read_pos);
                next_header += COMMON_DIFF_VAL;
                continue;
            }
            jpg_vec[write_pos++] = jpg_vec[read_pos++];
        }
        jpg_vec.resize(write_pos);
        jpg_vec.shrink_to_fit();
    }

    // Decrypt
    if (crypto_secretbox_open_easy(jpg_vec.data(), jpg_vec.data(), jpg_vec.size(), nonce.data(), key.data()) != 0) {
        std::println(std::cerr, "\nDecryption failed!");
        hasDecryptionFailed = true;
        return {};
    }

    jpg_vec.resize(jpg_vec.size() - TAG_BYTES);
    jpg_vec.shrink_to_fit();

    return decrypted_filename;
}

void writePinAttempts(const fs::path& path, std::streamoff offset, Byte value) {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file) {
        throw std::runtime_error("File Error: Unable to open image file for updating.");
    }
    file.seekp(offset);
    file.write(reinterpret_cast<char*>(&value), sizeof(value));
    file.close();
    if (!file) {
        throw std::runtime_error("File Error: Failed to update image file.");
    }
}

void destroyImageFile(const fs::path& path) {
    // Intentional: truncate the cover image file to zero bytes after too many failed PIN attempts.
    std::ofstream ofs(path, std::ios::out | std::ios::trunc | std::ios::binary);
    ofs.close();
}

void reassembleBlueskyData(vBytes& jpg_vec, std::size_t sig_length) {
    constexpr std::size_t SEARCH_LIMIT = 125480;
    constexpr auto
        PSHOP_SEGMENT_SIG = std::to_array<Byte>({ 0x73, 0x68, 0x6F, 0x70, 0x20, 0x33, 0x2E }),
        XMP_CREATOR_SIG   = std::to_array<Byte>({ 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x6C, 0x69 });

    auto index_opt = searchSig(jpg_vec, PSHOP_SEGMENT_SIG, SEARCH_LIMIT);
    if (!index_opt) return;

    constexpr std::size_t
        DATASET_MAX_SIZE              = 32800,
        PSHOP_SEGMENT_SIZE_INDEX_DIFF = 7,
        FIRST_DATASET_SIZE_INDEX_DIFF = 24,
        DATASET_FILE_INDEX_DIFF       = 2;

    const std::size_t
        pshop_segment_sig_index  = *index_opt,
        pshop_segment_size_index = pshop_segment_sig_index  - PSHOP_SEGMENT_SIZE_INDEX_DIFF,
        first_dataset_size_index = pshop_segment_sig_index  + FIRST_DATASET_SIZE_INDEX_DIFF,
        first_dataset_file_index = first_dataset_size_index + DATASET_FILE_INDEX_DIFF;

    const uint16_t
        pshop_segment_size = static_cast<uint16_t>(getValue(jpg_vec, pshop_segment_size_index)),
        first_dataset_size = static_cast<uint16_t>(getValue(jpg_vec, first_dataset_size_index));

    vBytes file_parts_vec;
    file_parts_vec.reserve(first_dataset_size * 5);
    file_parts_vec.insert(file_parts_vec.end(), jpg_vec.begin() + first_dataset_file_index, jpg_vec.begin() + first_dataset_file_index + first_dataset_size);

    bool has_xmp_segment = false;
    std::size_t xmp_creator_sig_index = 0;

    if (pshop_segment_size > DATASET_MAX_SIZE) {
        constexpr std::size_t SECOND_DATASET_SIZE_INDEX_DIFF = 3;
        const std::size_t
           second_dataset_size_index = first_dataset_file_index  + first_dataset_size + SECOND_DATASET_SIZE_INDEX_DIFF,
           second_dataset_file_index = second_dataset_size_index + DATASET_FILE_INDEX_DIFF;

        const uint16_t second_dataset_size = static_cast<uint16_t>(getValue(jpg_vec, second_dataset_size_index));

        file_parts_vec.insert(file_parts_vec.end(), jpg_vec.begin() + second_dataset_file_index, jpg_vec.begin() + second_dataset_file_index + second_dataset_size);

        auto xmp_opt = searchSig(jpg_vec, XMP_CREATOR_SIG, SEARCH_LIMIT);
        if (xmp_opt) {
            has_xmp_segment = true;
            xmp_creator_sig_index = *xmp_opt;

            constexpr Byte BASE64_END_SIG = 0x3C;
            const std::size_t
                base64_begin_index = xmp_creator_sig_index + sig_length + 1,
                base64_end_index   = static_cast<std::size_t>(std::ranges::find(jpg_vec.begin() + base64_begin_index, jpg_vec.end(), BASE64_END_SIG) - jpg_vec.begin());

            std::span<const Byte> base64_span(jpg_vec.data() + base64_begin_index, base64_end_index - base64_begin_index);

            appendBase64AsBinary(base64_span, file_parts_vec);
        }
    }
    const std::size_t
        exif_data_end_index_diff = has_xmp_segment ? 351 : 55,
        exif_data_end_index      = (has_xmp_segment ? xmp_creator_sig_index : pshop_segment_sig_index) - exif_data_end_index_diff;

    std::ranges::copy_n(file_parts_vec.begin(), file_parts_vec.size(), jpg_vec.begin() + exif_data_end_index);
}
