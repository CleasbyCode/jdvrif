void splitDataFile(std::vector<uint8_t>&segment_vec, std::vector<uint8_t>&data_file_vec, bool& shouldDisplayMastodonWarning) {
	constexpr uint8_t
		IMAGE_START_SIG_LENGTH 	  = 2,
		ICC_PROFILE_SIG_LENGTH	  = 2,
		ICC_PROFILE_HEADER_LENGTH = 16;

	uint16_t icc_segment_data_size = 65519;  // Max. data for each segment (Not including header and signature bytes).

	uint32_t 
		icc_profile_with_data_file_vec_size = static_cast<uint32_t>(segment_vec.size()),
		max_first_segment_size = icc_segment_data_size + IMAGE_START_SIG_LENGTH + ICC_PROFILE_SIG_LENGTH + ICC_PROFILE_HEADER_LENGTH;

	uint8_t val_bit_length = 16;

	if (icc_profile_with_data_file_vec_size > max_first_segment_size) { // Data file is too large for a single segment, so split data file in to multiple segments.
		constexpr uint8_t LIBSODIUM_DISCREPANCY_VAL = 20;

		icc_profile_with_data_file_vec_size -= LIBSODIUM_DISCREPANCY_VAL;

		uint16_t 
			icc_segments_required        = (icc_profile_with_data_file_vec_size / icc_segment_data_size) + 1, // There will almost always be a remainder segment, so plus 1 here.
			icc_segment_remainder_size  = icc_profile_with_data_file_vec_size % icc_segment_data_size,
			icc_segments_sequence_val   = 1;
			
		constexpr uint16_t ICC_SEGMENTS_TOTAL_VAL_INDEX = 0x2E0;  // The value stored here is used by jdvout when extracting the data file.
		valueUpdater(segment_vec, ICC_SEGMENTS_TOTAL_VAL_INDEX, !icc_segment_remainder_size ? --icc_segments_required : icc_segments_required, val_bit_length);

		uint8_t 
			icc_segments_sequence_val_index = 0x11,
			icc_segment_remainder_size_index = 0x04;

		std::vector<uint8_t> icc_profile_header_vec { segment_vec.begin(), segment_vec.begin() + IMAGE_START_SIG_LENGTH + ICC_PROFILE_SIG_LENGTH + ICC_PROFILE_HEADER_LENGTH };

		// Because of some duplicate data, erase the first 20 bytes of segment_vec because they will be replaced when splitting the data file.
    		segment_vec.erase(segment_vec.begin(), segment_vec.begin() + (IMAGE_START_SIG_LENGTH + ICC_PROFILE_SIG_LENGTH + ICC_PROFILE_HEADER_LENGTH));

		data_file_vec.reserve(icc_profile_with_data_file_vec_size + (icc_segments_required * (ICC_PROFILE_SIG_LENGTH + ICC_PROFILE_HEADER_LENGTH)));

		uint32_t byte_index = 0;

		while (icc_segments_required--) {	
			if (!icc_segments_required) {
				if (!icc_segment_remainder_size) {
					break;
				}
				icc_segment_data_size = icc_segment_remainder_size;	
			   	valueUpdater(icc_profile_header_vec, icc_segment_remainder_size_index, (icc_segment_remainder_size + ICC_PROFILE_HEADER_LENGTH), val_bit_length); 	
			}
			std::copy_n(icc_profile_header_vec.begin() + IMAGE_START_SIG_LENGTH, ICC_PROFILE_SIG_LENGTH + ICC_PROFILE_HEADER_LENGTH, std::back_inserter(data_file_vec));
			std::copy_n(segment_vec.begin() + byte_index, icc_segment_data_size, std::back_inserter(data_file_vec));
			valueUpdater(icc_profile_header_vec, icc_segments_sequence_val_index, ++icc_segments_sequence_val, val_bit_length);
			byte_index += icc_segment_data_size;
		}

		std::vector<uint8_t>().swap(segment_vec);
		
		// Insert the start of image sig bytes that were removed.
		data_file_vec.insert(data_file_vec.begin(), icc_profile_header_vec.begin(), icc_profile_header_vec.begin() + IMAGE_START_SIG_LENGTH);

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