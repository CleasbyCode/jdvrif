#pragma once

#include <cstdint>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <regex>
#include <iostream>
#include <string>
#include <vector>
#include <zlib.h>

#include "fourBytes.cpp"
#include "searchFunc.cpp"
#include "findProfileHeaders.cpp"
#include "decryptFile.cpp"
#include "inflateFile.cpp"
#include "information.cpp"
#include "jdvout.cpp"

std::string decryptFile(std::vector<uint_fast8_t>&, std::vector<uint_fast32_t>&, std::string&);

template <uint_fast8_t N>
uint_fast32_t searchFunc(std::vector<uint_fast8_t>&, uint_fast32_t, uint_fast8_t, const uint_fast8_t (&)[N]);

uint_fast32_t 
	getFourByteValue(const std::vector<uint_fast8_t>&, const uint_fast32_t),
	inflateFile(std::vector<uint_fast8_t>&);
void
	startJdv(const std::string&),
	findProfileHeaders(std::vector<uint_fast8_t>&, std::vector<uint_fast32_t>&, uint_fast16_t),
	displayInfo();
	
