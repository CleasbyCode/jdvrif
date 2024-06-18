void insertProfileHeaders(std::vector<uint_fast8_t>&Profile_Vec, std::vector<uint_fast8_t>&File_Vec) {

	constexpr uint_fast8_t
		PROFILE_HEADER_LENGTH = 18,
		PROFILE_HEADER_SIZE_INDEX = 0x16,	
		PROFILE_SIZE_INDEX = 0x28,	
		PROFILE_TALLY_INDEX = 0x8A,	
		DEFLATED_FILE_SIZE_INDEX = 0x90,
		JPG_HEADER_LENGTH = 20;

	constexpr uint_fast16_t PROFILE_SIZE = 663;

	constexpr uint_fast32_t BLOCK_SIZE = 65519 + PROFILE_HEADER_LENGTH;

	const uint_fast32_t 
		PROFILE_VECTOR_SIZE = static_cast<uint_fast32_t>(Profile_Vec.size()),
		DEFLATED_FILE_SIZE = PROFILE_VECTOR_SIZE - PROFILE_SIZE;
	
	uint_fast8_t bits = 16;	
		
	if (BLOCK_SIZE + JPG_HEADER_LENGTH >= PROFILE_VECTOR_SIZE) {
		const uint_fast32_t
			PROFILE_HEADER_BLOCK_SIZE = PROFILE_VECTOR_SIZE - (PROFILE_HEADER_LENGTH + 4),
			PROFILE_BLOCK_SIZE = PROFILE_HEADER_BLOCK_SIZE - 16;

		valueUpdater(Profile_Vec, PROFILE_HEADER_SIZE_INDEX, PROFILE_HEADER_BLOCK_SIZE, bits);
		valueUpdater(Profile_Vec, PROFILE_SIZE_INDEX, PROFILE_BLOCK_SIZE, bits);

		File_Vec.swap(Profile_Vec);
		
	} else {

		std::vector<uint_fast8_t>Profile_Header_Vec = { 0xFF, 0xE2, 0xFF, 0xFF, 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45, 0x00, 0x01, 0x01 };
		
		uint_fast8_t profile_count_header_insert_index = 0x0F;

		uint_fast16_t profile_count = 1;

		uint_fast32_t 
			byte_index{},
			profile_header_insert_tally = PROFILE_VECTOR_SIZE / BLOCK_SIZE,
			profile_header_total_byte_value = profile_header_insert_tally * PROFILE_HEADER_LENGTH,
			block_tally = BLOCK_SIZE + JPG_HEADER_LENGTH,
			last_block_size = (PROFILE_VECTOR_SIZE % BLOCK_SIZE) + (profile_header_total_byte_value - JPG_HEADER_LENGTH) - 2;
		
		while (PROFILE_VECTOR_SIZE > byte_index) {
			
			File_Vec.emplace_back(Profile_Vec[byte_index++]);

			if (byte_index == block_tally) {
				profile_count++;
				valueUpdater(Profile_Header_Vec, profile_count_header_insert_index, profile_count, bits);

				File_Vec.insert(File_Vec.begin() + block_tally, Profile_Header_Vec.begin(), Profile_Header_Vec.end());
				block_tally += BLOCK_SIZE;	
			}
		}
		
		if (last_block_size > BLOCK_SIZE * 2) {
			
			last_block_size += (PROFILE_HEADER_LENGTH * 2) - 2;
						
			profile_count++;
			valueUpdater(Profile_Header_Vec, profile_count_header_insert_index, profile_count, bits);

			File_Vec.insert(File_Vec.begin() + block_tally, Profile_Header_Vec.begin(), Profile_Header_Vec.end());

			profile_header_insert_tally++;
			block_tally += BLOCK_SIZE;

			profile_count++;
			valueUpdater(Profile_Header_Vec, profile_count_header_insert_index, profile_count, bits);		

			File_Vec.insert(File_Vec.begin() + block_tally, Profile_Header_Vec.begin(), Profile_Header_Vec.end());
			profile_header_insert_tally++;

			valueUpdater(File_Vec, block_tally + 2, last_block_size, bits);	
		
		} else if (last_block_size > BLOCK_SIZE) {
			
			last_block_size += PROFILE_HEADER_LENGTH - 1;
			
			profile_count++;
			valueUpdater(Profile_Header_Vec, profile_count_header_insert_index, profile_count, bits);

			File_Vec.insert(File_Vec.begin() + block_tally, Profile_Header_Vec.begin(), Profile_Header_Vec.end());
			profile_header_insert_tally++;

			valueUpdater(File_Vec, block_tally + 2, last_block_size, bits);	
		
		} else {  

			block_tally -= BLOCK_SIZE;
			valueUpdater(File_Vec, block_tally + 2, last_block_size, bits);		
		}

		valueUpdater(File_Vec, PROFILE_TALLY_INDEX, profile_header_insert_tally, bits);
		profile_header_insert_tally++;

		constexpr uint_fast8_t	
			ICC_PROFILE_SIG[] { 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45 },
			profile_total_insert_index_diff = 13,
			pos_addition = 1,
			profile_tally_update_max = 255;
			
		uint_fast32_t profile_total_insert_index{}; 
		
		uint_fast16_t counter = profile_header_insert_tally;

		while (counter--) {
			profile_total_insert_index = searchFunc(File_Vec, profile_total_insert_index, pos_addition, ICC_PROFILE_SIG) + profile_total_insert_index_diff;
			File_Vec[profile_total_insert_index] = profile_header_insert_tally > profile_tally_update_max ? profile_tally_update_max : profile_header_insert_tally;
		}	
	}
	
	bits = 32; 
	valueUpdater(File_Vec, DEFLATED_FILE_SIZE_INDEX, DEFLATED_FILE_SIZE, bits);
}
