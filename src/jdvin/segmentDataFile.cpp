bool segmentDataFile(std::vector<uint8_t>&profile_vec, std::vector<uint8_t>&data_file_vec) {
	constexpr uint8_t
		SEGMENT_HEADER_LENGTH = 18,
		JPG_HEADER_LENGTH     = 20,
		APP2_SIG_LENGTH       = 2; 

	const uint32_t COLOR_PROFILE_WITH_DATA_FILE_VEC_SIZE = static_cast<uint32_t>(profile_vec.size());

	uint32_t segment_data_size = 65519;

	uint8_t value_bit_length = 16;	

	bool shouldDisplayMastodonWarning = false;

	if (segment_data_size + JPG_HEADER_LENGTH + SEGMENT_HEADER_LENGTH >= COLOR_PROFILE_WITH_DATA_FILE_VEC_SIZE) { 
		constexpr uint8_t
			SEGMENT_HEADER_SIZE_INDEX = 0x16, 
			SEGMENT_TOTAL_VALUE_INDEX = 0x25,
			COLOR_PROFILE_SIZE_INDEX  = 0x28, 
			COLOR_PROFILE_SIZE_DIFF   = 16;	  
		
		const uint32_t 
			SEGMENT_SIZE 	   = COLOR_PROFILE_WITH_DATA_FILE_VEC_SIZE - (JPG_HEADER_LENGTH + APP2_SIG_LENGTH),
			COLOR_PROFILE_SIZE = SEGMENT_SIZE - COLOR_PROFILE_SIZE_DIFF;

		valueUpdater(profile_vec, SEGMENT_HEADER_SIZE_INDEX, SEGMENT_SIZE, value_bit_length);
		valueUpdater(profile_vec, COLOR_PROFILE_SIZE_INDEX, COLOR_PROFILE_SIZE, value_bit_length);
		profile_vec[SEGMENT_TOTAL_VALUE_INDEX] = 1; 
		data_file_vec = std::move(profile_vec);
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
			
		valueUpdater(profile_vec, SEGMENTS_TOTAL_VAL_INDEX, segments_required_approx_val, value_bit_length);

		segment_data_size = FIRST_SEGMENT_DATA_SIZE;

		uint8_t 
			segments_sequence_value_index = 0x0F,
			segment_remainder_size_index = 2;

		uint16_t segments_sequence_value = 1;

		std::vector<uint8_t> segment_vec { 0xFF, 0xE2, 0xFF, 0xFF, 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45, 0x00, 0x00, 0xFF };
		segment_vec.reserve(segment_data_size + SEGMENT_REMAINDER_SIZE);

		if (SEGMENT_REMAINDER_SIZE) {
    			segments_required_approx_val++;
		}	

		data_file_vec.reserve(segment_data_size * segments_sequence_value);

		while (segments_required_approx_val--) {		
			if (!segments_required_approx_val && SEGMENT_REMAINDER_SIZE) {
				segment_data_size = SEGMENT_REMAINDER_SIZE;		
			   	valueUpdater(segment_vec, segment_remainder_size_index, SEGMENT_REMAINDER_SIZE + SEGMENT_REMAINDER_DIFF, value_bit_length); 		
			}
			std::copy_n(profile_vec.begin() + byte_index, segment_data_size, std::back_inserter(segment_vec));
			byte_index += segment_data_size;	
			
			if (segment_data_size == FIRST_SEGMENT_DATA_SIZE) {
			         data_file_vec.insert(data_file_vec.end(), segment_vec.begin() + SEGMENT_HEADER_LENGTH, segment_vec.end());
			} else {
				data_file_vec.insert(data_file_vec.end(), segment_vec.begin(), segment_vec.end());
    			}

    			segment_vec.erase(segment_vec.begin() + SEGMENT_HEADER_LENGTH, segment_vec.end());
			segment_data_size = 65519;
			
			valueUpdater(segment_vec, segments_sequence_value_index, ++segments_sequence_value, value_bit_length);
		}
		
		std::vector<uint8_t>().swap(profile_vec);
		
		constexpr uint8_t MASTODON_SEGMENTS_LIMIT = 100;		   
		constexpr uint32_t MASTODON_IMAGE_UPLOAD_LIMIT = 16 * 1024 * 1024; 
					   
		shouldDisplayMastodonWarning = segments_sequence_value > MASTODON_SEGMENTS_LIMIT && MASTODON_IMAGE_UPLOAD_LIMIT > NEW_COLOR_PROFILE_WITH_DATA_FILE_VEC_SIZE;
	}
	value_bit_length = 32; 

	constexpr uint16_t 
		DEFLATED_DATA_FILE_SIZE_INDEX = 0x203,
		PROFILE_SIZE = 901; 
	
	valueUpdater(data_file_vec, DEFLATED_DATA_FILE_SIZE_INDEX, static_cast<uint32_t>(data_file_vec.size()) - PROFILE_SIZE, value_bit_length);

	return shouldDisplayMastodonWarning;
}