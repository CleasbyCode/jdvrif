void insertProfileHeaders(std::vector<uint_fast8_t>&Profile_Vec, std::vector<uint_fast8_t>&File_Vec, uint_fast32_t deflated_file_size) {

	const uint_fast32_t PROFILE_VECTOR_SIZE = static_cast<uint_fast32_t>(Profile_Vec.size());	

	constexpr uint_fast8_t
		ICC_PROFILE_HEADER[18] { 0xFF, 0xE2, 0xFF, 0xFF, 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45, 0x00, 0x01, 0x01 },
		PROFILE_HEADER_LENGTH = 18,
		PROFILE_HEADER_SIZE_INDEX = 0x16,	
		PROFILE_SIZE_INDEX = 0x28,	
		PROFILE_TALLY_INDEX = 0x8A,	
		PROFILE_DATA_SIZE_INDEX = 0x90,
		JPG_HEADER_LENGTH = 20;

	constexpr uint_fast32_t BLOCK_SIZE = 65519 + PROFILE_HEADER_LENGTH;
	
	uint_fast8_t bits = 16;	

	if (BLOCK_SIZE + JPG_HEADER_LENGTH >= PROFILE_VECTOR_SIZE) {
		const uint_fast32_t
			PROFILE_HEADER_BLOCK_SIZE = PROFILE_VECTOR_SIZE - (PROFILE_HEADER_LENGTH + 4),
			PROFILE_BLOCK_SIZE = PROFILE_HEADER_BLOCK_SIZE - 16;

		Value_Updater(Profile_Vec, PROFILE_HEADER_SIZE_INDEX, PROFILE_HEADER_BLOCK_SIZE, bits);
		Value_Updater(Profile_Vec, PROFILE_SIZE_INDEX, PROFILE_BLOCK_SIZE, bits);

		File_Vec.swap(Profile_Vec);
		
	} else {
		
		uint_fast32_t 
			byte_index{},
			profile_header_insert_tally = PROFILE_VECTOR_SIZE / BLOCK_SIZE,
			profile_header_total_byte_value = profile_header_insert_tally * PROFILE_HEADER_LENGTH,
			block_tally = BLOCK_SIZE + JPG_HEADER_LENGTH,
			last_block_size = (PROFILE_VECTOR_SIZE % BLOCK_SIZE) + (profile_header_total_byte_value - JPG_HEADER_LENGTH) - 2;
		
		while (PROFILE_VECTOR_SIZE > byte_index) {
			
			File_Vec.emplace_back(Profile_Vec[byte_index++]);

			if (byte_index == block_tally) {
				File_Vec.insert(File_Vec.begin() + block_tally, std::begin(ICC_PROFILE_HEADER), std::end(ICC_PROFILE_HEADER));
				block_tally += BLOCK_SIZE;	
			}
		}
		
		if (last_block_size > BLOCK_SIZE * 2) {
			
			File_Vec.insert(File_Vec.begin() + block_tally, std::begin(ICC_PROFILE_HEADER), std::end(ICC_PROFILE_HEADER));
			profile_header_insert_tally++;

			block_tally += BLOCK_SIZE;

			File_Vec.insert(File_Vec.begin() + block_tally, std::begin(ICC_PROFILE_HEADER), std::end(ICC_PROFILE_HEADER));
			profile_header_insert_tally++;

			Value_Updater(File_Vec, block_tally + 2, last_block_size, bits);	
		
		} else if (last_block_size > BLOCK_SIZE) {

			File_Vec.insert(File_Vec.begin() + block_tally, std::begin(ICC_PROFILE_HEADER), std::end(ICC_PROFILE_HEADER));
			profile_header_insert_tally++;

			Value_Updater(File_Vec, block_tally + 2, last_block_size, bits);	
		
		} else {  

			block_tally -= BLOCK_SIZE;
			Value_Updater(File_Vec, block_tally + 2, last_block_size, bits);
			
		}

		Value_Updater(File_Vec, PROFILE_TALLY_INDEX, profile_header_insert_tally, bits);
	}

	bits = 32; 
	Value_Updater(File_Vec, PROFILE_DATA_SIZE_INDEX, deflated_file_size, bits);
}
