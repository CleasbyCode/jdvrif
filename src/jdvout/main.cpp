//	JPG Data Vehicle (jdvout v1.2.2) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023
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

		constexpr const char* REG_EXP = ("(\\.[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+)?[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+?(\\.[a-zA-Z0-9]+)?");
		const std::regex regex_pattern(REG_EXP);

		std::filesystem::path image_path(IMAGE_FILENAME);
		std::string image_extension = image_path.extension().string();

		image_extension = image_extension == ".jpeg" || image_extension == ".jfif" ? ".jpg" : image_extension;

		if (image_extension == ".jpg" && regex_match(IMAGE_FILENAME, regex_pattern) && std::filesystem::exists(IMAGE_FILENAME)) {
			jdvOut(IMAGE_FILENAME);
		} else {
			std::cerr << (image_extension != ".jpg" 
				? "\nFile Type Error: Invalid file extension found. Expecting only \"jpg\""
				: !regex_match(IMAGE_FILENAME, regex_pattern)
					? "\nInvalid Input Error: Characters not supported by this program found within filename arguments"
						: "\nImage File Error: File not found. Check the filename and try again")
			 << ".\n\n";
			return 1;
		}
	} 
}
