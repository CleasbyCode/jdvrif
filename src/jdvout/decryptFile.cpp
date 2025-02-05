// This project uses [libsodium](https://libsodium.org/) for cryptographic functions.
const std::string decryptFile(std::vector<uint8_t>&Image_Vec) {

	constexpr uint16_t 
		ENCRYPTED_FILE_START_INDEX 	= 0x35B,
		FILE_SIZE_INDEX 		= 0x1D9,
		PROFILE_COUNT_VALUE_INDEX 	= 0x1DD,
		ENCRYPTED_FILENAME_INDEX 	= 0x1C5;

	constexpr uint8_t
		SODIUM_XOR_KEY_START_INDEX	= 0x6A,
		PROFILE_HEADER_LENGTH		= 18, 
		SODIUM_XOR_KEY_LENGTH		= 9;  
	
	const uint32_t 
		EMBEDDED_FILE_SIZE = getByteValue<uint32_t>(Image_Vec, FILE_SIZE_INDEX);

	const uint16_t PROFILE_COUNT = (static_cast<uint16_t>(Image_Vec[PROFILE_COUNT_VALUE_INDEX]) << 8) | static_cast<uint16_t>(Image_Vec[PROFILE_COUNT_VALUE_INDEX + 1]);

	uint32_t* Headers_Index_Arr = new uint32_t[PROFILE_COUNT];

	uint16_t 
		filename_xor_key_index = 0x2CB,
		sodium_key_index = 0x31B,
		nonce_key_index = 0x33B;

	uint8_t
		encrypted_filename_length = Image_Vec[ENCRYPTED_FILENAME_INDEX - 1],
		sodium_xor_key_pos = SODIUM_XOR_KEY_START_INDEX,
		sodium_part_key_index = 0x6B, 	
		sodium_keys_length = 48,
		value_bit_length = 64,
		filename_char_pos = 0;
		
	const std::string ENCRYPTED_FILENAME { Image_Vec.begin() + ENCRYPTED_FILENAME_INDEX, Image_Vec.begin() + ENCRYPTED_FILENAME_INDEX + encrypted_filename_length };
	
	std::cout << "\nPIN: ";
	uint64_t pin = getPin();
		
	valueUpdater(Image_Vec, sodium_part_key_index, pin, value_bit_length);	 
	valueUpdater(Image_Vec, sodium_key_index, pin, value_bit_length); 
	
	sodium_key_index += 8;

	while(sodium_keys_length--) {
		Image_Vec[sodium_key_index] = Image_Vec[sodium_key_index] ^ Image_Vec[sodium_xor_key_pos++];
		sodium_key_index++;
		sodium_xor_key_pos = (sodium_xor_key_pos >= SODIUM_XOR_KEY_LENGTH + SODIUM_XOR_KEY_START_INDEX) 
			? SODIUM_XOR_KEY_START_INDEX 
			: sodium_xor_key_pos;
	}

	sodium_key_index = 0x31B;

	uint8_t nonce[crypto_secretbox_NONCEBYTES];
	uint8_t key[crypto_secretbox_KEYBYTES];

	std::copy(Image_Vec.begin() + nonce_key_index, 
          	Image_Vec.begin() + nonce_key_index + crypto_secretbox_NONCEBYTES, 
          	nonce);

	std::copy(Image_Vec.begin() + sodium_key_index, 
        	  Image_Vec.begin() + sodium_key_index + crypto_secretbox_KEYBYTES, 
          	key);

	std::string decrypted_filename;

	while (encrypted_filename_length--) {
		decrypted_filename += ENCRYPTED_FILENAME[filename_char_pos++] ^ Image_Vec[filename_xor_key_index++];
	}

	std::vector<uint8_t> Temp_Vec(Image_Vec.begin() + ENCRYPTED_FILE_START_INDEX, Image_Vec.begin() + ENCRYPTED_FILE_START_INDEX + EMBEDDED_FILE_SIZE);
	Image_Vec = std::move(Temp_Vec);

	if (PROFILE_COUNT) {	
		constexpr uint8_t 
			ICC_PROFILE_SIG[] { 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45 },
			INC_NEXT_SEARCH_INDEX 	= 5,
			INDEX_DIFF 		= 4;

		uint32_t header_index = 0;

		for (int i = 0; PROFILE_COUNT > i; ++i) {
			Headers_Index_Arr[i] = header_index = searchFunc(Image_Vec, header_index, INC_NEXT_SEARCH_INDEX, ICC_PROFILE_SIG) - INDEX_DIFF;
		}
	}

	uint32_t 
		encrypted_file_size = static_cast<uint32_t>(Image_Vec.size()),
		next_header_index = 0,
		index_pos = 0;
	
	std::vector<uint8_t>Sanitize_Vec; 
	Sanitize_Vec.reserve(encrypted_file_size);

	while (encrypted_file_size > index_pos) {
		Sanitize_Vec.emplace_back(Image_Vec[index_pos++]);
		if (PROFILE_COUNT && index_pos == Headers_Index_Arr[next_header_index]) {
			index_pos += PROFILE_HEADER_LENGTH; 
			++next_header_index;
		}	
	}

	std::vector<uint8_t>().swap(Image_Vec);

	std::vector<uint8_t>Decrypted_File_Vec(Sanitize_Vec.size() - crypto_secretbox_MACBYTES);

	if (crypto_secretbox_open_easy(Decrypted_File_Vec.data(), Sanitize_Vec.data(), Sanitize_Vec.size(), nonce, key) !=0 ) {
		std::cerr << "\nDecryption failed!" << std::endl;
	}
	
	std::vector<uint8_t>().swap(Sanitize_Vec);
	
	delete[] Headers_Index_Arr;
	Image_Vec.swap(Decrypted_File_Vec);
	return decrypted_filename;
}
