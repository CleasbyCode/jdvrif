uint64_t encryptFile(std::vector<uint8_t>& profile_vec, std::vector<uint8_t>& data_file_vec, std::string& data_filename) {
	
	std::random_device rd;
 	std::mt19937 gen(rd());
	std::uniform_int_distribution<unsigned short> dis(1, 255); // For the basic 80 byte XOR key, used for encrypting filename of user's data file.
		
	constexpr uint16_t DATA_FILENAME_XOR_KEY_INDEX = 0x2F5;
		
	uint16_t 
		data_filename_index = 0x1EF,  					// Store filename of user's data file within the color profile segment
		data_filename_xor_key_pos = DATA_FILENAME_XOR_KEY_INDEX;	// Store the main XOR key within the color profile segment. Used to encrypt the filename.

	uint8_t 
		data_filename_xor_key_length = 80,
		data_filename_length = profile_vec[data_filename_index - 1],
		data_filename_char_pos = 0;
		
	// Generate & store the XOR key for encrypting the filename.
	while(data_filename_xor_key_length--) {
		profile_vec[data_filename_xor_key_pos++] = static_cast<uint8_t>(dis(gen));
	}

	data_filename_xor_key_pos = DATA_FILENAME_XOR_KEY_INDEX;	// Reset index position.

	// Just use simple XOR to encrypt and store filename of the user's data file.
	while (data_filename_length--) {
		profile_vec[data_filename_index++] = data_filename[data_filename_char_pos++] ^ profile_vec[data_filename_xor_key_pos++];
	}	
	
	uint32_t data_file_vec_size = static_cast<uint32_t>(data_file_vec.size());

	profile_vec.reserve(profile_vec.size() + data_file_vec_size);
	
	// Use the encryption library libsodium (XSalsa20-Poly1305) to encrypt the user's data file.

	std::array<uint8_t, crypto_secretbox_KEYBYTES> key;	// 32 Bytes.
    	crypto_secretbox_keygen(key.data());

	std::array<uint8_t, crypto_secretbox_NONCEBYTES> nonce; // 24 Bytes.
   	randombytes_buf(nonce.data(), nonce.size());

	// Where we store the sodium keys, within the color profile segment.
	constexpr uint16_t
		SODIUM_KEY_INDEX = 0x345,     
		NONCE_KEY_INDEX  = 0x365;  
	
	// Write keys within the color profile segment.
	std::copy(key.begin(), key.end(), profile_vec.begin() + SODIUM_KEY_INDEX); 	
	std::copy(nonce.begin(), nonce.end(), profile_vec.begin() + NONCE_KEY_INDEX);

    	std::vector<uint8_t> encrypted_vec(data_file_vec_size + crypto_secretbox_MACBYTES); // 16 Bytes for the MACBYTES.

    	// Using libsodium, encrypt the data file stored in the vector File_Vec.
    	crypto_secretbox_easy(encrypted_vec.data(), data_file_vec.data(), data_file_vec_size, nonce.data(), key.data());

	// Append the now encrypted data file to the vector Profile_Vec.
	profile_vec.insert(profile_vec.end(), encrypted_vec.begin(), encrypted_vec.end()); 
	
	std::vector<uint8_t>().swap(encrypted_vec); // Clear vector.
	
	// The PIN value is the first 8 bytes of the main sodium key. User needs to keep this safe, for file recovery.
	const uint64_t PIN = getByteValue<uint64_t>(profile_vec, SODIUM_KEY_INDEX); 

	uint16_t 
		sodium_xor_key_pos = SODIUM_KEY_INDEX,
		sodium_key_pos = SODIUM_KEY_INDEX;

	uint8_t
		sodium_keys_length = 48,	// 24 bytes of the 32 byte sodium key + 24 bytes (full) of the nonce key.
		value_bit_length = 64;

	// XOR part of the sodium key and all of the nonce key.
	sodium_key_pos += 8; // move past the first 8 bytes of the main sodium key.

	constexpr uint8_t SODIUM_XOR_KEY_LENGTH = 8;  	// Length of XOR key, used to encrypt sodium keys. 

	while (sodium_keys_length--) {   // XOR encrypt 48 bytes.
    		profile_vec[sodium_key_pos] = profile_vec[sodium_key_pos] ^ profile_vec[sodium_xor_key_pos++];
		sodium_key_pos++;
    		sodium_xor_key_pos = (sodium_xor_key_pos >= SODIUM_XOR_KEY_LENGTH + SODIUM_KEY_INDEX) 
                         ? SODIUM_KEY_INDEX // Reset PIN position once we hit PIN length.
                         : sodium_xor_key_pos;
	}
	
	sodium_key_pos = SODIUM_KEY_INDEX; // Reset index position.

	std::mt19937_64 gen64(rd()); 
    	std::uniform_int_distribution<uint64_t> dis64; 

    	uint64_t random_val = dis64(gen64); // Generate a random 8 byte value.

	// Overwrite the first 8 bytes of the main 32 byte sodium key with the random value.
	valueUpdater(profile_vec, sodium_key_pos, random_val, value_bit_length);

	return PIN;
}