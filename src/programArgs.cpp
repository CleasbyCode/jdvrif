#include "programArgs.h"
#include "information.h"

#include <stdexcept>
#include <cstdlib>
#include <string_view>
#include <filesystem>

ProgramArgs ProgramArgs::parse(int argc, char** argv) {
	using std::string_view;

    auto arg = [&](int i) -> string_view {
    	return (i >= 0 && i < argc) ? string_view(argv[i]) : string_view{};
    };

    const std::string prog = std::filesystem::path(argv[0]).filename().string();
    const std::string USAGE =
        "Usage: " + prog + " conceal [-b|-r] <cover_image> <secret_file>\n\t\b"
        + prog + " recover <cover_image>\n\t\b"
        + prog + " --info";

    auto die = [&]() -> void {
    	throw std::runtime_error(USAGE);
    };

    if (argc < 2) die();

    if (argc == 2 && arg(1) == "--info") {
        displayInfo();
        std::exit(0);
    }

    ProgramArgs out{}; 

    const string_view cmd = arg(1);

    if (cmd == "conceal") {
    	int i = 2;

        if (arg(i) == "-b" || arg(i) == "-r") {
        	out.platform = (arg(i) == "-b") ? ArgOption::bluesky : ArgOption::reddit;
        	++i;
        }

        if (i + 1 >= argc || (i + 2) != argc) die();

        out.cover_image = std::string(arg(i));
        out.data_file   = std::string(arg(i + 1));
        out.mode        = ArgMode::conceal;
        return out;
    }

    if (cmd == "recover") {
        if (argc != 3) die();
        out.cover_image = std::string(arg(2));
        out.mode        = ArgMode::recover;
        return out;
    }
    die();
    
    return out;
}
