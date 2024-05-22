
void startJdv(std::string& image_file_name) {

	std::ifstream image_ifs(image_file_name, std::ios::binary);

	image_ifs.seekg(0, image_ifs.end);
	const size_t TMP_IMAGE_FILE_SIZE = image_ifs.tellg();
	image_ifs.seekg(0, image_ifs.beg);
	
	constexpr uint_fast32_t MAX_FILE_SIZE = 209715200;

	if (TMP_IMAGE_FILE_SIZE > MAX_FILE_SIZE) {
		std::cerr << "\nImage File Error: Size of file exceeds the maximum limit for this program.\n\n";
		std::exit(EXIT_FAILURE);
	}

	std::vector<uint_fast8_t>Image_Vec((std::istreambuf_iterator<char>(image_ifs)), std::istreambuf_iterator<char>());

	uint_fast32_t image_file_size = static_cast<uint_fast32_t>(Image_Vec.size());

	constexpr uint_fast8_t 
		JDV_SIG[6] 	{ 0xB4, 0x6A, 0x3E, 0xEA, 0x5E, 0x90 },
		PROFILE_SIG[7]	{ 0x6D, 0x6E, 0x74, 0x72, 0x52, 0x47, 0x42 };

	const uint_fast32_t 
		JDV_SIG_INDEX = static_cast<uint_fast32_t>(std::search(Image_Vec.begin(), Image_Vec.end(), std::begin(JDV_SIG), std::end(JDV_SIG)) - Image_Vec.begin()),
		PROFILE_SIG_INDEX = static_cast<uint_fast32_t>(std::search(Image_Vec.begin(), Image_Vec.end(), std::begin(PROFILE_SIG), std::end(PROFILE_SIG)) - Image_Vec.begin());	

	if (JDV_SIG_INDEX == image_file_size) {
		std::cerr << "\nImage File Error: Signature check failure. This is not a valid jdvrif file-embedded image.\n\n";
		std::exit(EXIT_FAILURE);
	}

	Image_Vec.erase(Image_Vec.begin(), Image_Vec.begin() + (PROFILE_SIG_INDEX - 8));

	std::vector<uint_fast32_t> Profile_Headers_Offset_Vec;
	Profile_Headers_Offset_Vec.reserve(4000);

	findProfileHeaders(Image_Vec, Profile_Headers_Offset_Vec, image_file_name);

	std::vector<uint_fast8_t>File_Vec;
	File_Vec.reserve(image_file_size);

	decryptFile(Image_Vec, File_Vec, Profile_Headers_Offset_Vec, image_file_name);

	inflateFile(File_Vec);

	std::reverse(File_Vec.begin(), File_Vec.end());

	std::ofstream file_ofs(image_file_name, std::ios::binary);

	if (!file_ofs) {
		std::cerr << "\nWrite Error: Unable to write to file.\n\n";
		std::exit(EXIT_FAILURE);
	}

	file_ofs.write((char*)&File_Vec[0], File_Vec.size());

	std::cout << "\nExtracted hidden file: " + image_file_name + '\x20' + std::to_string(File_Vec.size()) + " Bytes.\n\nComplete! Please check your file.\n\n";
}
