void splitDataFile(std::vector<uint8_t>&segment_vec, std::vector<uint8_t>&data_file_vec, bool& shouldDisplayMastodonWarning) {
	constexpr uint8_t
		PROFILE_HEADER_SEGMENT_LENGTH 	= 18,
		APP2_SIG_LENGTH       		= 2; 

	constexpr uint32_t MAX_FIRST_SEGMENT_SIZE = 65539;

	uint32_t 
		color_profile_with_data_file_vec_size = static_cast<uint32_t>(segment_vec.size()),
		segment_data_size = 65519;

	uint8_t val_bit_length = 16;

	if (color_profile_with_data_file_vec_size > MAX_FIRST_SEGMENT_SIZE) { // Data file is too large for a single segment, so split data file in to multiple segments.
		constexpr uint8_t LIBSODIUM_DISCREPANCY_VAL = 20;

		color_profile_with_data_file_vec_size -= LIBSODIUM_DISCREPANCY_VAL;

		uint16_t 
			segments_required_val	= (color_profile_with_data_file_vec_size / segment_data_size) + 1,
			segment_remainder_size 	= color_profile_with_data_file_vec_size % segment_data_size,
			segments_sequence_val = 1;
			
		constexpr uint16_t SEGMENTS_TOTAL_VAL_INDEX = 0x2E0;  
		valueUpdater(segment_vec, SEGMENTS_TOTAL_VAL_INDEX, !segment_remainder_size ? --segments_required_val : segments_required_val, val_bit_length);

		segment_data_size = MAX_FIRST_SEGMENT_SIZE;

		uint8_t 
			segments_sequence_val_index = 0x0F,
			segment_remainder_size_index = 0x02,
			temp_header_length = PROFILE_HEADER_SEGMENT_LENGTH;

		std::vector<uint8_t> split_vec { 0xFF, 0xE2, 0xFF, 0xFF, 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45, 0x00, 0x00, 0xFF };
		split_vec.reserve(MAX_FIRST_SEGMENT_SIZE);

		data_file_vec.reserve(segment_data_size * segments_required_val);
		
		uint32_t byte_index = 0;
	
		while (segments_required_val--) {	
			if (!segments_required_val && segment_remainder_size) {
				segment_data_size = segment_remainder_size;	
				segment_remainder_size += PROFILE_HEADER_SEGMENT_LENGTH - APP2_SIG_LENGTH;
			   	valueUpdater(split_vec, segment_remainder_size_index, segment_remainder_size, val_bit_length); 		
			}
			std::copy_n(segment_vec.begin() + byte_index, segment_data_size, std::back_inserter(split_vec));
			byte_index += segment_data_size;	
			
			data_file_vec.insert(data_file_vec.end(), split_vec.begin() + temp_header_length, split_vec.end()); 
    		
			temp_header_length = 0;

    			split_vec.resize(PROFILE_HEADER_SEGMENT_LENGTH); // Keep the segment profile header, delete all other content. 
			segment_data_size = 65519;
			
			valueUpdater(split_vec, segments_sequence_val_index, ++segments_sequence_val, val_bit_length);
		}

		std::vector<uint8_t>().swap(segment_vec);
		std::vector<uint8_t>().swap(split_vec);
		
		constexpr uint8_t MASTODON_SEGMENTS_LIMIT = 100;		   
		constexpr uint32_t MASTODON_IMAGE_UPLOAD_LIMIT = 16 * 1024 * 1024; 
					   
		shouldDisplayMastodonWarning = segments_sequence_val > MASTODON_SEGMENTS_LIMIT && MASTODON_IMAGE_UPLOAD_LIMIT > color_profile_with_data_file_vec_size;
	} else {  // Data file is small enough to fit within a single color profile segment...
		constexpr uint8_t
			SEGMENT_HEADER_SIZE_INDEX = 0x04, 
			SEGMENT_TOTAL_VAL_INDEX   = 0x13,
			COLOR_PROFILE_SIZE_INDEX  = 0x16, 
			COLOR_PROFILE_SIZE_DIFF   = 16,
			JPG_SIG_LENGTH		  = 2;
			
		const uint16_t 
			SEGMENT_SIZE 	   = color_profile_with_data_file_vec_size - (JPG_SIG_LENGTH + APP2_SIG_LENGTH),
			COLOR_PROFILE_SIZE = SEGMENT_SIZE - COLOR_PROFILE_SIZE_DIFF;

		valueUpdater(segment_vec, SEGMENT_HEADER_SIZE_INDEX, SEGMENT_SIZE, val_bit_length);
		valueUpdater(segment_vec, COLOR_PROFILE_SIZE_INDEX, COLOR_PROFILE_SIZE, val_bit_length);

		segment_vec[SEGMENT_TOTAL_VAL_INDEX] = 1; 
		data_file_vec = std::move(segment_vec);
	}
		
	val_bit_length = 32; 

	constexpr uint16_t 
		DEFLATED_DATA_FILE_SIZE_INDEX = 0x2E2,
		PROFILE_SIZE = 851; 
	
	valueUpdater(data_file_vec, DEFLATED_DATA_FILE_SIZE_INDEX, static_cast<uint32_t>(data_file_vec.size()) - PROFILE_SIZE, val_bit_length);
}
