#pragma once

#include <cstdint>
#include <algorithm>
#include <fstream>
#include <regex>
#include <iostream>
#include <string>
#include <vector>

#include "find_profile_headers.cpp"
#include "decrypt.cpp"
#include "information.cpp"
#include "jdvout.cpp"

void	
	startJdv(std::string&),
	findProfileHeaders(std::vector<uint_fast8_t>&, std::vector<uint_fast32_t>&, std::string&),
	decryptFile(std::vector<uint_fast8_t>&, std::vector<uint_fast8_t>&, std::vector<uint_fast32_t>&, std::string&),
	displayInfo();
	