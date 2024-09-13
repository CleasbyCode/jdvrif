//	JPG Data Vehicle (jdvin v1.2.3) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023
//
//	To compile program (Linux):
// 	$ g++ main.cpp -O2 -lz -s -o jdvin

// 	Run it:
// 	$ ./jdvin

#include "jdvin.h"

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--info") {
        displayInfo();
    }

    if (argc < 3 || argc > 4) {
        std::cout << "\nUsage: jdvin [-r] <cover_image> <data_file>\n\t\bjdvin --info\n\n";
        return 1;
    }
   
    const bool isRedditOption = (argc > 3);

    const std::string IMAGE_FILENAME = isRedditOption ? argv[2] : argv[1];
    std::string data_filename = isRedditOption ? argv[3] : argv[2];

    const std::filesystem::path image_path(IMAGE_FILENAME);
    std::string image_extension = image_path.extension().string();

    image_extension = image_extension == ".jpeg" || image_extension == ".jfif" ? ".jpg" : image_extension;

    if (argc > 3 && std::string(argv[1]) != "-r") {
        std::cerr << "\nInput Error: Invalid arguments. Expecting only \"-r\" as the first argument option.\n\n";
        return 1;
    }

    if (image_extension != ".jpg") {
        std::cerr << "\nFile Type Error: Invalid file extension. Expecting only \".jpg\" image extension.\n\n";
        return 1;
    }

    constexpr const char* REG_EXP = ("(\\.[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+)?[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+?(\\.[a-zA-Z0-9]+)?");
    const std::regex regex_pattern(REG_EXP);

    if (!std::regex_match(IMAGE_FILENAME, regex_pattern) || !std::regex_match(data_filename, regex_pattern)) {
        std::cerr << "\nInvalid Input Error: Characters not supported by this program found within filename arguments.\n\n";
        return 1;
    }

    if (!std::filesystem::exists(IMAGE_FILENAME)) {
        std::cerr << "\nImage File Error: File not found. Check the filename and try again.\n\n";
        return 1;
    }

    if (!std::filesystem::exists(data_filename) || !std::filesystem::is_regular_file(data_filename)) {
        std::cerr << "\nData File Error: File not found or not a regular file. Check the filename and try again.\n\n";
        return 1;
    }
    jdvIn(IMAGE_FILENAME, data_filename, isRedditOption);
}
