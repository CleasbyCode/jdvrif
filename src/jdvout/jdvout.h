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

#include "getByteValue.cpp"
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
	getByteValue(const std::vector<uint_fast8_t>&, const uint_fast32_t),
	inflateFile(std::vector<uint_fast8_t>&);
void
	findProfileHeaders(std::vector<uint_fast8_t>&, std::vector<uint_fast32_t>&, uint_fast16_t),
	displayInfo();

uint_fast8_t jdvOut(const std::string&);	
