#pragma once

// zlib allocation callbacks that zero every block before releasing it.
//
// zlib's own state holds plaintext: inflate's 32 KiB history window and
// deflate's window/pending buffers. inflateEnd/deflateEnd free those through
// zfree without clearing them, so the default allocator leaves recovered or
// about-to-be-encrypted data in released heap memory. These callbacks store
// each block's size in a header ahead of it so zfree can wipe the whole block.

#include <sodium.h>
#include <zlib.h>

#include <cstddef>
#include <cstdlib>
#include <limits>

namespace zlib_wipe_alloc {

// Header size keeps the returned pointer aligned like malloc's own result.
inline constexpr std::size_t kHeaderSize = alignof(std::max_align_t) > sizeof(std::size_t)
    ? alignof(std::max_align_t) : sizeof(std::size_t);

inline voidpf allocate(voidpf, uInt items, uInt size) noexcept {
    const std::size_t a = items, b = size;
    if (b != 0 && a > (std::numeric_limits<std::size_t>::max() - kHeaderSize) / b) return Z_NULL;
    const std::size_t bytes = a * b;
    auto* block = static_cast<unsigned char*>(std::malloc(kHeaderSize + bytes));
    if (block == nullptr) return Z_NULL;
    *reinterpret_cast<std::size_t*>(block) = bytes;
    return block + kHeaderSize;
}

inline void release(voidpf, voidpf address) noexcept {
    if (address == Z_NULL) return;
    auto* block = static_cast<unsigned char*>(address) - kHeaderSize;
    const std::size_t bytes = *reinterpret_cast<const std::size_t*>(block);
    sodium_memzero(block, kHeaderSize + bytes);
    std::free(block);
}

// Call on a zero-initialised z_stream before inflateInit/deflateInit.
inline void install(z_stream& stream) noexcept {
    stream.zalloc = &allocate;
    stream.zfree = &release;
    stream.opaque = Z_NULL;
}

} // namespace zlib_wipe_alloc
