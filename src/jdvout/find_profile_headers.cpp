
void findProfileHeaders(std::vector<uint_fast8_t>&Image_Vec, std::vector<uint_fast32_t>&Profile_Headers_Offset_Vec, std::string& encrypted_file_name) {

	constexpr uint_fast8_t	
		ENCRYPTED_NAME_INDEX = 0x27,			
		PROFILE_COUNT_VALUE_INDEX = 0x60,	
		FILE_SIZE_INDEX = 0x66,
		ICC_PROFILE_SIG[11] { 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45};

	constexpr uint_fast16_t	FILE_START_INDEX = 0x26D;
		
	const uint_fast32_t EMBEDDED_FILE_SIZE = (static_cast<uint_fast32_t>(Image_Vec[FILE_SIZE_INDEX]) << 24)
					| (static_cast<uint_fast32_t>(Image_Vec[FILE_SIZE_INDEX + 1]) << 16)
					| (static_cast<uint_fast32_t>(Image_Vec[FILE_SIZE_INDEX + 2]) << 8)
					| static_cast<uint_fast32_t>(Image_Vec[FILE_SIZE_INDEX + 3]);

	uint_fast16_t 
		xor_key_index = 0x236,
		profile_count = (static_cast<uint_fast16_t>(Image_Vec[PROFILE_COUNT_VALUE_INDEX]) << 8)
				| static_cast<uint_fast16_t>(Image_Vec[PROFILE_COUNT_VALUE_INDEX + 1]);
		
	encrypted_file_name = { Image_Vec.begin() + ENCRYPTED_NAME_INDEX, Image_Vec.begin() + ENCRYPTED_NAME_INDEX + Image_Vec[ENCRYPTED_NAME_INDEX - 1] };

	for (int i = 0; i < 12; ++i) {
		encrypted_file_name += Image_Vec[xor_key_index++]; 
    	}

	Image_Vec.erase(Image_Vec.begin(), Image_Vec.begin() + FILE_START_INDEX);

	uint_fast32_t profile_header_index{}; 

	if (profile_count) { 

		while (profile_count--) {
			Profile_Headers_Offset_Vec.emplace_back(profile_header_index = static_cast<uint_fast32_t>(std::search(Image_Vec.begin() + profile_header_index + 5, Image_Vec.end(), std::begin(ICC_PROFILE_SIG), std::end(ICC_PROFILE_SIG)) - Image_Vec.begin() - 4));
		}
	}

	Image_Vec.erase(Image_Vec.begin() + EMBEDDED_FILE_SIZE, Image_Vec.end());
}
