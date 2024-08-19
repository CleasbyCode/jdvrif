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

std::string decryptFile(std::vector<uint8_t>&, std::vector<uint32_t>&, std::string&);

template <uint8_t N>
uint32_t searchFunc(std::vector<uint8_t>&, uint32_t, uint8_t, const uint8_t (&)[N]);

uint32_t 
	getByteValue(const std::vector<uint8_t>&, const uint32_t),
	inflateFile(std::vector<uint8_t>&);
void
	findProfileHeaders(std::vector<uint8_t>&, std::vector<uint32_t>&, uint16_t),
	displayInfo();

int jdvOut(const std::string&);	
