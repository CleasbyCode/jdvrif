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

#define SODIUM_STATIC
#include <C:\Users\Nick\source\repos\jdvout\libsodium\include\sodium.h>

// https://github.com/madler/zlib
#include <C:\Users\Nick\source\zlib-1.3.1\zlib.h>
// Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler

#ifdef _WIN32
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

#include "getPin.cpp"
#include "getByteValue.cpp"
#include "searchFunc.cpp"
#include "valueUpdater.cpp"
#include "decryptFile.cpp"
#include "inflateFile.cpp"
#include "information.cpp"
#include "jdvout.cpp"

const std::string decryptFile(std::vector<uint8_t>&);

template <uint8_t N>
uint32_t searchFunc(std::vector<uint8_t>&, uint32_t, uint8_t, const uint8_t (&SIG)[N]);

template <typename T>
T getByteValue(const std::vector<uint8_t>&, uint32_t);

uint64_t getPin();

const uint32_t inflateFile(std::vector<uint8_t>&);

void 
	valueUpdater(std::vector<uint8_t>&, uint32_t, const uint64_t, uint8_t),
	displayInfo();

int jdvOut(const std::string&);	
