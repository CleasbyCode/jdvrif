
void decryptFile(std::vector<uint_fast8_t>&Image_Vec, std::vector<uint_fast8_t>&File_Vec, std::vector<uint_fast32_t>&Profile_Headers_Offset_Vec, std::string& encrypted_file_name) {

	std::string decrypted_file_name;

	constexpr uint_fast8_t
		XOR_KEY_LENGTH = 12,		
		PROFILE_HEADER_LENGTH = 18;
	
	uint_fast8_t
		xor_key[XOR_KEY_LENGTH],
		encrypted_file_name_length = static_cast<uint_fast8_t>(encrypted_file_name.length() - XOR_KEY_LENGTH),
		xor_key_pos{},
		name_key_pos{};

	uint_fast32_t
		file_size = static_cast<uint_fast32_t>(Image_Vec.size()),
		offset_index{},
		index_pos{};

	for (int j = 0, i = encrypted_file_name_length; i < encrypted_file_name_length + XOR_KEY_LENGTH;) {
        	xor_key[j++] = encrypted_file_name[i++]; 
    	}

	while (file_size > index_pos) {

		std::reverse(std::begin(xor_key), std::end(xor_key));

		if (index_pos >= encrypted_file_name_length) {
			name_key_pos = name_key_pos >= encrypted_file_name_length ? 0 : name_key_pos;
		}
		else {
			xor_key_pos = xor_key_pos >= XOR_KEY_LENGTH ? 0 : xor_key_pos;
			decrypted_file_name += encrypted_file_name[index_pos] ^ xor_key[xor_key_pos++];
		}

		File_Vec.emplace_back(Image_Vec[index_pos++] ^ encrypted_file_name[name_key_pos++]);

		if (Profile_Headers_Offset_Vec.size() && index_pos == Profile_Headers_Offset_Vec[offset_index]) {
			index_pos += PROFILE_HEADER_LENGTH; 
			file_size += PROFILE_HEADER_LENGTH; 
			offset_index++;	
		}
	}

	encrypted_file_name = decrypted_file_name;
}
