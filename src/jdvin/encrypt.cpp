void encryptFile(std::vector<uint_fast8_t>& Profile_Vec, std::vector<uint_fast8_t>& File_Vec, std::vector<uint_fast8_t>&Encrypted_Vec, std::string& FILE_NAME) {
	
	std::string
		xor_key = "\xB4\x6A\x3E\xEA\x5E\x90",
		output_name;

	const uint_fast8_t
		XOR_KEY_LENGTH = static_cast<uint_fast8_t>(xor_key.length()),
		FILE_NAME_LENGTH = static_cast<uint_fast8_t>(FILE_NAME.length());

	uint_fast32_t
		file_size = static_cast<uint_fast32_t>(File_Vec.size()),
		index_pos = 0;

	uint_fast8_t
		xor_key_pos = 0,
		name_key_pos = 0;

	while (file_size > index_pos) {

		if (index_pos >= FILE_NAME_LENGTH) {
			name_key_pos = name_key_pos > FILE_NAME_LENGTH ? 0 : name_key_pos;
		}
		else {
			xor_key_pos = xor_key_pos > XOR_KEY_LENGTH ? 0 : xor_key_pos;
			output_name += FILE_NAME[index_pos] ^ xor_key[xor_key_pos++];
		}

		Encrypted_Vec.emplace_back(File_Vec[index_pos++] ^ output_name[name_key_pos++]);			
	}
		constexpr uint_fast16_t PROFILE_VEC_SIZE = 663;	
		constexpr uint_fast8_t 
				PROFILE_NAME_LENGTH_INDEX = 80,	
				PROFILE_NAME_INDEX = 81;	

		Profile_Vec[PROFILE_NAME_LENGTH_INDEX] = FILE_NAME_LENGTH;

		std::copy(output_name.begin(), output_name.end(), Profile_Vec.begin() + PROFILE_NAME_INDEX);

		Profile_Vec.insert(Profile_Vec.begin() + PROFILE_VEC_SIZE, Encrypted_Vec.begin(), Encrypted_Vec.end());
}
