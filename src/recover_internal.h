#pragma once

#include "common.h"

#include <cstddef>
#include <cstdint>
#include <span>

// Fill `out` from `path` at `offset`; a file too short to supply it is an error.
void readExactAt(const fs::path& path, std::size_t offset, std::span<Byte> out);

[[nodiscard]] std::size_t extractDefaultCiphertextToFile(
    const fs::path& image_path,
    std::size_t image_size,
    std::size_t base_offset,
    std::size_t embedded_file_size,
    std::uint16_t total_profile_header_segments,
    const fs::path& output_path);

[[nodiscard]] std::size_t extractBlueskyCiphertextToFile(
    const fs::path& image_path,
    std::size_t image_size,
    std::size_t embedded_file_size,
    const fs::path& output_path);
