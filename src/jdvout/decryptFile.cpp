std::string decryptFile(std::vector<uint8_t>&Image_Vec, const uint32_t* HEADERS_INDEX_ARR, const uint8_t* XOR_KEY_ARR, const uint16_t PROFILE_COUNT, std::string& encrypted_filename) {

	constexpr uint8_t 
		PROFILE_HEADER_LENGTH = 18,
		XOR_KEY_LENGTH = 234;

	uint32_t 
		encrypted_file_size 	= static_cast<uint32_t>(Image_Vec.size()),
		next_header_index 	= 0,
		decrypt_pos 		= 0,
		index_pos 		= 0;

	uint8_t
		encrypted_filename_length = static_cast<uint8_t>(encrypted_filename.length()),
		xor_key_pos = 0,
		name_pos = 0;

	std::string decrypted_filename;
	
	while (encrypted_filename_length--) {
		decrypted_filename += encrypted_filename[name_pos++] ^ XOR_KEY_ARR[xor_key_pos++];
	}
			
	while (encrypted_file_size > index_pos) {
		Image_Vec[decrypt_pos++] = Image_Vec[index_pos++] ^ XOR_KEY_ARR[xor_key_pos++];
		xor_key_pos = xor_key_pos >= XOR_KEY_LENGTH ? 0 : xor_key_pos;
		
		// Skip over the 18 byte ICC Profile header found at each index location within vector, so that we don't include them within the decrypted file.
		if (PROFILE_COUNT && index_pos == HEADERS_INDEX_ARR[next_header_index]) {
			index_pos += PROFILE_HEADER_LENGTH; 
			encrypted_file_size += PROFILE_HEADER_LENGTH; 
			next_header_index++;
		}	
	}
	return decrypted_filename;
}
