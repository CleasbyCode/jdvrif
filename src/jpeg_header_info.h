#pragma once

// The frame header facts both coefficient carriers read (jpeg_codec::headerInfo
// fills it from libjpeg). Kept free of libjpeg itself so carrier headers can
// expose it. Compiled into the C++ CLI, the web variant and the Rust port's
// FFI build, so keep it byte-identical across the three trees.

#include <cstdint>

namespace jpeg_codec {

struct HeaderInfo {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t y_blocks_wide{};
    std::uint32_t y_blocks_high{};
    int components{};
    bool progressive{};
    bool is_420{};
    bool is_ycbcr{};

    [[nodiscard]] std::uint64_t yBlockCount() const noexcept {
        return static_cast<std::uint64_t>(y_blocks_wide) * y_blocks_high;
    }
};

} // namespace jpeg_codec
