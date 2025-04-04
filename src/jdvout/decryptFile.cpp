// This project uses libsodium (https://libsodium.org/) for cryptographic functions.
// Copyright (c) 2013-2025 Frank Denis <github@pureftpd.org>
const std::string decryptFile(std::vector<uint8_t>& image_vec, bool hasBlueskyOption) {	
	const uint16_t 
		SODIUM_KEY_INDEX = hasBlueskyOption ? 0x18D : 0x2FB,
		NONCE_KEY_INDEX =  hasBlueskyOption ? 0x1AD : 0x31B;

	uint16_t 
		sodium_key_pos = SODIUM_KEY_INDEX,
		sodium_xor_key_pos = SODIUM_KEY_INDEX;

	uint8_t
		sodium_keys_length = 48,
		value_bit_length = 64;
		
	std::cout << "\nPIN: ";
	uint64_t pin = getPin();

	valueUpdater(image_vec, sodium_key_pos, pin, value_bit_length); 
	
	sodium_key_pos += 8;

	constexpr uint8_t SODIUM_XOR_KEY_LENGTH	= 8; 

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

	std::string decrypted_filename;

	const uint16_t ENCRYPTED_FILENAME_INDEX = hasBlueskyOption ? 0x161 : 0x2CF;

	uint16_t filename_xor_key_pos = hasBlueskyOption ? 0x175 : 0x2E3;
	
	uint8_t
		encrypted_filename_length = image_vec[ENCRYPTED_FILENAME_INDEX - 1],
		filename_char_pos = 0;

	const std::string ENCRYPTED_FILENAME { image_vec.begin() + ENCRYPTED_FILENAME_INDEX, image_vec.begin() + ENCRYPTED_FILENAME_INDEX + encrypted_filename_length };

	while (encrypted_filename_length--) {
		decrypted_filename += ENCRYPTED_FILENAME[filename_char_pos++] ^ image_vec[filename_xor_key_pos++];
	}
	
	const uint16_t 
		ENCRYPTED_FILE_START_INDEX 		= hasBlueskyOption ? 0x1D1 : 0x33B,
		FILE_SIZE_INDEX 			= hasBlueskyOption ? 0x1CD : 0x2CA,
		TOTAL_PROFILE_HEADER_SEGMENTS_INDEX 	= hasBlueskyOption ? 0x110 : 0x2C8;  

	const uint32_t 
		EMBEDDED_FILE_SIZE = getByteValue<uint32_t>(image_vec, FILE_SIZE_INDEX),
		COMMON_DIFF_VAL = 65537; // Size difference between each segment profile header.

	const uint16_t TOTAL_PROFILE_HEADER_SEGMENTS = (static_cast<uint16_t>(image_vec[TOTAL_PROFILE_HEADER_SEGMENTS_INDEX]) << 8) 
								| static_cast<uint16_t>(image_vec[TOTAL_PROFILE_HEADER_SEGMENTS_INDEX + 1]);

	constexpr uint8_t PROFILE_HEADER_LENGTH	= 18;

	std::vector<uint8_t> temp_vec(image_vec.begin() + ENCRYPTED_FILE_START_INDEX, image_vec.begin() + ENCRYPTED_FILE_START_INDEX + EMBEDDED_FILE_SIZE);
	image_vec = std::move(temp_vec);

	uint32_t 
		encrypted_file_size = static_cast<uint32_t>(image_vec.size()),
		header_index = 0xFCB0,	// First split segment profile header location, this is after the main header/color profile, which has already been removed.
		index_pos = 0;
	
	std::vector<uint8_t>sanitize_vec; 
	sanitize_vec.reserve(encrypted_file_size);

	// We need to avoid including the segment profile headers within the decrypted output file.
	// Because we know the total number of profile headers and their location (common difference val), 
	// we can just skip the header bytes when copying the data to the sanitize vector.
        // This is much faster than having to search for and then using something like vec.erase to remove the headers from the vector.
	while (encrypted_file_size > index_pos) {
		sanitize_vec.emplace_back(image_vec[index_pos++]);
		if (TOTAL_PROFILE_HEADER_SEGMENTS && index_pos == header_index) {
			index_pos += PROFILE_HEADER_LENGTH; 
			header_index += COMMON_DIFF_VAL;
		}	
	}
	
	std::vector<uint8_t>().swap(image_vec);

	std::vector<uint8_t>decrypted_file_vec(sanitize_vec.size() - crypto_secretbox_MACBYTES);

	if (crypto_secretbox_open_easy(decrypted_file_vec.data(), sanitize_vec.data(), sanitize_vec.size(), nonce.data(), key.data()) !=0 ) {
		std::cerr << "\nDecryption failed!" << std::endl;
	}
	
	std::vector<uint8_t>().swap(sanitize_vec);
	image_vec.swap(decrypted_file_vec);
	
	return decrypted_filename;
}