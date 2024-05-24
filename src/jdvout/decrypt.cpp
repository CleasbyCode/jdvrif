
void decryptFile(std::vector<uint_fast8_t>&Image_Vec, std::vector<uint_fast32_t>&Profile_Headers_Offset_Vec, std::string& file_name) {

	constexpr uint_fast8_t
		XOR_KEY_LENGTH = 12,		
		PROFILE_HEADER_LENGTH = 18;
	
	uint_fast8_t
		xor_key[XOR_KEY_LENGTH],
		file_name_length = static_cast<uint_fast8_t>(file_name.length() - XOR_KEY_LENGTH),
		xor_key_pos{},
		name_key_pos{};

	uint_fast32_t
		file_size = static_cast<uint_fast32_t>(Image_Vec.size()),
		profile_vec_size = static_cast<uint_fast32_t>(Profile_Headers_Offset_Vec.size()),
		offset_index{},
		decrypt_pos{},
		index_pos{};

	std::string decrypted_file_name;

	for (int j = 0, i = file_name_length; i < file_name_length + XOR_KEY_LENGTH;) {
        	xor_key[j++] = file_name[i++]; 
    	}

	while (file_size > index_pos) {

		std::reverse(std::begin(xor_key), std::end(xor_key));

		if (index_pos >= file_name_length) {
			name_key_pos = name_key_pos >= file_name_length ? 0 : name_key_pos;
		}
		else {
			xor_key_pos = xor_key_pos >= XOR_KEY_LENGTH ? 0 : xor_key_pos;
			decrypted_file_name += file_name[index_pos] ^ xor_key[xor_key_pos++];
		}
	
		Image_Vec[decrypt_pos++] = Image_Vec[index_pos++] ^ file_name[name_key_pos++];

		if (profile_vec_size && index_pos == Profile_Headers_Offset_Vec[offset_index]) {
			index_pos += PROFILE_HEADER_LENGTH; 
			file_size += PROFILE_HEADER_LENGTH; 
			offset_index++;	
		}
	}
	file_name = decrypted_file_name;
}
