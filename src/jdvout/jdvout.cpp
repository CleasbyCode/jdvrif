uint_fast8_t jdvOut(const std::string& IMAGE_FILENAME) {

	constexpr uint_fast32_t 
		MAX_FILE_SIZE 	= 2684354560, // 2.5GB
		LARGE_FILE_SIZE = 419430400;  // 400MB

	const size_t TMP_IMAGE_FILE_SIZE = std::filesystem::file_size(IMAGE_FILENAME);
	
	std::ifstream image_ifs(IMAGE_FILENAME, std::ios::binary);

	if (!image_ifs || TMP_IMAGE_FILE_SIZE > MAX_FILE_SIZE) {
		std::cerr << (!image_ifs 
			? "\nOpen File Error: Unable to read image file"
			: "\nImage File Error: Size of file exceeds the maximum limit for this program")
		<< ".\n\n";
		return 1;
	}

	if (TMP_IMAGE_FILE_SIZE > LARGE_FILE_SIZE) {
		std::cout << "\nPlease wait. Larger files will take longer to complete this process.\n";
	}	

	std::vector<uint_fast8_t> Image_Vec;
	Image_Vec.reserve(TMP_IMAGE_FILE_SIZE);

	std::copy(std::istreambuf_iterator<char>(image_ifs), std::istreambuf_iterator<char>(), std::back_inserter(Image_Vec));
	
	const uint_fast32_t IMAGE_FILE_SIZE = static_cast<uint_fast32_t>(Image_Vec.size());

	constexpr uint_fast8_t 
		JDV_SIG[] 	{ 0xB4, 0x6A, 0x3E, 0xEA, 0x5E, 0x9D, 0xF9, 0x90 },
		PROFILE_SIG[]	{ 0x6D, 0x6E, 0x74, 0x72, 0x52, 0x47, 0x42 },
		PROFILE_COUNT_VALUE_INDEX 	= 0x60,	
		FILE_SIZE_INDEX 		= 0x66,
		ENCRYPTED_FILENAME_INDEX 	= 0x27,
		XOR_KEY_LENGTH 			= 234;

	const uint_fast32_t 
		JDV_SIG_INDEX 	= searchFunc(Image_Vec, 0, 0, JDV_SIG),
		PROFILE_SIG_INDEX = searchFunc(Image_Vec, 0, 0, PROFILE_SIG);
		
	if (JDV_SIG_INDEX == IMAGE_FILE_SIZE) {
		std::cerr << "\nImage File Error: Signature check failure. This is not a valid jdvrif file-embedded image.\n\n";
		return 1;
	}
	
	Image_Vec.erase(Image_Vec.begin(), Image_Vec.begin() + (PROFILE_SIG_INDEX - 8));
	
	const uint_fast32_t EMBEDDED_FILE_SIZE = getByteValue(Image_Vec, FILE_SIZE_INDEX);

	uint_fast16_t 
		xor_key_index = 0x274,
		profile_count = (static_cast<uint_fast16_t>(Image_Vec[PROFILE_COUNT_VALUE_INDEX]) << 8) | 
				static_cast<uint_fast16_t>(Image_Vec[PROFILE_COUNT_VALUE_INDEX + 1]);

	const uint_fast8_t ENCRYPTED_FILENAME_LENGTH = Image_Vec[ENCRYPTED_FILENAME_INDEX - 1];

	std::string encrypted_filename { Image_Vec.begin() + ENCRYPTED_FILENAME_INDEX, Image_Vec.begin() + ENCRYPTED_FILENAME_INDEX + ENCRYPTED_FILENAME_LENGTH };

	for (uint_fast8_t i = 0; i < XOR_KEY_LENGTH; ++i) {
		encrypted_filename += Image_Vec[xor_key_index++]; 
	}

	constexpr uint_fast16_t FILE_START_INDEX = 0x366;
	
	Image_Vec.erase(Image_Vec.begin(), Image_Vec.begin() + FILE_START_INDEX);

	std::vector<uint_fast32_t> Profile_Headers_Index_Vec;
	
	if (profile_count) {
		findProfileHeaders(Image_Vec, Profile_Headers_Index_Vec, profile_count);
	}

	Image_Vec.erase(Image_Vec.begin() + EMBEDDED_FILE_SIZE, Image_Vec.end());

	std::string decrypted_filename = decryptFile(Image_Vec, Profile_Headers_Index_Vec, encrypted_filename, ENCRYPTED_FILENAME_LENGTH);

	inflateFile(Image_Vec);
	
	if (Image_Vec.empty()) {
		std::cerr << "\nFile Size Error: File is zero bytes. Probable failure uncompressing file.\n\n";
		return 1;
	}

	uint_fast32_t inflated_file_size = static_cast<uint_fast32_t>(Image_Vec.size());
	
	std::reverse(Image_Vec.begin(), Image_Vec.end());

	std::ofstream file_ofs(decrypted_filename, std::ios::binary);

	if (!file_ofs) {
		std::cerr << "\nWrite Error: Unable to write to file.\n\n";
		return 1;
	}

	file_ofs.write((char*)&Image_Vec[0], inflated_file_size);
	
	std::vector<uint_fast8_t>().swap(Image_Vec);
				 
	std::cout << "\nExtracted hidden file: " + decrypted_filename + '\x20' + std::to_string(inflated_file_size) + " Bytes.\n\nComplete! Please check your file.\n\n";
	return 0;
}
