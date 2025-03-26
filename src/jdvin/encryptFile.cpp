// This project uses libsodium (https://libsodium.org/) for cryptographic functions.
// Copyright (c) 2013-2025 Frank Denis <github@pureftpd.org>
uint64_t encryptFile(std::vector<uint8_t>& segment_vec, std::vector<uint8_t>& data_file_vec, std::string& data_filename, bool hasBlueskyOption) {
	std::random_device rd;
 	std::mt19937 gen(rd());
	std::uniform_int_distribution<unsigned short> dis(1, 255); 
		
	uint16_t DATA_FILENAME_XOR_KEY_INDEX = hasBlueskyOption ? 0x175 : 0x2F5;
		
	uint16_t 
		data_filename_index = hasBlueskyOption ? 0x161: 0x1EF,  	
		data_filename_xor_key_pos = DATA_FILENAME_XOR_KEY_INDEX;	

	uint8_t 
		data_filename_xor_key_length = hasBlueskyOption ? 24 : 80,
		data_filename_length = segment_vec[data_filename_index - 1],
		data_filename_char_pos = 0;

	while(data_filename_xor_key_length--) {
		segment_vec[data_filename_xor_key_pos++] = static_cast<uint8_t>(dis(gen));
	}

	data_filename_xor_key_pos = DATA_FILENAME_XOR_KEY_INDEX;

	while (data_filename_length--) {
		segment_vec[data_filename_index++] = data_filename[data_filename_char_pos++] ^ segment_vec[data_filename_xor_key_pos++];
	}	
	
	uint32_t data_file_vec_size = static_cast<uint32_t>(data_file_vec.size());

	segment_vec.reserve(segment_vec.size() + data_file_vec_size);
	
	std::array<uint8_t, crypto_secretbox_KEYBYTES> key;
    	crypto_secretbox_keygen(key.data());

	std::array<uint8_t, crypto_secretbox_NONCEBYTES> nonce;
   	randombytes_buf(nonce.data(), nonce.size());

	const uint16_t
		EXIF_DATA_INSERT_INDEX = 0x1D1,
		SODIUM_KEY_INDEX = hasBlueskyOption ? 0x18D : 0x345,     
		NONCE_KEY_INDEX  = hasBlueskyOption ? 0x1AD : 0x365;  
	
	std::copy(key.begin(), key.end(), segment_vec.begin() + SODIUM_KEY_INDEX); 	
	std::copy(nonce.begin(), nonce.end(), segment_vec.begin() + NONCE_KEY_INDEX);

    	std::vector<uint8_t> encrypted_vec(data_file_vec_size + crypto_secretbox_MACBYTES); 

    	crypto_secretbox_easy(encrypted_vec.data(), data_file_vec.data(), data_file_vec_size, nonce.data(), key.data());

	if (hasBlueskyOption) { // User has selected the -b argument option for the Bluesky platform.
		constexpr uint16_t EXIF_DATA_SIZE_LIMIT = 0xFE03; // + With EXIF overhead segment data (0x1FF) - 4 bytes we don't count (FFD8 FFE1) = Max. segment size 0xFFFE.
								  // Can't have 0xFFFF as Bluesky will strip the EXIF segment.
		uint32_t 
			index_pos = 0,
			encrypted_vec_size = static_cast<uint32_t>(encrypted_vec.size()),
			compressed_file_size_index = 0x1CD;

		uint8_t value_bit_length = 32;
		valueUpdater(segment_vec, compressed_file_size_index, encrypted_vec.size(), value_bit_length);

		// Split the data file if it exceeds the max compressed EXIF capacity of ~64KB. 
		// We can then use the second segment (XMP) for the excess data.

		if (encrypted_vec_size > EXIF_DATA_SIZE_LIMIT) {
			std::vector<uint8_t> tmp_exif_vec;
			tmp_exif_vec.reserve(encrypted_vec_size);

			std::vector<uint8_t> tmp_xmp_vec;
			tmp_xmp_vec.reserve(encrypted_vec_size);	
	
			while (encrypted_vec_size > index_pos) {
				if (EXIF_DATA_SIZE_LIMIT > index_pos) {
					tmp_exif_vec.emplace_back(encrypted_vec[index_pos++]);
				} else {
					tmp_xmp_vec.emplace_back(encrypted_vec[index_pos++]);
				}
			}
		
			// Store the first part of the file (as binary) within the EXIF segment 
			segment_vec.insert(segment_vec.begin() + EXIF_DATA_INSERT_INDEX, tmp_exif_vec.begin(), tmp_exif_vec.end());
			
			// We can only store Base64 encoded data in the XMP segment, so convert the binary data here.
			convertToBase64(tmp_xmp_vec);
			
			constexpr uint16_t XMP_DATA_INSERT_INDEX = 0x139;

			// Store the second part of the file (as Base64) within the XMP segment.
			bluesky_xmp_vec.insert(bluesky_xmp_vec.begin() + XMP_DATA_INSERT_INDEX, tmp_xmp_vec.begin(), tmp_xmp_vec.end());	

		} else { // Data file was small enough to fit within the EXIF segment, XMP segment not required.
			segment_vec.insert(segment_vec.begin() + EXIF_DATA_INSERT_INDEX, encrypted_vec.begin(), encrypted_vec.end());
		}

	} else { // Used the default color profile segment for data storage.
		segment_vec.insert(segment_vec.end(), encrypted_vec.begin(), encrypted_vec.end()); 
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

    	uint64_t random_val = dis64(gen64); 

	valueUpdater(segment_vec, sodium_key_pos, random_val, value_bit_length);

	return PIN;
}