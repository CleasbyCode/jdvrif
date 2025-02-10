//	JPG Data Vehicle (jdvin v3.2) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023
//
//	To compile program (Linux):

//	$ sudo apt-get install libsodium-dev
//	$ g++ main.cpp -O2 -lz -lsodium -s -o jdvin
//	$ sudo cp jdvin /usr/bin	

// 	Run it:
// 	$ jdvin

enum class ArgOption {
	Default,
	Reddit
};

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
   
	ArgOption platformOption = ArgOption::Default;
    	uint8_t argIndex = 1;

	if (argc == 4) {
		if (std::string(argv[1]) != "-r") {
         		std::cerr << "\nInput Error: Invalid arguments. Expecting \"-r\" as the only optional argument.\n\n";
         		return 1;
    	 	}
     	 platformOption = ArgOption::Reddit;
     	 argIndex = 2;
    	}
   
        const std::string IMAGE_FILENAME = argv[argIndex];
        std::string data_filename        = argv[++argIndex];

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
		std::cerr << "\nFile Type Error: Invalid file extension. Only expecting \".jpg, .jpeg or .jfif\" image extensions.\n\n";
        	return 1;
   	 }
	
    	if (!std::filesystem::exists(IMAGE_FILENAME) || !std::filesystem::exists(data_filename) || !std::filesystem::is_regular_file(data_filename)) {
		std::cerr << (!std::filesystem::exists(IMAGE_FILENAME)
			? "\nImage File Error: File not found."
			: "\nData File Error: File not found or not a regular file.")
            	<< " Check the filename and try again.\n\n";
        	return 1;
	}
	
	const std::set<std::string> COMPRESSED_FILE_EXTENSIONS = { ".zip", "jar", ".rar", ".7z", ".bz2", ".gz", ".xz", ".tar", ".lz", ".lz4", ".cab", ".rpm", ".deb", ".mp4", ".mp3", ".jpg", ".png", ".ogg", ".flac" };
	const bool isCompressedFile = COMPRESSED_FILE_EXTENSIONS.count(DATA_FILE_EXTENSION) > 0;
	
	jdvIn(IMAGE_FILENAME, data_filename, platformOption, isCompressedFile);
}
