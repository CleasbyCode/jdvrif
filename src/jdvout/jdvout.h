#pragma once

#include <cstdint>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <regex>
#include <iostream>
#include <string>
#include <vector>
#include <iterator>
#include <zlib.h>

#include "getByteValue.cpp"
#include "searchFunc.cpp"
#include "findProfileHeaders.cpp"
#include "decryptFile.cpp"
#include "inflateFile.cpp"
#include "information.cpp"
#include "jdvout.cpp"

std::string decryptFile(std::vector<uint8_t>&, std::vector<uint_fast32_t>&, std::string&, const uint_fast8_t);

template <uint_fast8_t N>
uint_fast32_t searchFunc(std::vector<uint8_t>&, uint_fast32_t, uint_fast8_t, const uint_fast8_t (&)[N]);

uint_fast32_t getByteValue(const std::vector<uint8_t>&, const uint_fast32_t);

void
	findProfileHeaders(std::vector<uint8_t>&, std::vector<uint_fast32_t>&, uint_fast16_t),
	inflateFile(std::vector<uint8_t>&),
	displayInfo();

uint_fast8_t jdvOut(const std::string&);	
