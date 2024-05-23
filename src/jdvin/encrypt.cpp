	void encryptFile(std::vector<uint_fast8_t>& Profile_Vec, std::vector<uint_fast8_t>& File_Vec, std::vector<uint_fast8_t>&Encrypted_Vec, std::string& FILE_NAME) {
	
	std::random_device rd;
 	std::mt19937 gen(rd());
	std::uniform_int_distribution<unsigned short> dis(1, 255); 
	
	constexpr uint_fast16_t 
		PROFILE_XOR_KEY_INSERT_INDEX = 0x260,
		PROFILE_VEC_SIZE = 663;	

	constexpr uint_fast8_t 
		XOR_KEY_LENGTH = 12,
		PROFILE_NAME_INDEX = 0x51;

	uint_fast8_t 
		xor_key[XOR_KEY_LENGTH],
		xor_key_pos{},
		name_key_pos{};

	for (int i = 0; i < 12; ++i) {
        	xor_key[i] = static_cast<uint_fast8_t>(dis(gen));
    	}

	std::copy(std::begin(xor_key), std::end(xor_key), Profile_Vec.begin() + PROFILE_XOR_KEY_INSERT_INDEX);

	std::string encrypted_file_name;	

	const uint_fast8_t FILE_NAME_LENGTH = static_cast<uint_fast8_t>(FILE_NAME.length());

	uint_fast32_t
		file_size = static_cast<uint_fast32_t>(File_Vec.size()),
		index_pos{};

	while (file_size > index_pos) {

		std::reverse(std::begin(xor_key), std::end(xor_key));

		if (index_pos >= FILE_NAME_LENGTH) {
			name_key_pos = name_key_pos >= FILE_NAME_LENGTH ? 0 : name_key_pos;
		}
		else {
			xor_key_pos = xor_key_pos >= XOR_KEY_LENGTH ? 0 : xor_key_pos;
			encrypted_file_name += FILE_NAME[index_pos] ^ xor_key[xor_key_pos++];
		}

		Encrypted_Vec.emplace_back(File_Vec[index_pos++] ^ encrypted_file_name[name_key_pos++]);			
	}

		Profile_Vec[PROFILE_NAME_INDEX - 1] = FILE_NAME_LENGTH;

		std::copy(encrypted_file_name.begin(), encrypted_file_name.end(), Profile_Vec.begin() + PROFILE_NAME_INDEX);

		Profile_Vec.insert(Profile_Vec.begin() + PROFILE_VEC_SIZE, Encrypted_Vec.begin(), Encrypted_Vec.end());

		Encrypted_Vec.clear();
		Encrypted_Vec.shrink_to_fit();
}
