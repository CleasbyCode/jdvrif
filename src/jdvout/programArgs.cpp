#include "programArgs.h"
#include "information.h" 
#include <stdexcept>
#include <cstdlib>

ProgramArgs ProgramArgs::parse(int argc, char** argv) {
	ProgramArgs args;

	if (argc != 2) {
        	throw std::runtime_error("Usage: jdvout <file_embedded_image>\n\t\bjdvout --info");
    	}

	if (std::string(argv[1]) == "--info") {
		displayInfo();
        	std::exit(0);
	}

    	args.image_file = argv[1];
    	return args;
}
