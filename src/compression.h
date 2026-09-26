#pragma once

#include "common.h"

void zlibCompressFileToPath(const fs::path& input_path, const fs::path& output_path, std::size_t expected_input_size);
