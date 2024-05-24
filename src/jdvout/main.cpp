//	JPG Data Vehicle (jdvout v1.0.2) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023
//
//	To compile program (Linux):
// 	$ g++ main.cpp -O2 -lz -s -o jdvout

// 	Run it:
// 	$ ./jdvout

#include "jdvout.h"

int main(int argc, char** argv) {

	if (argc == 2 && std::string(argv[1]) == "--info") {
		displayInfo();

	}
	else if (argc == 2) {

		std::string file_name = std::string(argv[1]);

		const std::regex REG_EXP("(\\.[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+)?[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+?(\\.[a-zA-Z0-9]+)?");

		std::string file_extension = file_name.length() > 3 ? file_name.substr(file_name.length() - 4) : file_name;

		file_extension = file_extension == "jpeg" || file_extension == "jiff" ? ".jpg" : file_extension;

		if (file_extension != ".jpg" || !regex_match(file_name, REG_EXP)) {
			std::cerr << (file_extension != ".jpg" ?
				"\nFile Type Error: Invalid file extension found. Expecting only '.jpg'"
				: "\nInvalid Input Error: Characters not supported by this program found within file name arguments") << ".\n\n";
			std::exit(EXIT_FAILURE);
		}

		startJdv(file_name);
	
	}
	else {
		std::cout << "\nUsage: jdvout <file_embedded_image>\n\t\bjdvout --info\n\n";
	}
	return 0;
}