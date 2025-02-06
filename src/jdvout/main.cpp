//	JPG Data Vehicle (jdvout v3.2) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023
//
//	To compile program (Linux):

// 	$ sudo apt-get install libsodium-dev
//	$ g++ main.cpp -O2 -lz -lsodium -s -o jdvout
//	$ sudo cp jdvout /usr/bin

// 	Run it:
// 	$ jdvout

#include "jdvout.h"

int main(int argc, char** argv) {
	if (argc != 2) {
		std::cout << "\nUsage: jdvout <file_embedded_image>\n\t\bjdvout --info\n\n";
        	return 1;
    	}
	
	if (std::string(argv[1]) == "--info") {
		displayInfo();
        	return 0;
    	}

    const std::string IMAGE_FILENAME = std::string(argv[1]);

    constexpr const char* REG_EXP = ("(\\.[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+)?[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+?(\\.[a-zA-Z0-9]+)?");
    const std::regex regex_pattern(REG_EXP);

    if (!std::regex_match(IMAGE_FILENAME, regex_pattern)) {
		std::cerr << "\nInvalid Input Error: Characters not supported by this program found within filename arguments.\n\n";
        	return 1;
    	}
	
    const std::filesystem::path IMAGE_PATH(IMAGE_FILENAME);
    const std::string IMAGE_EXTENSION = IMAGE_PATH.extension().string();

    if (IMAGE_EXTENSION != ".jpg" && IMAGE_EXTENSION != ".jpeg" && IMAGE_EXTENSION != ".jfif") {
	std::cerr << "\nFile Type Error: Invalid file extension. Only expecting \"jpg, jpeg or jfif\" image extensions.\n\n";
        return 1;
    }

    	if (!std::filesystem::exists(IMAGE_FILENAME)) {
		std::cerr << "\nImage File Error: File not found. Check the filename and try again.\n\n";
        	return 1;
    	}
	jdvOut(IMAGE_FILENAME);
}
