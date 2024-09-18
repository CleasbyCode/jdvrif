std::string decryptFile(std::vector<uint_fast8_t>&Image_Vec, std::vector<uint_fast32_t>&Profile_Headers_Index_Vec, std::string& encrypted_filename, uint_fast8_t encrypted_filename_length) {
	
	constexpr uint_fast8_t
		PROFILE_HEADER_LENGTH 	= 18,
		XOR_KEY_LENGTH 		= 234;	
		
	const uint_fast32_t PROFILE_VEC_SIZE = static_cast<uint_fast32_t>(Profile_Headers_Index_Vec.size());

	uint_fast32_t 
		encrypted_file_size 	= static_cast<uint_fast32_t>(Image_Vec.size()),
		next_header_index 	= 0,
		decrypt_pos 		= 0,
		index_pos 		= 0;

	uint_fast8_t
		xor_key[XOR_KEY_LENGTH],
		xor_key_pos = 0,
		name_pos = 0;

	for (uint_fast8_t j = 0, i = encrypted_filename_length; i < encrypted_filename_length + XOR_KEY_LENGTH;) {
		xor_key[j++] = encrypted_filename[i++]; 
	}

	std::string decrypted_filename;
	
	while (encrypted_filename_length--) {
		decrypted_filename += encrypted_filename[name_pos++] ^ xor_key[xor_key_pos++];
	}
			
	while (encrypted_file_size > index_pos) {
		Image_Vec[decrypt_pos++] = Image_Vec[index_pos++] ^ xor_key[xor_key_pos++];
		xor_key_pos = xor_key_pos >= XOR_KEY_LENGTH ? 0 : xor_key_pos;
		
		// Skip over the 18 byte ICC Profile header found at each index location within vector, so that we don't include them within the decrypted file.
		if (PROFILE_VEC_SIZE && index_pos == Profile_Headers_Index_Vec[next_header_index]) {
			index_pos += PROFILE_HEADER_LENGTH; 
			encrypted_file_size += PROFILE_HEADER_LENGTH; 
			next_header_index++;
		}	
	}
	return decrypted_filename;
}
