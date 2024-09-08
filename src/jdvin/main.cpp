//	JPG Data Vehicle (jdvin v1.2.2) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023
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
		
		constexpr const char* REG_EXP = ("(\\.[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+)?[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+?(\\.[a-zA-Z0-9]+)?");
		const std::regex regex_pattern(REG_EXP);

		const std::string IMAGE_FILENAME = isRedditOption ? argv[2] : argv[1];
		
		std::string data_filename = isRedditOption ? argv[3] : argv[2];
		
		std::filesystem::path image_path(IMAGE_FILENAME);
		std::string image_extension = image_path.extension().string();
		
		image_extension = image_extension == ".jpeg" || image_extension == ".jfif" ? ".jpg" : image_extension;

		if (image_extension != ".jpg" || (argc > 3 && std::string(argv[1]) != "-r")) {
			std::cerr << (image_extension != ".jpg" 
				? "\nFile Type Error: Invalid file extension. Expecting only \"jpg\""
				: "\nInput Error: Invalid arguments. Expecting only -r") 
			<< ".\n\n";
		} else if (!regex_match(IMAGE_FILENAME, regex_pattern) || !regex_match(data_filename, regex_pattern)) {
			std::cerr << "\nInvalid Input Error: Characters not supported by this program found within filename arguments.\n\n";
		} else if (!std::filesystem::exists(IMAGE_FILENAME) || !std::filesystem::exists(data_filename) || !std::filesystem::is_regular_file(data_filename)) {
			std::cerr << (!std::filesystem::exists(IMAGE_FILENAME) 
				? "\nImage"
				: "\nData") 
			<< " File Error: File not found. Check the filename and try again.\n\n";
		} else {
			isFileCheckSuccess = true;
		}
		if (isFileCheckSuccess) {
			jdvIn(IMAGE_FILENAME, data_filename, isRedditOption);
		} else {
			return 1;
		}
	} else {
		std::cout << "\nUsage: jdvin [-r] <cover_image> <data_file>\n\t\bjdvin --info\n\n";
	}
}
