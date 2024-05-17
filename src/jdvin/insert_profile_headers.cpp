void insertProfileHeaders(std::vector<uint_fast8_t>&Profile_Vec, std::vector<uint_fast8_t>&File_Vec, std::vector<uint_fast8_t>&Image_Vec, uint_fast32_t data_file_size, bool isRedditOption) {
	
	const uint_fast32_t PROFILE_VECTOR_SIZE = static_cast<uint_fast32_t>(Profile_Vec.size());	
	constexpr uint_fast16_t BLOCK_SIZE = 65535;	

	uint_fast32_t tally_size = 20;			
	
	uint_fast16_t profile_count = 0;	

	constexpr uint_fast8_t
		PROFILE_HEADER_LENGTH = 18,
		PROFILE_HEADER_INDEX = 20,	
		PROFILE_HEADER_SIZE_INDEX = 22,	
		PROFILE_SIZE_INDEX = 40,	
		PROFILE_COUNT_INDEX = 138,	
		PROFILE_DATA_SIZE_INDEX = 144;	

	uint_fast8_t bits = 16;	

	const std::string ICC_PROFILE_HEADER = { Profile_Vec.begin() + PROFILE_HEADER_INDEX, Profile_Vec.begin() + PROFILE_HEADER_INDEX + PROFILE_HEADER_LENGTH };

	if (BLOCK_SIZE + PROFILE_HEADER_LENGTH + 4 >= PROFILE_VECTOR_SIZE) {

		constexpr uint_fast8_t PROFILE_SIZE_DIFF = 16;
		const uint_fast32_t
			PROFILE_HEADER_BLOCK_SIZE = PROFILE_VECTOR_SIZE - (PROFILE_HEADER_LENGTH + 4),
			PROFILE_BLOCK_SIZE = PROFILE_HEADER_BLOCK_SIZE - PROFILE_SIZE_DIFF;

		Value_Updater(Profile_Vec, PROFILE_HEADER_SIZE_INDEX, PROFILE_HEADER_BLOCK_SIZE, bits);
		Value_Updater(Profile_Vec, PROFILE_SIZE_INDEX, PROFILE_BLOCK_SIZE, bits);

		File_Vec.swap(Profile_Vec);
	}
	else {
		uint_fast32_t byte_index = 0;

		tally_size += BLOCK_SIZE + 2;

		while (PROFILE_VECTOR_SIZE > byte_index) {

			File_Vec.emplace_back(Profile_Vec[byte_index++]);

			if (byte_index == tally_size) {

				File_Vec.insert(File_Vec.begin() + tally_size, ICC_PROFILE_HEADER.begin(), ICC_PROFILE_HEADER.end());

				profile_count++;

				tally_size += BLOCK_SIZE + 2;
			}
		}

		if (tally_size > PROFILE_VECTOR_SIZE + (profile_count * PROFILE_HEADER_LENGTH) + 2) {

			tally_size -= BLOCK_SIZE + 2;

			Value_Updater(File_Vec, tally_size + 2, PROFILE_VECTOR_SIZE - tally_size + (profile_count * PROFILE_HEADER_LENGTH) - 2, bits);
		}
		else {  

			File_Vec.insert(File_Vec.begin() + tally_size, ICC_PROFILE_HEADER.begin(), ICC_PROFILE_HEADER.end());

			profile_count++;

			Value_Updater(File_Vec, tally_size + 2, PROFILE_VECTOR_SIZE - tally_size + (profile_count * PROFILE_HEADER_LENGTH) - 2, bits);
		}

		Value_Updater(File_Vec, PROFILE_COUNT_INDEX, profile_count, bits);
	}

	bits = 32; 

	Value_Updater(File_Vec, PROFILE_DATA_SIZE_INDEX, data_file_size, bits);

	if (isRedditOption) {
		Image_Vec.insert(Image_Vec.end() - 2, File_Vec.begin() + PROFILE_HEADER_LENGTH, File_Vec.end());
	}
	else {
		Image_Vec.insert(Image_Vec.begin(), File_Vec.begin(), File_Vec.end());
	}
}
