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
#include "writeOutFile.cpp"
#include "searchFunc.cpp"
#include "eraseSegments.cpp"
#include "valueUpdater.cpp"
#include "encryptFile.cpp"
#include "deflateFile.cpp"
#include "insertProfileHeaders.cpp"
#include "information.cpp"
#include "jdvin.cpp"

template <uint8_t N>
uint32_t searchFunc(std::vector<uint8_t>&, uint32_t, uint8_t, const uint8_t (&)[N]);

void
	startJdv(const std::string&, std::string&, bool),
	eraseSegments(std::vector<uint8_t>&, bool&),
	deflateFile(std::vector<uint8_t>&, const std::string),
	encryptFile(std::vector<uint8_t>&, std::vector<uint8_t>&, std::string&),
	insertProfileHeaders(std::vector<uint8_t>&, std::vector<uint8_t>&),
	writeOutFile(std::vector<uint8_t>&),
	valueUpdater(std::vector<uint8_t>&, uint32_t, const uint32_t, uint8_t),
	displayInfo();
