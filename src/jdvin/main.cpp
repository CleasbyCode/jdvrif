//	JPG Data Vehicle (jdvin v1.9) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023
//
//	To compile program (Linux):
// 	$ g++ main.cpp -O2 -lz -s -o jdvin
//	$ sudo cp jdvin /usr/bin	

// 	Run it:
// 	$ jdvin

#include "jdvin.h"

int main(int argc, char** argv) {
	if (argc == 2 && std::string(argv[1]) == "--info") {
        	displayInfo();
        	return 0;
    	}

    	if (argc < 3 || argc > 4) {
		std::cout << "\nUsage: jdvin [-r] <cover_image> <data_file>\n\t\bjdvin --info\n\n";
        	return 1;
    	}
   
    	const bool 
            isRedditOption  =  (argc > 3 && std::string(argv[1]) == "-r"),
            isInvalidOption =  (argc > 3 && !isRedditOption);

    	if (isInvalidOption) {
		std::cerr << "\nInput Error: Invalid arguments. Expecting only \"-r\" as the first optional argument.\n\n";
		return 1;
	}
	
        const std::string IMAGE_FILENAME = isRedditOption ? argv[2] : argv[1];
        std::string data_filename        = isRedditOption ? argv[3] : argv[2];

        constexpr const char* REG_EXP = ("(\\.[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+)?[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+?(\\.[a-zA-Z0-9]+)?");
    	const std::regex regex_pattern(REG_EXP);

    	if (!std::regex_match(IMAGE_FILENAME, regex_pattern) || !std::regex_match(data_filename, regex_pattern)) {
		std::cerr << "\nInvalid Input Error: Characters not supported by this program found within filename arguments.\n\n";
        	return 1;
    	}

    	const std::filesystem::path
		IMAGE_PATH(IMAGE_FILENAME),
		DATA_FILE_PATH(data_filename);
		 
        const std::string 
		IMAGE_EXTENSION = IMAGE_PATH.extension().string(),
		DATA_FILE_EXTENSION = DATA_FILE_PATH.extension().string();

    	if (IMAGE_EXTENSION != ".jpg" && IMAGE_EXTENSION != ".jpeg" && IMAGE_EXTENSION != ".jfif") {
		std::cerr << "\nFile Type Error: Invalid file extension. Expecting only \"jpg, jpeg or jfif\" image extensions.\n\n";
        	return 1;
	}
	
    	if (!std::filesystem::exists(IMAGE_FILENAME) || !std::filesystem::exists(data_filename) || !std::filesystem::is_regular_file(data_filename)) {
		std::cerr << (!std::filesystem::exists(IMAGE_FILENAME)
			? "\nImage File Error: File not found."
			: "\nData File Error: File not found or not a regular file.")
            	<< " Check the filename and try again.\n\n";
        	return 1;
	}

	const std::set<std::string> COMPRESSED_FILE_EXTENSIONS = { ".zip", ".rar", ".7z", ".bz2", ".gz", ".xz", ".mp4", ".flac" };
	const bool isCompressedFile = COMPRESSED_FILE_EXTENSIONS.count(DATA_FILE_EXTENSION) > 0;

	jdvIn(IMAGE_FILENAME, data_filename, isRedditOption, isCompressedFile);
}