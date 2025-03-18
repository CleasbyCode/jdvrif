int jdvOut(const std::string& IMAGE_FILENAME) {
	const uintmax_t IMAGE_FILE_SIZE = std::filesystem::file_size(IMAGE_FILENAME);
	
	std::ifstream image_file_ifs(IMAGE_FILENAME, std::ios::binary);

	if (!image_file_ifs) {
		std::cerr << "\nOpen File Error: Unable to read image file.\n\n";
		return 1;
    	} 

	std::vector<uint8_t> image_vec;
	image_vec.resize(IMAGE_FILE_SIZE);

	image_file_ifs.read(reinterpret_cast<char*>(image_vec.data()), IMAGE_FILE_SIZE);
	image_file_ifs.close();

	
	constexpr std::array<uint8_t, 7>
		JDV_SIG		{ 0xB4, 0x6A, 0x3E, 0xEA, 0x5E, 0x9D, 0xF9 },
		PROFILE_SIG	{ 0x6D, 0x6E, 0x74, 0x72, 0x52, 0x47, 0x42 };

	const uint8_t INDEX_DIFF = 8;
				
	const uint32_t 
		JDV_SIG_INDEX 	= searchFunc(image_vec, 0, 0, JDV_SIG),
		PROFILE_SIG_INDEX = searchFunc(image_vec, 0, 0, PROFILE_SIG);

	if (JDV_SIG_INDEX == image_vec.size()) {
		std::cerr << "\nImage File Error: Signature check failure. This is not a valid jdvrif file-embedded image.\n\n";
		return 1;
	}
	
	uint8_t extract_success_byte_val = image_vec[JDV_SIG_INDEX + INDEX_DIFF - 1];

	bool hasBlueskyOption = true;

	if (PROFILE_SIG_INDEX != image_vec.size()) {
		image_vec.erase(image_vec.begin(), image_vec.begin() + (PROFILE_SIG_INDEX - INDEX_DIFF));
		hasBlueskyOption = false;
	}

	constexpr uint32_t LARGE_FILE_SIZE = 400 * 1024 * 1024;

	if (IMAGE_FILE_SIZE > LARGE_FILE_SIZE) {
		std::cout << "\nPlease wait. Larger files will take longer to complete this process.\n";
	}

	const std::string DECRYPTED_FILENAME = decryptFile(image_vec, hasBlueskyOption);	
	
	const uint32_t INFLATED_FILE_SIZE = inflateFile(image_vec);

	bool hasInflateFailed = !INFLATED_FILE_SIZE;
				 
	if (hasInflateFailed) {	
		std::fstream file(IMAGE_FILENAME, std::ios::in | std::ios::out | std::ios::binary);
		std::streampos failure_index = JDV_SIG_INDEX + INDEX_DIFF - 1;

		file.seekg(failure_index);

		uint8_t byte;
		file.read(reinterpret_cast<char*>(&byte), sizeof(byte));

		if (byte == 0x90) {
			byte = 0;
		} else {
    			byte++;
		}
		
		if (byte > 2) {
			file.close();
			std::ofstream file(IMAGE_FILENAME, std::ios::out | std::ios::trunc | std::ios::binary);
		} else {
			file.seekp(failure_index);
			file.write(reinterpret_cast<char*>(&byte), sizeof(byte));
		}

		file.close();

		std::cerr << "\nFile Recovery Error: Invalid PIN or file is corrupt.\n\n";
		return 1;
	}

	if (extract_success_byte_val != 0x90) {
		std::fstream file(IMAGE_FILENAME, std::ios::in | std::ios::out | std::ios::binary);
		std::streampos success_index = JDV_SIG_INDEX + INDEX_DIFF - 1;
	
		uint8_t byte = 0x90;

		file.seekp(success_index);
		file.write(reinterpret_cast<char*>(&byte), sizeof(byte));

		file.close();
	}

	std::reverse(image_vec.begin(), image_vec.end());

	std::ofstream file_ofs(DECRYPTED_FILENAME, std::ios::binary);

	if (!file_ofs) {
		std::cerr << "\nWrite Error: Unable to write to file.\n\n";
		return 1;
	}

	file_ofs.write(reinterpret_cast<const char*>(image_vec.data()), INFLATED_FILE_SIZE);

	std::vector<uint8_t>().swap(image_vec);

	std::cout << "\nExtracted hidden file: " << DECRYPTED_FILENAME << " (" << INFLATED_FILE_SIZE << " bytes).\n\nComplete! Please check your file.\n\n";
	return 0;
}