// If required, split & store users data file into multiple APP2 profile segments. 
// The first APP2 segment contains the color profile data, followed by the users data file.
// Additional segments start with the 18 byte JPG APP2 profile header, followed by the data file.
void insertProfileHeaders(std::vector<uint8_t>&Profile_Vec, std::vector<uint8_t>&File_Vec) {
	constexpr uint8_t
		PROFILE_SEGMENT_HEADER_LENGTH 	= 18,
		JPG_HEADER_LENGTH 		= 20,
		APP2_SIG_LENGTH 		= 2; // FFE2.

	uint32_t 
		profile_with_data_file_vec_size = static_cast<uint32_t>(Profile_Vec.size()),
		segment_data_size = 65519;

	uint8_t value_bit_length = 16;	

	if (segment_data_size + JPG_HEADER_LENGTH + PROFILE_SEGMENT_HEADER_LENGTH >= profile_with_data_file_vec_size) { 
		// Data file is small enough to fit all within the first/main APP2 profile segment, appended to the color profile data.
		constexpr uint8_t
			PROFILE_HEADER_SEGMENT_SIZE_INDEX = 0x16, // Two byte JPG APP2 profile header segment size field index.
			PROFILE_SIZE_INDEX 		  = 0x28, // Four byte ICC profile size field index.
			PROFILE_SIZE_DIFF  		  = 16;
		
		const uint32_t 
			PROFILE_HEADER_SEGMENT_SIZE = profile_with_data_file_vec_size - (JPG_HEADER_LENGTH + APP2_SIG_LENGTH),
			PROFILE_SIZE 		    = PROFILE_HEADER_SEGMENT_SIZE - PROFILE_SIZE_DIFF;

		valueUpdater(Profile_Vec, PROFILE_HEADER_SEGMENT_SIZE_INDEX, PROFILE_HEADER_SEGMENT_SIZE, value_bit_length);
		valueUpdater(Profile_Vec, PROFILE_SIZE_INDEX, PROFILE_SIZE, value_bit_length);

		File_Vec = std::move(Profile_Vec);
	} else { 
		// Data file is too large for the first APP2 profile segment. Create additional segments as needed, to store the data file.
		uint32_t 
			segments_approx_val 	= profile_with_data_file_vec_size / segment_data_size,
			remainder_size	 	= profile_with_data_file_vec_size % segment_data_size,
			byte_index 		= 0;
		
		constexpr uint8_t 
			PROFILE_SEGMENT_HEADER[PROFILE_SEGMENT_HEADER_LENGTH] { 0xFF, 0xE2, 0xFF, 0xFF, 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45, 0x00, 0x01, 0x01 },
			SEGMENTS_TOTAL_VAL_INDEX = 0x8A;  // Index location within color profile data area, to store total value of APP2 profile segments (-1). For jdvout.
		
		uint16_t segments_total_val = segments_approx_val;
		
		// Write total number of APP2 profile segments (minus the first one) within the index position of the first profile segment. For jdvout.
		valueUpdater(Profile_Vec, SEGMENTS_TOTAL_VAL_INDEX, segments_total_val, value_bit_length);

		// Create vector of vectors to store the individual segment vectors.
		std::vector<std::vector<uint8_t>> Segments_Arr_Vec;
		
		while (segments_approx_val--) {	
			std::vector<uint8_t> Segment_Vec;
			Segment_Vec.reserve(segment_data_size + remainder_size);
			if (byte_index) {
				Segment_Vec.insert(Segment_Vec.end(), std::begin(PROFILE_SEGMENT_HEADER), std::end(PROFILE_SEGMENT_HEADER));
			}
			
			segment_data_size = (!byte_index) ? segment_data_size + JPG_HEADER_LENGTH + PROFILE_SEGMENT_HEADER_LENGTH : segment_data_size;
			
			while (segment_data_size--) {
				Segment_Vec.emplace_back(Profile_Vec[byte_index++]);
			}
			
    			Segments_Arr_Vec.emplace_back(Segment_Vec);
			segment_data_size = 65519;
		}
	
		if (remainder_size) {
			constexpr uint8_t REMAINDER_DIFF = 16;
			uint8_t remainder_size_insert_index = 2;
			std::vector<uint8_t> Segment_Vec;
			Segment_Vec.reserve(segment_data_size + remainder_size);
			Segment_Vec.insert(Segment_Vec.end(), std::begin(PROFILE_SEGMENT_HEADER), std::end(PROFILE_SEGMENT_HEADER));			

			valueUpdater(Segment_Vec, remainder_size_insert_index, remainder_size + REMAINDER_DIFF, value_bit_length);
			
			while (remainder_size--) {
				Segment_Vec.emplace_back(Profile_Vec[byte_index++]);	
			}
			Segments_Arr_Vec.emplace_back(Segment_Vec);
		}

		std::vector<uint8_t>().swap(Profile_Vec);
		File_Vec.reserve(segment_data_size * segments_total_val);
		for (auto& vec : Segments_Arr_Vec) {
        		File_Vec.insert(File_Vec.end(), vec.begin(), vec.end());
			std::vector<uint8_t>().swap(vec);
    		}
		std::vector<std::vector<uint8_t>>().swap(Segments_Arr_Vec);
		
		// The final section deals with updating and storing various values required for the image and/or for the extraction program, jdvout.
	
		uint32_t 
			segments_sequence_val_index = 0,
			segments_total_val_index = 0; 
		
		uint16_t 
			segments_sequence_val = 1,
			total_segments = ++segments_total_val;

		constexpr uint8_t	
			ICC_PROFILE_SIG[] { 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45 },
			SEGMENTS_TOTAL_INDEX_DIFF 	= 0x0D,
			SEGMENTS_SEQUENCE_INDEX_DIFF	= 0x02,
			MASTODON_SEGMENTS_LIMIT 	= 100,
			MAX_SEGMENTS 			= 255, // We can have more segments than this total segments value, but we can't increase the number, so it stays 255 for all additional segments. 
							       // See the segments sequence value for the correct number of segments.
			POS_ADDITION 			= 1;
					   
		constexpr uint32_t MASTODON_IMAGE_UPLOAD_LIMIT = 16777216;
					   
		// Within the relevant index positions for each APP2 profile header found within File_Vec, write the total value & individual sequence value of inserted profile headers/segments.
		// This is a requirement for image viewers and platforms such as Mastodon. Mastodon has a limit of 100 (0x64) segments, which gives it a maximum storage size of ~6MB.
		// For the profile sequence count, we are using two bytes to store the value. While this is non-standard it provides the best compatibility (imo) for embedding files over 16MB.
		while (total_segments--) {
			segments_total_val_index = searchFunc(File_Vec, segments_total_val_index, POS_ADDITION, ICC_PROFILE_SIG) + SEGMENTS_TOTAL_INDEX_DIFF;
			segments_sequence_val_index = segments_total_val_index - SEGMENTS_SEQUENCE_INDEX_DIFF; 
			File_Vec[segments_total_val_index] = segments_total_val > MAX_SEGMENTS ? MAX_SEGMENTS : segments_total_val;		
			valueUpdater(File_Vec, segments_sequence_val_index, segments_sequence_val, value_bit_length);
			segments_sequence_val++;
		}	
		if (segments_total_val > MASTODON_SEGMENTS_LIMIT && MASTODON_IMAGE_UPLOAD_LIMIT > profile_with_data_file_vec_size) {
			std::cout << "\n**Warning**\n\nEmbedded image is not compatible with Mastodon. Image file exceeds platform's segments limit.\n";
		}
	}
	value_bit_length = 32; 

	constexpr uint8_t DEFLATED_DATA_FILE_SIZE_INSERT_INDEX = 0x90;  
	
	constexpr uint16_t PROFILE_SIZE = 912; // Includes JPG header, profile/segment header and color profile data.
	
	// Write the compressed file size of the data file, which now includes multiple segments with the 18 byte profile/segment headers,
	// minus profile size, within index position of the profile data section. Value used by jdvout.	
	valueUpdater(File_Vec, DEFLATED_DATA_FILE_SIZE_INSERT_INDEX, static_cast<uint32_t>(File_Vec.size()) - PROFILE_SIZE, value_bit_length);
}
