std::string decryptFile(std::vector<uint8_t>&Image_Vec, std::vector<uint32_t>&Profile_Headers_Offset_Vec, std::string& encrypted_data_filename) {

	constexpr uint8_t
		XOR_KEY_LENGTH = 12,		
		PROFILE_HEADER_LENGTH = 18;

	const uint32_t PROFILE_VEC_SIZE = static_cast<uint32_t>(Profile_Headers_Offset_Vec.size());

	const uint8_t ENCRYPTED_DATA_FILENAME_LENGTH = static_cast<uint8_t>(encrypted_data_filename.length() - XOR_KEY_LENGTH);

	uint32_t
		encrypted_data_file_size = static_cast<uint32_t>(Image_Vec.size()),
		offset_index{},
		decrypt_pos{},
		index_pos{};

	uint8_t
		xor_key[XOR_KEY_LENGTH],
		xor_key_pos{},
		name_key_pos{};

	for (int j = 0, i = ENCRYPTED_DATA_FILENAME_LENGTH; i < ENCRYPTED_DATA_FILENAME_LENGTH + XOR_KEY_LENGTH;) {
        	xor_key[j++] = encrypted_data_filename[i++]; 
    	}

	std::string decrypted_data_filename;

	while (encrypted_data_file_size > index_pos) {
		if (index_pos >= ENCRYPTED_DATA_FILENAME_LENGTH) {
			name_key_pos = name_key_pos >= ENCRYPTED_DATA_FILENAME_LENGTH ? 0 : name_key_pos;
		} else {
			std::reverse(std::begin(xor_key), std::end(xor_key));

			xor_key_pos = xor_key_pos >= XOR_KEY_LENGTH ? 0 : xor_key_pos;
			decrypted_data_filename += encrypted_data_filename[index_pos] ^ xor_key[xor_key_pos++];
		}

		Image_Vec[decrypt_pos++] = Image_Vec[index_pos++] ^ encrypted_data_filename[name_key_pos++];

		if (PROFILE_VEC_SIZE && index_pos == Profile_Headers_Offset_Vec[offset_index]) {
			index_pos += PROFILE_HEADER_LENGTH; 
			encrypted_data_file_size += PROFILE_HEADER_LENGTH; 
			offset_index++;	
		}	
	}
	return decrypted_data_filename;
}
