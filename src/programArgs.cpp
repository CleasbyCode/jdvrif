#include "programArgs.h"
#include "information.h"       
#include <stdexcept>       
#include <cstdlib> 

ProgramArgs ProgramArgs::parse(int argc, char** argv) {
	ProgramArgs args;
	if (argc == 2 && std::string(argv[1]) == "--info") {
		displayInfo();
        std::exit(0);
	}

	const std::string USAGE_MSG = "Usage: jdvrif conceal [-b|-r] <cover_image> <secret_file>\n\t\bjdvrif recover <cover_image>\n\t\bjdvrif --info";

	if (argc < 3 || argc > 5) {
    	throw std::runtime_error(USAGE_MSG);
    }

    int arg_index = 1;
    	
    if (std::string(argv[arg_index]) != "conceal" && std::string(argv[arg_index]) != "recover") {
    	throw std::runtime_error(USAGE_MSG);
    }
	
    if (argc == 3 && std::string(argv[arg_index]) == "conceal") {
    	throw std::runtime_error(USAGE_MSG);
	}
    
    if (argc > 3 && std::string(argv[arg_index]) == "recover") {
    	throw std::runtime_error(USAGE_MSG);
    }
    
    ++arg_index;
    
    if ((argc == 3 || argc == 4) && (std::string(argv[arg_index]) == "-b" || std::string(argv[arg_index]) == "-r")) {
    	throw std::runtime_error(USAGE_MSG);
    }
		
    if (argc == 5) {
		if (std::string(argv[arg_index]) != "-b" && std::string(argv[arg_index]) != "-r") {
    		throw std::runtime_error(USAGE_MSG);
    	}
		if (std::string(argv[arg_index]) == "-b") {
    		args.platform = ArgOption::bluesky;
		} else {
			args.platform = ArgOption::reddit;
		}
    	++arg_index;
    	args.cover_image = argv[arg_index];
    	args.data_file = argv[++arg_index];
    } else if (argc == 4) {
    	args.cover_image = argv[arg_index];
    	args.data_file = argv[++arg_index];
    } else {
    	args.cover_image = argv[arg_index];
    	args.mode = ArgMode::recover;
    }
    return args;
}
