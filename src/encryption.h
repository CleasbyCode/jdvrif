#pragma once

#include "common.h"

template<typename T>
struct SecureBuffer {
    T buf{};
    SecureBuffer() = default;
    ~SecureBuffer() { sodium_memzero(buf.data(), buf.size()); }
    SecureBuffer(const SecureBuffer&) = delete;
    SecureBuffer& operator=(const SecureBuffer&) = delete;
    auto data() { return buf.data(); }
    auto size() const { return buf.size(); }
};

void buildBlueskySegments(vBytes& segment_vec, const vBytes& data_vec);

[[nodiscard]] std::size_t encryptDataFile(vBytes& segment_vec, vBytes& data_vec, vBytes& jpg_vec,
    vString& platforms_vec, const std::string& data_filename, bool hasBlueskyOption, bool hasRedditOption);

[[nodiscard]] std::string decryptDataFile(vBytes& jpg_vec, bool isBlueskyFile, bool& hasDecryptionFailed);

void writePinAttempts(const fs::path& path, std::streamoff offset, Byte value);
void destroyImageFile(const fs::path& path);
void reassembleBlueskyData(vBytes& jpg_vec, std::size_t sig_length);
