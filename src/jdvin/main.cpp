//	JPG Data Vehicle (jdvin v4.3) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023

//	Compile program (Linux):

//	$ sudo apt-get install libsodium-dev
//	$ sudo apt-get install libturbojpeg-dev

//	$ chmod +x compile_jdvin.sh
//	$ ./compile_jdvin.sh
	
//	$ Compilation successful. Executable 'jdvin' created.
//	$ sudo cp jdvin /usr/bin
//	$ jdvin

#include "fileChecks.h" 
#include "jdvIn.h"     

#include <filesystem>
#include <iostream>         
#include <stdexcept>      

int main(int argc, char** argv) {
	try {
		ProgramArgs args = ProgramArgs::parse(argc, argv);
		if (!hasValidFilename(args.image_file) || !hasValidFilename(args.data_file)) {
            		throw std::runtime_error("Invalid Input Error: Unsupported characters in filename arguments.");
        	}
        	validateFiles(args.image_file, args.data_file, args.platform);
        	std::filesystem::path data_path(args.data_file);

        	bool isCompressed = isCompressedFile(data_path.extension().string());
        	jdvIn(args.image_file, args.data_file, args.platform, isCompressed);
    	}
	catch (const std::runtime_error& e) {
        	std::cerr << "\n" << e.what() << "\n\n";
        	return 1;
    	}
}
