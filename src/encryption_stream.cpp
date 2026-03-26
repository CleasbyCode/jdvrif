#include "encryption_internal.h"
#include "file_utils.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>

namespace {
constexpr std::size_t STREAM_INFLATE_OUT_CHUNK_SIZE = 2 * 1024 * 1024, STREAM_INFLATE_MAX_OUTPUT = 3ULL * 1024 * 1024 * 1024;
constexpr const char* WRITE_COMPLETE_ERROR = "Write Error: Failed to write complete output file.";
constexpr const char* CORRUPT_FILENAME_ERROR = "File Extraction Error: Corrupt encrypted filename metadata.";

using StreamHeader = std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>;
using PlainChunk = std::array<Byte, STREAM_CHUNK_SIZE>;
using CipherChunk = std::array<Byte, STREAM_CHUNK_SIZE + crypto_secretstream_xchacha20poly1305_ABYTES>;

template<typename Buffer>
struct ZeroGuard {
    Buffer* buf;
    ~ZeroGuard() { sodium_memzero(buf->data(), buf->size()); }
};

[[nodiscard]] uint32_t decodeFrameLength(std::span<const Byte, STREAM_FRAME_LEN_BYTES> frame_len_bytes) {
    return (static_cast<uint32_t>(frame_len_bytes[0]) << 24) |
           (static_cast<uint32_t>(frame_len_bytes[1]) << 16) |
           (static_cast<uint32_t>(frame_len_bytes[2]) << 8) |
           static_cast<uint32_t>(frame_len_bytes[3]);
}

[[nodiscard]] std::array<Byte, STREAM_FRAME_LEN_BYTES> encodeFrameLength(std::size_t frame_len) {
    if (frame_len > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
        throw std::runtime_error("File Size Error: Stream chunk exceeds size limit.");
    }

    return {
        static_cast<Byte>((frame_len >> 24) & 0xFF),
        static_cast<Byte>((frame_len >> 16) & 0xFF),
        static_cast<Byte>((frame_len >> 8) & 0xFF),
        static_cast<Byte>(frame_len & 0xFF)
    };
}

[[nodiscard]] std::size_t checkedCipherChunkSize(std::size_t plain_size) {
    constexpr std::size_t STREAM_ABYTES = crypto_secretstream_xchacha20poly1305_ABYTES;
    const std::size_t cipher_size = plain_size + STREAM_ABYTES;
    if (cipher_size > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
        throw std::runtime_error("File Size Error: Stream chunk exceeds size limit.");
    }
    return cipher_size;
}

template<typename ChunkFn>
void forEachPlainChunk(std::span<const Byte> plaintext, ChunkFn&& chunk_fn) {
    for (std::size_t offset = 0; offset < plaintext.size(); offset += STREAM_CHUNK_SIZE) {
        const std::size_t size = std::min(STREAM_CHUNK_SIZE, plaintext.size() - offset);
        chunk_fn(plaintext.subspan(offset, size), offset + size == plaintext.size());
    }
}

template<typename EmitFrameFn>
void pushSecretStreamFrame(
    crypto_secretstream_xchacha20poly1305_state& state,
    std::span<const Byte> plain_chunk,
    bool is_final,
    CipherChunk& cipher_chunk,
    EmitFrameFn&& emit_frame) {

    const std::size_t cipher_size = checkedCipherChunkSize(plain_chunk.size());
    unsigned long long written = 0;
    const unsigned char tag = is_final ? crypto_secretstream_xchacha20poly1305_TAG_FINAL : 0;

    if (crypto_secretstream_xchacha20poly1305_push(
            &state,
            cipher_chunk.data(),
            &written,
            plain_chunk.data(),
            static_cast<unsigned long long>(plain_chunk.size()),
            nullptr,
            0,
            tag) != 0) {
        throw std::runtime_error("crypto_secretstream push failed");
    }
    if (written != cipher_size) {
        throw std::runtime_error("crypto_secretstream push produced unexpected size");
    }

    emit_frame(std::span<const Byte>(cipher_chunk.data(), cipher_size));
}

template<typename SourceFn, typename EmitFrameFn>
void encryptSecretStreamFrames(
    const Key& key,
    StreamHeader& header,
    SourceFn&& source,
    EmitFrameFn&& emit_frame) {

    crypto_secretstream_xchacha20poly1305_state state{};
    if (crypto_secretstream_xchacha20poly1305_init_push(&state, header.data(), key.data()) != 0) {
        throw std::runtime_error("crypto_secretstream init_push failed");
    }

    CipherChunk cipher_chunk{};
    ZeroGuard<CipherChunk> cipher_chunk_guard{&cipher_chunk};

    source([&](std::span<const Byte> plain_chunk, bool is_final) {
        pushSecretStreamFrame(state, plain_chunk, is_final, cipher_chunk, emit_frame);
    });
}

void appendFramedCipherBytes(vBytes& output_vec, std::span<const Byte> cipher_frame) {
    const auto frame_len = encodeFrameLength(cipher_frame.size());
    output_vec.insert(output_vec.end(), frame_len.begin(), frame_len.end());
    output_vec.insert(output_vec.end(), cipher_frame.begin(), cipher_frame.end());
}

void writeFramedCipherBytes(std::ostream& output, std::span<const Byte> cipher_frame) {
    const auto frame_len = encodeFrameLength(cipher_frame.size());
    writeBytesOrThrow(output, frame_len, WRITE_COMPLETE_ERROR);
    writeBytesOrThrow(output, cipher_frame, WRITE_COMPLETE_ERROR);
}

template<typename ConsumeFn>
[[nodiscard]] bool pullSecretStreamFrame(
    crypto_secretstream_xchacha20poly1305_state& state,
    std::span<const Byte> cipher_frame,
    PlainChunk& plain_chunk,
    bool& has_final_tag,
    ConsumeFn&& consume) {

    constexpr std::size_t STREAM_ABYTES = crypto_secretstream_xchacha20poly1305_ABYTES;
    if (cipher_frame.size() < STREAM_ABYTES || cipher_frame.size() > plain_chunk.size() + STREAM_ABYTES) {
        return false;
    }

    const std::size_t max_plain_chunk = cipher_frame.size() - STREAM_ABYTES;
    unsigned long long plain_size = 0;
    unsigned char tag = 0;

    if (crypto_secretstream_xchacha20poly1305_pull(
            &state,
            plain_chunk.data(),
            &plain_size,
            &tag,
            cipher_frame.data(),
            cipher_frame.size(),
            nullptr,
            0) != 0) {
        return false;
    }
    if (plain_size > max_plain_chunk) {
        return false;
    }
    if (plain_size > 0) {
        consume(std::span<const Byte>(plain_chunk.data(), static_cast<std::size_t>(plain_size)));
    }

    has_final_tag = (tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL);
    return true;
}

template<typename ConsumeFn>
[[nodiscard]] bool decryptWithSecretStreamChunks(
    std::span<const Byte> encrypted_data,
    const Key& key,
    const StreamHeader& header,
    ConsumeFn&& consume) {

    crypto_secretstream_xchacha20poly1305_state state{};
    if (crypto_secretstream_xchacha20poly1305_init_pull(&state, header.data(), key.data()) != 0) {
        return false;
    }

    PlainChunk plain_chunk{};
    ZeroGuard<PlainChunk> guard{&plain_chunk};

    std::size_t offset = 0;
    bool has_final_tag = false;

    while (offset < encrypted_data.size()) {
        if (encrypted_data.size() - offset < STREAM_FRAME_LEN_BYTES) {
            return false;
        }

        const uint32_t frame_len = decodeFrameLength(
            std::span<const Byte, STREAM_FRAME_LEN_BYTES>(encrypted_data.data() + offset, STREAM_FRAME_LEN_BYTES));
        offset += STREAM_FRAME_LEN_BYTES;

        if (frame_len > encrypted_data.size() - offset ||
            !pullSecretStreamFrame(
                state,
                encrypted_data.subspan(offset, frame_len),
                plain_chunk,
                has_final_tag,
                consume)) {
            return false;
        }

        offset += frame_len;
        if (has_final_tag) {
            break;
        }
    }

    return has_final_tag && (offset == encrypted_data.size());
}

template<typename ConsumeFn>
[[nodiscard]] bool decryptWithSecretStreamFileInputChunks(
    const fs::path& encrypted_input_path,
    const Key& key,
    const StreamHeader& header,
    ConsumeFn&& consume) {

    std::ifstream input = openBinaryInputOrThrow(
        encrypted_input_path,
        "Read Error: Failed to open encrypted stream input.");

    [[maybe_unused]] const std::size_t encrypted_input_size = checkedFileSize(
        encrypted_input_path,
        "Read Error: Invalid encrypted stream input size.",
        true);

    crypto_secretstream_xchacha20poly1305_state state{};
    if (crypto_secretstream_xchacha20poly1305_init_pull(&state, header.data(), key.data()) != 0) {
        return false;
    }

    CipherChunk cipher_chunk{};
    PlainChunk plain_chunk{};
    ZeroGuard<CipherChunk> cipher_guard{&cipher_chunk};
    ZeroGuard<PlainChunk> plain_guard{&plain_chunk};

    bool has_final_tag = false;
    while (true) {
        std::array<Byte, STREAM_FRAME_LEN_BYTES> frame_len_bytes{};
        if (!tryReadExact(input, frame_len_bytes.data(), frame_len_bytes.size())) {
            return false;
        }

        const uint32_t frame_len = decodeFrameLength(frame_len_bytes);
        if (frame_len > cipher_chunk.size() || !tryReadExact(input, cipher_chunk.data(), frame_len)) {
            return false;
        }
        if (!pullSecretStreamFrame(
                state,
                std::span<const Byte>(cipher_chunk.data(), frame_len),
                plain_chunk,
                has_final_tag,
                consume)) {
            return false;
        }
        if (has_final_tag) {
            break;
        }
    }

    input.peek();
    return input.eof();
}

class StreamInflateToFile {
public:
    explicit StreamInflateToFile(const fs::path& output_path) : output_(openBinaryOutputForWriteOrThrow(output_path)) {
        if (inflateInit(&stream_) != Z_OK) throw std::runtime_error("zlib: inflateInit failed");
        initialized_ = true;
    }

    StreamInflateToFile(const StreamInflateToFile&) = delete;
    StreamInflateToFile& operator=(const StreamInflateToFile&) = delete;

    ~StreamInflateToFile() { if (initialized_) inflateEnd(&stream_); }

    void consume(std::span<const Byte> compressed_chunk) {
        if (compressed_chunk.empty()) return;
        if (finished_) throw std::runtime_error("zlib inflate error: trailing compressed data");
        if (compressed_chunk.size() > static_cast<std::size_t>(std::numeric_limits<uInt>::max())) {
            throw std::runtime_error("zlib inflate error: compressed chunk exceeds supported size");
        }

        stream_.next_in = const_cast<Byte*>(compressed_chunk.data());
        stream_.avail_in = static_cast<uInt>(compressed_chunk.size());

        while (stream_.avail_in > 0) {
            stream_.next_out = out_chunk_.data();
            stream_.avail_out = static_cast<uInt>(out_chunk_.size());

            const int ret = inflate(&stream_, Z_NO_FLUSH);
            writeProduced(out_chunk_.size() - stream_.avail_out);

            if (ret == Z_STREAM_END) {
                finished_ = true;
                if (stream_.avail_in != 0) {
                    throw std::runtime_error("zlib inflate error: trailing compressed data");
                }
                break;
            }
            if (ret == Z_BUF_ERROR) {
                if (stream_.avail_out == 0) {
                    continue;
                }
                if (stream_.avail_in == 0) {
                    break;
                }
                throw std::runtime_error("zlib inflate error: truncated or corrupt input");
            }
            if (ret != Z_OK) {
                throw std::runtime_error(std::format(
                    "zlib inflate error: {}",
                    stream_.msg ? stream_.msg : std::to_string(ret)));
            }
        }
    }

    [[nodiscard]] std::size_t finish() {
        while (!finished_) {
            stream_.next_in = nullptr;
            stream_.avail_in = 0;
            stream_.next_out = out_chunk_.data();
            stream_.avail_out = static_cast<uInt>(out_chunk_.size());

            const int ret = inflate(&stream_, Z_FINISH);
            writeProduced(out_chunk_.size() - stream_.avail_out);

            if (ret == Z_STREAM_END) {
                finished_ = true;
                break;
            }
            if (ret == Z_BUF_ERROR) {
                if (stream_.avail_out == 0) {
                    continue;
                }
                throw std::runtime_error("zlib inflate error: truncated or corrupt input");
            }
            if (ret != Z_OK) {
                throw std::runtime_error(std::format(
                    "zlib inflate error: {}",
                    stream_.msg ? stream_.msg : std::to_string(ret)));
            }
        }

        flushOutputOrThrow(output_, WRITE_COMPLETE_ERROR);
        if (output_size_ == 0) {
            throw std::runtime_error("Zlib Compression Error: Output file is empty. Inflating file failed.");
        }
        return output_size_;
    }

private:
    void writeProduced(std::size_t produced) {
        if (produced == 0) return;
        if (produced > STREAM_INFLATE_MAX_OUTPUT || output_size_ > STREAM_INFLATE_MAX_OUTPUT - produced) {
            throw std::runtime_error("zlib inflate error: output exceeds safe size limit");
        }
        writeBytesOrThrow(
            output_,
            std::span<const Byte>(out_chunk_.data(), produced),
            WRITE_COMPLETE_ERROR);
        output_size_ += produced;
    }

    z_stream stream_{};
    bool initialized_{false};
    bool finished_{false};
    std::ofstream output_{};
    std::array<Byte, STREAM_INFLATE_OUT_CHUNK_SIZE> out_chunk_{};
    std::size_t output_size_{0};
};

class FilenamePrefixExtractor {
public:
    template <typename PayloadFn>
    void consume(std::span<const Byte> chunk, PayloadFn&& payload_consume) {
        std::size_t pos = 0;

        if (!has_length_) {
            if (chunk.empty()) {
                return;
            }
            expected_len_ = chunk[pos++];
            has_length_ = true;
            if (expected_len_ == 0) {
                throw std::runtime_error(CORRUPT_FILENAME_ERROR);
            }
            filename_.reserve(expected_len_);
        }

        if (filename_.size() < expected_len_) {
            const std::size_t need = expected_len_ - filename_.size();
            const std::size_t take = std::min(need, chunk.size() - pos);
            if (take > 0) {
                filename_.append(
                    reinterpret_cast<const char*>(chunk.data() + static_cast<std::ptrdiff_t>(pos)),
                    take);
                pos += take;
            }
        }

        if (pos < chunk.size()) {
            payload_consume(chunk.subspan(pos));
        }
    }

    [[nodiscard]] bool isComplete() const noexcept { return has_length_ && filename_.size() == expected_len_; }
    [[nodiscard]] const std::string& filename() const noexcept { return filename_; }

private:
    bool has_length_{false};
    std::size_t expected_len_{0};
    std::string filename_{};
};

template<typename ChunkFn>
void forEachPrefixedPlainChunkFromFile(
    const fs::path& data_path,
    std::size_t input_size,
    std::span<const Byte> prefix_plaintext,
    ChunkFn&& chunk_fn);

template<typename EmitFrameFn>
void encryptFileWithSecretStreamPrefixedImpl(
    const fs::path& data_path,
    std::size_t input_size,
    std::span<const Byte> prefix_plaintext,
    const Key& key,
    StreamHeader& header,
    EmitFrameFn&& emit_frame) {
    encryptSecretStreamFrames(
        key,
        header,
        [&](auto&& emit_plain_chunk) { forEachPrefixedPlainChunkFromFile(data_path, input_size, prefix_plaintext, emit_plain_chunk); },
        emit_frame);
}

template<typename ChunkFn>
void forEachPrefixedPlainChunkFromFile(
    const fs::path& data_path,
    std::size_t input_size,
    std::span<const Byte> prefix_plaintext,
    ChunkFn&& chunk_fn) {

    if (input_size == 0) {
        throw std::runtime_error("Data File Error: File is empty.");
    }
    if (prefix_plaintext.size() > std::numeric_limits<std::size_t>::max() - input_size) {
        throw std::runtime_error("File Size Error: Encrypted output overflow.");
    }

    std::ifstream input = openBinaryInputOrThrow(data_path, "Read Error: Failed to open file for encryption.");

    std::array<Byte, STREAM_CHUNK_SIZE> in_chunk{};
    ZeroGuard<std::array<Byte, STREAM_CHUNK_SIZE>> in_chunk_guard{&in_chunk};

    const std::size_t total_plain_size = input_size + prefix_plaintext.size();
    std::size_t remaining_plain = total_plain_size;
    std::size_t prefix_offset = 0;
    std::size_t input_left = input_size;

    while (remaining_plain > 0) {
        std::size_t filled = 0;

        if (prefix_offset < prefix_plaintext.size()) {
            const std::size_t prefix_bytes = std::min(
                STREAM_CHUNK_SIZE,
                prefix_plaintext.size() - prefix_offset);
            std::memcpy(
                in_chunk.data(),
                prefix_plaintext.data() + static_cast<std::ptrdiff_t>(prefix_offset),
                prefix_bytes);
            prefix_offset += prefix_bytes;
            filled = prefix_bytes;
        }

        if (filled < STREAM_CHUNK_SIZE && input_left > 0) {
            const std::size_t file_bytes = std::min(STREAM_CHUNK_SIZE - filled, input_left);
            const std::streamsize read_count = readSomeOrThrow(
                input,
                in_chunk.data() + static_cast<std::ptrdiff_t>(filled),
                file_bytes,
                "Read Error: Failed while reading input file.");
            if (read_count != static_cast<std::streamsize>(file_bytes)) {
                throw std::runtime_error("Read Error: Failed to read full input while encrypting.");
            }

            filled += file_bytes;
            input_left -= file_bytes;
        }

        if (filled == 0) {
            throw std::runtime_error("Internal Error: Plaintext size accounting mismatch.");
        }

        chunk_fn(std::span<const Byte>(in_chunk.data(), filled), remaining_plain == filled);
        sodium_memzero(in_chunk.data(), filled);
        remaining_plain -= filled;
    }

    if (prefix_offset != prefix_plaintext.size() || input_left != 0 || remaining_plain != 0) {
        throw std::runtime_error("Internal Error: Plaintext size accounting mismatch.");
    }
}
}

[[nodiscard]] std::size_t computeStreamEncryptedSize(std::size_t plaintext_size) {
    constexpr std::size_t STREAM_ABYTES = crypto_secretstream_xchacha20poly1305_ABYTES;
    if (plaintext_size == 0) {
        return 0;
    }
    const std::size_t chunk_count =
        plaintext_size / STREAM_CHUNK_SIZE + ((plaintext_size % STREAM_CHUNK_SIZE) != 0 ? 1 : 0);
    const std::size_t per_chunk_overhead = STREAM_ABYTES + STREAM_FRAME_LEN_BYTES;

    if (chunk_count > (std::numeric_limits<std::size_t>::max() - plaintext_size) / per_chunk_overhead) {
        throw std::runtime_error("File Size Error: Encrypted output overflow.");
    }
    return plaintext_size + chunk_count * per_chunk_overhead;
}

[[nodiscard]] std::size_t computeStreamEncryptedSizePrefixed(
    std::size_t input_plaintext_size,
    std::size_t prefix_plaintext_size) {
    if (prefix_plaintext_size > std::numeric_limits<std::size_t>::max() - input_plaintext_size) {
        throw std::runtime_error("File Size Error: Encrypted output overflow.");
    }
    return computeStreamEncryptedSize(input_plaintext_size + prefix_plaintext_size);
}

void encryptWithSecretStream(vBytes& data_vec, const Key& key, std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header) {
    vBytes encrypted_vec;
    encrypted_vec.reserve(computeStreamEncryptedSize(data_vec.size()));
    encryptSecretStreamFrames(
        key,
        header,
        [&](auto&& emit_plain_chunk) {
            forEachPlainChunk(std::span<const Byte>(data_vec), emit_plain_chunk);
        },
        [&](std::span<const Byte> cipher_frame) {
            appendFramedCipherBytes(encrypted_vec, cipher_frame);
        });

    if (!data_vec.empty()) {
        sodium_memzero(data_vec.data(), data_vec.size());
    }
    data_vec = std::move(encrypted_vec);
}

void encryptFileWithSecretStreamPrefixed(
    const fs::path& data_path,
    std::size_t input_size,
    std::span<const Byte> prefix_plaintext,
    const Key& key,
    std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header,
    vBytes& output_vec) {
    const std::size_t total_plain_size = input_size + prefix_plaintext.size();

    output_vec.clear();
    output_vec.reserve(computeStreamEncryptedSize(total_plain_size));
    encryptFileWithSecretStreamPrefixedImpl(
        data_path, input_size, prefix_plaintext, key, header,
        [&](std::span<const Byte> cipher_frame) { appendFramedCipherBytes(output_vec, cipher_frame); });
}

void encryptFileWithSecretStreamPrefixedToFile(
    const fs::path& data_path,
    std::size_t input_size,
    std::span<const Byte> prefix_plaintext,
    const Key& key,
    std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header,
    const fs::path& output_path) {
    std::ofstream output = openBinaryOutputForWriteOrThrow(output_path);
    encryptFileWithSecretStreamPrefixedImpl(
        data_path, input_size, prefix_plaintext, key, header,
        [&](std::span<const Byte> cipher_frame) { writeFramedCipherBytes(output, cipher_frame); });
    closeOutputOrThrow(output, WRITE_COMPLETE_ERROR);
}

[[nodiscard]] bool decryptWithSecretStream(vBytes& data_vec, const Key& key,
    const std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header) {

    vBytes plain_vec;
    plain_vec.reserve(data_vec.size());

    const bool ok = decryptWithSecretStreamChunks(
        std::span<const Byte>(data_vec),
        key,
        header,
        [&](std::span<const Byte> chunk) {
            plain_vec.insert(plain_vec.end(), chunk.begin(), chunk.end());
        }
    );
    if (!ok) {
        return false;
    }

    if (!data_vec.empty()) {
        sodium_memzero(data_vec.data(), data_vec.size());
    }
    data_vec = std::move(plain_vec);
    return true;
}

namespace {
void appendOutputChunkToFile(std::ostream& output, std::span<const Byte> chunk, std::size_t& output_size) {
    if (chunk.size() > std::numeric_limits<std::size_t>::max() - output_size) {
        throw std::runtime_error("File Size Error: Decrypted output size overflow.");
    }
    writeBytesOrThrow(output, chunk, WRITE_COMPLETE_ERROR);
    output_size += chunk.size();
}

template<typename DecryptFn>
[[nodiscard]] bool decryptToFileImpl(DecryptFn&& decrypt_fn, const fs::path& output_path, std::size_t& output_size) {
    std::ofstream output = openBinaryOutputForWriteOrThrow(output_path);
    output_size = 0;
    const bool ok = decrypt_fn([&](std::span<const Byte> chunk) { appendOutputChunkToFile(output, chunk, output_size); });
    if (!ok) return false;
    flushOutputOrThrow(output, WRITE_COMPLETE_ERROR);
    return true;
}

template<typename DecryptFn>
[[nodiscard]] bool decryptToFileExtractingFilenameImpl(
    DecryptFn&& decrypt_fn, bool is_compressed_payload,
    const fs::path& output_path, std::size_t& output_size, std::string& decrypted_filename) {
    output_size = 0;
    decrypted_filename.clear();
    FilenamePrefixExtractor prefix_extractor;

    if (is_compressed_payload) {
        StreamInflateToFile inflater(output_path);
        const bool ok = decrypt_fn([&](std::span<const Byte> chunk) {
            prefix_extractor.consume(chunk, [&](std::span<const Byte> p) { inflater.consume(p); });
        });
        if (!ok || !prefix_extractor.isComplete()) return false;
        output_size = inflater.finish();
    } else {
        std::ofstream output = openBinaryOutputForWriteOrThrow(output_path);
        const bool ok = decrypt_fn([&](std::span<const Byte> chunk) {
            prefix_extractor.consume(chunk, [&](std::span<const Byte> p) { appendOutputChunkToFile(output, p, output_size); });
        });
        if (!ok || !prefix_extractor.isComplete()) return false;
        flushOutputOrThrow(output, WRITE_COMPLETE_ERROR);
    }

    decrypted_filename = prefix_extractor.filename();
    return true;
}
}

[[nodiscard]] bool decryptWithSecretStreamToFile(std::span<const Byte> encrypted_data, const Key& key,
    const std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header,
    const fs::path& output_path, std::size_t& output_size) {
    return decryptToFileImpl([&](auto&& consume) { return decryptWithSecretStreamChunks(encrypted_data, key, header, consume); }, output_path, output_size);
}

[[nodiscard]] bool decryptWithSecretStreamFileInputToFile(const fs::path& encrypted_input_path, const Key& key,
    const std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header,
    const fs::path& output_path, std::size_t& output_size) {
    return decryptToFileImpl([&](auto&& consume) { return decryptWithSecretStreamFileInputChunks(encrypted_input_path, key, header, consume); }, output_path, output_size);
}

[[nodiscard]] bool decryptWithSecretStreamToFileExtractingFilename(
    std::span<const Byte> encrypted_data, const Key& key,
    const std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header,
    bool is_compressed_payload, const fs::path& output_path,
    std::size_t& output_size, std::string& decrypted_filename) {
    return decryptToFileExtractingFilenameImpl([&](auto&& consume) { return decryptWithSecretStreamChunks(encrypted_data, key, header, consume); }, is_compressed_payload, output_path, output_size, decrypted_filename);
}

[[nodiscard]] bool decryptWithSecretStreamFileInputToFileExtractingFilename(
    const fs::path& encrypted_input_path, const Key& key,
    const std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header,
    bool is_compressed_payload, const fs::path& output_path,
    std::size_t& output_size, std::string& decrypted_filename) {
    return decryptToFileExtractingFilenameImpl([&](auto&& consume) { return decryptWithSecretStreamFileInputChunks(encrypted_input_path, key, header, consume); }, is_compressed_payload, output_path, output_size, decrypted_filename);
}
