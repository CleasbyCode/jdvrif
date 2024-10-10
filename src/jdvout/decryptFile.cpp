const std::string decryptFile(std::vector<uint8_t>&Image_Vec, std::vector<uint8_t>&Decrypted_File_Vec) {
	constexpr uint16_t ENCRYPTED_FILE_START_INDEX = 0x366;
	constexpr uint8_t	
		FILE_SIZE_INDEX 		= 0x66,
		PROFILE_COUNT_VALUE_INDEX 	= 0x60,
		ENCRYPTED_FILENAME_INDEX 	= 0x27,
		XOR_KEY_LENGTH 			= 234,	
		PROFILE_HEADER_LENGTH 		= 18;
	
	const uint32_t EMBEDDED_FILE_SIZE = getByteValue(Image_Vec, FILE_SIZE_INDEX);

	uint16_t PROFILE_COUNT = (static_cast<uint16_t>(Image_Vec[PROFILE_COUNT_VALUE_INDEX]) << 8) | static_cast<uint16_t>(Image_Vec[PROFILE_COUNT_VALUE_INDEX + 1]);

	uint32_t* Headers_Index_Arr = new uint32_t[PROFILE_COUNT];
	uint32_t	
		next_header_index = 0,
		index_pos = 0;

	uint16_t xor_key_index = 0x274;

	uint8_t
		encrypted_filename_length = Image_Vec[ENCRYPTED_FILENAME_INDEX - 1],
		Xor_Key_Arr[XOR_KEY_LENGTH],
		xor_key_pos = 0,
		name_pos = 0;

	const std::string ENCRYPTED_FILENAME { Image_Vec.begin() + ENCRYPTED_FILENAME_INDEX, Image_Vec.begin() + ENCRYPTED_FILENAME_INDEX + encrypted_filename_length };

	// Read in the xor key stored in the profile data.
	for (uint8_t i = 0; XOR_KEY_LENGTH > i; ++i) {
		Xor_Key_Arr[i] = Image_Vec[xor_key_index++]; 
	}

	// Remove profile data & cover image data from vector. Leaving just the encrypted/compressed data file.
	std::vector<uint8_t> Temp_Vec(Image_Vec.begin() + ENCRYPTED_FILE_START_INDEX, Image_Vec.begin() + ENCRYPTED_FILE_START_INDEX + EMBEDDED_FILE_SIZE);
	Image_Vec = std::move(Temp_Vec);
	
	// Search the "file-embedded" image for ICC Profile headers. Store index location of each found header within the vector.
	// We will use these index positions to skip over the headers when decrypting the data file, 
	// so that they are not included within the restored data file.
	if (PROFILE_COUNT) {	
		constexpr uint8_t 
			ICC_PROFILE_SIG[] { 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45 },
			NEXT_SEARCH_POS_INC = 5,
			INDEX_DIFF = 4;

		uint32_t header_index = 0;

		for (uint16_t i = 0; PROFILE_COUNT > i; ++i) {
			Headers_Index_Arr[i] = header_index = searchFunc(Image_Vec, header_index, NEXT_SEARCH_POS_INC, ICC_PROFILE_SIG) - INDEX_DIFF;
		}
	}

	uint32_t encrypted_file_size = static_cast<uint32_t>(Image_Vec.size());

	std::string decrypted_filename;

	while (encrypted_filename_length--) {
		decrypted_filename += ENCRYPTED_FILENAME[name_pos++] ^ Xor_Key_Arr[xor_key_pos++];
	}
			
	while (encrypted_file_size > index_pos) {
		Decrypted_File_Vec.emplace_back(Image_Vec[index_pos++] ^ Xor_Key_Arr[xor_key_pos++]);
		xor_key_pos = xor_key_pos >= XOR_KEY_LENGTH ? 0 : xor_key_pos;
		
		// Skip over the 18 byte ICC Profile header found at each index location within vector, 
		// so that we don't include them within the decrypted file.
		if (PROFILE_COUNT && index_pos == Headers_Index_Arr[next_header_index]) {
			index_pos += PROFILE_HEADER_LENGTH; 
			next_header_index++;
		}	
	}
	std::vector<uint8_t>().swap(Image_Vec);
	delete[] Headers_Index_Arr;
	return decrypted_filename;
}
