//	JPG Data Vehicle (jdvin v1.1.7) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023
//
//	To compile program (Linux):
// 	$ g++ main.cpp -O2 -lz -s -o jdvin

// 	Run it:
// 	$ ./jdvin

#include "jdvin.h"

int main(int argc, char** argv) {
	if (argc == 2 && std::string(argv[1]) == "--info") {
		displayInfo();
	} else if (argc > 2 && argc < 5) {
		bool 
			isFileCheckSuccess = false,
			isRedditOption = argc > 3 ? true : false;

		const std::regex REG_EXP("(\\.[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+)?[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+?(\\.[a-zA-Z0-9]+)?");

		const std::string IMAGE_FILENAME = isRedditOption ? argv[2] : argv[1];

		std::string 
			image_file_extension = IMAGE_FILENAME.length() > 3 ? IMAGE_FILENAME.substr(IMAGE_FILENAME.length() - 4) : IMAGE_FILENAME,
			data_filename = isRedditOption ? argv[3] : argv[2];

		image_file_extension = image_file_extension == "jpeg" || image_file_extension == "jfif" ? ".jpg" : image_file_extension;

		if (image_file_extension != ".jpg" || (argc > 3 && std::string(argv[1]) != "-r")) {
			std::cerr << (image_file_extension != ".jpg" 
				? "\nFile Type Error: Invalid file extension. Expecting only \"jpg\""
				: "\nInput Error: Invalid arguments. Expecting only -r") 
			<< ".\n\n";
		} else if (!regex_match(IMAGE_FILENAME, REG_EXP) || !regex_match(data_filename, REG_EXP)) {
			std::cerr << "\nInvalid Input Error: Characters not supported by this program found within filename arguments.\n\n";
		} else if (!std::filesystem::exists(IMAGE_FILENAME) || !std::filesystem::exists(data_filename) || !std::filesystem::is_regular_file(data_filename)) {
			std::cerr << (!std::filesystem::exists(IMAGE_FILENAME) 
				? "\nImage"
				: "\nData") 
			<< " File Error: File not found. Check the filename and try again.\n\n";
		} else {
			isFileCheckSuccess = true;
		}
		isFileCheckSuccess ? startJdv(IMAGE_FILENAME, data_filename, isRedditOption) : std::exit(EXIT_FAILURE);	  	
	} else {
		std::cout << "\nUsage: jdvin [-r] <cover_image> <data_file>\n\t\bjdvin --info\n\n";
	}
}
