#pragma once

#include "common.h"

#include <span>

void zlibCompressFileToPath(const fs::path& input_path, const fs::path& output_path, std::size_t expected_input_size);
void zlibInflateToFile(std::span<const Byte> compressed_data, const fs::path& output_path);
