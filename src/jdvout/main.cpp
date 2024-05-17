//	JPG Data Vehicle (jdvout v1.0.1) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023
//
//	To compile program (Linux):
// 	$ g++ main.cpp -O2 -s -o jdvout

// 	Run it:
// 	$ ./jdvout

#include "jdvout.h"

int main(int argc, char** argv) {

	if (argc == 2 && std::string(argv[1]) == "--info") {
		displayInfo();
	}
	else if (argc == 2) {

		std::string image_file_name = std::string(argv[1]);

		const std::regex REG_EXP("(\\.[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+)?[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+?(\\.[a-zA-Z0-9]+)?");

		std::string GET_IMAGE_FILE_EXTENSION = image_file_name.length() > 3 ? image_file_name.substr(image_file_name.length() - 4) : image_file_name;

		GET_IMAGE_FILE_EXTENSION = GET_IMAGE_FILE_EXTENSION == "jpeg" || GET_IMAGE_FILE_EXTENSION == "jiff" ? ".jpg" : GET_IMAGE_FILE_EXTENSION;

		if (GET_IMAGE_FILE_EXTENSION != ".jpg" || !regex_match(image_file_name, REG_EXP)) {
			std::cerr << (GET_IMAGE_FILE_EXTENSION != ".jpg" ?
				"\nFile Type Error: Invalid file extension found. Expecting only '.jpg'"
				: "\nInvalid Input Error: Characters not supported by this program found within file name arguments") << ".\n\n";
			std::exit(EXIT_FAILURE);
		}

		startJdv(image_file_name);
	}
	else {
		std::cout << "\nUsage: jdvout <file_embedded_image>\n\t\bjdvout --info\n\n";
	}
	return 0;
}
