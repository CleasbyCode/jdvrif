#pragma once

#include "common.h"

void concealData(vBytes& jpg_vec, Option option, const fs::path& image_file_path, const fs::path& data_file_path);
// capsize: the -r (Option::Reddit) or -x (Option::Twitter) carrier capacity.
void displayCapacity(vBytes& jpg_vec, Option option);
