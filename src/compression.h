#pragma once

#include "common.h"

#include <span>

void zlibFunc(vBytes& data_vec, Mode mode);
void zlibCompressFile(const fs::path& input_path, vBytes& output_vec);
void zlibCompressFileToPath(const fs::path& input_path, const fs::path& output_path);
void zlibInflateFileToFile(const fs::path& input_path, const fs::path& output_path);
void zlibInflateToFile(std::span<const Byte> compressed_data, const fs::path& output_path);
