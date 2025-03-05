
// JPG Data Vehicle(jdvout v3.3) Created by Nicholas Cleasby(@CleasbyCode) 10/04/2023
//
//	Compile program (Linux):

//	$ sudo apt-get install libsodium-dev
//	$ g++ main.cpp -O2 -lz -lsodium -s -o jdvout
//	$ sudo cp jdvout /usr/bin

// 	Run it:
// 	$ jdvout

#include "jdvout.h"

int main(int argc, char** argv) {
    try {
        ProgramArgs args = ProgramArgs::parse(argc, argv);
        if (!hasValidFilename(args.image_file)) {
            throw std::runtime_error("Invalid Input Error: Unsupported characters in filename arguments.");
        }
        validateFiles(args.image_file);
        jdvOut(args.image_file);
    }
    catch (const std::runtime_error& e) {
        std::cerr << "\n" << e.what() << "\n\n";
        return 1;
    }
}
