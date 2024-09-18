// If required, split and store data file into multiple ICC Profile segments. 
// The first/main ICC Profile contains the color profile data, followed by the embedded data file.
// Additional profile segments just contain the 18 byte ICC Profile header, followed by the embedded data file.
void insertProfileHeaders(std::vector<uint_fast8_t>&Profile_Vec, std::vector<uint_fast8_t>&File_Vec) {

	constexpr uint_fast8_t
		PROFILE_HEADER_LENGTH 	= 18,
		JPG_HEADER_LENGTH 	= 20,
		APP2_SIG_LENGTH 	= 2; 	// 0xFFE2.
		
	constexpr uint_fast32_t SEGMENT_SIZE = 65537;

	const uint_fast32_t PROFILE_WITH_DATA_FILE_VEC_SIZE = static_cast<uint_fast32_t>(Profile_Vec.size());
			
	uint_fast8_t value_bit_length = 16;	
		
	if (SEGMENT_SIZE + JPG_HEADER_LENGTH >= PROFILE_WITH_DATA_FILE_VEC_SIZE) { 
		// Data file is small enough to fit within the first/main ICC Profile segment, along with the color profile data.
		constexpr uint_fast8_t
			PROFILE_HEADER_SEGMENT_SIZE_INDEX = 0x16, // Two byte JPG ICC Profile header segment size field index.
			PROFILE_SIZE_INDEX 		  = 0x28, // Four byte ICC Profile size field index.
			PROFILE_SIZE_DIFF  		  = 16;

		const uint_fast32_t 
			PROFILE_HEADER_SEGMENT_SIZE = PROFILE_WITH_DATA_FILE_VEC_SIZE - (JPG_HEADER_LENGTH + APP2_SIG_LENGTH),
			PROFILE_SIZE 		    = PROFILE_HEADER_SEGMENT_SIZE - PROFILE_SIZE_DIFF;

		valueUpdater(Profile_Vec, PROFILE_HEADER_SEGMENT_SIZE_INDEX, PROFILE_HEADER_SEGMENT_SIZE, value_bit_length);
		valueUpdater(Profile_Vec, PROFILE_SIZE_INDEX, PROFILE_SIZE, value_bit_length);

		File_Vec.swap(Profile_Vec);
	} else { 
		// Data file is too large for the first/main ICC Profile segment. Create additional profile segments as needed, to store the data file.
		constexpr uint_fast8_t PROFILE_HEADER[] { 0xFF, 0xE2, 0xFF, 0xFF, 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45, 0x00, 0x01, 0x01 };
		
		uint_fast32_t 
			read_byte_index 			= 0,
			profile_headers_approx_count 		= PROFILE_WITH_DATA_FILE_VEC_SIZE / SEGMENT_SIZE, 
			profile_headers_tally 			= (PROFILE_WITH_DATA_FILE_VEC_SIZE % SEGMENT_SIZE) / SEGMENT_SIZE + profile_headers_approx_count,
			profile_headers_total_byte_value 	= (profile_headers_tally * PROFILE_HEADER_LENGTH) - (JPG_HEADER_LENGTH + APP2_SIG_LENGTH),	
			final_segments_remainder_size 		= (PROFILE_WITH_DATA_FILE_VEC_SIZE % SEGMENT_SIZE) + profile_headers_total_byte_value,
			final_segments_profile_headers_count 	= final_segments_remainder_size / SEGMENT_SIZE,
			segment_tally 				= SEGMENT_SIZE + JPG_HEADER_LENGTH;

		File_Vec.reserve(PROFILE_WITH_DATA_FILE_VEC_SIZE + profile_headers_total_byte_value);
		
		// Insert the majority of profile headers/segments here.
		while (PROFILE_WITH_DATA_FILE_VEC_SIZE > read_byte_index) {
			File_Vec.emplace_back(Profile_Vec[read_byte_index++]);
			if (read_byte_index == segment_tally) { 
				// Another profile segment required.
				File_Vec.insert(File_Vec.begin() + segment_tally, std::begin(PROFILE_HEADER), std::end(PROFILE_HEADER));
				segment_tally += SEGMENT_SIZE;	
			}
		}
	
		// This next section we deal with remainder of data file and split that into profile segments if required.

		auto insert_remainder_segments = [&](int_fast8_t repeat_val) {
			uint_fast8_t index_diff_value = 0;
			while (repeat_val--) {  // Split the remainder size into required number of segments. 
				File_Vec.insert(File_Vec.begin() + segment_tally, std::begin(PROFILE_HEADER), std::end(PROFILE_HEADER));
        			profile_headers_tally++;
				index_diff_value += PROFILE_HEADER_LENGTH - 1;
				if (repeat_val) {
					segment_tally += SEGMENT_SIZE;
				}
			}
			final_segments_remainder_size += index_diff_value;
			uint_fast32_t segment_size_index = segment_tally + 2;
			valueUpdater(File_Vec, segment_size_index, final_segments_remainder_size, value_bit_length);
		};
		
		// Additional remainder segments to create, based on count value here.
		if (final_segments_profile_headers_count) {
			insert_remainder_segments(final_segments_profile_headers_count);
		} else {
			segment_tally -= SEGMENT_SIZE;
			insert_remainder_segments(final_segments_profile_headers_count);
		}
			
		// The final section deals with updating and storing various values required for the image and/or for the extraction program, jdvout.
	
		constexpr uint_fast8_t PROFILE_HEADER_TALLY_INDEX = 0x8A;  // Index location within ICC Profile where we store the total value of inserted profile headers/segments (-1). For jdvout.
		
		// Write total number of profile headers / segments (not including the first one) within the index position of the main ICC Profile. For jdvout.
		valueUpdater(File_Vec, PROFILE_HEADER_TALLY_INDEX, profile_headers_tally, value_bit_length);

		uint_fast32_t 
			profile_headers_sequence_index 	= 0,
			profile_headers_total_index 	= 0; 
		
		uint_fast16_t 
			profile_headers_sequence = 1,
			counter = ++profile_headers_tally;

		constexpr uint_fast8_t	
			ICC_PROFILE_SIG[] { 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45 },
			PROFILE_HEADERS_TOTAL_INDEX_DIFF 	= 0x0D,
			PROFILE_HEADERS_SEQUENCE_INDEX_DIFF 	= 0x02,
			MASTODON_PROFILE_LIMIT 			= 100,
			PROFILE_HEADERS_MAX 			= 255,
			POS_ADDITION 				= 1;
					   
		constexpr uint_fast32_t MASTODON_IMAGE_UPLOAD_LIMIT = 16777216;
					   
		// Within the relevant index positions for each ICC Profile header found within File_Vec, write the total value & individual sequence value of inserted profile headers/segments.
		// This is a requirement for image viewers and platforms such as Mastodon. Mastodon has a limit of 100 (0x64) profiles/segments, which gives it a Max. storage size of ~6MB.
		// For the profile sequence count, we are using two bytes to store the value. While this is non-standard it provides the best compatibility (imo) for embedding files over 16MB.
		while (counter--) {
			profile_headers_total_index = searchFunc(File_Vec, profile_headers_total_index, POS_ADDITION, ICC_PROFILE_SIG) + PROFILE_HEADERS_TOTAL_INDEX_DIFF;
			profile_headers_sequence_index = profile_headers_total_index - PROFILE_HEADERS_SEQUENCE_INDEX_DIFF; 
			File_Vec[profile_headers_total_index] = profile_headers_tally > PROFILE_HEADERS_MAX ? PROFILE_HEADERS_MAX : profile_headers_tally;		
			while (value_bit_length) {
				static_cast<uint_fast16_t>(File_Vec[profile_headers_sequence_index++] = (profile_headers_sequence >> (value_bit_length -= 8)) & 0xff);
			}
			value_bit_length = 16;	
			profile_headers_sequence++;
		}	
		if (profile_headers_tally > MASTODON_PROFILE_LIMIT && PROFILE_WITH_DATA_FILE_VEC_SIZE < MASTODON_IMAGE_UPLOAD_LIMIT) {
			std::cout << "\n**Warning**\n\nEmbedded image is not compatible with Mastodon. Image file exceeds platform's profile limit.\n";
		}
	}
	value_bit_length = 32; 

	constexpr uint_fast8_t DEFLATED_DATA_FILE_SIZE_INDEX = 0x90; // Index start location within the ICC Profile where we store the compressed data file size (minus ICC Profile data size). 
	
	constexpr uint_fast16_t ICC_PROFILE_SIZE = 912;
	
	const uint_fast32_t DEFLATED_DATA_FILE_SIZE = PROFILE_WITH_DATA_FILE_VEC_SIZE - ICC_PROFILE_SIZE;
		
	// Write the compressed file size of the data file (minus ICC Profile size) within index position of ICC Profile. Value used by jdvout.	
	valueUpdater(File_Vec, DEFLATED_DATA_FILE_SIZE_INDEX, DEFLATED_DATA_FILE_SIZE, value_bit_length);
}
