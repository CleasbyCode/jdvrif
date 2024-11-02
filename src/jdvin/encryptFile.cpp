uint32_t encryptFile(std::vector<uint8_t>& Profile_Vec, std::vector<uint8_t>& File_Vec, uint32_t data_file_size, std::string& data_filename) {
	
	std::random_device rd;
 	std::mt19937 gen(rd());
	std::uniform_int_distribution<unsigned short> dis(1, 255); 
	
	constexpr uint8_t XOR_KEY_LENGTH = 234;
	
	uint8_t xor_key[XOR_KEY_LENGTH];

	uint16_t xor_key_index = 0x29E;

	for (uint8_t i = 0; i < XOR_KEY_LENGTH; ++i) {
        	xor_key[i] = static_cast<uint8_t>(dis(gen));
		Profile_Vec[xor_key_index++] = xor_key[i];
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
		Profile_Vec.emplace_back(File_Vec[index_pos++] ^ xor_key[xor_key_pos++ % XOR_KEY_LENGTH]);
	}
	
	constexpr uint8_t PIN_LENGTH = 4;
	constexpr uint16_t DATA_FILE_START_INDEX = 0x390;

	uint8_t 
		xor_key_length = XOR_KEY_LENGTH,
		cover_index = 0x50;

	uint16_t
		pin_index = DATA_FILE_START_INDEX,
		encrypt_xor_pos = 0x29E,
		index_xor_pos = encrypt_xor_pos;
	
	while(xor_key_length--) {
		Profile_Vec[encrypt_xor_pos++] = Profile_Vec[index_xor_pos++] ^ Profile_Vec[pin_index++];
		pin_index = pin_index >= PIN_LENGTH ? DATA_FILE_START_INDEX : pin_index;
	}

	pin_index = DATA_FILE_START_INDEX;

	const uint32_t 
		PIN = getByteValue(Profile_Vec, pin_index),
		COVER_PIN = getByteValue(Profile_Vec, cover_index);
	
	uint8_t value_bit_length = 32;

	valueUpdater(Profile_Vec, pin_index, COVER_PIN, value_bit_length);
	
	return PIN;
}