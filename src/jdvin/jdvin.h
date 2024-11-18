#pragma once

#include <algorithm>
#include <filesystem>
#include <random>
#include <cstdint>
#include <fstream>
#include <regex>
#include <set>
#include <iostream>
#include <string>
#include <vector>
#include <iterator>
#include <zlib.h>

#include "profilesVec.cpp"
#include "writeFile.cpp"
#include "crc32.cpp"
#include "searchFunc.cpp"
#include "eraseSegments.cpp"
#include "valueUpdater.cpp"
#include "getByteValue.cpp"
#include "encryptFile.cpp"
#include "deflateFile.cpp"
#include "segmentDataFile.cpp"
#include "information.cpp"
#include "jdvin.cpp"

template <uint8_t N>
uint32_t searchFunc(std::vector<uint8_t>&, uint32_t, const uint8_t, const uint8_t (&)[N]);

uint32_t 
	crcUpdate(uint8_t*, uint32_t),
	encryptFile(std::vector<uint8_t>&, std::vector<uint8_t>&, uint32_t, std::string&),
	deflateFile(std::vector<uint8_t>&, bool),
	getByteValue(const std::vector<uint8_t>&, const uint32_t);

bool writeFile(std::vector<uint8_t>&);

void
	eraseSegments(std::vector<uint8_t>&),
	segmentDataFile(std::vector<uint8_t>&, std::vector<uint8_t>&),
	valueUpdater(std::vector<uint8_t>&, uint32_t, const uint32_t, uint8_t),
	displayInfo();

int jdvIn(const std::string&, std::string&, bool, bool);
