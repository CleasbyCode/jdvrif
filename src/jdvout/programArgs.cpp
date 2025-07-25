#include "programArgs.h"
#include <iostream>
#include <stdexcept>
#include <cstdlib>

ProgramArgs ProgramArgs::parse(int argc, char** argv) {
	ProgramArgs args;

	if (argc != 2) {
        	throw std::runtime_error("Usage: jdvout <file_embedded_image>\n\t\bjdvout --info");
    	}

	if (std::string(argv[1]) == "--info") {
		std::cout << R"(
 JPG Data Vehicle (jdvout v4.4) 
 Created by Nicholas Cleasby (@CleasbyCode) 24/01/2023.

 jdvout is a CLI tool for extracting hidden data from a jdvin "file-embedded" JPG image. 

 Compile & run jdvout (Linux):
		
 $ sudo apt-get install libsodium-dev

 $ chmod +x compile_jdvout.sh
 $ ./compile_jdvout.sh

 $ Compilation successful. Executable 'jdvout' created.

 $ sudo cp jdvout /usr/bin
 $ jdvout

 Usage: jdvout <file-embedded-image>
 jdvout --info

)";
        	std::exit(0);
	}

    	args.image_file = argv[1];
    	return args;
}
