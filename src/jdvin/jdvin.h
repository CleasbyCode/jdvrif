#pragma once

#include <algorithm>
#include <filesystem>
#include <random>
#include <cstdint>
#include <string_view>
#include <fstream>
#include <regex>
#include <iostream>
#include <string>
#include <vector>
#include <zlib.h>

#include "value_updater.cpp"
#include "encrypt.cpp"
#include "deflate.cpp"
#include "insert_profile_headers.cpp"
#include "information.cpp"
#include "jdvin.cpp"

uint_fast32_t deflateFile(std::vector<uint_fast8_t>&);

void
	startJdv(const std::string&, std::string&, bool),
	encryptFile(std::vector<uint_fast8_t>&, std::vector<uint_fast8_t>&, std::string&),
	insertProfileHeaders(std::vector<uint_fast8_t>&, std::vector<uint_fast8_t>&, uint_fast32_t),
	valueUpdater(std::vector<uint_fast8_t>&, uint_fast32_t, const uint_fast32_t, uint_fast8_t),
	displayInfo();

