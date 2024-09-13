//	JPG Data Vehicle (jdvout v1.2.3) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023
//
//	To compile program (Linux):
// 	$ g++ main.cpp -O2 -lz -s -o jdvout

// 	Run it:
// 	$ ./jdvout

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

    std::filesystem::path image_path(IMAGE_FILENAME);
    std::string image_extension = image_path.extension().string();

    image_extension = image_extension == ".jpeg" || image_extension == ".jfif" ? ".jpg" : image_extension;

    if (image_extension != ".jpg") {
        std::cerr << "\nFile Type Error: Invalid file extension. Expecting only \".jpg\" image extension.\n\n";
        return 1;
    }

    constexpr const char* REG_EXP = ("(\\.[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+)?[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+?(\\.[a-zA-Z0-9]+)?");
    const std::regex regex_pattern(REG_EXP);

    if (!std::regex_match(IMAGE_FILENAME, regex_pattern)) {
        std::cerr << "\nInvalid Input Error: Characters not supported by this program found within filename arguments.\n\n";
        return 1;
    }

    if (!std::filesystem::exists(IMAGE_FILENAME) || !std::filesystem::is_regular_file(IMAGE_FILENAME)) {
        std::cerr << "\nImage File Error: File not found or not a regular file. Check the filename and try again.\n\n";
        return 1;
    }
    jdvOut(IMAGE_FILENAME);
}
