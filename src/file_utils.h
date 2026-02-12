#pragma once

#include "common.h"

#include <initializer_list>

[[nodiscard]] bool hasValidFilename(const fs::path& p);
[[nodiscard]] bool hasFileExtension(const fs::path& p, std::initializer_list<std::string_view> exts);
[[nodiscard]] vBytes readFile(const fs::path& path, FileTypeCheck FileType = FileTypeCheck::data_file);
