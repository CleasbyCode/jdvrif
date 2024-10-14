void encryptFile(std::vector<uint8_t>& Profile_Vec, std::vector<uint8_t>& File_Vec, uint32_t data_file_size, std::string& data_filename) {
	std::random_device rd;
 	std::mt19937 gen(rd());
	std::uniform_int_distribution<unsigned short> dis(1, 255); 
	
	constexpr uint8_t XOR_KEY_LENGTH = 234;
	
	uint8_t xor_key[XOR_KEY_LENGTH];

	uint16_t xor_key_insert_index = 0x29E;

	for (uint8_t i = 0; XOR_KEY_LENGTH > i; ++i) {
        	xor_key[i] = static_cast<uint8_t>(dis(gen));
		Profile_Vec[xor_key_insert_index++] = xor_key[i];
    	}

	uint8_t
		data_filename_index = 0x51,
		data_filename_length = Profile_Vec[data_filename_index - 1],
		xor_key_pos = 0,
		char_pos = 0;

	while (data_filename_length--) {
		data_filename[char_pos] = data_filename[char_pos] ^ xor_key[xor_key_pos++];
		Profile_Vec[data_filename_index++] = data_filename[char_pos++];
	}	
	
	uint32_t index_pos = 0;

	Profile_Vec.reserve(Profile_Vec.size() + data_file_size);

	while (data_file_size--) {
		Profile_Vec.emplace_back(File_Vec[index_pos++] ^ xor_key[xor_key_pos++]);
		xor_key_pos = xor_key_pos >= XOR_KEY_LENGTH ? 0 : xor_key_pos;
	}
}
