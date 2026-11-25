#pragma once

#include "file_utils.h"

#include <string_view>

[[nodiscard]] fs::path validatedRecoveryPath(std::string_view decrypted_filename);
[[nodiscard]] fs::path commitRecoveredOutput(const StagingFile& staged_file, const fs::path& base_output_path);
