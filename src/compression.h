#pragma once

#include "common.h"

#include <span>

void zlibCompressFileToPath(const fs::path& input_path, const fs::path& output_path);
void zlibInflateToFile(std::span<const Byte> compressed_data, const fs::path& output_path);
