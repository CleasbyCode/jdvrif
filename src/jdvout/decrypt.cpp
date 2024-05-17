void decryptFile(std::vector<uint_fast8_t>&Image_Vec, std::vector<uint_fast8_t>&File_Vec, std::vector<uint_fast32_t>&Profile_Headers_Offset_Vec, std::string& image_file_name) {

	std::string
		xor_key = "\xB4\x6A\x3E\xEA\x5E\x90",
		decrypted_file_name;

	const uint_fast8_t
		XOR_KEY_LENGTH = static_cast<uint_fast8_t>(xor_key.length()),
		image_file_name_length = static_cast<uint_fast8_t>(image_file_name.length()),
		PROFILE_HEADER_LENGTH = 18;

	uint_fast32_t
		file_size = static_cast<uint_fast32_t>(Image_Vec.size()),
		offset_index = 0,
		index_pos = 0;

	uint_fast8_t
		xor_key_pos = 0,
		name_key_pos = 0;

	while (file_size > index_pos) {

		std::reverse(xor_key.begin(), xor_key.end());

		if (index_pos >= image_file_name_length) {
			name_key_pos = name_key_pos > image_file_name_length ? 0 : name_key_pos;
		}
		else {
			xor_key_pos = xor_key_pos > XOR_KEY_LENGTH ? 0 : xor_key_pos;
			decrypted_file_name += image_file_name[index_pos] ^ xor_key[xor_key_pos++];
		}

		File_Vec.emplace_back(Image_Vec[index_pos++] ^ image_file_name[name_key_pos++]);

		if (Profile_Headers_Offset_Vec.size() && index_pos == Profile_Headers_Offset_Vec[offset_index]) {
			index_pos += PROFILE_HEADER_LENGTH; 
			file_size += PROFILE_HEADER_LENGTH; 
			offset_index++;	
		}
	}
	image_file_name = decrypted_file_name;
}