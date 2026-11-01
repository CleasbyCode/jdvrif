#pragma once

#include "cipher_source.h"
#include "common.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

// Fill `out` from `path` at `offset`; a file too short to supply it is an error.
void readExactAt(const fs::path& path, std::size_t offset, std::span<Byte> out);

// Both are opened after the PIN is taken, and read the ciphertext where it
// sits rather than staging a copy of it.
[[nodiscard]] std::unique_ptr<CipherSource> openDefaultCiphertext(
    const fs::path& image_path,
    std::size_t image_size,
    std::size_t base_offset,
    std::size_t embedded_file_size,
    std::uint16_t total_profile_header_segments);

[[nodiscard]] std::unique_ptr<CipherSource> openBlueskyCiphertext(
    const fs::path& image_path,
    std::size_t image_size,
    std::size_t embedded_file_size);
