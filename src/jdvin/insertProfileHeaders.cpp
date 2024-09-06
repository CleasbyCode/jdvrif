// If required, split and store data file into multiple color profile segment blocks.
void insertProfileHeaders(std::vector<uint_fast8_t>&Profile_Vec, std::vector<uint_fast8_t>&File_Vec) {
	constexpr uint_fast8_t
		PROFILE_HEADER_LENGTH = 18,
		PROFILE_HEADER_SEGMENT_SIZE_INDEX = 0x16, // Two byte JPG color profile header segment size field index.	
		PROFILE_SIZE_INDEX = 0x28,		  // Four byte profile size field index.	
		PROFILE_HEADER_TALLY_INDEX = 0x8A,	  // Index location within ICC Profile where we store the value of total inserted profile headers/segments. Value used by jdvout.
		DEFLATED_DATA_FILE_SIZE_INDEX = 0x90,
		JPG_HEADER_LENGTH = 20;

	constexpr uint_fast16_t COLOR_PROFILE_SIZE = 663;

	constexpr uint_fast32_t SEGMENT_SIZE = 65537;

	const uint_fast32_t 
		PROFILE_WITH_DATA_FILE_VEC_SIZE = static_cast<uint_fast32_t>(Profile_Vec.size()),
		DEFLATED_DATA_FILE_SIZE = PROFILE_WITH_DATA_FILE_VEC_SIZE - COLOR_PROFILE_SIZE;
	
	uint_fast8_t value_bit_length = 16;	
		
	// Default profile and data file fit within the first profile segment block.
	if (SEGMENT_SIZE + JPG_HEADER_LENGTH >= PROFILE_WITH_DATA_FILE_VEC_SIZE) {
		constexpr uint_fast8_t SIZE_DIFF = 16;
		const uint_fast32_t
			PROFILE_HEADER_SEGMENT_SIZE = PROFILE_WITH_DATA_FILE_VEC_SIZE - (PROFILE_HEADER_LENGTH + 4), 
			PROFILE_SIZE = PROFILE_HEADER_SEGMENT_SIZE - SIZE_DIFF;

		valueUpdater(Profile_Vec, PROFILE_HEADER_SEGMENT_SIZE_INDEX, PROFILE_HEADER_SEGMENT_SIZE, value_bit_length);
		valueUpdater(Profile_Vec, PROFILE_SIZE_INDEX, PROFILE_SIZE, value_bit_length);

		File_Vec.swap(Profile_Vec);
		
	} else { // Data file is too large for just the first profile segment. Create additional profile segments as needed, to store the data file.
		constexpr uint_fast8_t PROFILE_HEADER[] { 0xFF, 0xE2, 0xFF, 0xFF, 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45, 0x00, 0x01, 0x01 };
		
		uint_fast32_t 
			read_byte_index{},
			profile_header_count_inserted_tally = PROFILE_WITH_DATA_FILE_VEC_SIZE / SEGMENT_SIZE, // Approx. number of profile segments required. Does not include last remainder profile/segment(s).
			profile_header_count_total_byte_value = profile_header_count_inserted_tally * PROFILE_HEADER_LENGTH,			
			last_segment_remainder_size = (PROFILE_WITH_DATA_FILE_VEC_SIZE % SEGMENT_SIZE) + profile_header_count_total_byte_value,
			segment_tally = SEGMENT_SIZE + JPG_HEADER_LENGTH;

		while (PROFILE_WITH_DATA_FILE_VEC_SIZE > read_byte_index) {
			File_Vec.emplace_back(Profile_Vec[read_byte_index++]);
			if (read_byte_index == segment_tally) { // Another profile segment required.
				File_Vec.insert(File_Vec.begin() + segment_tally, std::begin(PROFILE_HEADER), std::end(PROFILE_HEADER));
				segment_tally += SEGMENT_SIZE;	
			}
		}
		
		auto insert_profile_header = [&](int_fast8_t repeat_val, const int_fast8_t DIFF_VAL) {
			while (repeat_val--) {  // Split the last remainder size into required number of segments. 
				File_Vec.insert(File_Vec.begin() + segment_tally, std::begin(PROFILE_HEADER), std::end(PROFILE_HEADER));
        			profile_header_count_inserted_tally++;
				if (repeat_val) {
					segment_tally += SEGMENT_SIZE;
				}
		     	}
			last_segment_remainder_size += DIFF_VAL;
			valueUpdater(File_Vec, segment_tally + 2, last_segment_remainder_size, value_bit_length);
		};
				
		constexpr int_fast8_t DIFF_VALUE[] { -5, 12, 29, 46, 63, -22};  // Adjustment values required for correct last segment size.

		switch (last_segment_remainder_size / SEGMENT_SIZE) {
  			case 1:
				insert_profile_header(1, DIFF_VALUE[0]);
				break;
  			case 2:
				insert_profile_header(2, DIFF_VALUE[1]);
    				break;
			case 3:
				insert_profile_header(3, DIFF_VALUE[2]);
				break;
			case 4: 
				insert_profile_header(4, DIFF_VALUE[3]);
				break;
			case 5:						
				insert_profile_header(5, DIFF_VALUE[4]);
				break;
  			default:
		    		segment_tally -= SEGMENT_SIZE;
				insert_profile_header(0, DIFF_VALUE[5]);
		}

		valueUpdater(File_Vec, PROFILE_HEADER_TALLY_INDEX, profile_header_count_inserted_tally, value_bit_length);

		uint_fast32_t 
			profile_header_count_insert_index{},
			profile_header_total_insert_index{}; 
		
		uint_fast16_t 
			profile_header_insert_count = 1,
			counter = ++profile_header_count_inserted_tally;

		constexpr uint_fast8_t	
			ICC_PROFILE_SIG[] { 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45 },
			PROFILE_HEADER_TOTAL_INSERT_INDEX_DIFF = 0x0D,
			PROFILE_HEADER_TALLY_MAX = 255,
			POS_ADDITION = 1;		
		
		// Within the relevant index positions for each profile header found within File_Vec, insert the total value and individual current count value of inserted profile headers/segments.
		// This is a requirement for image viewers and platforms such as Mastodon. Mastodon has a limit of 100 (0x64) profiles/segments, which gives it a max storage size of ~6MB.
		while (counter--) {
			profile_header_total_insert_index = searchFunc(File_Vec, profile_header_total_insert_index, POS_ADDITION, ICC_PROFILE_SIG) + PROFILE_HEADER_TOTAL_INSERT_INDEX_DIFF;
			profile_header_count_insert_index = profile_header_total_insert_index - 2; 
			File_Vec[profile_header_total_insert_index] = profile_header_count_inserted_tally > PROFILE_HEADER_TALLY_MAX ? PROFILE_HEADER_TALLY_MAX : profile_header_count_inserted_tally;
				
			while (value_bit_length) {
				static_cast<uint_fast16_t>(File_Vec[profile_header_count_insert_index++] = (profile_header_insert_count >> (value_bit_length -= 8)) & 0xff);
			}
			value_bit_length = 16;	
			profile_header_insert_count++;
		}	
		if (profile_header_count_inserted_tally > 100) {
			std::cout << "\n**Warning**\n\nEmbedded image is not compatible with Mastodon. Image file exceeds platform size limit.\n";
		}   
	}
	value_bit_length = 32; 
	valueUpdater(File_Vec, DEFLATED_DATA_FILE_SIZE_INDEX, DEFLATED_DATA_FILE_SIZE, value_bit_length);
}
