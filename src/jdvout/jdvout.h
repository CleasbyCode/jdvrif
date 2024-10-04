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

std::string decryptFile(std::vector<uint8_t>&, const uint32_t*, const uint8_t*, const uint16_t, const std::string&);

template <uint8_t N>
uint32_t searchFunc(std::vector<uint8_t>&, uint32_t, uint8_t, const uint8_t (&SIG)[N]);

uint32_t getByteValue(const std::vector<uint8_t>&, const uint32_t);

void
	findProfileHeaders(std::vector<uint8_t>&, uint32_t*, const uint16_t),
	inflateFile(std::vector<uint8_t>&),
	displayInfo();

uint8_t jdvOut(const std::string&);	
