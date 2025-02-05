void segmentDataFile(std::vector<uint8_t>&Profile_Vec, std::vector<uint8_t>&File_Vec) {
	constexpr uint8_t
		SEGMENT_HEADER_LENGTH = 18,
		JPG_HEADER_LENGTH     = 20,
		APP2_SIG_LENGTH       = 2; 

	const uint32_t COLOR_PROFILE_WITH_DATA_FILE_VEC_SIZE = static_cast<uint32_t>(Profile_Vec.size());

	uint32_t segment_data_size = 65519;

	uint8_t value_bit_length = 16;	

	if (segment_data_size + JPG_HEADER_LENGTH + SEGMENT_HEADER_LENGTH >= COLOR_PROFILE_WITH_DATA_FILE_VEC_SIZE) { 
		constexpr uint8_t
			SEGMENT_HEADER_SIZE_INDEX = 0x16, 
			SEGMENT_TOTAL_VALUE_INDEX = 0x25,
			COLOR_PROFILE_SIZE_INDEX  = 0x28, 
			COLOR_PROFILE_SIZE_DIFF   = 16;	  
		
		const uint32_t 
			SEGMENT_SIZE 	   = COLOR_PROFILE_WITH_DATA_FILE_VEC_SIZE - (JPG_HEADER_LENGTH + APP2_SIG_LENGTH),
			COLOR_PROFILE_SIZE = SEGMENT_SIZE - COLOR_PROFILE_SIZE_DIFF;

		valueUpdater(Profile_Vec, SEGMENT_HEADER_SIZE_INDEX, SEGMENT_SIZE, value_bit_length);
		valueUpdater(Profile_Vec, COLOR_PROFILE_SIZE_INDEX, COLOR_PROFILE_SIZE, value_bit_length);
		Profile_Vec[SEGMENT_TOTAL_VALUE_INDEX] = 1; 
		File_Vec = std::move(Profile_Vec);
	} else { 
		constexpr uint8_t LIBSODIUM_DISCREPANCY_VALUE = 38;

		const uint32_t NEW_COLOR_PROFILE_WITH_DATA_FILE_VEC_SIZE = COLOR_PROFILE_WITH_DATA_FILE_VEC_SIZE - LIBSODIUM_DISCREPANCY_VALUE;

		uint32_t 
			segments_required_approx_val = (NEW_COLOR_PROFILE_WITH_DATA_FILE_VEC_SIZE) / segment_data_size,
			byte_index 		     = 0;

		const uint32_t 
			SEGMENT_REMAINDER_SIZE = (NEW_COLOR_PROFILE_WITH_DATA_FILE_VEC_SIZE) % segment_data_size,
			FIRST_SEGMENT_DATA_SIZE = segment_data_size + JPG_HEADER_LENGTH + SEGMENT_HEADER_LENGTH;

		constexpr uint8_t SEGMENT_REMAINDER_DIFF = 16;
		constexpr uint16_t SEGMENTS_TOTAL_VAL_INDEX = 0x207;  
		
		valueUpdater(Profile_Vec, SEGMENTS_TOTAL_VAL_INDEX, segments_required_approx_val, value_bit_length);

		std::vector<std::vector<uint8_t>> Segments_Arr_Vec;
	
		segment_data_size = FIRST_SEGMENT_DATA_SIZE;

		uint8_t 
			segments_sequence_value_index = 0x0F,
			segment_remainder_size_index = 2;

		uint16_t segments_sequence_value = 1;

		std::vector<uint8_t> Segment_Vec { 0xFF, 0xE2, 0xFF, 0xFF, 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45, 0x00, 0x00, 0xFF };
		Segment_Vec.reserve(segment_data_size + SEGMENT_REMAINDER_SIZE);

		if (SEGMENT_REMAINDER_SIZE) {
    			segments_required_approx_val++;
		}	

		while (segments_required_approx_val--) {		
			if (!segments_required_approx_val && SEGMENT_REMAINDER_SIZE) {
				segment_data_size = SEGMENT_REMAINDER_SIZE;		
			   	valueUpdater(Segment_Vec, segment_remainder_size_index, SEGMENT_REMAINDER_SIZE + SEGMENT_REMAINDER_DIFF, value_bit_length); 		
			}

			std::copy_n(Profile_Vec.begin() + byte_index, segment_data_size, std::back_inserter(Segment_Vec));
			byte_index += segment_data_size;	
			
			if (segment_data_size == FIRST_SEGMENT_DATA_SIZE) {
       			 	Segments_Arr_Vec.emplace_back(Segment_Vec.begin() + SEGMENT_HEADER_LENGTH, Segment_Vec.end());
			} else {
        			Segments_Arr_Vec.emplace_back(Segment_Vec);
    			}

    			Segment_Vec.erase(Segment_Vec.begin() + SEGMENT_HEADER_LENGTH, Segment_Vec.end());
			segment_data_size = 65519;
			
			valueUpdater(Segment_Vec, segments_sequence_value_index, ++segments_sequence_value, value_bit_length);
		}
		
		std::vector<uint8_t>().swap(Profile_Vec);
		File_Vec.reserve(segment_data_size * segments_sequence_value);

		for (auto& vec : Segments_Arr_Vec) {
        		File_Vec.insert(File_Vec.end(), vec.begin(), vec.end());
			std::vector<uint8_t>().swap(vec);
    		}
		std::vector<std::vector<uint8_t>>().swap(Segments_Arr_Vec);
		
		constexpr uint8_t MASTODON_SEGMENTS_LIMIT = 100;		   
		constexpr uint32_t MASTODON_IMAGE_UPLOAD_LIMIT = 16 * 1024 * 1024; 
					   
		if (segments_sequence_value > MASTODON_SEGMENTS_LIMIT && MASTODON_IMAGE_UPLOAD_LIMIT > NEW_COLOR_PROFILE_WITH_DATA_FILE_VEC_SIZE) {
			std::cout << "\n**Warning**\n\nEmbedded image is not compatible with Mastodon. Image file exceeds platform's segments limit.\n";
		}
	}
	value_bit_length = 32; 

	constexpr uint16_t 
		DEFLATED_DATA_FILE_SIZE_INDEX = 0x203,
		PROFILE_SIZE = 901; 

	valueUpdater(File_Vec, DEFLATED_DATA_FILE_SIZE_INDEX, static_cast<uint32_t>(File_Vec.size()) - PROFILE_SIZE, value_bit_length);
}
