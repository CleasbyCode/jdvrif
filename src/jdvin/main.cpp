//	JPG Data Vehicle (jdvin v3.3) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023
//
//	Compile program (Linux):

//	$ sudo apt-get install libsodium-dev
//	$ g++ main.cpp -O2 -lz -lsodium -s -o jdvin
//	$ sudo cp jdvin /usr/bin

// 	Run it:
// 	$ jdvin

#include "jdvin.h"

int main(int argc, char** argv) {
	try {
		ProgramArgs args = ProgramArgs::parse(argc, argv);
		if (!hasValidFilename(args.imageFile) || !hasValidFilename(args.dataFile)) {
            		throw std::runtime_error("Invalid Input Error: Unsupported characters in filename arguments.");
        	}
        	validateFiles(args.imageFile, args.dataFile, args.platform);
        	std::filesystem::path dataPath(args.dataFile);

        	bool isCompressed = isCompressedFile(dataPath.extension().string());
        	jdvIn(args.imageFile, args.dataFile, args.platform, isCompressed);
    	}
	catch (const std::runtime_error& e) {
        	std::cerr << "\n" << e.what() << "\n\n";
        	return 1;
    	}
}