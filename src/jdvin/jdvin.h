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

#include "profilesVec.cpp"
#include "writeFile.cpp"
#include "searchFunc.cpp"
#include "eraseSegments.cpp"
#include "valueUpdater.cpp"
#include "encryptFile.cpp"
#include "deflateFile.cpp"
#include "insertProfileHeaders.cpp"
#include "information.cpp"
#include "jdvin.cpp"

template <uint_fast8_t N>
uint_fast32_t searchFunc(std::vector<uint_fast8_t>&, uint_fast32_t, const uint_fast8_t, const uint_fast8_t (&)[N]);

uint_fast32_t deflateFile(std::vector<uint_fast8_t>&);

bool writeFile(std::vector<uint_fast8_t>&);

void
	eraseSegments(std::vector<uint_fast8_t>&, bool&),
	encryptFile(std::vector<uint_fast8_t>&, std::vector<uint_fast8_t>&, uint_fast32_t, std::string&),
	insertProfileHeaders(std::vector<uint_fast8_t>&, std::vector<uint_fast8_t>&),
	valueUpdater(std::vector<uint_fast8_t>&, uint_fast32_t, const uint_fast32_t, uint_fast8_t),
	displayInfo();

uint_fast8_t jdvIn(const std::string&, std::string&, bool);
