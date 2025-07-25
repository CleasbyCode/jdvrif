// 	JPG Data Vehicle(jdvout v4.4) Created by Nicholas Cleasby(@CleasbyCode) 10/04/2023

//	Compile program (Linux):

//	$ sudo apt-get install libsodium-dev

//	$ chmod +x compile_jdvout.sh
//	$ ./compile_jdvout.sh

//	$ Compilation successful. Executable 'jdvout' created.
//	$ sudo cp jdvout /usr/bin
//	$ jdvout

#include "fileChecks.h"
#include "programArgs.h" 
#include "jdvOut.h"
#include <iostream>
#include <stdexcept>

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
