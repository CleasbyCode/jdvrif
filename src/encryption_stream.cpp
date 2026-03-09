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
constexpr std::size_t
    STREAM_INFLATE_OUT_CHUNK_SIZE = 2 * 1024 * 1024,
    STREAM_INFLATE_MAX_OUTPUT = 3ULL * 1024 * 1024 * 1024;
constexpr const char* WRITE_COMPLETE_ERROR = "Write Error: Failed to write complete output file.";
constexpr const char* CORRUPT_FILENAME_ERROR = "File Extraction Error: Corrupt encrypted filename metadata.";

template<typename ConsumeFn>
[[nodiscard]] bool decryptWithSecretStreamChunks(std::span<const Byte> encrypted_data, const Key& key,
    const std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header, ConsumeFn&& consume) {

    crypto_secretstream_xchacha20poly1305_state state{};
    if (crypto_secretstream_xchacha20poly1305_init_pull(&state, header.data(), key.data()) != 0) {
        return false;
    }

    constexpr std::size_t STREAM_ABYTES = crypto_secretstream_xchacha20poly1305_ABYTES;
    std::array<Byte, STREAM_CHUNK_SIZE> plain_chunk{};
    struct PlainChunkGuard {
        std::array<Byte, STREAM_CHUNK_SIZE>* buf;
        ~PlainChunkGuard() { sodium_memzero(buf->data(), buf->size()); }
    } guard{&plain_chunk};

    std::size_t offset = 0;
    bool has_final_tag = false;

    while (offset < encrypted_data.size()) {
        if (encrypted_data.size() - offset < STREAM_FRAME_LEN_BYTES) {
            return false;
        }

        const uint32_t frame_len =
            (static_cast<uint32_t>(encrypted_data[offset]) << 24) |
            (static_cast<uint32_t>(encrypted_data[offset + 1]) << 16) |
            (static_cast<uint32_t>(encrypted_data[offset + 2]) << 8) |
            static_cast<uint32_t>(encrypted_data[offset + 3]);
        offset += STREAM_FRAME_LEN_BYTES;

        if (frame_len < STREAM_ABYTES || frame_len > encrypted_data.size() - offset) {
            return false;
        }

        const std::size_t max_plain_chunk = frame_len - STREAM_ABYTES;
        if (max_plain_chunk > plain_chunk.size()) {
            return false;
        }

        unsigned long long mlen = 0;
        unsigned char tag = 0;

        if (crypto_secretstream_xchacha20poly1305_pull(
                &state,
                plain_chunk.data(),
                &mlen,
                &tag,
                encrypted_data.data() + offset,
                frame_len,
                nullptr,
                0) != 0) {
            return false;
        }

        if (mlen > max_plain_chunk) {
            return false;
        }
        if (mlen > 0) {
            consume(std::span<const Byte>(plain_chunk.data(), static_cast<std::size_t>(mlen)));
        }

        offset += frame_len;

        if (tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL) {
            has_final_tag = true;
            break;
        }
    }

    return has_final_tag && (offset == encrypted_data.size());
}

template<typename ConsumeFn>
[[nodiscard]] bool decryptWithSecretStreamFileInputChunks(
    const fs::path& encrypted_input_path,
    const Key& key,
    const std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header,
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

    constexpr std::size_t STREAM_ABYTES = crypto_secretstream_xchacha20poly1305_ABYTES;
    std::array<Byte, STREAM_CHUNK_SIZE + STREAM_ABYTES> cipher_chunk{};
    std::array<Byte, STREAM_CHUNK_SIZE> plain_chunk{};
    struct ChunkGuard {
        std::array<Byte, STREAM_CHUNK_SIZE + STREAM_ABYTES>* c;
        std::array<Byte, STREAM_CHUNK_SIZE>* p;
        ~ChunkGuard() {
            sodium_memzero(c->data(), c->size());
            sodium_memzero(p->data(), p->size());
        }
    } guard{&cipher_chunk, &plain_chunk};

    bool has_final_tag = false;
    while (true) {
        std::array<Byte, STREAM_FRAME_LEN_BYTES> len_buf{};
        if (!tryReadExact(input, len_buf.data(), len_buf.size())) {
            return false;
        }

        const uint32_t frame_len =
            (static_cast<uint32_t>(len_buf[0]) << 24) |
            (static_cast<uint32_t>(len_buf[1]) << 16) |
            (static_cast<uint32_t>(len_buf[2]) << 8) |
            static_cast<uint32_t>(len_buf[3]);

        if (frame_len < STREAM_ABYTES || frame_len > cipher_chunk.size()) {
            return false;
        }
        if (!tryReadExact(input, cipher_chunk.data(), frame_len)) {
            return false;
        }

        unsigned long long mlen = 0;
        unsigned char tag = 0;
        if (crypto_secretstream_xchacha20poly1305_pull(
                &state,
                plain_chunk.data(),
                &mlen,
                &tag,
                cipher_chunk.data(),
                frame_len,
                nullptr,
                0) != 0) {
            return false;
        }
        if (mlen > plain_chunk.size()) {
            return false;
        }
        if (mlen > 0) {
            consume(std::span<const Byte>(plain_chunk.data(), static_cast<std::size_t>(mlen)));
        }

        if (tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL) {
            has_final_tag = true;
            break;
        }
    }

    if (!has_final_tag) {
        return false;
    }

    input.peek();
    return input.eof();
}

class StreamInflateToFile {
public:
    explicit StreamInflateToFile(const fs::path& output_path)
        : output_(openBinaryOutputForWriteOrThrow(output_path)) {
        if (inflateInit(&stream_) != Z_OK) {
            throw std::runtime_error("zlib: inflateInit failed");
        }
        initialized_ = true;
    }

    StreamInflateToFile(const StreamInflateToFile&) = delete;
    StreamInflateToFile& operator=(const StreamInflateToFile&) = delete;

    ~StreamInflateToFile() {
        if (initialized_) {
            inflateEnd(&stream_);
        }
    }

    void consume(std::span<const Byte> compressed_chunk) {
        if (compressed_chunk.empty()) {
            return;
        }
        if (finished_) {
            throw std::runtime_error("zlib inflate error: trailing compressed data");
        }
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
        if (produced == 0) {
            return;
        }
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

    [[nodiscard]] bool isComplete() const noexcept {
        return has_length_ && filename_.size() == expected_len_;
    }

    [[nodiscard]] const std::string& filename() const noexcept {
        return filename_;
    }

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
    ChunkFn&& chunk_fn) {

    if (input_size == 0) {
        throw std::runtime_error("Data File Error: File is empty.");
    }
    if (prefix_plaintext.size() > std::numeric_limits<std::size_t>::max() - input_size) {
        throw std::runtime_error("File Size Error: Encrypted output overflow.");
    }

    std::ifstream input = openBinaryInputOrThrow(data_path, "Read Error: Failed to open file for encryption.");

    std::array<Byte, STREAM_CHUNK_SIZE> in_chunk{};
    struct InChunkGuard {
        std::array<Byte, STREAM_CHUNK_SIZE>* buf;
        ~InChunkGuard() { sodium_memzero(buf->data(), buf->size()); }
    } in_chunk_guard{&in_chunk};

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
    crypto_secretstream_xchacha20poly1305_state state{};
    if (crypto_secretstream_xchacha20poly1305_init_push(&state, header.data(), key.data()) != 0) {
        throw std::runtime_error("crypto_secretstream init_push failed");
    }

    const std::size_t plaintext_size = data_vec.size();
    constexpr std::size_t STREAM_ABYTES = crypto_secretstream_xchacha20poly1305_ABYTES;
    const std::size_t encrypted_size = computeStreamEncryptedSize(plaintext_size);

    vBytes encrypted_vec;
    encrypted_vec.reserve(encrypted_size);

    std::size_t offset = 0;
    while (offset < plaintext_size) {
        const std::size_t mlen = std::min(STREAM_CHUNK_SIZE, plaintext_size - offset);
        const std::size_t cbuf_size = mlen + STREAM_ABYTES;
        if (cbuf_size > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
            throw std::runtime_error("File Size Error: Stream chunk exceeds size limit.");
        }

        const std::size_t frame_offset = encrypted_vec.size();
        encrypted_vec.resize(frame_offset + STREAM_FRAME_LEN_BYTES + cbuf_size);

        unsigned long long clen = 0;
        const unsigned char tag = (offset + mlen == plaintext_size)
            ? crypto_secretstream_xchacha20poly1305_TAG_FINAL
            : 0;

        if (crypto_secretstream_xchacha20poly1305_push(
                &state,
                encrypted_vec.data() + frame_offset + STREAM_FRAME_LEN_BYTES,
                &clen,
                data_vec.data() + offset,
                static_cast<unsigned long long>(mlen),
                nullptr,
                0,
                tag) != 0) {
            throw std::runtime_error("crypto_secretstream push failed");
        }

        if (clen != cbuf_size) {
            throw std::runtime_error("crypto_secretstream push produced unexpected size");
        }

        const uint32_t frame_len = static_cast<uint32_t>(clen);
        encrypted_vec[frame_offset + 0] = static_cast<Byte>((frame_len >> 24) & 0xFF);
        encrypted_vec[frame_offset + 1] = static_cast<Byte>((frame_len >> 16) & 0xFF);
        encrypted_vec[frame_offset + 2] = static_cast<Byte>((frame_len >> 8) & 0xFF);
        encrypted_vec[frame_offset + 3] = static_cast<Byte>(frame_len & 0xFF);

        offset += mlen;
    }

    if (!data_vec.empty()) {
        sodium_memzero(data_vec.data(), data_vec.size());
    }
    data_vec = std::move(encrypted_vec);
}

void encryptFileWithSecretStream(const fs::path& data_path, std::size_t input_size, const Key& key,
    std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header, vBytes& output_vec) {
    encryptFileWithSecretStreamPrefixed(data_path, input_size, {}, key, header, output_vec);
}

void encryptFileWithSecretStreamPrefixed(
    const fs::path& data_path,
    std::size_t input_size,
    std::span<const Byte> prefix_plaintext,
    const Key& key,
    std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header,
    vBytes& output_vec) {
    const std::size_t total_plain_size = input_size + prefix_plaintext.size();

    crypto_secretstream_xchacha20poly1305_state state{};
    if (crypto_secretstream_xchacha20poly1305_init_push(&state, header.data(), key.data()) != 0) {
        throw std::runtime_error("crypto_secretstream init_push failed");
    }

    constexpr std::size_t STREAM_ABYTES = crypto_secretstream_xchacha20poly1305_ABYTES;

    output_vec.clear();
    output_vec.reserve(computeStreamEncryptedSize(total_plain_size));

    auto pushPlainChunk = [&](std::span<const Byte> plain_chunk, bool is_final) {
        const std::size_t mlen = plain_chunk.size();
        const std::size_t cbuf_size = mlen + STREAM_ABYTES;
        if (cbuf_size > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
            throw std::runtime_error("File Size Error: Stream chunk exceeds size limit.");
        }

        const std::size_t frame_offset = output_vec.size();
        output_vec.resize(frame_offset + STREAM_FRAME_LEN_BYTES + cbuf_size);

        unsigned long long clen = 0;
        const unsigned char tag = is_final ? crypto_secretstream_xchacha20poly1305_TAG_FINAL : 0;

        if (crypto_secretstream_xchacha20poly1305_push(
                &state,
                output_vec.data() + frame_offset + STREAM_FRAME_LEN_BYTES,
                &clen,
                plain_chunk.data(),
                static_cast<unsigned long long>(mlen),
                nullptr,
                0,
                tag) != 0) {
            throw std::runtime_error("crypto_secretstream push failed");
        }
        if (clen != cbuf_size) {
            throw std::runtime_error("crypto_secretstream push produced unexpected size");
        }

        const uint32_t frame_len = static_cast<uint32_t>(clen);
        output_vec[frame_offset + 0] = static_cast<Byte>((frame_len >> 24) & 0xFF);
        output_vec[frame_offset + 1] = static_cast<Byte>((frame_len >> 16) & 0xFF);
        output_vec[frame_offset + 2] = static_cast<Byte>((frame_len >> 8) & 0xFF);
        output_vec[frame_offset + 3] = static_cast<Byte>(frame_len & 0xFF);
    };

    forEachPrefixedPlainChunkFromFile(
        data_path,
        input_size,
        prefix_plaintext,
        pushPlainChunk);
}

void encryptFileWithSecretStreamPrefixedToFile(
    const fs::path& data_path,
    std::size_t input_size,
    std::span<const Byte> prefix_plaintext,
    const Key& key,
    std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header,
    const fs::path& output_path) {
    crypto_secretstream_xchacha20poly1305_state state{};
    if (crypto_secretstream_xchacha20poly1305_init_push(&state, header.data(), key.data()) != 0) {
        throw std::runtime_error("crypto_secretstream init_push failed");
    }

    constexpr std::size_t STREAM_ABYTES = crypto_secretstream_xchacha20poly1305_ABYTES;
    std::ofstream output = openBinaryOutputForWriteOrThrow(output_path);
    std::array<Byte, STREAM_CHUNK_SIZE + STREAM_ABYTES> cipher_chunk{};
    struct CipherChunkGuard {
        std::array<Byte, STREAM_CHUNK_SIZE + STREAM_ABYTES>* buf;
        ~CipherChunkGuard() { sodium_memzero(buf->data(), buf->size()); }
    } cipher_chunk_guard{&cipher_chunk};

    auto pushPlainChunk = [&](std::span<const Byte> plain_chunk, bool is_final) {
        const std::size_t mlen = plain_chunk.size();
        const std::size_t cbuf_size = mlen + STREAM_ABYTES;
        if (cbuf_size > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
            throw std::runtime_error("File Size Error: Stream chunk exceeds size limit.");
        }

        unsigned long long clen = 0;
        const unsigned char tag = is_final ? crypto_secretstream_xchacha20poly1305_TAG_FINAL : 0;
        if (crypto_secretstream_xchacha20poly1305_push(
                &state,
                cipher_chunk.data(),
                &clen,
                plain_chunk.data(),
                static_cast<unsigned long long>(mlen),
                nullptr,
                0,
                tag) != 0) {
            throw std::runtime_error("crypto_secretstream push failed");
        }
        if (clen != cbuf_size) {
            throw std::runtime_error("crypto_secretstream push produced unexpected size");
        }

        std::array<Byte, STREAM_FRAME_LEN_BYTES> frame_len{
            static_cast<Byte>((clen >> 24) & 0xFF),
            static_cast<Byte>((clen >> 16) & 0xFF),
            static_cast<Byte>((clen >> 8) & 0xFF),
            static_cast<Byte>(clen & 0xFF)
        };
        writeBytesOrThrow(output, frame_len, WRITE_COMPLETE_ERROR);
        writeBytesOrThrow(
            output,
            std::span<const Byte>(cipher_chunk.data(), static_cast<std::size_t>(clen)),
            WRITE_COMPLETE_ERROR);
    };

    forEachPrefixedPlainChunkFromFile(
        data_path,
        input_size,
        prefix_plaintext,
        pushPlainChunk);
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

[[nodiscard]] bool decryptWithSecretStreamToFile(std::span<const Byte> encrypted_data, const Key& key,
    const std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header,
    const fs::path& output_path, std::size_t& output_size) {

    std::ofstream output = openBinaryOutputForWriteOrThrow(output_path);

    output_size = 0;
    const bool ok = decryptWithSecretStreamChunks(
        encrypted_data,
        key,
        header,
        [&](std::span<const Byte> chunk) {
            if (chunk.size() > std::numeric_limits<std::size_t>::max() - output_size) {
                throw std::runtime_error("File Size Error: Decrypted output size overflow.");
            }
            writeBytesOrThrow(output, chunk, WRITE_COMPLETE_ERROR);
            output_size += chunk.size();
        }
    );
    if (!ok) {
        return false;
    }

    flushOutputOrThrow(output, WRITE_COMPLETE_ERROR);
    return true;
}

[[nodiscard]] bool decryptWithSecretStreamFileInputToFile(const fs::path& encrypted_input_path, const Key& key,
    const std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header,
    const fs::path& output_path, std::size_t& output_size) {

    std::ofstream output = openBinaryOutputForWriteOrThrow(output_path);
    output_size = 0;
    const bool ok = decryptWithSecretStreamFileInputChunks(
        encrypted_input_path,
        key,
        header,
        [&](std::span<const Byte> chunk) {
            if (chunk.size() > std::numeric_limits<std::size_t>::max() - output_size) {
                throw std::runtime_error("File Size Error: Decrypted output size overflow.");
            }
            writeBytesOrThrow(output, chunk, WRITE_COMPLETE_ERROR);
            output_size += chunk.size();
        }
    );
    if (!ok) {
        return false;
    }

    flushOutputOrThrow(output, WRITE_COMPLETE_ERROR);
    return true;
}

[[nodiscard]] bool decryptWithSecretStreamAndInflateToFile(
    std::span<const Byte> encrypted_data,
    const Key& key,
    const std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header,
    const fs::path& output_path,
    std::size_t& output_size) {

    StreamInflateToFile inflater(output_path);
    const bool ok = decryptWithSecretStreamChunks(
        encrypted_data,
        key,
        header,
        [&](std::span<const Byte> chunk) { inflater.consume(chunk); }
    );
    if (!ok) {
        return false;
    }
    output_size = inflater.finish();
    return true;
}

[[nodiscard]] bool decryptWithSecretStreamFileInputAndInflateToFile(
    const fs::path& encrypted_input_path,
    const Key& key,
    const std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header,
    const fs::path& output_path,
    std::size_t& output_size) {

    StreamInflateToFile inflater(output_path);
    const bool ok = decryptWithSecretStreamFileInputChunks(
        encrypted_input_path,
        key,
        header,
        [&](std::span<const Byte> chunk) { inflater.consume(chunk); }
    );
    if (!ok) {
        return false;
    }
    output_size = inflater.finish();
    return true;
}

[[nodiscard]] bool decryptWithSecretStreamToFileExtractingFilename(
    std::span<const Byte> encrypted_data,
    const Key& key,
    const std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header,
    bool is_compressed_payload,
    const fs::path& output_path,
    std::size_t& output_size,
    std::string& decrypted_filename) {

    output_size = 0;
    decrypted_filename.clear();
    FilenamePrefixExtractor prefix_extractor;

    if (is_compressed_payload) {
        StreamInflateToFile inflater(output_path);
        const bool ok = decryptWithSecretStreamChunks(
            encrypted_data,
            key,
            header,
            [&](std::span<const Byte> chunk) {
                prefix_extractor.consume(chunk, [&](std::span<const Byte> payload_chunk) {
                    inflater.consume(payload_chunk);
                });
            }
        );
        if (!ok || !prefix_extractor.isComplete()) {
            return false;
        }
        output_size = inflater.finish();
    } else {
        std::ofstream output = openBinaryOutputForWriteOrThrow(output_path);
        const bool ok = decryptWithSecretStreamChunks(
            encrypted_data,
            key,
            header,
            [&](std::span<const Byte> chunk) {
                prefix_extractor.consume(chunk, [&](std::span<const Byte> payload_chunk) {
                    if (payload_chunk.size() > std::numeric_limits<std::size_t>::max() - output_size) {
                        throw std::runtime_error("File Size Error: Decrypted output size overflow.");
                    }
                    writeBytesOrThrow(output, payload_chunk, WRITE_COMPLETE_ERROR);
                    output_size += payload_chunk.size();
                });
            }
        );
        if (!ok || !prefix_extractor.isComplete()) {
            return false;
        }
        flushOutputOrThrow(output, WRITE_COMPLETE_ERROR);
    }

    decrypted_filename = prefix_extractor.filename();
    return true;
}

[[nodiscard]] bool decryptWithSecretStreamFileInputToFileExtractingFilename(
    const fs::path& encrypted_input_path,
    const Key& key,
    const std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>& header,
    bool is_compressed_payload,
    const fs::path& output_path,
    std::size_t& output_size,
    std::string& decrypted_filename) {

    output_size = 0;
    decrypted_filename.clear();
    FilenamePrefixExtractor prefix_extractor;

    if (is_compressed_payload) {
        StreamInflateToFile inflater(output_path);
        const bool ok = decryptWithSecretStreamFileInputChunks(
            encrypted_input_path,
            key,
            header,
            [&](std::span<const Byte> chunk) {
                prefix_extractor.consume(chunk, [&](std::span<const Byte> payload_chunk) {
                    inflater.consume(payload_chunk);
                });
            }
        );
        if (!ok || !prefix_extractor.isComplete()) {
            return false;
        }
        output_size = inflater.finish();
    } else {
        std::ofstream output = openBinaryOutputForWriteOrThrow(output_path);
        const bool ok = decryptWithSecretStreamFileInputChunks(
            encrypted_input_path,
            key,
            header,
            [&](std::span<const Byte> chunk) {
                prefix_extractor.consume(chunk, [&](std::span<const Byte> payload_chunk) {
                    if (payload_chunk.size() > std::numeric_limits<std::size_t>::max() - output_size) {
                        throw std::runtime_error("File Size Error: Decrypted output size overflow.");
                    }
                    writeBytesOrThrow(output, payload_chunk, WRITE_COMPLETE_ERROR);
                    output_size += payload_chunk.size();
                });
            }
        );
        if (!ok || !prefix_extractor.isComplete()) {
            return false;
        }
        flushOutputOrThrow(output, WRITE_COMPLETE_ERROR);
    }

    decrypted_filename = prefix_extractor.filename();
    return true;
}
