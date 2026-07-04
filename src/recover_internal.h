#pragma once

#include "common.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <span>

void readExactAt(std::ifstream& input, std::size_t offset, std::span<Byte> out);

[[nodiscard]] std::optional<std::size_t> findSignatureInFile(
    const fs::path& path,
    std::span<const Byte> sig,
    std::size_t search_limit = 0,
    std::size_t start_offset = 0);

// Single forward scan for the first occurrence of first_sig that also has
// second_sig at first_sig_index + second_sig_offset. Returns first_sig's index.
[[nodiscard]] std::optional<std::size_t> findSignaturePairInFile(
    const fs::path& path,
    std::span<const Byte> first_sig,
    std::span<const Byte> second_sig,
    std::size_t second_sig_offset);

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
