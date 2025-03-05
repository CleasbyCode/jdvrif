uint64_t encryptFile(std::vector<uint8_t>& profile_vec, std::vector<uint8_t>& data_file_vec, std::string& data_filename) {
	
	std::random_device rd;
 	std::mt19937 gen(rd());
	std::uniform_int_distribution<unsigned short> dis(1, 255); 
		
	constexpr uint16_t DATA_FILENAME_XOR_KEY_INDEX = 0x2F5;
		
	uint16_t 
		data_filename_index = 0x1EF,  			
		data_filename_xor_key_pos = DATA_FILENAME_XOR_KEY_INDEX;	

	uint8_t 
		data_filename_xor_key_length = 80,
		data_filename_length = profile_vec[data_filename_index - 1],
		data_filename_char_pos = 0;
	
	while(data_filename_xor_key_length--) {
		profile_vec[data_filename_xor_key_pos++] = static_cast<uint8_t>(dis(gen));
	}

	data_filename_xor_key_pos = DATA_FILENAME_XOR_KEY_INDEX;	

	while (data_filename_length--) {
		profile_vec[data_filename_index++] = data_filename[data_filename_char_pos++] ^ profile_vec[data_filename_xor_key_pos++];
	}	
	
	uint32_t data_file_vec_size = static_cast<uint32_t>(data_file_vec.size());

	profile_vec.reserve(profile_vec.size() + data_file_vec_size);
	
	std::array<uint8_t, crypto_secretbox_KEYBYTES> key;	
    	crypto_secretbox_keygen(key.data());

	std::array<uint8_t, crypto_secretbox_NONCEBYTES> nonce; 
   	randombytes_buf(nonce.data(), nonce.size());

	constexpr uint16_t
		SODIUM_KEY_INDEX = 0x345,     
		NONCE_KEY_INDEX  = 0x365;  
	
	std::copy(key.begin(), key.end(), profile_vec.begin() + SODIUM_KEY_INDEX); 	
	std::copy(nonce.begin(), nonce.end(), profile_vec.begin() + NONCE_KEY_INDEX);

    	std::vector<uint8_t> encrypted_vec(data_file_vec_size + crypto_secretbox_MACBYTES); 
	
    	crypto_secretbox_easy(encrypted_vec.data(), data_file_vec.data(), data_file_vec_size, nonce.data(), key.data());

	profile_vec.insert(profile_vec.end(), encrypted_vec.begin(), encrypted_vec.end()); 
	
	std::vector<uint8_t>().swap(encrypted_vec); 
	
	const uint64_t PIN = getByteValue<uint64_t>(profile_vec, SODIUM_KEY_INDEX); 

	uint16_t 
		sodium_xor_key_pos = SODIUM_KEY_INDEX,
		sodium_key_pos = SODIUM_KEY_INDEX;

	uint8_t
		sodium_keys_length = 48,	
		value_bit_length = 64;
	
	sodium_key_pos += 8; 

	constexpr uint8_t SODIUM_XOR_KEY_LENGTH = 8;  	

	while (sodium_keys_length--) {   
    		profile_vec[sodium_key_pos] = profile_vec[sodium_key_pos] ^ profile_vec[sodium_xor_key_pos++];
		sodium_key_pos++;
    		sodium_xor_key_pos = (sodium_xor_key_pos >= SODIUM_XOR_KEY_LENGTH + SODIUM_KEY_INDEX) 
                         ? SODIUM_KEY_INDEX 
                         : sodium_xor_key_pos;
	}
	
	sodium_key_pos = SODIUM_KEY_INDEX; 

	std::mt19937_64 gen64(rd()); 
    	std::uniform_int_distribution<uint64_t> dis64; 

    	uint64_t random_val = dis64(gen64); 

	valueUpdater(profile_vec, sodium_key_pos, random_val, value_bit_length);

	return PIN;
}
