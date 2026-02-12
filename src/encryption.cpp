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
        ENCRYPTED_VEC_SIZE     = data_vec.size(),
        SEGMENT_VEC_DATA_SIZE  = segment_vec.size() - FIRST_MARKER_BYTES_SIZE,
        EXIF_SEGMENT_DATA_SIZE = ENCRYPTED_VEC_SIZE > EXIF_SEGMENT_DATA_SIZE_LIMIT
            ? EXIF_SEGMENT_DATA_SIZE_LIMIT + SEGMENT_VEC_DATA_SIZE
            : ENCRYPTED_VEC_SIZE + SEGMENT_VEC_DATA_SIZE,
        ARTIST_FIELD_SIZE      = EXIF_SEGMENT_DATA_SIZE - ARTIST_FIELD_SIZE_DIFF;

    bool hasXmpSegment = false;

    updateValue(segment_vec, COMPRESSED_FILE_SIZE_INDEX, ENCRYPTED_VEC_SIZE, VALUE_BYTE_LENGTH);

    if (ENCRYPTED_VEC_SIZE <= EXIF_SEGMENT_DATA_SIZE_LIMIT) {
        updateValue(segment_vec, ARTIST_FIELD_SIZE_INDEX, ARTIST_FIELD_SIZE, VALUE_BYTE_LENGTH);
        updateValue(segment_vec, EXIF_SEGMENT_SIZE_INDEX, EXIF_SEGMENT_DATA_SIZE);
        segment_vec.insert(segment_vec.begin() + EXIF_SEGMENT_DATA_INSERT_INDEX, data_vec.begin(), data_vec.end());
        return;
    }

    // Data exceeds single EXIF segment — split across IPTC/XMP segments.
    segment_vec.insert(segment_vec.begin() + EXIF_SEGMENT_DATA_INSERT_INDEX, data_vec.begin(), data_vec.begin() + EXIF_SEGMENT_DATA_SIZE_LIMIT);

    constexpr std::size_t
        FIRST_DATASET_SIZE_LIMIT = 32767,
        LAST_DATASET_SIZE_LIMIT  = 32730,
        FIRST_DATASET_SIZE_INDEX = 0x21;

    vBytes pshop_vec = {
        0xFF, 0xED, 0xFF, 0xFF, 0x50, 0x68, 0x6F, 0x74, 0x6F, 0x73, 0x68, 0x6F, 0x70, 0x20, 0x33, 0x2E,
        0x30, 0x00, 0x38, 0x42, 0x49, 0x4D, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xE3, 0x1C, 0x08,
        0x0A, 0x7F, 0xFF
    };

    std::size_t
        remaining_data_size = ENCRYPTED_VEC_SIZE - EXIF_SEGMENT_DATA_SIZE_LIMIT,
        data_file_index     = EXIF_SEGMENT_DATA_SIZE_LIMIT;

    pshop_vec.reserve(pshop_vec.size() + remaining_data_size);

    const std::size_t FIRST_COPY_SIZE = std::min(FIRST_DATASET_SIZE_LIMIT, remaining_data_size);

    if (FIRST_DATASET_SIZE_LIMIT > FIRST_COPY_SIZE) {
        updateValue(pshop_vec, FIRST_DATASET_SIZE_INDEX, FIRST_COPY_SIZE);
    }

    pshop_vec.insert(pshop_vec.end(), data_vec.begin() + data_file_index, data_vec.begin() + data_file_index + FIRST_COPY_SIZE);

    vBytes xmp_vec;

    if (remaining_data_size > FIRST_DATASET_SIZE_LIMIT) {
        remaining_data_size -= FIRST_DATASET_SIZE_LIMIT;
        data_file_index += FIRST_DATASET_SIZE_LIMIT;

        const std::size_t LAST_COPY_SIZE = std::min(LAST_DATASET_SIZE_LIMIT, remaining_data_size);

        constexpr auto DATASET_MARKER_BASE = std::to_array<Byte>({ 0x1C, 0x08, 0x0A });

        pshop_vec.insert(pshop_vec.end(), DATASET_MARKER_BASE.begin(), DATASET_MARKER_BASE.end());
        pshop_vec.emplace_back(static_cast<Byte>((LAST_COPY_SIZE >> 8) & 0xFF));
        pshop_vec.emplace_back(static_cast<Byte>(LAST_COPY_SIZE & 0xFF));
        pshop_vec.insert(pshop_vec.end(), data_vec.begin() + data_file_index, data_vec.begin() + data_file_index + LAST_COPY_SIZE);

        if (remaining_data_size > LAST_DATASET_SIZE_LIMIT) {
            hasXmpSegment = true;

            remaining_data_size -= LAST_DATASET_SIZE_LIMIT;
            data_file_index += LAST_DATASET_SIZE_LIMIT;

            xmp_vec = {
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
            };

            constexpr std::size_t
                XMP_SEGMENT_SIZE_LIMIT = 60033,
                XMP_FOOTER_SIZE        = 92;

            const std::size_t BASE64_SIZE = ((remaining_data_size + 2) / 3) * 4;
            xmp_vec.reserve(xmp_vec.size() + BASE64_SIZE + XMP_FOOTER_SIZE);

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
            PSHOP_SEGMENT_DATA_SIZE = pshop_vec.size() - SEGMENT_MARKER_BYTES_SIZE,
            BIM_SECTION_SIZE        = PSHOP_SEGMENT_DATA_SIZE - BIM_SECTION_SIZE_DIFF;

        if (!hasXmpSegment) {
            updateValue(pshop_vec, SEGMENT_SIZE_INDEX, PSHOP_SEGMENT_DATA_SIZE);
            updateValue(pshop_vec, BIM_SECTION_SIZE_INDEX, BIM_SECTION_SIZE);
        }
        segment_vec.insert(segment_vec.end(), pshop_vec.begin(), pshop_vec.end());
    }
}

std::size_t encryptDataFile(vBytes& segment_vec, vBytes& data_vec, vBytes& jpg_vec,
    vString& platforms_vec, const std::string& data_filename, bool hasBlueskyOption, bool hasRedditOption) {

    const std::size_t
        DATA_FILENAME_XOR_KEY_INDEX = hasBlueskyOption ? 0x175 : 0x2FB,
        DATA_FILENAME_INDEX         = hasBlueskyOption ? 0x161 : 0x2E7,
        SODIUM_KEY_INDEX            = hasBlueskyOption ? 0x18D : 0x313,
        NONCE_KEY_INDEX             = hasBlueskyOption ? 0x1AD : 0x333;

    const Byte DATA_FILENAME_LENGTH = segment_vec[DATA_FILENAME_INDEX - 1];

    randombytes_buf(segment_vec.data() + DATA_FILENAME_XOR_KEY_INDEX, DATA_FILENAME_LENGTH);

    std::ranges::transform(
        data_filename | std::views::take(DATA_FILENAME_LENGTH),
        segment_vec | std::views::drop(DATA_FILENAME_XOR_KEY_INDEX) | std::views::take(DATA_FILENAME_LENGTH),
        segment_vec.begin() + DATA_FILENAME_INDEX,
        [](char a, Byte b) { return static_cast<Byte>(a) ^ b; }
    );

    SecureBuffer<Key>   key;
    SecureBuffer<Nonce> nonce;

    crypto_secretbox_keygen(key.data());
    randombytes_buf(nonce.data(), nonce.size());

    std::ranges::copy_n(key.data(),   key.size(),   segment_vec.begin() + SODIUM_KEY_INDEX);
    std::ranges::copy_n(nonce.data(), nonce.size(), segment_vec.begin() + NONCE_KEY_INDEX);

    const std::size_t DATA_LENGTH = data_vec.size();
    data_vec.resize(DATA_LENGTH + TAG_BYTES);

    if (crypto_secretbox_easy(data_vec.data(), data_vec.data(), DATA_LENGTH, nonce.data(), key.data()) != 0) {
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
        pin                = getValue(segment_vec, SODIUM_KEY_INDEX, VALUE_BYTE_LENGTH),
        sodium_keys_length = 48,
        sodium_xor_key_pos = SODIUM_KEY_INDEX,
        sodium_key_pos     = SODIUM_KEY_INDEX + SODIUM_XOR_KEY_LENGTH;

    while (sodium_keys_length--) {
        segment_vec[sodium_key_pos++] ^= segment_vec[sodium_xor_key_pos++];
        if (sodium_xor_key_pos >= SODIUM_KEY_INDEX + SODIUM_XOR_KEY_LENGTH) {
            sodium_xor_key_pos = SODIUM_KEY_INDEX;
        }
    }

    std::size_t random_val;
    randombytes_buf(&random_val, sizeof random_val);
    updateValue(segment_vec, SODIUM_KEY_INDEX, random_val, VALUE_BYTE_LENGTH);

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
        SODIUM_KEY_INDEX         = isBlueskyFile ? 0x18D : 0x2FB,
        NONCE_KEY_INDEX          = isBlueskyFile ? 0x1AD : 0x31B,
        ENCRYPTED_FILENAME_INDEX = isBlueskyFile ? 0x161 : 0x2CF,
        FILENAME_XOR_KEY_INDEX   = isBlueskyFile ? 0x175 : 0x2E3,
        FILE_SIZE_INDEX          = isBlueskyFile ? 0x1CD : 0x2CA,
        FILENAME_LENGTH_INDEX    = ENCRYPTED_FILENAME_INDEX - 1;

    const std::size_t RECOVERY_PIN = getPin();

    updateValue(jpg_vec, SODIUM_KEY_INDEX, RECOVERY_PIN, 8);

    std::size_t
        sodium_keys_length = 48,
        sodium_xor_key_pos = SODIUM_KEY_INDEX,
        sodium_key_pos     = SODIUM_KEY_INDEX + SODIUM_XOR_KEY_LENGTH;

    while (sodium_keys_length--) {
        jpg_vec[sodium_key_pos++] ^= jpg_vec[sodium_xor_key_pos++];
        if (sodium_xor_key_pos >= SODIUM_KEY_INDEX + SODIUM_XOR_KEY_LENGTH) {
            sodium_xor_key_pos = SODIUM_KEY_INDEX;
        }
    }

    // RAII: key and nonce are always zeroed, even on exception paths.
    SecureBuffer<Key>   key;
    SecureBuffer<Nonce> nonce;

    std::ranges::copy_n(jpg_vec.begin() + SODIUM_KEY_INDEX, key.size(), key.data());
    std::ranges::copy_n(jpg_vec.begin() + NONCE_KEY_INDEX, nonce.size(), nonce.data());

    // Decrypt the original filename.
    const Byte FILENAME_LENGTH = jpg_vec[FILENAME_LENGTH_INDEX];

    std::string decrypted_filename(FILENAME_LENGTH, '\0');

    std::ranges::transform(
        jpg_vec | std::views::drop(ENCRYPTED_FILENAME_INDEX) | std::views::take(FILENAME_LENGTH),
        jpg_vec | std::views::drop(FILENAME_XOR_KEY_INDEX) | std::views::take(FILENAME_LENGTH),
        decrypted_filename.begin(),
        [](Byte a, Byte b) { return static_cast<char>(a ^ b); }
    );

    // Validate segment integrity and extract embedded data.
    constexpr std::size_t
        TOTAL_PROFILE_HEADER_SEGMENTS_INDEX = 0x2C8,
        COMMON_DIFF_VAL                     = 65537;

    const uint16_t TOTAL_PROFILE_HEADER_SEGMENTS = static_cast<uint16_t>(getValue(jpg_vec, TOTAL_PROFILE_HEADER_SEGMENTS_INDEX));

    const std::size_t
        ENCRYPTED_FILE_START_INDEX = isBlueskyFile ? 0x1D1 : 0x33B,
        EMBEDDED_FILE_SIZE         = getValue(jpg_vec, FILE_SIZE_INDEX, 4);

    if (TOTAL_PROFILE_HEADER_SEGMENTS && !isBlueskyFile) {
        const std::size_t LAST_SEGMENT_INDEX = (static_cast<std::size_t>(TOTAL_PROFILE_HEADER_SEGMENTS) - 1) * COMMON_DIFF_VAL - 0x16;

        if (LAST_SEGMENT_INDEX >= jpg_vec.size() || jpg_vec[LAST_SEGMENT_INDEX] != 0xFF || jpg_vec[LAST_SEGMENT_INDEX + 1] != 0xE2) {
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
            EXIF_SIG_INDEX    = *index_opt,
            EXIF_SEGMENT_SIZE = getValue(jpg_vec, EXIF_SIG_INDEX + 2, 2);

        if (EMBEDDED_FILE_SIZE >= EXIF_MAX_SIZE && EXIF_MAX_SIZE > EXIF_SEGMENT_SIZE) {
            throw std::runtime_error("File Extraction Error: Invalid segment size. Embedded data file is corrupt!");
        }
    }

    // Isolate the encrypted data.
    std::memmove(jpg_vec.data(), jpg_vec.data() + ENCRYPTED_FILE_START_INDEX, EMBEDDED_FILE_SIZE);
    jpg_vec.resize(EMBEDDED_FILE_SIZE);

    // Strip ICC profile headers from multi-segment data before decryption.
    const bool hasNoProfileHeaders = (isBlueskyFile || !TOTAL_PROFILE_HEADER_SEGMENTS);

    if (!hasNoProfileHeaders) {
        constexpr std::size_t
            PROFILE_HEADER_LENGTH = 18,
            HEADER_INDEX          = 0xFCB0;

        const std::size_t LIMIT = jpg_vec.size();

        std::size_t
            read_pos    = 0,
            write_pos   = 0,
            next_header = HEADER_INDEX;

        while (read_pos < LIMIT) {
            if (read_pos == next_header) {
                read_pos    += std::min(PROFILE_HEADER_LENGTH, LIMIT - read_pos);
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
        PSHOP_SEGMENT_SIG_INDEX  = *index_opt,
        PSHOP_SEGMENT_SIZE_INDEX = PSHOP_SEGMENT_SIG_INDEX  - PSHOP_SEGMENT_SIZE_INDEX_DIFF,
        FIRST_DATASET_SIZE_INDEX = PSHOP_SEGMENT_SIG_INDEX  + FIRST_DATASET_SIZE_INDEX_DIFF,
        FIRST_DATASET_FILE_INDEX = FIRST_DATASET_SIZE_INDEX + DATASET_FILE_INDEX_DIFF;

    const uint16_t
        PSHOP_SEGMENT_SIZE = static_cast<uint16_t>(getValue(jpg_vec, PSHOP_SEGMENT_SIZE_INDEX)),
        FIRST_DATASET_SIZE = static_cast<uint16_t>(getValue(jpg_vec, FIRST_DATASET_SIZE_INDEX));

    vBytes file_parts_vec;
    file_parts_vec.reserve(FIRST_DATASET_SIZE * 5);
    file_parts_vec.insert(file_parts_vec.end(), jpg_vec.begin() + FIRST_DATASET_FILE_INDEX, jpg_vec.begin() + FIRST_DATASET_FILE_INDEX + FIRST_DATASET_SIZE);

    bool hasXmpSegment = false;
    std::size_t xmp_creator_sig_index = 0;

    if (PSHOP_SEGMENT_SIZE > DATASET_MAX_SIZE) {
        constexpr std::size_t SECOND_DATASET_SIZE_INDEX_DIFF = 3;
        const std::size_t
            SECOND_DATASET_SIZE_INDEX = FIRST_DATASET_FILE_INDEX + FIRST_DATASET_SIZE + SECOND_DATASET_SIZE_INDEX_DIFF,
            SECOND_DATASET_FILE_INDEX = SECOND_DATASET_SIZE_INDEX + DATASET_FILE_INDEX_DIFF;

        const uint16_t SECOND_DATASET_SIZE = static_cast<uint16_t>(getValue(jpg_vec, SECOND_DATASET_SIZE_INDEX));

        file_parts_vec.insert(file_parts_vec.end(), jpg_vec.begin() + SECOND_DATASET_FILE_INDEX, jpg_vec.begin() + SECOND_DATASET_FILE_INDEX + SECOND_DATASET_SIZE);

        auto xmp_opt = searchSig(jpg_vec, XMP_CREATOR_SIG, SEARCH_LIMIT);
        if (xmp_opt) {
            hasXmpSegment = true;
            xmp_creator_sig_index = *xmp_opt;

            constexpr Byte BASE64_END_SIG = 0x3C;
            const std::size_t
                BASE64_BEGIN_INDEX = xmp_creator_sig_index + sig_length + 1,
                BASE64_END_INDEX   = static_cast<std::size_t>(std::ranges::find(jpg_vec.begin() + BASE64_BEGIN_INDEX, jpg_vec.end(), BASE64_END_SIG) - jpg_vec.begin());

            std::span<const Byte> base64_span(jpg_vec.data() + BASE64_BEGIN_INDEX, BASE64_END_INDEX - BASE64_BEGIN_INDEX);

            appendBase64AsBinary(base64_span, file_parts_vec);
        }
    }
    const std::size_t
        EXIF_DATA_END_INDEX_DIFF = hasXmpSegment ? 351 : 55,
        EXIF_DATA_END_INDEX      = (hasXmpSegment ? xmp_creator_sig_index : PSHOP_SEGMENT_SIG_INDEX) - EXIF_DATA_END_INDEX_DIFF;

    std::ranges::copy_n(file_parts_vec.begin(), file_parts_vec.size(), jpg_vec.begin() + EXIF_DATA_END_INDEX);
}
