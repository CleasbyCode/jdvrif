void encryptFile(std::vector<uint_fast8_t>& Profile_Vec, std::vector<uint_fast8_t>& File_Vec, std::string& data_filename) {
	
	std::random_device rd;
 	std::mt19937 gen(rd());
	std::uniform_int_distribution<unsigned short> dis(1, 255); 
	
	constexpr uint_fast16_t PROFILE_XOR_KEY_INSERT_INDEX = 0x29E;	

	constexpr uint_fast8_t 
		PROFILE_NAME_INDEX = 0x51,
		XOR_KEY_LENGTH = 234;
	
	const uint_fast32_t DATA_FILE_SIZE = static_cast<uint_fast32_t>(File_Vec.size());

	uint_fast32_t index_pos = 0;

	uint_fast8_t 
		data_filename_length = static_cast<uint_fast8_t>(data_filename.length()),
		xor_key[XOR_KEY_LENGTH],
		xor_key_pos = 0,
		name_pos = 0,
		profile_name_index = PROFILE_NAME_INDEX;
	
	Profile_Vec[PROFILE_NAME_INDEX - 1] = data_filename_length;
	
	for (uint_fast8_t i = 0; i < XOR_KEY_LENGTH; ++i) {
        	xor_key[i] = static_cast<uint_fast8_t>(dis(gen));
    	}
	
	std::copy(std::begin(xor_key), std::end(xor_key), Profile_Vec.begin() + PROFILE_XOR_KEY_INSERT_INDEX);

	while (data_filename_length--) {
		data_filename[name_pos] = data_filename[name_pos] ^ xor_key[xor_key_pos++];
		Profile_Vec[profile_name_index++] = data_filename[name_pos++];
	}	

	while (DATA_FILE_SIZE > index_pos) {
		Profile_Vec.emplace_back(File_Vec[index_pos++] ^ xor_key[xor_key_pos++]);
		xor_key_pos = xor_key_pos >= XOR_KEY_LENGTH ? 0 : xor_key_pos;
	}
}
