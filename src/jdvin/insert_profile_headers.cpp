void insertProfileHeaders(std::vector<uint_fast8_t>&Profile_Vec, std::vector<uint_fast8_t>&File_Vec, uint_fast32_t data_file_size) {

	const uint_fast32_t PROFILE_VECTOR_SIZE = static_cast<uint_fast32_t>(Profile_Vec.size());	

	constexpr uint_fast16_t BLOCK_SIZE = 65535;	

	uint_fast32_t tally_size = 20;			
	
	uint_fast16_t profile_count{};	

	constexpr uint_fast8_t
		ICC_PROFILE_HEADER[18] { 0xFF, 0xE2, 0xFF, 0xFF, 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45, 0x00, 0x01, 0x01 },
		PROFILE_HEADER_LENGTH = 18,
		PROFILE_HEADER_SIZE_INDEX = 0x16,	
		PROFILE_SIZE_INDEX = 0x28,	
		PROFILE_COUNT_INDEX = 0x8A,	
		PROFILE_DATA_SIZE_INDEX = 0x90;	

	uint_fast8_t bits = 16;	

	if (BLOCK_SIZE + PROFILE_HEADER_LENGTH + 4 >= PROFILE_VECTOR_SIZE) {

		const uint_fast32_t
			PROFILE_HEADER_BLOCK_SIZE = PROFILE_VECTOR_SIZE - (PROFILE_HEADER_LENGTH + 4),
			PROFILE_BLOCK_SIZE = PROFILE_HEADER_BLOCK_SIZE - 16;

		Value_Updater(Profile_Vec, PROFILE_HEADER_SIZE_INDEX, PROFILE_HEADER_BLOCK_SIZE, bits);

		Value_Updater(Profile_Vec, PROFILE_SIZE_INDEX, PROFILE_BLOCK_SIZE, bits);

		File_Vec.swap(Profile_Vec);
	}
	else {

		uint_fast32_t byte_index{};

		tally_size += BLOCK_SIZE + 2;

		while (PROFILE_VECTOR_SIZE > byte_index) {

			File_Vec.emplace_back(Profile_Vec[byte_index++]);

			if (byte_index == tally_size) {

				File_Vec.insert(File_Vec.begin() + tally_size, std::begin(ICC_PROFILE_HEADER), std::end(ICC_PROFILE_HEADER));

				profile_count++;

				tally_size += BLOCK_SIZE + 2;
			}
		}

		if (tally_size > PROFILE_VECTOR_SIZE + (profile_count * PROFILE_HEADER_LENGTH) + 2) {

			tally_size -= BLOCK_SIZE + 2;

			Value_Updater(File_Vec, tally_size + 2, PROFILE_VECTOR_SIZE - tally_size + (profile_count * PROFILE_HEADER_LENGTH) - 2, bits);

		}
		else {  

			File_Vec.insert(File_Vec.begin() + tally_size, std::begin(ICC_PROFILE_HEADER), std::end(ICC_PROFILE_HEADER));

			profile_count++;

			Value_Updater(File_Vec, tally_size + 2, PROFILE_VECTOR_SIZE - tally_size + (profile_count * PROFILE_HEADER_LENGTH) - 2, bits);
		}

		Value_Updater(File_Vec, PROFILE_COUNT_INDEX, profile_count, bits);
	}

	bits = 32; 

	Value_Updater(File_Vec, PROFILE_DATA_SIZE_INDEX, data_file_size, bits);
}