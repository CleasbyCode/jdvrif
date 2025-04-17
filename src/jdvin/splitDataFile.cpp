void splitDataFile(std::vector<uint8_t>&segment_vec, std::vector<uint8_t>&data_file_vec, bool& shouldDisplayMastodonWarning) {
	constexpr uint8_t
		IMAGE_START_SIG_LENGTH 	  = 2,
		ICC_PROFILE_SIG_LENGTH	  = 2,
		ICC_PROFILE_HEADER_LENGTH = 16;
			
	constexpr uint32_t MAX_FIRST_SEGMENT_SIZE = 65539; // Size includes the data (65519), plus icc profile header and signature bytes (20);

	uint32_t 
		icc_profile_with_data_file_vec_size = static_cast<uint32_t>(segment_vec.size()),
		icc_segment_data_size = 65519;  // Max. data for each segment (Not including header and signature bytes).

	uint8_t val_bit_length = 16;

	if (icc_profile_with_data_file_vec_size > MAX_FIRST_SEGMENT_SIZE) { // Data file is too large for a single segment, so split data file in to multiple segments.
		constexpr uint8_t LIBSODIUM_DISCREPANCY_VAL = 20;

		icc_profile_with_data_file_vec_size -= LIBSODIUM_DISCREPANCY_VAL;

		uint16_t 
			icc_segments_required_val   = (icc_profile_with_data_file_vec_size / icc_segment_data_size) + 1, // There will almost always be a remainder segment, so plus 1 here.
			icc_segment_remainder_size  = icc_profile_with_data_file_vec_size % icc_segment_data_size,
			icc_segments_sequence_val   = 1;
			
		constexpr uint16_t ICC_SEGMENTS_TOTAL_VAL_INDEX = 0x2E0;  // The value stored here is used by jdvout when extracting the data file.
		valueUpdater(segment_vec, ICC_SEGMENTS_TOTAL_VAL_INDEX, !icc_segment_remainder_size ? --icc_segments_required_val : icc_segments_required_val, val_bit_length);

		uint8_t 
			icc_segments_sequence_val_index = 0x0F,
			icc_segment_remainder_size_index = 0x02;

		std::vector<uint8_t> split_vec { 0xFF, 0xE2, 0xFF, 0xFF, 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45, 0x00, 0x00, 0xFF };
		split_vec.reserve(MAX_FIRST_SEGMENT_SIZE);

		// Erase the first 20 bytes of segment_vec because they will be replaced when splitting the data file.
    		segment_vec.erase(segment_vec.begin(), segment_vec.begin() + (IMAGE_START_SIG_LENGTH + ICC_PROFILE_SIG_LENGTH + ICC_PROFILE_HEADER_LENGTH));

		data_file_vec.reserve(icc_segment_data_size * icc_segments_required_val);
		
		uint32_t byte_index = 0;
	
		while (icc_segments_required_val--) {	
			if (!icc_segments_required_val) {
				if (!icc_segment_remainder_size) {
					break;
				}
				icc_segment_data_size = icc_segment_remainder_size;	
				icc_segment_remainder_size += ICC_PROFILE_HEADER_LENGTH;
			   	valueUpdater(split_vec, icc_segment_remainder_size_index, icc_segment_remainder_size, val_bit_length); 		
			}
			std::copy_n(segment_vec.begin() + byte_index, icc_segment_data_size, std::back_inserter(split_vec));
			byte_index += icc_segment_data_size;	
			
			data_file_vec.insert(data_file_vec.end(), split_vec.begin(), split_vec.end()); 

    			split_vec.resize(ICC_PROFILE_SIG_LENGTH + ICC_PROFILE_HEADER_LENGTH); // Keep the segment profile header, delete all other content. 
			
			valueUpdater(split_vec, icc_segments_sequence_val_index, ++icc_segments_sequence_val, val_bit_length);
		}

		std::vector<uint8_t>().swap(segment_vec);
		std::vector<uint8_t>().swap(split_vec);
		
		// Insert the start of image sig bytes that were removed.
		constexpr std::array<uint8_t, 2> IMAGE_START_SIG { 0xFF, 0xD8 };
		data_file_vec.insert(data_file_vec.begin(), IMAGE_START_SIG.begin(), IMAGE_START_SIG.end());

		constexpr uint8_t MASTODON_SEGMENTS_LIMIT = 100;		   
		constexpr uint32_t MASTODON_IMAGE_UPLOAD_LIMIT = 16 * 1024 * 1024; 
					   
		// The warning is important because Mastodon will allow you to post an image that is greater than its 100 segments limit, as long as you do not exceed
		// the image size limit, which is 16MB. This seems fine until someone downloads/saves the image. Data segments over the limit will be truncated, so parts 
		// of the data file will be missing when an attempt is made to extract the (now corrupted) file from the image.
		shouldDisplayMastodonWarning = icc_segments_sequence_val > MASTODON_SEGMENTS_LIMIT && MASTODON_IMAGE_UPLOAD_LIMIT > icc_profile_with_data_file_vec_size;
	} else {  // Data file is small enough to fit within a single icc profile segment.
		constexpr uint8_t
			ICC_SEGMENT_HEADER_SIZE_INDEX 	= 0x04, 
			ICC_PROFILE_SIZE_INDEX  	= 0x16, 
			ICC_PROFILE_SIZE_DIFF   	= 16;
			
		const uint16_t 
			SEGMENT_SIZE 	 = icc_profile_with_data_file_vec_size - (IMAGE_START_SIG_LENGTH + ICC_PROFILE_SIG_LENGTH),
			ICC_PROFILE_SIZE = SEGMENT_SIZE - ICC_PROFILE_SIZE_DIFF;

		valueUpdater(segment_vec, ICC_SEGMENT_HEADER_SIZE_INDEX, SEGMENT_SIZE, val_bit_length);
		valueUpdater(segment_vec, ICC_PROFILE_SIZE_INDEX, ICC_PROFILE_SIZE, val_bit_length);

		data_file_vec = std::move(segment_vec);
	}
		
	val_bit_length = 32; 

	constexpr uint16_t 
		DEFLATED_DATA_FILE_SIZE_INDEX = 0x2E2,	// The size value stored here is used by jdvout when extracting the data file.
		ICC_PROFILE_SIZE = 851; 
	
	valueUpdater(data_file_vec, DEFLATED_DATA_FILE_SIZE_INDEX, static_cast<uint32_t>(data_file_vec.size()) - ICC_PROFILE_SIZE, val_bit_length);
}
