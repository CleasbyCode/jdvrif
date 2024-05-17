void startJdv(std::string& image_file_name) {

	std::ifstream image_ifs(image_file_name, std::ios::binary);

	image_ifs.seekg(0, image_ifs.end);
	const size_t TMP_IMAGE_FILE_SIZE = image_ifs.tellg();
	image_ifs.seekg(0, image_ifs.beg);
	
	constexpr uint_fast8_t MIN_FILE_SIZE = 134;

	constexpr uint_fast32_t 
		MAX_FILE_SIZE = 209715200, 
		MAX_FILE_SIZE_REDDIT = 20971520;

	if (TMP_IMAGE_FILE_SIZE > MAX_FILE_SIZE || MIN_FILE_SIZE > TMP_IMAGE_FILE_SIZE) {
		std::cerr << "\nImage File Error: " << (MIN_FILE_SIZE > TMP_IMAGE_FILE_SIZE ? 
				  "Size of image is too small to be a valid jdvrif file-embedded image." 
				: "Size of image exceeds the maximum limit of a jdvrif file-embedded image.") << "\n\n";
		std::exit(EXIT_FAILURE);
	}

	std::vector<uint_fast8_t>Image_Vec((std::istreambuf_iterator<char>(image_ifs)), std::istreambuf_iterator<char>());

	uint_fast32_t image_file_size = static_cast<uint_fast32_t>(Image_Vec.size());

	const std::string
		JPG_START_SIG = "\xFF\xD8\xFF",
		JPG_END_SIG =	"\xFF\xD9",
		PROFILE_SIG =	"mntrRGB";

	const std::string
		GET_IMAGE_START_SIG{ Image_Vec.begin(), Image_Vec.begin() + JPG_START_SIG.length() },
		GET_IMAGE_END_SIG{ Image_Vec.end() - JPG_END_SIG.length(), Image_Vec.end() };

	constexpr uint_fast8_t JDV_SIG[6] = {0xB4, 0x6A, 0x3E, 0xEA, 0x5E, 0x90};

	const uint_fast32_t
		JDV_SIG_INDEX = static_cast<uint_fast32_t>(std::search(Image_Vec.begin(), Image_Vec.end(), &JDV_SIG[0], &JDV_SIG[6]) - Image_Vec.begin()),
		PROFILE_SIG_INDEX = static_cast<uint_fast32_t>(std::search(Image_Vec.begin(), Image_Vec.end(), PROFILE_SIG.begin(), PROFILE_SIG.end()) - Image_Vec.begin());
	
	if (GET_IMAGE_START_SIG != JPG_START_SIG || GET_IMAGE_END_SIG != JPG_END_SIG || JDV_SIG_INDEX == image_file_size || PROFILE_SIG_INDEX == image_file_size) {
		std::cerr << "\nImage File Error: This is not a valid jdvrif file-embedded image.\n\n";
		std::exit(EXIT_FAILURE);
	}

	Image_Vec.erase(Image_Vec.begin(), Image_Vec.begin() + (PROFILE_SIG_INDEX - 8));

	std::vector<uint_fast32_t> Profile_Headers_Offset_Vec;

	findProfileHeaders(Image_Vec, Profile_Headers_Offset_Vec, image_file_name);

	std::vector<uint_fast8_t>File_Vec;

	decryptFile(Image_Vec, File_Vec, Profile_Headers_Offset_Vec, image_file_name);

	std::reverse(File_Vec.begin(), File_Vec.end());

	std::ofstream file_ofs(image_file_name, std::ios::binary);

	if (!file_ofs) {
		std::cerr << "\nWrite Error: Unable to write to file.\n\n";
		std::exit(EXIT_FAILURE);
	}

	file_ofs.write((char*)&File_Vec[0], File_Vec.size());

	std::cout << "\nExtracted hidden file: " + image_file_name + '\x20' + std::to_string(File_Vec.size()) + " Bytes.\n\nComplete! Please check your file.\n\n";
}