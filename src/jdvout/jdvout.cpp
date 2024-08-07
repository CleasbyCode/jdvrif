void startJdv(const std::string& IMAGE_FILENAME) {

	const size_t TMP_IMAGE_FILE_SIZE = std::filesystem::file_size(IMAGE_FILENAME);
	
	constexpr uint32_t 
		MAX_FILE_SIZE = 2147483648,
		LARGE_FILE_SIZE = 104857600;

	std::ifstream image_ifs(IMAGE_FILENAME, std::ios::binary);

	if (!image_ifs || TMP_IMAGE_FILE_SIZE > MAX_FILE_SIZE) {
		std::cerr << (!image_ifs 
			? "\nOpen File Error: Unable to read image file"
			: "\nImage File Error: Size of file exceeds the maximum limit for this program")
		<< ".\n\n";

		std::exit(EXIT_FAILURE);
	}

	std::cout << (TMP_IMAGE_FILE_SIZE > LARGE_FILE_SIZE ? "\nPlease wait. Larger files will take longer to process.\n" : "");

	std::vector<uint8_t>Image_Vec((std::istreambuf_iterator<char>(image_ifs)), std::istreambuf_iterator<char>());
	
	const uint32_t IMAGE_FILE_SIZE = static_cast<uint32_t>(Image_Vec.size());

	constexpr uint8_t 
		JDV_SIG[] 	{ 0xB4, 0x6A, 0x3E, 0xEA, 0x5E, 0x90 },
		PROFILE_SIG[]	{ 0x6D, 0x6E, 0x74, 0x72, 0x52, 0x47, 0x42 },	
		PROFILE_COUNT_VALUE_INDEX = 0x60,	
		FILE_SIZE_INDEX = 0x66,
		ENCRYPTED_FILENAME_INDEX = 0x27,
		XOR_KEY_LENGTH = 12;

	constexpr uint16_t FILE_START_INDEX = 0x26D;

	const uint32_t 
		JDV_SIG_INDEX = searchFunc(Image_Vec, 0, 0, JDV_SIG),
		PROFILE_SIG_INDEX = searchFunc(Image_Vec, 0, 0, PROFILE_SIG);
		
	if (JDV_SIG_INDEX == IMAGE_FILE_SIZE) {
		std::cerr << "\nImage File Error: Signature check failure. This is not a valid jdvrif file-embedded image.\n\n";
		std::exit(EXIT_FAILURE);
	}

	Image_Vec.erase(Image_Vec.begin(), Image_Vec.begin() + (PROFILE_SIG_INDEX - 8));

	const uint32_t EMBEDDED_FILE_SIZE = getFourByteValue(Image_Vec, FILE_SIZE_INDEX);

	uint16_t 
		xor_key_index = 0x236,
		profile_count = (static_cast<uint16_t>(Image_Vec[PROFILE_COUNT_VALUE_INDEX]) << 8) | 
				static_cast<uint16_t>(Image_Vec[PROFILE_COUNT_VALUE_INDEX + 1]);

	std::string encrypted_data_filename = { Image_Vec.begin() + ENCRYPTED_FILENAME_INDEX, Image_Vec.begin() + ENCRYPTED_FILENAME_INDEX + Image_Vec[ENCRYPTED_FILENAME_INDEX - 1] };

	for (int i = 0; i < XOR_KEY_LENGTH; ++i) {
		encrypted_data_filename += Image_Vec[xor_key_index++]; 
	}

	Image_Vec.erase(Image_Vec.begin(), Image_Vec.begin() + FILE_START_INDEX);

	std::vector<uint32_t> Profile_Headers_Offset_Vec;
	
	if (profile_count) {
		findProfileHeaders(Image_Vec, Profile_Headers_Offset_Vec, profile_count);
	}

	Image_Vec.erase(Image_Vec.begin() + EMBEDDED_FILE_SIZE, Image_Vec.end());

	std::string decrypted_data_filename = decryptFile(Image_Vec, Profile_Headers_Offset_Vec, encrypted_data_filename);
	
	uint32_t inflated_file_size = inflateFile(Image_Vec);

	std::reverse(Image_Vec.begin(), Image_Vec.end());
	
	std::ofstream file_ofs(decrypted_data_filename, std::ios::binary);

	if (!file_ofs) {
		std::cerr << "\nWrite Error: Unable to write to file.\n\n";
		std::exit(EXIT_FAILURE);
	}

	file_ofs.write((char*)&Image_Vec[0], inflated_file_size);

	std::cout << "\nExtracted hidden file: " + decrypted_data_filename + '\x20' + std::to_string(inflated_file_size) + " Bytes.\n\nComplete! Please check your file.\n\n";
}
