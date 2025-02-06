// This project uses libsodium (https://libsodium.org/) for cryptographic functions.
// Copyright (c) 2013-2025 Frank Denis <github@pureftpd.org>
uint64_t encryptFile(std::vector<uint8_t>& Profile_Vec, std::vector<uint8_t>& File_Vec, std::string& filename) {
	std::random_device rd;
 	std::mt19937 gen(rd());
	std::uniform_int_distribution<unsigned short> dis(1, 255); 
		
	constexpr uint16_t FILENAME_XOR_KEY_INDEX = 0x2F5;
		
	uint16_t 
		filename_index = 0x1EF,  			
		filename_xor_key_pos = FILENAME_XOR_KEY_INDEX;	

	uint8_t 
		filename_xor_key_length = 80,
		filename_length = Profile_Vec[filename_index - 1],
		filename_char_pos = 0;
		
	while(filename_xor_key_length--) {
		Profile_Vec[filename_xor_key_pos++] = static_cast<uint8_t>(dis(gen));
	}

	filename_xor_key_pos = FILENAME_XOR_KEY_INDEX;	

	while (filename_length--) {
		Profile_Vec[filename_index++] = filename[filename_char_pos++] ^ Profile_Vec[filename_xor_key_pos++];
	}	
	
	uint32_t file_vec_size = static_cast<uint32_t>(File_Vec.size());

	Profile_Vec.reserve(Profile_Vec.size() + file_vec_size);
	
	uint8_t key[crypto_secretbox_KEYBYTES];	
    	crypto_secretbox_keygen(key);

   	uint8_t nonce[crypto_secretbox_NONCEBYTES]; 
   	randombytes_buf(nonce, sizeof nonce);
	
	constexpr uint16_t
		SODIUM_KEY_INDEX = 0x345,     
		NONCE_KEY_INDEX  = 0x365;  
	
	std::copy(std::begin(key), std::end(key), Profile_Vec.begin() + SODIUM_KEY_INDEX); 	
	std::copy(std::begin(nonce), std::end(nonce), Profile_Vec.begin() + NONCE_KEY_INDEX);

    	std::vector<uint8_t> Encrypted_Vec(file_vec_size + crypto_secretbox_MACBYTES); 
	
    	crypto_secretbox_easy(Encrypted_Vec.data(), File_Vec.data(), file_vec_size, nonce, key);

	Profile_Vec.insert(Profile_Vec.end(), Encrypted_Vec.begin(), Encrypted_Vec.end()); 
	
	std::vector<uint8_t>().swap(Encrypted_Vec); 

	const uint64_t PIN = getByteValue<uint64_t>(Profile_Vec, SODIUM_KEY_INDEX); 

	uint16_t 
		sodium_xor_key_pos = SODIUM_KEY_INDEX,
		sodium_key_pos = SODIUM_KEY_INDEX;

	uint8_t
		sodium_keys_length = 48,
		value_bit_length = 64;
	
	sodium_key_pos += 8; 

	constexpr uint8_t SODIUM_XOR_KEY_LENGTH = 8;  	

	while (sodium_keys_length--) {   
    		Profile_Vec[sodium_key_pos] = Profile_Vec[sodium_key_pos] ^ Profile_Vec[sodium_xor_key_pos++];
		sodium_key_pos++;
    		sodium_xor_key_pos = (sodium_xor_key_pos >= SODIUM_XOR_KEY_LENGTH + SODIUM_KEY_INDEX) 
                         ? SODIUM_KEY_INDEX 
                         : sodium_xor_key_pos;
	}
	
	sodium_key_pos = SODIUM_KEY_INDEX; 

	std::mt19937_64 gen64(rd()); 
    	std::uniform_int_distribution<uint64_t> dis64; 

    	uint64_t random_val = dis64(gen64); 

	valueUpdater(Profile_Vec, sodium_key_pos, random_val, value_bit_length);

	return PIN;
}
