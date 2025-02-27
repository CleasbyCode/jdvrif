int jdvOut(const std::string& IMAGE_FILENAME) {
	
	const uintmax_t IMAGE_FILE_SIZE = std::filesystem::file_size(IMAGE_FILENAME);
	
	std::ifstream image_file_ifs(IMAGE_FILENAME, std::ios::binary);

	if (!image_file_ifs) {
		std::cerr << "\nOpen File Error: Unable to read image file.\n\n";
		return 1;
    	} 

	std::vector<uint8_t> Image_Vec;
	Image_Vec.resize(IMAGE_FILE_SIZE);

	image_file_ifs.read(reinterpret_cast<char*>(Image_Vec.data()), IMAGE_FILE_SIZE);
	image_file_ifs.close();

	constexpr uint8_t
		JDV_SIG[]	{ 0xB4, 0x6A, 0x3E, 0xEA, 0x5E, 0x9D, 0xF9 },
		PROFILE_SIG[] 	{ 0x6D, 0x6E, 0x74, 0x72, 0x52, 0x47, 0x42 },
		INDEX_DIFF = 8;
				
	const uint32_t 
		JDV_SIG_INDEX 	= searchFunc(Image_Vec, 0, 0, JDV_SIG),
		PROFILE_SIG_INDEX = searchFunc(Image_Vec, 0, 0, PROFILE_SIG);

	if (JDV_SIG_INDEX == Image_Vec.size()) {
		std::cerr << "\nImage File Error: Signature check failure. This is not a valid jdvrif file-embedded image.\n\n";
		return 1;
	}
	
	uint8_t extract_success_byte_val = Image_Vec[JDV_SIG_INDEX + INDEX_DIFF - 1];

	// Remove JPG header and the APP2 ICC Profile/segment header,
	// also, any other segments that could be added by hosting sites (e.g. Mastodon), such as EXIF. 
	// Vector now contains color profile data, encrypted/compressed data file and cover image data.
	Image_Vec.erase(Image_Vec.begin(), Image_Vec.begin() + (PROFILE_SIG_INDEX - INDEX_DIFF));

	constexpr uint32_t LARGE_FILE_SIZE = 400 * 1024 * 1024;  // 400MB.

	if (IMAGE_FILE_SIZE > LARGE_FILE_SIZE) {
		std::cout << "\nPlease wait. Larger files will take longer to complete this process.\n";
	}

	const std::string DECRYPTED_FILENAME = decryptFile(Image_Vec);	
	
	const uint32_t INFLATED_FILE_SIZE = inflateFile(Image_Vec);

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

	std::reverse(Image_Vec.begin(), Image_Vec.end());

	std::ofstream file_ofs(DECRYPTED_FILENAME, std::ios::binary);

	if (!file_ofs) {
		std::cerr << "\nWrite Error: Unable to write to file.\n\n";
		return 1;
	}

	file_ofs.write(reinterpret_cast<const char*>(Image_Vec.data()), INFLATED_FILE_SIZE);

	std::vector<uint8_t>().swap(Image_Vec);

	std::cout << "\nExtracted hidden file: " << DECRYPTED_FILENAME << " (" << INFLATED_FILE_SIZE << " bytes).\n\nComplete! Please check your file.\n\n";
	return 0;
}
