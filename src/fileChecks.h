#pragma once

#include "programArgs.h"

bool isCompressedFile(const std::string&);
bool hasValidImageExtension(const std::string&);
bool hasValidFilename(const std::string&);
void validateImageFile(const std::string&, ArgMode, ArgOption, const uintmax_t);
void validateDataFile(const std::string&, ArgOption, const uintmax_t, const uintmax_t);
