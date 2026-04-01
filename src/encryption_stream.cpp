#include "encryption_internal.h"
#include "encryption_stream_shared.h"
#include "file_utils.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>

namespace {
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
    const std::size_t total_plain_size = checkedAdd(input_size, prefix_plaintext.size(), "File Size Error: Encrypted output overflow.");

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
