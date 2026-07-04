#pragma once

#include "file_utils.h"

#include <cstddef>
#include <string>

[[nodiscard]] fs::path validatedRecoveryPath(std::string decrypted_filename);
[[nodiscard]] fs::path tempRecoveryPath(const fs::path& output_path);
[[nodiscard]] fs::path commitRecoveredOutput(TempFileCleanupGuard& staged_file, const fs::path& base_output_path);
void printRecoverySuccess(const fs::path& output_path, std::size_t output_size);
