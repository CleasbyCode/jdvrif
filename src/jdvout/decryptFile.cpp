const std::string decryptFile(std::vector<uint8_t>& image_vec) {	
	constexpr uint16_t 
		SODIUM_KEY_INDEX = 0x31B,
		NONCE_KEY_INDEX = 0x33B;

	uint16_t 
		sodium_key_pos = SODIUM_KEY_INDEX,
		sodium_xor_key_pos = SODIUM_KEY_INDEX;

	uint8_t
		sodium_keys_length = 48,
		value_bit_length = 64;
		
	std::cout << "\nPIN: ";
	uint64_t pin = getPin();

	// Insert the 8 byte PIN value, supplied by user into index location. This value completes the missing part of the main sodium key.
	valueUpdater(image_vec, sodium_key_pos, pin, value_bit_length); 
	
	sodium_key_pos += 8;

	constexpr uint8_t SODIUM_XOR_KEY_LENGTH	= 8; 

	// XOR decrypt 48 bytes. This includes 24 bytes of the 32 byte main sodium key and 24 bytes of the nonce key.
	while(sodium_keys_length--) {
		image_vec[sodium_key_pos] = image_vec[sodium_key_pos] ^ image_vec[sodium_xor_key_pos++];
		sodium_key_pos++;
		sodium_xor_key_pos = (sodium_xor_key_pos >= SODIUM_XOR_KEY_LENGTH + SODIUM_KEY_INDEX) 
			? SODIUM_KEY_INDEX 
			: sodium_xor_key_pos;
	}

	std::array<uint8_t, crypto_secretbox_KEYBYTES> key;
	std::array<uint8_t, crypto_secretbox_NONCEBYTES> nonce;

	std::copy(image_vec.begin() + SODIUM_KEY_INDEX, image_vec.begin() + SODIUM_KEY_INDEX + crypto_secretbox_KEYBYTES, key.data());

	std::copy(image_vec.begin() + NONCE_KEY_INDEX, image_vec.begin() + NONCE_KEY_INDEX + crypto_secretbox_NONCEBYTES, nonce.data());

	// decrypt filename start
	std::string decrypted_filename;

	constexpr uint16_t ENCRYPTED_FILENAME_INDEX = 0x1C5;

	uint16_t filename_xor_key_pos = 0x2CB;
	
	uint8_t
		encrypted_filename_length = image_vec[ENCRYPTED_FILENAME_INDEX - 1],
		filename_char_pos = 0;

	const std::string ENCRYPTED_FILENAME { image_vec.begin() + ENCRYPTED_FILENAME_INDEX, image_vec.begin() + ENCRYPTED_FILENAME_INDEX + encrypted_filename_length };

	while (encrypted_filename_length--) {
		decrypted_filename += ENCRYPTED_FILENAME[filename_char_pos++] ^ image_vec[filename_xor_key_pos++];
	}
	// decrpyted filename end

	constexpr uint16_t 
		ENCRYPTED_FILE_START_INDEX 	= 0x35B,
		FILE_SIZE_INDEX 		= 0x1D9,
		PROFILE_COUNT_VALUE_INDEX 	= 0x1DD;

	const uint32_t EMBEDDED_FILE_SIZE = getByteValue<uint32_t>(image_vec, FILE_SIZE_INDEX);

	// How many ICC_PROFILE segments? (Don't count the first, default color profile segment).
	const uint16_t PROFILE_COUNT = (static_cast<uint16_t>(image_vec[PROFILE_COUNT_VALUE_INDEX]) << 8) | static_cast<uint16_t>(image_vec[PROFILE_COUNT_VALUE_INDEX + 1]);

	uint32_t* Headers_Index_Arr = new uint32_t[PROFILE_COUNT];

	// Remove profile data & cover image data from vector. Leaving just the encrypted/compressed data file.
	std::vector<uint8_t> temp_vec(image_vec.begin() + ENCRYPTED_FILE_START_INDEX, image_vec.begin() + ENCRYPTED_FILE_START_INDEX + EMBEDDED_FILE_SIZE);
	image_vec = std::move(temp_vec);

	// Search the "file-embedded" image for ICC Profile headers. Store index location of each found header within the vector.
	// We will use these index positions to skip over the headers when decrypting the data file, 
	// so that they are not included within the restored data file.
	if (PROFILE_COUNT) {	
		constexpr std::array<uint8_t, 11> ICC_PROFILE_SIG { 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45 };

		constexpr uint8_t
			INC_NEXT_SEARCH_INDEX 	= 5,
			INDEX_DIFF 		= 4;

		uint32_t header_index = 0;

		for (int i = 0; PROFILE_COUNT > i; ++i) {
			Headers_Index_Arr[i] = header_index = searchFunc(image_vec, header_index, INC_NEXT_SEARCH_INDEX, ICC_PROFILE_SIG) - INDEX_DIFF;
		}
	}

	uint32_t 
		encrypted_file_size = static_cast<uint32_t>(image_vec.size()),
		next_header_index = 0,
		index_pos = 0;
	
	std::vector<uint8_t>sanitize_vec; // Will contain THE encrypted data file without ICC_Profile headers.
	sanitize_vec.reserve(encrypted_file_size);

	constexpr uint8_t PROFILE_HEADER_LENGTH	= 18;

	while (encrypted_file_size > index_pos) {
		sanitize_vec.emplace_back(image_vec[index_pos++]);
		// Skip over the 18 byte ICC Profile header found at each index location within "Headers_Index_Arr", 
		// so that we don't include them along with the decrypted file.
		if (PROFILE_COUNT && index_pos == Headers_Index_Arr[next_header_index]) {
			index_pos += PROFILE_HEADER_LENGTH; 
			++next_header_index;
		}	
	}
	
	delete[] Headers_Index_Arr;

	std::vector<uint8_t>().swap(image_vec);

	std::vector<uint8_t>decrypted_file_vec(sanitize_vec.size() - crypto_secretbox_MACBYTES);

	if (crypto_secretbox_open_easy(decrypted_file_vec.data(), sanitize_vec.data(), sanitize_vec.size(), nonce.data(), key.data()) !=0 ) {
		std::cerr << "\nDecryption failed!" << std::endl;
	}
	
	std::vector<uint8_t>().swap(sanitize_vec);
	image_vec.swap(decrypted_file_vec);
	return decrypted_filename;
}