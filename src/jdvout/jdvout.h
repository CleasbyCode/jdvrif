#pragma once

#include <cstdint>
#include <array>
#include <limits>
#include <set>
#include <filesystem>
#include <algorithm>
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

#ifdef _WIN32
	#include <conio.h>
#else
	#include <termios.h>
	#include <unistd.h>
#endif

#include "getPin.cpp"
#include "fromBase64.cpp"
#include "information.cpp"
#include "programArgs.cpp"
#include "fileChecks.cpp"
#include "getByteValue.cpp"
#include "searchFunc.cpp"
#include "valueUpdater.cpp"
#include "decryptFile.cpp"
#include "inflateFile.cpp"
#include "jdvout.cpp"

const std::string decryptFile(std::vector<uint8_t>&, bool);

template <typename T, size_t N>
uint32_t searchFunc(std::vector<uint8_t>&, uint32_t, const uint8_t, const std::array<T, N>&);

template <typename T>
T getByteValue(const std::vector<uint8_t>&, uint32_t);

uint64_t getPin();

const uint32_t inflateFile(std::vector<uint8_t>&);

void 
	validateFiles(const std::string&),
	convertFromBase64(std::vector<uint8_t>&),
	valueUpdater(std::vector<uint8_t>&, uint32_t, const uint64_t, uint8_t),
	displayInfo();

bool hasValidFilename(const std::string&);

uint8_t jdvOut(const std::string&);
