#pragma once

#include <cstdint>
#include <limits>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <regex>
#include <iostream>
#include <string>
#include <vector>
#include <iterator>
#include <zlib.h>

#ifdef _WIN32
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

#include "getPin.cpp"
#include "getByteValue.cpp"
#include "crc32.cpp"
#include "searchFunc.cpp"
#include "valueUpdater.cpp"
#include "decryptFile.cpp"
#include "inflateFile.cpp"
#include "information.cpp"
#include "jdvout.cpp"

const std::string decryptFile(std::vector<uint8_t>&, std::vector<uint8_t>&);

template <uint8_t N>
uint32_t searchFunc(std::vector<uint8_t>&, uint32_t, uint8_t, const uint8_t (&SIG)[N]);

uint32_t 
	crcUpdate(uint8_t*, uint32_t),
	getByteValue(const std::vector<uint8_t>&, const uint32_t),
	getPin();

const uint32_t inflateFile(std::vector<uint8_t>&);

void 
	valueUpdater(std::vector<uint8_t>&, uint32_t, const uint32_t, uint8_t),
	displayInfo();

int jdvOut(const std::string&);	
