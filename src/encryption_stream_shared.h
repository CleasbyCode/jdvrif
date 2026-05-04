#pragma once

#include "encryption_internal.h"

#include <array>

inline constexpr const char* WRITE_COMPLETE_ERROR = "Write Error: Failed to write complete output file.";

using StreamHeader = std::array<Byte, crypto_secretstream_xchacha20poly1305_HEADERBYTES>;
using PlainChunk = std::array<Byte, STREAM_CHUNK_SIZE>;
using CipherChunk = std::array<Byte, STREAM_CHUNK_SIZE + crypto_secretstream_xchacha20poly1305_ABYTES>;

template<typename Buffer>
struct ZeroGuard {
    Buffer* buf;
    ~ZeroGuard() { sodium_memzero(buf->data(), buf->size()); }
};
