//	JPG Data Vehicle (jdvin v1.0.2) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023
//
//	To compile program (Linux):
// 	$ g++ jdvrif.cpp -O2 -lz -s -o jdvrif

// 	Run it:
// 	$ ./jdvrif

#include "jdvin.h"

int main(int argc, char** argv) {

	bool isRedditOption = false;

	if (argc == 2 && std::string(argv[1]) == "--info") {
		displayInfo();
	}
	else if (argc > 2 && argc <= 4) {

		if (argc == 4 && (std::string(argv[1]) != "-r")) {
			std::cerr << "\nInput Error: Invalid arguments: -r only.\n\n";
			std::exit(EXIT_FAILURE);
		}
		else {
			isRedditOption = (argc == 4 && std::string(argv[1]) == "-r") ? true : false;

			std::string
				image_file_name = isRedditOption ? argv[2] : argv[1],
				data_file_name = isRedditOption ? argv[3] : argv[2];

			const std::regex REG_EXP("(\\.[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+)?[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+?(\\.[a-zA-Z0-9]+)?");

			std::string GET_IMAGE_FILE_EXTENSION = image_file_name.length() > 3 ? image_file_name.substr(image_file_name.length() - 4) : image_file_name;

			GET_IMAGE_FILE_EXTENSION = GET_IMAGE_FILE_EXTENSION == "jpeg" || GET_IMAGE_FILE_EXTENSION == "jiff" ? ".jpg" : GET_IMAGE_FILE_EXTENSION;

			if (GET_IMAGE_FILE_EXTENSION != ".jpg" || !regex_match(image_file_name, REG_EXP) || !regex_match(data_file_name, REG_EXP)) {
				std::cerr << (GET_IMAGE_FILE_EXTENSION != ".jpg" ?
					"\nFile Type Error: Invalid file extension found. Expecting only '.jpg'"
					: "\nInvalid Input Error: Characters not supported by this program found within file name arguments") << ".\n\n";
				std::exit(EXIT_FAILURE);
			}
			startJdv(image_file_name, data_file_name, isRedditOption);
		}
	}
	else {
		std::cout << "\nUsage: jdvin [-r] <cover_image> <data_file>\n\t\bjdvin --info\n\n";
	}
	return 0;	
}