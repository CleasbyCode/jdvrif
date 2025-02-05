uint64_t encryptFile(std::vector<uint8_t>& Profile_Vec, std::vector<uint8_t>& File_Vec, std::string& filename) {
	
	std::random_device rd;
 	std::mt19937 gen(rd());
	std::uniform_int_distribution<unsigned short> dis(1, 255); // For the basic 80 byte XOR key, used for encrypting filename of user's data file.
		
	constexpr uint8_t FILENAME_XOR_KEY_LENGTH = 80;	// Length of main XOR key used to XOR encrypt the stored/embedded filename of user's data file. 
		
	uint16_t 
		filename_index = 0x1EF,  	// Store filename of user's data file within the color profile segment
		filename_xor_key_index = 0x2F5;	// Store the main XOR key within the color profile segment. Used to encrypt the filename.

	uint8_t 
		filename_length = Profile_Vec[filename_index - 1],
		filename_char_pos = 0;
		
	// Generate & store the XOR key for encrypting the filename.
	for (; filename_xor_key_index < 0x2F5 + FILENAME_XOR_KEY_LENGTH; ++filename_xor_key_index) {
        	Profile_Vec[filename_xor_key_index] = static_cast<uint8_t>(dis(gen));
    	}

	filename_xor_key_index = 0x2F5;	// Reset index position.

	// Just use simple XOR to encrypt the stored filename of the user's data file.
	while (filename_length--) {
		Profile_Vec[filename_index++] = filename[filename_char_pos++] ^ Profile_Vec[filename_xor_key_index++];
	}	
	
	uint32_t file_vec_size = static_cast<uint32_t>(File_Vec.size());

	Profile_Vec.reserve(Profile_Vec.size() + file_vec_size);
	
	// Use the encryption library libsodium (XSalsa20-Poly1305) to encrypt the user's data file.

	uint8_t key[crypto_secretbox_KEYBYTES];	// 32 Bytes.
    	crypto_secretbox_keygen(key);

   	uint8_t nonce[crypto_secretbox_NONCEBYTES]; // 24 Bytes.
   	randombytes_buf(nonce, sizeof nonce);

	// Where we store the sodium keys, within the color profile segment.
	uint16_t
		sodium_key_index = 0x345,     
		nonce_key_index = 0x365;  
	
	// Write keys within the color profile segment.
	std::copy(std::begin(key), std::end(key), Profile_Vec.begin() + sodium_key_index); 	
	std::copy(std::begin(nonce), std::end(nonce), Profile_Vec.begin() + nonce_key_index);

    	std::vector<uint8_t> Encrypted_Vec(file_vec_size + crypto_secretbox_MACBYTES); // 16 Bytes for the MACBYTES.

    	// Using libsodium, encrypt the data file stored in the vector File_Vec.
    	crypto_secretbox_easy(Encrypted_Vec.data(), File_Vec.data(), file_vec_size, nonce, key);

	// Append the now encrypted data file to the vector Profile_Vec.
	Profile_Vec.insert(Profile_Vec.end(), Encrypted_Vec.begin(), Encrypted_Vec.end()); 
	
	std::vector<uint8_t>().swap(Encrypted_Vec); // Clear vector.
	
	// The PIN value is the first 8 bytes of the main sodium key. User needs to keep this safe, for file recovery.
	const uint64_t PIN = getByteValue<uint64_t>(Profile_Vec, sodium_key_index); 

	constexpr uint8_t 
		SODIUM_XOR_KEY_LENGTH = 9,	     // Length of XOR key, used to encrypt sodium keys. 
		SODIUM_XOR_KEY_START_INDEX = 0x94;   // Start index of 9 byte XOR key, used to encrypt part (24 bytes) of the main sodium key and all (24 bytes) of the nonce key.

	uint8_t
		sodium_xor_key_pos = SODIUM_XOR_KEY_START_INDEX,
		sodium_part_key_index = 0x95, 	// Index location where we temp store 8 bytes of the 32 byte sodium key. Used as part of a simple 9 byte XOR key.	
		sodium_keys_length = 48,	// 24 bytes of the 32 byte sodium key + 24 bytes (full) of the nonce key.
		value_bit_length = 64;

	valueUpdater(Profile_Vec, sodium_part_key_index, PIN, value_bit_length);

	// XOR part of the sodium key and all of the nonce key.
	
	sodium_key_index += 8; // Skip over the first 8 bytes of the main sodium key.

	while (sodium_keys_length--) {   // XOR encrypt 48 bytes.
    		Profile_Vec[sodium_key_index] = Profile_Vec[sodium_key_index] ^ Profile_Vec[sodium_xor_key_pos++];
		sodium_key_index++;
    		sodium_xor_key_pos = (sodium_xor_key_pos >= SODIUM_XOR_KEY_LENGTH + SODIUM_XOR_KEY_START_INDEX) 
                         ? SODIUM_XOR_KEY_START_INDEX // Reset PIN position once we hit PIN length.
                         : sodium_xor_key_pos;
	}
	
	// Wipe (zero) the temp 8 bytes of the sodium key we used as part of the 9 byte sodium XOR key, that we inserted at 0x95.
	valueUpdater(Profile_Vec, sodium_part_key_index, 0, value_bit_length);
	
	sodium_key_index = 0x345; // Reset index position.

	std::mt19937_64 gen64(rd()); 
    	std::uniform_int_distribution<uint64_t> dis64; 

    	uint64_t random_val = dis64(gen64); // Generate a random 8 byte value.

	// Overwrite the first 8 bytes of the main 32 byte sodium key with the random value.
	valueUpdater(Profile_Vec, sodium_key_index, random_val, value_bit_length);

	return PIN;
}