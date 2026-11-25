#pragma once

// zlib allocation callbacks that zero every block before releasing it (and
// the size-header blocks compression.cpp's libdeflate callbacks share).
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

// A block with its size stored in a header ahead of it, so release can wipe
// the whole block. Shared with compression.cpp's libdeflate callbacks, whose
// free_func, like zfree, is given only the pointer.
[[nodiscard]] inline void* allocateBlock(std::size_t bytes) noexcept {
    if (bytes > std::numeric_limits<std::size_t>::max() - kHeaderSize) return nullptr;
    auto* block = static_cast<unsigned char*>(std::malloc(kHeaderSize + bytes));
    if (block == nullptr) return nullptr;
    *reinterpret_cast<std::size_t*>(block) = bytes;
    return block + kHeaderSize;
}

inline void releaseBlock(void* address) noexcept {
    if (address == nullptr) return;
    auto* block = static_cast<unsigned char*>(address) - kHeaderSize;
    const std::size_t bytes = *reinterpret_cast<const std::size_t*>(block);
    sodium_memzero(block, kHeaderSize + bytes);
    std::free(block);
}

// zlib.h declares alloc_func/free_func inside extern "C", and language linkage
// is part of a function type, so the callbacks need C linkage too. Their names
// are unqualified at link level, hence the distinctive prefix.
extern "C" inline voidpf jdvrifZlibWipeAllocate(voidpf, uInt items, uInt size) noexcept {
    const std::size_t a = items, b = size;
    if (b != 0 && a > (std::numeric_limits<std::size_t>::max() - kHeaderSize) / b) return Z_NULL;
    return allocateBlock(a * b);
}

extern "C" inline void jdvrifZlibWipeRelease(voidpf, voidpf address) noexcept {
    releaseBlock(address);
}

// Call on a zero-initialised z_stream before inflateInit/deflateInit.
inline void install(z_stream& stream) noexcept {
    stream.zalloc = &jdvrifZlibWipeAllocate;
    stream.zfree = &jdvrifZlibWipeRelease;
    stream.opaque = Z_NULL;
}

} // namespace zlib_wipe_alloc
