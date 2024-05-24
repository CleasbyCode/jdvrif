#pragma once

#include <cstdint>
#include <algorithm>
#include <fstream>
#include <regex>
#include <iostream>
#include <string>
#include <vector>
#include <C:\Users\Nick\source\zlib-1.3.1\zlib.h>


#include "find_profile_headers.cpp"
#include "decrypt.cpp"
#include "inflate.cpp"
#include "information.cpp"
#include "jdvout.cpp"

void	
	startJdv(std::string&),
	findProfileHeaders(std::vector<uint_fast8_t>&, std::vector<uint_fast32_t>&, std::string&),
	decryptFile(std::vector<uint_fast8_t>&, std::vector<uint_fast32_t>&, std::string&),
	inflateFile(std::vector<uint_fast8_t>&),
	displayInfo();
	