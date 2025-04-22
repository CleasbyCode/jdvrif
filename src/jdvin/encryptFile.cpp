// This project uses libsodium (https://libsodium.org/) for cryptographic functions.
// Copyright (c) 2013-2025 Frank Denis <github@pureftpd.org>
uint64_t encryptFile(std::vector<uint8_t>& segment_vec, std::vector<uint8_t>& data_file_vec, std::string& data_filename, bool hasBlueskyOption) {
	std::random_device rd;
 	std::mt19937 gen(rd());
	std::uniform_int_distribution<unsigned short> dis(1, 255); 
		
	constexpr uint8_t XOR_KEY_LENGTH = 24;
	
	uint16_t
		data_filename_xor_key_index = hasBlueskyOption ? 0x175 : 0x2FB,
		data_filename_index = hasBlueskyOption ? 0x161: 0x2E7;
		
	uint8_t 
		data_filename_length = segment_vec[data_filename_index - 1],
		data_filename_char_pos = 0;

	std::generate_n(segment_vec.begin() + data_filename_xor_key_index, XOR_KEY_LENGTH, [&dis, &gen]() { return static_cast<uint8_t>(dis(gen)); });

	while (data_filename_length--) {
		segment_vec[data_filename_index++] = data_filename[data_filename_char_pos++] ^ segment_vec[data_filename_xor_key_index++];
	}	
	const uint32_t DATA_FILE_VEC_SIZE = static_cast<uint32_t>(data_file_vec.size());

	segment_vec.reserve(segment_vec.size() + DATA_FILE_VEC_SIZE);
	
	std::array<uint8_t, crypto_secretbox_KEYBYTES> key;
    	crypto_secretbox_keygen(key.data());

	std::array<uint8_t, crypto_secretbox_NONCEBYTES> nonce;
   	randombytes_buf(nonce.data(), nonce.size());

	constexpr uint16_t EXIF_SEGMENT_DATA_INSERT_INDEX = 0x1D1;

	const uint16_t
		SODIUM_KEY_INDEX = hasBlueskyOption ? 0x18D : 0x313,     
		NONCE_KEY_INDEX  = hasBlueskyOption ? 0x1AD : 0x333;  
	
	std::copy_n(key.begin(), crypto_secretbox_KEYBYTES, segment_vec.begin() + SODIUM_KEY_INDEX); 	
	std::copy_n(nonce.begin(), crypto_secretbox_NONCEBYTES, segment_vec.begin() + NONCE_KEY_INDEX);

    	std::vector<uint8_t> encrypted_vec(DATA_FILE_VEC_SIZE + crypto_secretbox_MACBYTES); 

    	crypto_secretbox_easy(encrypted_vec.data(), data_file_vec.data(), DATA_FILE_VEC_SIZE, nonce.data(), key.data());

	if (hasBlueskyOption) { // User has selected the -b argument option for the Bluesky platform.
		constexpr uint16_t EXIF_SEGMENT_DATA_SIZE_LIMIT = 65027; 
		// + With EXIF overhead segment data (511) - four bytes we don't count (FFD8 FFE1),  
		// = Max. segment size 65534 (0xFFFE). Can't have 65535 (0xFFFF) as Bluesky will strip the EXIF segment.
		const uint32_t ENCRYPTED_VEC_SIZE = static_cast<uint32_t>(encrypted_vec.size());
		
		uint16_t compressed_file_size_index = 0x1CD;
		uint8_t value_bit_length = 32;					 	 
		
		valueUpdater(segment_vec, compressed_file_size_index, ENCRYPTED_VEC_SIZE, value_bit_length);

		// Split the data file if it exceeds the max compressed EXIF capacity of ~64KB. 
		// We can then use the second segment (XMP) for the remaining data.

		if (ENCRYPTED_VEC_SIZE > EXIF_SEGMENT_DATA_SIZE_LIMIT) {
			segment_vec.insert(segment_vec.begin() + EXIF_SEGMENT_DATA_INSERT_INDEX, encrypted_vec.begin(), encrypted_vec.begin() + EXIF_SEGMENT_DATA_SIZE_LIMIT);
			
			const uint32_t REMAINING_DATA_SIZE = ENCRYPTED_VEC_SIZE - EXIF_SEGMENT_DATA_SIZE_LIMIT;
			
			std::vector<uint8_t> tmp_xmp_vec(REMAINING_DATA_SIZE);
			std::copy_n(encrypted_vec.begin() + EXIF_SEGMENT_DATA_SIZE_LIMIT, REMAINING_DATA_SIZE, tmp_xmp_vec.begin());
			
			// We can only store Base64 encoded data in the XMP segment, so convert the binary data here.
			convertToBase64(tmp_xmp_vec);
			
			constexpr uint16_t XMP_SEGMENT_DATA_INSERT_INDEX = 0x139;
			
			// Store the second part of the file (as Base64) within the XMP segment.
			bluesky_xmp_vec.insert(bluesky_xmp_vec.begin() + XMP_SEGMENT_DATA_INSERT_INDEX, tmp_xmp_vec.begin(), tmp_xmp_vec.end());
			
			std::vector<uint8_t>().swap(tmp_xmp_vec);
		} else { // Data file was small enough to fit within the EXIF segment, XMP segment not required.
			segment_vec.insert(segment_vec.begin() + EXIF_SEGMENT_DATA_INSERT_INDEX, encrypted_vec.begin(), encrypted_vec.end());
		}
	} else { // Used the default color profile segment for data storage.
		std::copy_n(encrypted_vec.begin(), encrypted_vec.size(), std::back_inserter(segment_vec));
	}	
	std::vector<uint8_t>().swap(encrypted_vec);
	
	const uint64_t PIN = getByteValue<uint64_t>(segment_vec, SODIUM_KEY_INDEX); 

	uint16_t 
		sodium_xor_key_pos = SODIUM_KEY_INDEX,
		sodium_key_pos = SODIUM_KEY_INDEX;

	uint8_t
		sodium_keys_length = 48,
		value_bit_length = 64;

	sodium_key_pos += 8; 

	constexpr uint8_t SODIUM_XOR_KEY_LENGTH = 8;  

	while (sodium_keys_length--) {   
    		segment_vec[sodium_key_pos] = segment_vec[sodium_key_pos] ^ segment_vec[sodium_xor_key_pos++];
		sodium_key_pos++;
    		sodium_xor_key_pos = (sodium_xor_key_pos >= SODIUM_XOR_KEY_LENGTH + SODIUM_KEY_INDEX) 
                         ? SODIUM_KEY_INDEX 
                         : sodium_xor_key_pos;
	}
	sodium_key_pos = SODIUM_KEY_INDEX; 

	std::mt19937_64 gen64(rd()); 
    	std::uniform_int_distribution<uint64_t> dis64; 

    	const uint64_t RANDOM_VAL = dis64(gen64); 

	valueUpdater(segment_vec, sodium_key_pos, RANDOM_VAL, value_bit_length);

	return PIN;
}
