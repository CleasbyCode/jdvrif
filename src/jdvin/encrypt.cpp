void encryptFile(std::vector<uint_fast8_t>& Profile_Vec, std::vector<uint_fast8_t>& File_Vec, std::string& data_filename) {
	
	std::random_device rd;
 	std::mt19937 gen(rd());
	std::uniform_int_distribution<unsigned short> dis(1, 255); 
	
	constexpr uint_fast16_t 
		PROFILE_XOR_KEY_INSERT_INDEX = 0x260,
		PROFILE_VEC_SIZE = 663;	

	constexpr uint_fast8_t 
		XOR_KEY_LENGTH = 12,
		PROFILE_NAME_INDEX = 0x51;

	const uint_fast32_t DATA_FILE_SIZE = static_cast<uint_fast32_t>(File_Vec.size());

	const uint_fast8_t DATA_FILENAME_LENGTH = static_cast<uint_fast8_t>(data_filename.length());
       
	uint_fast32_t index_pos{};

	uint_fast8_t 
		xor_key[XOR_KEY_LENGTH],
		xor_key_pos{},
		name_key_pos{};
	
	for (int i = 0; i < XOR_KEY_LENGTH; ++i) {
        	xor_key[i] = static_cast<uint_fast8_t>(dis(gen));
    	}
	
	std::copy(std::begin(xor_key), std::end(xor_key), Profile_Vec.begin() + PROFILE_XOR_KEY_INSERT_INDEX);

	while (DATA_FILE_SIZE > index_pos) {
		if (index_pos >= DATA_FILENAME_LENGTH) {
			name_key_pos = name_key_pos >= DATA_FILENAME_LENGTH ? 0 : name_key_pos;
		} else {
			std::reverse(std::begin(xor_key), std::end(xor_key));

			xor_key_pos = xor_key_pos >= XOR_KEY_LENGTH ? 0 : xor_key_pos;
			data_filename[index_pos] = data_filename[index_pos] ^ xor_key[xor_key_pos++];
		}
		Profile_Vec.emplace_back(File_Vec[index_pos++] ^ data_filename[name_key_pos++]);			
	}
	Profile_Vec[PROFILE_NAME_INDEX - 1] = DATA_FILENAME_LENGTH;
	std::copy(data_filename.begin(), data_filename.end(), Profile_Vec.begin() + PROFILE_NAME_INDEX);
}
