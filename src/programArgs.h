#pragma once

#include <string>

enum class ArgMode {
	conceal,
	recover
};

enum class ArgOption {
	none,
	bluesky,
	reddit
};

struct ProgramArgs {
	ArgMode mode = ArgMode::conceal;
	ArgOption platform = ArgOption::none;
    	std::string image_file;
    	std::string data_file;

    	static ProgramArgs parse(int argc, char** argv);
};


