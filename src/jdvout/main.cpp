//	JPG Data Vehicle (jdvout v1.1.8) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023
//
//	To compile program (Linux):
// 	$ g++ main.cpp -O2 -lz -s -o jdvout

// 	Run it:
// 	$ ./jdvout

#include "jdvout.h"

int main(int argc, char** argv) {
	if (argc !=2) {
		std::cout << "\nUsage: jdvout <file_embedded_image>\n\t\bjdvout --info\n\n";
	} else if (std::string(argv[1]) == "--info") {
		displayInfo();
	} else {
		const std::string IMAGE_FILENAME = std::string(argv[1]);

		const std::regex REG_EXP("(\\.[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+)?[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+?(\\.[a-zA-Z0-9]+)?");

		std::string file_extension = IMAGE_FILENAME.length() > 3 ? IMAGE_FILENAME.substr(IMAGE_FILENAME.length() - 4) : IMAGE_FILENAME;

		file_extension = file_extension == "jpeg" || file_extension == "jfif" ? ".jpg" : file_extension;

		if (file_extension == ".jpg" && regex_match(IMAGE_FILENAME, REG_EXP) && std::filesystem::exists(IMAGE_FILENAME)) {
			jdvOut(IMAGE_FILENAME);
		} else {
			std::cerr << (file_extension != ".jpg" 
				? "\nFile Type Error: Invalid file extension found. Expecting only \"jpg\""
				: !regex_match(IMAGE_FILENAME, REG_EXP)
					? "\nInvalid Input Error: Characters not supported by this program found within filename arguments"
						: "\nImage File Error: File not found. Check the filename and try again")
			 << ".\n\n";
			return 1;
		}
	} 
}
