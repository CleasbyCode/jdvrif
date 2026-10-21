#pragma once

#include "encryption_internal.h"

#include <array>

// StreamHeader is defined in common.h (shared with the public decrypt API).
using PlainChunk = std::array<Byte, STREAM_CHUNK_SIZE>;
using CipherChunk = std::array<Byte, STREAM_CHUNK_SIZE + crypto_secretstream_xchacha20poly1305_ABYTES>;

// libsodium's secretstream state holds the derived subkey and nonce.
using SecretStreamState = SecureBuffer<crypto_secretstream_xchacha20poly1305_state>;
