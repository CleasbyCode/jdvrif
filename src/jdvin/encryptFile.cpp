uint32_t encryptFile(std::vector<uint8_t>& Profile_Vec, std::vector<uint8_t>& File_Vec, uint32_t data_file_size, std::string& data_filename) {
	
	std::random_device rd;
 	std::mt19937 gen(rd());
	std::uniform_int_distribution<unsigned short> dis(1, 255); 
		
	constexpr uint8_t 
		DEFAULT_PIN_INDEX = 0x95,
		DEFAULT_PIN_XOR_INDEX = 0x90,
		XOR_KEY_LENGTH = 234,
		PIN_LENGTH = 9;

	uint32_t index_pos = 0;
	
	uint16_t 
		data_filename_index = 0x1EF,
		xor_key_index = 0x2F5,
		encrypt_xor_pos = xor_key_index,
		index_xor_pos = encrypt_xor_pos;

	uint8_t 
		data_filename_length = Profile_Vec[data_filename_index - 1],
		pin_index = DEFAULT_PIN_INDEX,
		pin_xor_index = DEFAULT_PIN_XOR_INDEX,
		xor_key[XOR_KEY_LENGTH],
		value_bit_length = 32,
		xor_key_length = XOR_KEY_LENGTH,
		xor_key_pos = 0,
		char_pos = 0;		

	for (int i = 0; i < XOR_KEY_LENGTH; ++i) {
        	xor_key[i] = static_cast<uint8_t>(dis(gen));
		Profile_Vec[xor_key_index++] = xor_key[i];
    	}

	while (data_filename_length--) {
		Profile_Vec[data_filename_index++] = data_filename[char_pos++] ^ xor_key[xor_key_pos++];
	}	
	
	Profile_Vec.reserve(Profile_Vec.size() + data_file_size);
	
	while (data_file_size--) {
		Profile_Vec.emplace_back(File_Vec[index_pos++] ^ xor_key[xor_key_pos++ % XOR_KEY_LENGTH]);	
	}

	xor_key_index = 0x2F5;

	const uint32_t PIN = crcUpdate(&Profile_Vec[xor_key_index], XOR_KEY_LENGTH);
	valueUpdater(Profile_Vec, pin_index, PIN, value_bit_length);

	while(xor_key_length--) {
		Profile_Vec[encrypt_xor_pos++] = Profile_Vec[index_xor_pos++] ^ Profile_Vec[pin_xor_index++];
		pin_xor_index = pin_xor_index >= PIN_LENGTH + DEFAULT_PIN_XOR_INDEX ? DEFAULT_PIN_XOR_INDEX : pin_xor_index;
	}

	valueUpdater(Profile_Vec, pin_index, 0, value_bit_length);

	return PIN;
}
