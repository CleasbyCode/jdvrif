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
#include <C:\Users\Nick\source\zlib-1.3.1\zlib.h>

#include "valueUpdater.cpp"
#include "encryptFile.cpp"
#include "deflateFile.cpp"
#include "searchFunc.cpp"
#include "insertProfileHeaders.cpp"
#include "information.cpp"
#include "jdvin.cpp"

template <uint_fast8_t N>
uint_fast32_t searchFunc(std::vector<uint_fast8_t>&, uint_fast32_t, uint_fast8_t, const uint_fast8_t (&)[N]);

uint_fast32_t deflateFile(std::vector<uint_fast8_t>&);

void
	startJdv(const std::string&, std::string&, bool),
	encryptFile(std::vector<uint_fast8_t>&, std::vector<uint_fast8_t>&, std::string&),
	insertProfileHeaders(std::vector<uint_fast8_t>&, std::vector<uint_fast8_t>&, uint_fast32_t),
	valueUpdater(std::vector<uint_fast8_t>&, uint_fast32_t, const uint_fast32_t, uint_fast8_t),
	displayInfo();

