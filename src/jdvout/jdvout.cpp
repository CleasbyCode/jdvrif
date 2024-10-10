uint8_t jdvOut(const std::string& IMAGE_FILENAME) {
	constexpr uint32_t 
		MAX_FILE_SIZE 	= 2684354560, // 2.5GB.
		LARGE_FILE_SIZE = 419430400;  // 400MB.

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

	std::vector<uint8_t> Image_Vec;
	Image_Vec.reserve(TMP_IMAGE_FILE_SIZE);

	std::copy(std::istreambuf_iterator<char>(image_ifs), std::istreambuf_iterator<char>(), std::back_inserter(Image_Vec));
	
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
	
	// Remove JPG header and the APP2 ICC Profile/segment header,
	// also, any other segments that could be added by hosting sites (e.g. Mastodon), such as EXIF. 
	// Vector now contains color profile data, encrypted/compressed data file and cover image data.
	Image_Vec.erase(Image_Vec.begin(), Image_Vec.begin() + (PROFILE_SIG_INDEX - INDEX_DIFF));

	std::vector<uint8_t>Decrypted_File_Vec;
	Decrypted_File_Vec.reserve(TMP_IMAGE_FILE_SIZE);

	const std::string DECRYPTED_FILENAME = decryptFile(Image_Vec, Decrypted_File_Vec);	
	
	const uint32_t INFLATED_FILE_SIZE = inflateFile(Decrypted_File_Vec);
	
	if (Decrypted_File_Vec.empty()) {
		std::cerr << "\nFile Size Error: File is zero bytes. Probable failure uncompressing file.\n\n";
		return 1;
	}
	
	std::reverse(Decrypted_File_Vec.begin(), Decrypted_File_Vec.end());

	std::ofstream file_ofs(DECRYPTED_FILENAME, std::ios::binary);

	if (!file_ofs) {
		std::cerr << "\nWrite Error: Unable to write to file.\n\n";
		return 1;
	}

	file_ofs.write((char*)&Decrypted_File_Vec[0], INFLATED_FILE_SIZE);

	std::vector<uint8_t>().swap(Decrypted_File_Vec);

	std::cout << "\nExtracted hidden file: " + DECRYPTED_FILENAME + '\x20' + std::to_string(INFLATED_FILE_SIZE) + " Bytes.\n\nComplete! Please check your file.\n\n";
	return 0;
}
