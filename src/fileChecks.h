#pragma once

#include "programArgs.h"
#include <vector>
#include <cstdint>

bool isCompressedFile(const std::string&);
bool hasValidImageExtension(const std::string&);
bool hasValidFilename(const std::string&);
void validateImageFile(std::string&, ArgMode, ArgOption, uintmax_t&, std::vector<uint8_t>&);
void validateDataFile(std::string&, ArgOption, uintmax_t&, uintmax_t&, std::vector<uint8_t>&, bool&);
