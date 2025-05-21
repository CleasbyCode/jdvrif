#pragma once

#include <string>

struct ProgramArgs {
    	std::string image_file;
    	static ProgramArgs parse(int argc, char** argv);
};
