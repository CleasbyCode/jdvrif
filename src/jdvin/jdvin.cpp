uint8_t jdvIn(const std::string& IMAGE_FILENAME, std::string& data_filename, ArgOption platform, bool isCompressedFile) {
	std::ifstream
		image_file_ifs(IMAGE_FILENAME, std::ios::binary),
		data_file_ifs(data_filename, std::ios::binary);

	if (!image_file_ifs || !data_file_ifs) {
		std::cerr << "\nRead File Error: Unable to read " << (!image_file_ifs ? "image file" : "data file") << ".\n\n";
		return 1;
	}

	const uintmax_t 
		IMAGE_FILE_SIZE = std::filesystem::file_size(IMAGE_FILENAME),
		DATA_FILE_SIZE = std::filesystem::file_size(data_filename);

	std::vector<uint8_t> image_vec;
	image_vec.resize(IMAGE_FILE_SIZE); 
	
	image_file_ifs.read(reinterpret_cast<char*>(image_vec.data()), IMAGE_FILE_SIZE);
	image_file_ifs.close();

	constexpr std::array<uint8_t, 2>
		IMAGE_START_SIG	{ 0xFF, 0xD8 },
		IMAGE_END_SIG   { 0xFF, 0xD9 };

	if (!std::equal(IMAGE_START_SIG.begin(), IMAGE_START_SIG.end(), image_vec.begin()) || !std::equal(IMAGE_END_SIG.begin(), IMAGE_END_SIG.end(), image_vec.end() - 2)) {
        	std::cerr << "\nImage File Error: This is not a valid JPG image.\n\n";
		return 1;
	}
	
	eraseSegments(image_vec);
	
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
		segment_vec.swap(bluesky_exif_vec);	// Use the EXIF segment instead of the default color profile segment to store user data.
	}						// The color profile segment (FFE2) is removed by Bluesky, so we use EXIF.

	const uint16_t DATA_FILENAME_LENGTH_INDEX = hasBlueskyOption ? 0x160 : 0x2E6;

	segment_vec[DATA_FILENAME_LENGTH_INDEX] = DATA_FILENAME_LENGTH;	

	constexpr uint32_t LARGE_FILE_SIZE = 300 * 1024 * 1024;  

	if (DATA_FILE_SIZE > LARGE_FILE_SIZE) {
		std::cout << "\nPlease wait. Larger files will take longer to complete this process.\n";
	}

	std::vector<uint8_t> data_file_vec;
	data_file_vec.resize(DATA_FILE_SIZE); 

	data_file_ifs.read(reinterpret_cast<char*>(data_file_vec.data()), DATA_FILE_SIZE);
	data_file_ifs.close();

	deflateFile(data_file_vec, isCompressedFile);  // zlib compression.
	
	const uint64_t RECOVERY_PIN = encryptFile(segment_vec, data_file_vec, data_filename, hasBlueskyOption);

	std::vector<uint8_t>().swap(data_file_vec);

	bool shouldDisplayMastodonWarning = false; 

	if (hasBlueskyOption) {	 // We can store binary data within the first (EXIF) segment, with a max compressed storage capacity close to ~64KB. See encryptFile.cpp
		constexpr uint8_t MARKER_BYTES_VAL = 4; // FFD8, FFE1
		uint16_t exif_segment_size = static_cast<uint32_t>(segment_vec.size() - MARKER_BYTES_VAL); 

		uint8_t	
			value_bit_length = 16,
			exif_segment_size_field_index = 0x04,  
			exif_segment_xres_offset_field_index = 0x2A,
			exif_segment_yres_offset_field_index = 0x36, 
			exif_segment_artist_size_field_index = 0x4A,
			exif_segment_subifd_offset_field_index = 0x5A;

		uint16_t 
			exif_xres_offset = exif_segment_size - 0x36,
			exif_yres_offset = exif_segment_size - 0x2E,
			exif_subifd_offset = exif_segment_size - 0x26,
			exif_artist_size = (exif_segment_size - 0x90) + MARKER_BYTES_VAL;

		valueUpdater(segment_vec, exif_segment_size_field_index, exif_segment_size, value_bit_length);
		
		value_bit_length = 32;

		valueUpdater(segment_vec, exif_segment_xres_offset_field_index, exif_xres_offset, value_bit_length);
		valueUpdater(segment_vec, exif_segment_yres_offset_field_index, exif_yres_offset, value_bit_length);
		valueUpdater(segment_vec, exif_segment_artist_size_field_index, exif_artist_size, value_bit_length); 
		valueUpdater(segment_vec, exif_segment_subifd_offset_field_index, exif_subifd_offset, value_bit_length);

		constexpr uint16_t BLUESKY_XMP_VEC_DEFAULT_SIZE = 405;  // XMP segment size without user data.
		
		const uint32_t BLUESKY_XMP_VEC_SIZE = static_cast<uint32_t>(bluesky_xmp_vec.size());

		// Are we using the second (XMP) segment?
		if (BLUESKY_XMP_VEC_SIZE > BLUESKY_XMP_VEC_DEFAULT_SIZE) {

			// Size includes segment SIG two bytes (don't count). Bluesky will strip XMP data segment greater than 60031.
			// With the overhead of the XMP default segment data (405) and the Base64 encoding overhead (~33%),
			// The max compressed data storage in this segment is probably around ~40KB. 

 			constexpr uint16_t XMP_SEGMENT_SIZE_LIMIT = 60033;  

			if (BLUESKY_XMP_VEC_SIZE > XMP_SEGMENT_SIZE_LIMIT) {
				std::cerr << "\nFile Size Error: Data file exceeds segment size limit.\n\n";
				return 1;
			}

			constexpr uint8_t segment_sig_bytes = 2; // FFE1

			exif_segment_size_field_index = 0x02,
			value_bit_length = 16;
			valueUpdater(bluesky_xmp_vec, exif_segment_size_field_index, BLUESKY_XMP_VEC_SIZE - segment_sig_bytes, value_bit_length);
			segment_vec.insert(segment_vec.end(), bluesky_xmp_vec.begin(), bluesky_xmp_vec.end());

			image_vec.insert(image_vec.begin(), segment_vec.begin(), segment_vec.end());
			std::cout << "\nBluesky option selected: Only post this \"file-embedded\" JPG image on Bluesky.\n\n"
			  		<< "Make sure to use the Python script \"bsky_post.py\" (found in the repo src folder)\nto post the image to Bluesky.\n";
		}
	} else {
		splitDataFile(segment_vec, data_file_vec, shouldDisplayMastodonWarning); // Default segment_vec uses color profile segment (FFE2). Use multiple segments for larger files.
		image_vec.reserve(IMAGE_FILE_SIZE + data_file_vec.size());	
		if (platform == ArgOption::Reddit) {
			image_vec.insert(image_vec.begin(), IMAGE_START_SIG.begin(), IMAGE_START_SIG.end());
			image_vec.insert(image_vec.end() - 2, 8000, 0x23);
			image_vec.insert(image_vec.end() - 2, data_file_vec.begin() + 2, data_file_vec.end());
			std::cout << "\nReddit option selected: Only post this file-embedded JPG image on Reddit.\n";
		} else {
			image_vec.insert(image_vec.begin(), data_file_vec.begin(), data_file_vec.end());
			if (shouldDisplayMastodonWarning) {
				std::cout << "\n**Warning**\n\nEmbedded image is not compatible with Mastodon. Image file exceeds platform's segments limit.\n";
			}
		}
		std::vector<uint8_t>().swap(data_file_vec);
	}	

	if (!writeFile(image_vec)) {
		std::cerr << "\nWrite File Error: Unable to write to file.\n\n";
		return 1;
	}

	std::cout << "\nRecovery PIN: [***" << RECOVERY_PIN << "***]\n\nImportant: Keep your PIN safe, so that you can extract the hidden file.\n\nComplete!\n\n";
	return 0;
}
