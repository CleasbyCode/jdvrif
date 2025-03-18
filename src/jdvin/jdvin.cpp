int jdvIn(const std::string& IMAGE_FILENAME, std::string& data_filename, ArgOption platform, bool isCompressedFile) {
	std::ifstream
		image_file_ifs(IMAGE_FILENAME, std::ios::binary),
		data_file_ifs(data_filename, std::ios::binary);

	if (!image_file_ifs || !data_file_ifs) {
		std::cerr << "\nRead File Error: Unable to read " << (!image_file_ifs ? "image file" : "data file") << ".\n\n";
		return 1;
	}

	const uintmax_t IMAGE_FILE_SIZE = std::filesystem::file_size(IMAGE_FILENAME);

	std::vector<uint8_t> image_vec;
	image_vec.resize(IMAGE_FILE_SIZE); 
	
	image_file_ifs.read(reinterpret_cast<char*>(image_vec.data()), IMAGE_FILE_SIZE);
	image_file_ifs.close();

	constexpr std::array<uint8_t, 2>
		SOI_SIG	{ 0xFF, 0xD8 },
		EOI_SIG { 0xFF, 0xD9 };

	if (!std::equal(SOI_SIG.begin(), SOI_SIG.end(), image_vec.begin()) || !std::equal(EOI_SIG.begin(), EOI_SIG.end(), image_vec.end() - 2)) {
        	std::cerr << "\nImage File Error: This is not a valid JPG image.\n\n";
		return 1;
	}
	
	eraseSegments(image_vec);

	const uintmax_t DATA_FILE_SIZE = std::filesystem::file_size(data_filename);
	
	std::filesystem::path file_path(data_filename);
    	data_filename = file_path.filename().string();

	constexpr uint8_t DATA_FILENAME_MAX_LENGTH = 20;

	const uint8_t DATA_FILENAME_LENGTH = static_cast<uint8_t>(data_filename.length());

	if (DATA_FILENAME_LENGTH > DATA_FILENAME_MAX_LENGTH) {
    		std::cerr << "\nData File Error: For compatibility requirements, length of data filename must not exceed 20 characters.\n\n";
    	 	return 1;
	}

	bool hasBlueskyOption = (platform == ArgOption::Bluesky);

	if (hasBlueskyOption) {
		profile_vec.swap(bluesky_vec);	
	}

	uint16_t DATA_FILENAME_LENGTH_INDEX = hasBlueskyOption ? 0x160 : 0x1EE;

	profile_vec[DATA_FILENAME_LENGTH_INDEX] = DATA_FILENAME_LENGTH;	

	constexpr uint32_t LARGE_FILE_SIZE = 400 * 1024 * 1024;  

	if (DATA_FILE_SIZE > LARGE_FILE_SIZE) {
		std::cout << "\nPlease wait. Larger files will take longer to complete this process.\n";
	}

	std::vector<uint8_t> data_file_vec;
	data_file_vec.resize(DATA_FILE_SIZE); 

	data_file_ifs.read(reinterpret_cast<char*>(data_file_vec.data()), DATA_FILE_SIZE);
	data_file_ifs.close();

	std::reverse(data_file_vec.begin(), data_file_vec.end());

	deflateFile(data_file_vec, isCompressedFile);
	
	if (data_file_vec.empty()) {
		std::cerr << "\nFile Size Error: File is zero bytes. Probable compression failure.\n\n";
		return 1;
	}
	
	const uint64_t PIN = encryptFile(profile_vec, data_file_vec, data_filename, hasBlueskyOption);

	std::vector<uint8_t>().swap(data_file_vec);

	bool shouldDisplayMastodonWarning = false; 

	if (hasBlueskyOption) {
		constexpr uint8_t IDENTITY_BYTES_VAL = 4;
		constexpr uint16_t MAX_SEGMENT_SIZE = 0xFFFF;

		uint32_t segment_size = static_cast<uint32_t>(profile_vec.size() - IDENTITY_BYTES_VAL); 
		
		if (segment_size > MAX_SEGMENT_SIZE) {
			std::cerr << "\nFile Size Error: Data file exceeds segment size limit for the Bluesky platform.\n\n";
			return 1;
		}

		uint8_t 
			segment_size_field_index = 0x04,  
			value_bit_length = 16;

		valueUpdater(profile_vec, segment_size_field_index, segment_size, value_bit_length);

		uint8_t		
			exif_segment_xres_offset_field_index = 0x2A,
			exif_segment_xres_offset_size_diff = 0x36, 

			exif_segment_yres_offset_field_index = 0x36,
			exif_segment_yres_offset_size_diff = 0x2E, 

			exif_segment_artist_size_field_index = 0x4A,
			exif_segment_size_diff = 0x90, 

			exif_segment_subifd_offset_index = 0x5A,
			exif_segment_subifd_offset_size_diff = 0x26; 

		value_bit_length = 32;

		uint32_t exif_xres_offset = segment_size - exif_segment_xres_offset_size_diff;
		valueUpdater(profile_vec, exif_segment_xres_offset_field_index, exif_xres_offset, value_bit_length);

		uint32_t exif_yres_offset = segment_size - exif_segment_yres_offset_size_diff;
		valueUpdater(profile_vec, exif_segment_yres_offset_field_index, exif_yres_offset, value_bit_length);

		uint32_t exif_artist_size = (segment_size - exif_segment_size_diff) + IDENTITY_BYTES_VAL; 
		valueUpdater(profile_vec, exif_segment_artist_size_field_index, exif_artist_size, value_bit_length); 

		uint32_t exif_subifd_offset = segment_size - exif_segment_subifd_offset_size_diff;
		valueUpdater(profile_vec, exif_segment_subifd_offset_index, exif_subifd_offset, value_bit_length);
	} else {
		shouldDisplayMastodonWarning = segmentDataFile(profile_vec, data_file_vec);
	}

	constexpr uint8_t PROFILE_HEADER_LENGTH = 18;
	
	image_vec.reserve(IMAGE_FILE_SIZE + hasBlueskyOption ? profile_vec.size() : data_file_vec.size());	

	bool hasRedditOption = (platform == ArgOption::Reddit);

	if (hasRedditOption) {
		image_vec.insert(image_vec.begin(), SOI_SIG.begin(), SOI_SIG.end());
		image_vec.insert(image_vec.end() - 2, 8000, 0x23);
		image_vec.insert(image_vec.end() - 2, data_file_vec.begin() + PROFILE_HEADER_LENGTH, data_file_vec.end());
	} else if (hasBlueskyOption) {
		image_vec.insert(image_vec.begin(), profile_vec.begin(), profile_vec.end());
	} else {
		image_vec.insert(image_vec.begin(), data_file_vec.begin(), data_file_vec.end());
	}

	std::vector<uint8_t>().swap(data_file_vec);

	if (!writeFile(image_vec)) {
		return 1;
	}
	
	std::cout << "\nRecovery PIN: [***" << PIN << "***]\n\nImportant: Keep your PIN safe, so that you can extract the hidden file.\n";

	if (shouldDisplayMastodonWarning && !hasRedditOption) {
		std::cout << "\n**Warning**\n\nEmbedded image is not compatible with Mastodon. Image file exceeds platform's segments limit.\n";
	}
	if (hasRedditOption) {
		std::cout << "\nReddit option selected: Only post/share this file-embedded JPG image on Reddit.\n";	
	} 
	if (hasBlueskyOption) {
		std::cout << "\nBluesky option selected: Only post/share this file-embedded JPG image on Bluesky.\nMake sure to use the Python script \"bsky_post.py\" (found in the repo src folder) to post the image to Bluesky.\n";
	} 	
	std::cout << "\nComplete!\n\n";

	return 0;
}