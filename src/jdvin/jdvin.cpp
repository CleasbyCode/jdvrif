int jdvIn(const std::string& IMAGE_FILENAME, std::string& data_filename, ArgOption platformOption, bool isCompressedFile) {
	std::ifstream
		image_file_ifs(IMAGE_FILENAME, std::ios::binary),
		data_file_ifs(data_filename, std::ios::binary);

	if (!image_file_ifs || !data_file_ifs) {
		std::cerr << "\nRead File Error: Unable to read " << (!image_file_ifs ? "image file" : "data file") << ".\n\n";
		return 1;
	}

	const uintmax_t IMAGE_FILE_SIZE = std::filesystem::file_size(IMAGE_FILENAME);

	std::vector<uint8_t> Image_Vec;
	Image_Vec.resize(IMAGE_FILE_SIZE); 
	
	image_file_ifs.read(reinterpret_cast<char*>(Image_Vec.data()), IMAGE_FILE_SIZE);
	image_file_ifs.close();

	constexpr uint8_t
		SOI_SIG[]	{ 0xFF, 0xD8 },
		EOI_SIG[] 	{ 0xFF, 0xD9 };

	if (!std::equal(std::begin(SOI_SIG), std::end(SOI_SIG), std::begin(Image_Vec)) || !std::equal(std::begin(EOI_SIG), std::end(EOI_SIG), std::end(Image_Vec) - 2)) {
        	std::cerr << "\nImage File Error: This is not a valid JPG image.\n\n";
		return 1;
	}
	
	eraseSegments(Image_Vec);

	const uintmax_t DATA_FILE_SIZE = std::filesystem::file_size(data_filename);
	
	std::filesystem::path filePath(data_filename);
    	data_filename = filePath.filename().string();

	constexpr uint8_t DATA_FILENAME_MAX_LENGTH = 20;

	const uint8_t DATA_FILENAME_LENGTH = static_cast<uint8_t>(data_filename.length());

	if (DATA_FILENAME_LENGTH > DATA_FILENAME_MAX_LENGTH) {
    		std::cerr << "\nData File Error: Length of data filename is too long.\n\nFor compatibility requirements, length of data filename must not exceed 20 characters.\n\n";
    	 	return 1;
	}

	constexpr uint16_t DATA_FILENAME_LENGTH_INDEX = 0x1EE;
	
	uint8_t 
		data_file_size_index = 0x90,
		value_bit_length = 32;

	Profile_Vec[DATA_FILENAME_LENGTH_INDEX] = DATA_FILENAME_LENGTH;

	valueUpdater(Profile_Vec, data_file_size_index, static_cast<uint32_t>(DATA_FILE_SIZE), value_bit_length);

	constexpr uint32_t LARGE_FILE_SIZE = 400 * 1024 * 1024;  // 400MB.

	if (DATA_FILE_SIZE > LARGE_FILE_SIZE) {
		std::cout << "\nPlease wait. Larger files will take longer to complete this process.\n";
	}

	std::vector<uint8_t> File_Vec;
	File_Vec.resize(DATA_FILE_SIZE); 

	data_file_ifs.read(reinterpret_cast<char*>(File_Vec.data()), DATA_FILE_SIZE);
	data_file_ifs.close();

	std::reverse(File_Vec.begin(), File_Vec.end());

	Profile_Vec[data_file_size_index + 4] = data_filename[0];

	deflateFile(File_Vec, isCompressedFile);
	
	if (File_Vec.empty()) {
		std::cerr << "\nFile Size Error: File is zero bytes. Probable compression failure.\n\n";
		return 1;
	}
	
	const uint64_t PIN = encryptFile(Profile_Vec, File_Vec, data_filename);

	std::vector<uint8_t>().swap(File_Vec);

	bool shouldDisplayMastodonWarning = segmentDataFile(Profile_Vec, File_Vec);

	constexpr uint8_t PROFILE_HEADER_LENGTH = 18;
	
	Image_Vec.reserve(IMAGE_FILE_SIZE + File_Vec.size());	

	bool hasRedditOption = (platformOption == ArgOption::Reddit);

	if (hasRedditOption) {
		Image_Vec.insert(Image_Vec.begin(), std::begin(SOI_SIG), std::end(SOI_SIG));
		Image_Vec.insert(Image_Vec.end() - 2, 8000, 0x23);
		Image_Vec.insert(Image_Vec.end() - 2, File_Vec.begin() + PROFILE_HEADER_LENGTH, File_Vec.end());
	} else {
		Image_Vec.insert(Image_Vec.begin(), File_Vec.begin(), File_Vec.end());
	}

	std::vector<uint8_t>().swap(File_Vec);

	if (!writeFile(Image_Vec)) {
		return 1;
	}
	
	std::cout << "\nRecovery PIN: [***" << PIN << "***]\n\nImportant: Keep your PIN safe, so that you can extract the hidden file.\n";

	if (shouldDisplayMastodonWarning && !hasRedditOption) {
		std::cout << "\n**Warning**\n\nEmbedded image is not compatible with Mastodon. Image file exceeds platform's segments limit.\n";
	}

	std::cout << ((hasRedditOption) 
		?  "\nReddit option selected: Only share/post this file-embedded JPG image on Reddit.\n\nComplete!\n\n"
		:  "\nComplete!\n\n");	

	return 0;
}