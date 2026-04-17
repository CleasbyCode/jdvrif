#pragma once

#include "common.h"

template<typename T>
struct SecureBuffer {
    T buf{};
    SecureBuffer() = default;
    ~SecureBuffer() { sodium_memzero(buf.data(), buf.size()); }
    SecureBuffer(const SecureBuffer&) = delete;
    SecureBuffer& operator=(const SecureBuffer&) = delete;
    SecureBuffer(SecureBuffer&&) = delete;
    SecureBuffer& operator=(SecureBuffer&&) = delete;
    auto data() { return buf.data(); } auto size() const { return buf.size(); }
};

void buildBlueskySegments(vBytes& segment_vec, const vBytes& data_vec);

[[nodiscard]] std::size_t estimateStreamEncryptedSize(std::size_t plaintext_size);
[[nodiscard]] std::size_t estimateStreamEncryptedSizePrefixed(std::size_t input_plaintext_size, std::size_t prefix_plaintext_size);

[[nodiscard]] std::uint64_t encryptDataFileFromPath(vBytes& segment_vec, const fs::path& data_path,
    std::size_t input_size, vBytes& jpg_vec, vString& platforms_vec, const std::string& data_filename,
    bool hasBlueskyOption, bool hasRedditOption);

[[nodiscard]] std::uint64_t encryptDataFileFromPathToFile(vBytes& segment_vec, const fs::path& data_path, std::size_t input_size,
    const std::string& data_filename, bool hasBlueskyOption, const fs::path& encrypted_output_path);

struct DecryptRequest { const fs::path* stream_output_path = nullptr; const fs::path* encrypted_input_path = nullptr; bool is_data_compressed = true; };

struct DecryptResult { std::string filename{}; std::size_t output_size{0}; bool used_stream_output{false}; bool failed{false}; };

[[nodiscard]] DecryptResult decryptDataFile(vBytes& jpg_vec, bool isBlueskyFile, const DecryptRequest& request = {});
