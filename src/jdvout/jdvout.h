#pragma once

#include <cstdint>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <regex>
#include <iostream>
#include <string>
#include <vector>
#include <C:\Users\Nick\source\zlib-1.3.1\zlib.h>

#include "four_bytes.cpp"
#include "find_profile_headers.cpp"
#include "decrypt.cpp"
#include "inflate.cpp"
#include "information.cpp"
#include "jdvout.cpp"

std::string decryptFile(std::vector<uint_fast8_t>&, std::vector<uint_fast32_t>&, std::string&);

uint_fast32_t 
	getFourByteValue(const std::vector<uint_fast8_t>&, uint_fast32_t),
	inflateFile(std::vector<uint_fast8_t>&);
void
	startJdv(const std::string&),
	findProfileHeaders(std::vector<uint_fast8_t>&, std::vector<uint_fast32_t>&, uint_fast16_t),
	displayInfo();
	