// If required, split and store data file into multiple color profile segment blocks.
void insertProfileHeaders(std::vector<uint_fast8_t>&Profile_Vec, std::vector<uint_fast8_t>&File_Vec) {

	constexpr uint_fast8_t
		PROFILE_HEADER_LENGTH = 18,
		PROFILE_HEADER_SEGMENT_SIZE_INDEX = 0x16, // Two byte JPG color profile header segment size field index.	
		PROFILE_SIZE_INDEX = 0x28,		  // Four byte profile size field index.	
		PROFILE_TALLY_INDEX = 0x8A,	
		DEFLATED_DATA_FILE_SIZE_INDEX = 0x90,
		JPG_HEADER_LENGTH = 20;

	constexpr uint_fast16_t COLOR_PROFILE_SIZE = 663;

	constexpr uint_fast32_t SEGMENT_SIZE = 65537;

	const uint_fast32_t 
		PROFILE_WITH_DATA_FILE_VEC_SIZE = static_cast<uint_fast32_t>(Profile_Vec.size()),
		DEFLATED_DATA_FILE_SIZE = PROFILE_WITH_DATA_FILE_VEC_SIZE - COLOR_PROFILE_SIZE;
	
	uint_fast8_t bits = 16;	
		
	// Default profile and data file fit within the first profile segment block.
	if (SEGMENT_SIZE + JPG_HEADER_LENGTH >= PROFILE_WITH_DATA_FILE_VEC_SIZE) {
		constexpr uint_fast8_t SIZE_DIFF = 16;
		const uint_fast32_t
			PROFILE_HEADER_SEGMENT_SIZE = PROFILE_WITH_DATA_FILE_VEC_SIZE - (PROFILE_HEADER_LENGTH + 4), 
			PROFILE_SIZE = PROFILE_HEADER_SEGMENT_SIZE - SIZE_DIFF;

		valueUpdater(Profile_Vec, PROFILE_HEADER_SEGMENT_SIZE_INDEX, PROFILE_HEADER_SEGMENT_SIZE, bits);
		valueUpdater(Profile_Vec, PROFILE_SIZE_INDEX, PROFILE_SIZE, bits);

		File_Vec.swap(Profile_Vec);
		
	} else { // Data file is too large for a single profile segment. Create multiple profile segements to store data file.

		std::vector<uint_fast8_t>Profile_Header_Vec = { 0xFF, 0xE2, 0xFF, 0xFF, 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45, 0x00, 0x01, 0x01 };
		
		uint_fast8_t profile_header_count_insert_index = 0x0F;

		uint_fast16_t profile_header_count = 1;

		uint_fast32_t 
			read_byte_index{},
			profile_header_count_inserted_tally = PROFILE_WITH_DATA_FILE_VEC_SIZE / SEGMENT_SIZE, // Approx. number of profile segments required. Does not include remainder profile/segments.
			profile_header_count_total_byte_value = profile_header_count_inserted_tally * PROFILE_HEADER_LENGTH,
			segment_tally = SEGMENT_SIZE + JPG_HEADER_LENGTH,
			last_segment_remainder_size = (PROFILE_WITH_DATA_FILE_VEC_SIZE % SEGMENT_SIZE) + (profile_header_count_total_byte_value - PROFILE_HEADER_LENGTH) - 4;
		
		while (PROFILE_WITH_DATA_FILE_VEC_SIZE > read_byte_index) {
			
			File_Vec.emplace_back(Profile_Vec[read_byte_index++]);

			if (read_byte_index == segment_tally) { // Another profile segment required.
				profile_header_count++;
				valueUpdater(Profile_Header_Vec, profile_header_count_insert_index, profile_header_count, bits);

				File_Vec.insert(File_Vec.begin() + segment_tally, Profile_Header_Vec.begin(), Profile_Header_Vec.end());
				segment_tally += SEGMENT_SIZE;	
			}
		}
		
		if (last_segment_remainder_size > SEGMENT_SIZE) {

			profile_header_count++;
			valueUpdater(Profile_Header_Vec, profile_header_count_insert_index, profile_header_count, bits);

			File_Vec.insert(File_Vec.begin() + segment_tally, Profile_Header_Vec.begin(), Profile_Header_Vec.end());
			profile_header_count_inserted_tally++;

		}
		
		switch (last_segment_remainder_size / SEGMENT_SIZE) {
  			case 1:
    				last_segment_remainder_size += PROFILE_HEADER_LENGTH - 1;
				valueUpdater(File_Vec, segment_tally + 2, last_segment_remainder_size, bits);
				break;
  			case 2:
		    		segment_tally += SEGMENT_SIZE;
				profile_header_count++;
				valueUpdater(Profile_Header_Vec, profile_header_count_insert_index, profile_header_count, bits);		

				File_Vec.insert(File_Vec.begin() + segment_tally, Profile_Header_Vec.begin(), Profile_Header_Vec.end());
				profile_header_count_inserted_tally++;
			
				last_segment_remainder_size += (PROFILE_HEADER_LENGTH * 2) - 2;
				valueUpdater(File_Vec, segment_tally + 2, last_segment_remainder_size, bits);
    				break;
			case 3:
				for (int repeat = 2; repeat > 0; repeat--) {
					segment_tally += SEGMENT_SIZE;
					profile_header_count++;
					valueUpdater(Profile_Header_Vec, profile_header_count_insert_index, profile_header_count, bits);		

					File_Vec.insert(File_Vec.begin() + segment_tally, Profile_Header_Vec.begin(), Profile_Header_Vec.end());
					profile_header_count_inserted_tally++;
				}
				
				last_segment_remainder_size += (PROFILE_HEADER_LENGTH * 3) - 3;
				valueUpdater(File_Vec, segment_tally + 2, last_segment_remainder_size, bits);
				break;
			case 4: 
				for (int repeat = 3; repeat > 0; repeat--) {
					segment_tally += SEGMENT_SIZE;
					profile_header_count++;
					valueUpdater(Profile_Header_Vec, profile_header_count_insert_index, profile_header_count, bits);		

					File_Vec.insert(File_Vec.begin() + segment_tally, Profile_Header_Vec.begin(), Profile_Header_Vec.end());
					profile_header_count_inserted_tally++;
				}
			
				last_segment_remainder_size += (PROFILE_HEADER_LENGTH * 4) - 4;
				valueUpdater(File_Vec, segment_tally + 2, last_segment_remainder_size, bits);	
				break;
  			default:
		    		segment_tally -= SEGMENT_SIZE;
				valueUpdater(File_Vec, segment_tally + 2, last_segment_remainder_size, bits);	
		}

		valueUpdater(File_Vec, PROFILE_TALLY_INDEX, profile_header_count_inserted_tally, bits);
		profile_header_count_inserted_tally++;

		constexpr uint_fast8_t	
			ICC_PROFILE_SIG[] { 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45 },
			profile_total_insert_index_diff = 13,
			pos_addition = 1,
			profile_tally_update_max = 255;
			
		uint_fast32_t profile_total_insert_index{}; 
		
		uint_fast16_t counter = profile_header_count_inserted_tally;
		
		// Within the relevant index position for each profile header found within File_Vec, insert the total value of inserted profile headers/segments.
		// This is a requirement for platforms such as Mastodon. Mastodon has a limit of 100 (0x64) profiles/segments. Which gives it a max storage size of ~6MB.
		while (counter--) {
			profile_total_insert_index = searchFunc(File_Vec, profile_total_insert_index, pos_addition, ICC_PROFILE_SIG) + profile_total_insert_index_diff;
			File_Vec[profile_total_insert_index] = profile_header_count_inserted_tally > profile_tally_update_max ? profile_tally_update_max : profile_header_count_inserted_tally;
		}	
		
		if (profile_header_count_inserted_tally > 100) {
			std::cout << "\n**Warning**\n\nEmbedded image is not compatible with Mastodon. Image file exceeds platform size limit.\n";
		}   
	}
	
	bits = 32; 
	valueUpdater(File_Vec, DEFLATED_DATA_FILE_SIZE_INDEX, DEFLATED_DATA_FILE_SIZE, bits);
}
