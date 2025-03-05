enum class ArgOption {
	Default,
	Reddit
};

struct ProgramArgs {
	ArgOption platform = ArgOption::Default;
    	std::string image_file;
    	std::string data_file;

    	static ProgramArgs parse(int argc, char** argv);
};

ProgramArgs ProgramArgs::parse(int argc, char** argv) {
	ProgramArgs args;
	if (argc == 2 && std::string(argv[1]) == "--info") {
		displayInfo();
        	std::exit(0);
	}

	if (argc < 3 || argc > 4) {
        	throw std::runtime_error("Usage: jdvin [-r] <cover_image> <secret_file>\n\t\bjdvin --info");
    	}

    	uint8_t arg_index = 1;

    	if (argc == 4) {
		if (std::string(argv[arg_index]) != "-r") {
            		throw std::runtime_error("Input Error: Invalid arguments. Expecting \"-r\" as the only optional argument.");
        	}
        	args.platform = ArgOption::Reddit;
        	arg_index = 2;
    	}

    	args.image_file = argv[arg_index];
    	args.data_file = argv[++arg_index];
    	return args;
}
