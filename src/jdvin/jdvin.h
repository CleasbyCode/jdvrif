#pragma once

#include <algorithm>
#include <filesystem>
#include <set>
#include <random>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <iterator>

// This project uses libsodium (https://libsodium.org/) for cryptographic functions.
#define SODIUM_STATIC
#include <sodium.h>
// Copyright (c) 2013-2025 Frank Denis <github@pureftpd.org>

// https://github.com/madler/zlib
#include <zlib.h>
// Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler

#include "profilesVec.cpp"
#include "information.cpp"
#include "programArgs.cpp"
#include "fileChecks.cpp"
#include "writeFile.cpp"
#include "searchFunc.cpp"
#include "eraseSegments.cpp"
#include "valueUpdater.cpp"
#include "getByteValue.cpp"
#include "encryptFile.cpp"
#include "deflateFile.cpp"
#include "segmentDataFile.cpp"
#include "jdvin.cpp"

template <uint8_t N>
uint32_t searchFunc(std::vector<uint8_t>&, uint32_t, const uint8_t, const uint8_t (&)[N]);

template <typename T>
T getByteValue(const std::vector<uint8_t>&, uint32_t);

uint64_t encryptFile(std::vector<uint8_t>&, std::vector<uint8_t>&, std::string&);

bool 
	writeFile(std::vector<uint8_t>&),
	segmentDataFile(std::vector<uint8_t>&, std::vector<uint8_t>&);

void
	eraseSegments(std::vector<uint8_t>&),
	deflateFile(std::vector<uint8_t>&, bool),
	valueUpdater(std::vector<uint8_t>&, uint32_t, const uint64_t, uint8_t),
	displayInfo();

int jdvIn(const std::string&, std::string&, ArgOption, bool);
bool isCompressedFile(const std::string&);
bool hasValidFilename(const std::string&);
void validateFiles(const std::string&, const std::string&, ArgOption);
