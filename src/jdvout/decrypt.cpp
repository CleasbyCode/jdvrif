std::string decryptFile(std::vector<uint_fast8_t>&Image_Vec, std::vector<uint_fast32_t>&Profile_Headers_Offset_Vec, std::string& encrypted_file_name) {

	constexpr uint_fast8_t
		XOR_KEY_LENGTH = 12,		
		PROFILE_HEADER_LENGTH = 18;

	const uint_fast32_t PROFILE_VEC_SIZE = static_cast<uint_fast32_t>(Profile_Headers_Offset_Vec.size());

	const uint_fast8_t ENCRYPTED_NAME_LENGTH = static_cast<uint_fast8_t>(encrypted_file_name.length() - XOR_KEY_LENGTH);

	uint_fast32_t
		data_file_size = static_cast<uint_fast32_t>(Image_Vec.size()),
		offset_index{},
		decrypt_pos{},
		index_pos{};

	uint_fast8_t
		xor_key[XOR_KEY_LENGTH],
		xor_key_pos{},
		name_key_pos{};

	for (int j = 0, i = ENCRYPTED_NAME_LENGTH; i < ENCRYPTED_NAME_LENGTH + XOR_KEY_LENGTH;) {
        	xor_key[j++] = encrypted_file_name[i++]; 
    	}

	std::string decrypted_file_name;

	while (data_file_size > index_pos) {

		if (index_pos >= ENCRYPTED_NAME_LENGTH) {
			name_key_pos = name_key_pos >= ENCRYPTED_NAME_LENGTH ? 0 : name_key_pos;
		} else {
			std::reverse(std::begin(xor_key), std::end(xor_key));

			xor_key_pos = xor_key_pos >= XOR_KEY_LENGTH ? 0 : xor_key_pos;
			decrypted_file_name += encrypted_file_name[index_pos] ^ xor_key[xor_key_pos++];
		}

		Image_Vec[decrypt_pos++] = Image_Vec[index_pos++] ^ encrypted_file_name[name_key_pos++];

		if (PROFILE_VEC_SIZE && index_pos == Profile_Headers_Offset_Vec[offset_index]) {
			index_pos += PROFILE_HEADER_LENGTH; 
			data_file_size += PROFILE_HEADER_LENGTH; 
			offset_index++;	
		}	
	}
	return decrypted_file_name;
}
