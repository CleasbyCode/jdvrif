#include "jdvOut.h"

#define SODIUM_STATIC
#include <sodium.h>

// This project uses libsodium (https://libsodium.org/) for cryptographic functions.
// Copyright (c) 2013-2025 Frank Denis <github@pureftpd.org>

#include <zlib.h> 

// zlib.h -- interface of the 'zlib' general purpose compression library
// version 1.3.1, January 22nd, 2024

// Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler

// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.

// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:

// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.

// Jean-loup Gailly        Mark Adler
// jloup@gzip.org          madler@alumni.caltech.edu

#ifdef _WIN32
	#include <conio.h>
#else
	#include <termios.h>
	#include <unistd.h>
#endif

#include <array>
#include <vector>
#include <iterator> 
#include <cstddef>
#include <cstdlib>
#include <stdexcept>
#include <algorithm> 
#include <utility>  
#include <fstream>
#include <filesystem>
#include <iostream>

int jdvOut(const std::string& IMAGE_FILENAME) {
	const uintmax_t IMAGE_FILE_SIZE = std::filesystem::file_size(IMAGE_FILENAME);
	
	std::ifstream image_file_ifs(IMAGE_FILENAME, std::ios::binary);

	if (!image_file_ifs) {
		std::cerr << "\nOpen File Error: Unable to read image file.\n\n";
		return 1;
    	} 

	std::vector<uint8_t> image_vec(IMAGE_FILE_SIZE);

	image_file_ifs.read(reinterpret_cast<char*>(image_vec.data()), IMAGE_FILE_SIZE);
	image_file_ifs.close();
	
	constexpr uint8_t 
		SIG_LENGTH = 7,
		INDEX_DIFF = 8;

	constexpr std::array<uint8_t, SIG_LENGTH>
		JDVRIF_SIG		{ 0xB4, 0x6A, 0x3E, 0xEA, 0x5E, 0x9D, 0xF9 },
		ICC_PROFILE_SIG		{ 0x6D, 0x6E, 0x74, 0x72, 0x52, 0x47, 0x42 };
	
	auto searchSig = []<typename T, size_t N>(std::vector<uint8_t>& vec, const std::array<T, N>& SIG) -> uint32_t {
    		return static_cast<uint32_t>(std::search(vec.begin(), vec.end(), SIG.begin(), SIG.end()) - vec.begin());
	};
				
	const uint32_t 
		JDVRIF_SIG_INDEX	= searchSig(image_vec, JDVRIF_SIG),
		ICC_PROFILE_SIG_INDEX 	= searchSig(image_vec, ICC_PROFILE_SIG);

	if (JDVRIF_SIG_INDEX == image_vec.size()) {
		std::cerr << "\nImage File Error: Signature check failure. This is not a valid jdvrif \"file-embedded\" image.\n\n";
		return 1;
	}
	
	uint8_t pin_attempts_val = image_vec[JDVRIF_SIG_INDEX + INDEX_DIFF - 1];

	bool hasBlueskyOption = true;
		
	if (ICC_PROFILE_SIG_INDEX != image_vec.size()) {
		image_vec.erase(image_vec.begin(), image_vec.begin() + (ICC_PROFILE_SIG_INDEX - INDEX_DIFF));
		hasBlueskyOption = false;
	}

	if (hasBlueskyOption) { // EXIF segment (FFE1) is being used. Check for XMP segment.
		constexpr std::array<uint8_t, SIG_LENGTH> 
			XMP_SIG 	{ 0x68, 0x74, 0x74, 0x70, 0x3A, 0x2F, 0x2F },
			XMP_CREATOR_SIG { 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x6C, 0x69 };

		const uint32_t XMP_SIG_INDEX = searchSig(image_vec, XMP_SIG);

		if (XMP_SIG_INDEX != image_vec.size()) { // Found XMP segment.
			const uint32_t 
				XMP_CREATOR_SIG_INDEX = searchSig(image_vec, XMP_CREATOR_SIG),
				BEGIN_BASE64_DATA_INDEX = XMP_CREATOR_SIG_INDEX + SIG_LENGTH + 1;
			
			constexpr uint8_t END_BASE64_DATA_SIG = 0x3C;
			const uint32_t 
				END_BASE64_DATA_SIG_INDEX = static_cast<uint32_t>(std::find(image_vec.begin() + BEGIN_BASE64_DATA_INDEX,
											image_vec.end(), END_BASE64_DATA_SIG) - image_vec.begin()),
				BASE64_DATA_SIZE = END_BASE64_DATA_SIG_INDEX - BEGIN_BASE64_DATA_INDEX;
	
			std::vector<uint8_t> base64_data_vec(BASE64_DATA_SIZE);
			std::copy_n(image_vec.begin() + BEGIN_BASE64_DATA_INDEX, BASE64_DATA_SIZE, base64_data_vec.begin());

			// Convert Base64 data back to binary.
			static constexpr int8_t base64_decode_table[256] = {
        			-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        			-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        			-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63, 
        			52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1, 
        			-1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 
        			15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1, 
        			-1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 
        			41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1, 
        			-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        			-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        			-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        			-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        			-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        			-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        			-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        			-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
    			};

    			uint32_t input_size = static_cast<uint32_t>(base64_data_vec.size());
    			if (input_size == 0 || input_size % 4 != 0) {
        			throw std::invalid_argument("Base64 input size must be a multiple of 4 and non-empty");
    			}

    			uint32_t padding_count = 0;
    			if (base64_data_vec[input_size - 1] == '=') padding_count++;
    			if (base64_data_vec[input_size - 2] == '=') padding_count++;
    			for (uint32_t i = 0; i < input_size - padding_count; i++) {
        			if (base64_data_vec[i] == '=') {
            				throw std::invalid_argument("Invalid '=' character in Base64 input");
        			}
    			}

    			uint32_t output_size = (input_size / 4) * 3 - padding_count;
    			std::vector<uint8_t> temp_vec;
    			temp_vec.reserve(output_size);

    			for (uint32_t i = 0; i < input_size; i += 4) {
        			int sextet_a = base64_decode_table[base64_data_vec[i]];
        			int sextet_b = base64_decode_table[base64_data_vec[i + 1]];
        			int sextet_c = base64_decode_table[base64_data_vec[i + 2]];
        			int sextet_d = base64_decode_table[base64_data_vec[i + 3]];

        			if (sextet_a == -1 || sextet_b == -1 ||
            				(sextet_c == -1 && base64_data_vec[i + 2] != '=') ||
            					(sextet_d == -1 && base64_data_vec[i + 3] != '=')) {
            						throw std::invalid_argument("Invalid Base64 character encountered");
        			}

        			uint32_t triple = (sextet_a << 18) | (sextet_b << 12) | ((sextet_c & 0x3F) << 6) | (sextet_d & 0x3F);
        
        			temp_vec.emplace_back((triple >> 16) & 0xFF);
        			if (base64_data_vec[i + 2] != '=') temp_vec.emplace_back((triple >> 8) & 0xFF);
        			if (base64_data_vec[i + 3] != '=') temp_vec.emplace_back(triple & 0xFF);
    			}
    			base64_data_vec.swap(temp_vec);
    			std::vector<uint8_t>().swap(temp_vec);
    			// ------------

			const uint32_t END_OF_EXIF_DATA_INDEX = XMP_SIG_INDEX - 0x32;

			// Now append the XMP binary data to the EXIF binary segment data, so that we have the complete, single data file.
			std::copy_n(base64_data_vec.begin(), base64_data_vec.size(), image_vec.begin() + END_OF_EXIF_DATA_INDEX);
		}
	}
	constexpr uint32_t LARGE_FILE_SIZE = 300 * 1024 * 1024;

	if (IMAGE_FILE_SIZE > LARGE_FILE_SIZE) {
		std::cout << "\nPlease wait. Larger files will take longer to complete this process.\n";
	}
	
	// Decrypt embedded data file using the Libsodium cryptographic library.
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
	
	// Get recovery PIN from user input
	const std::string MAX_UINT64_STR = "18446744073709551615";
    	std::string input;
    	char ch; 
    	bool sync_status = std::cout.sync_with_stdio(false);
	
	#ifdef _WIN32
    		while (input.length() < 20) { 
	 		ch = _getch();
        		if (ch >= '0' && ch <= '9') {
            			input.push_back(ch);
            			std::cout << '*' << std::flush;  
        		} else if (ch == '\b' && !input.empty()) {  
            			std::cout << "\b \b" << std::flush;  
            			input.pop_back();
        		} else if (ch == '\r') {
            			break;
        		}
    		}
	#else   
    		struct termios oldt, newt;
    		tcgetattr(STDIN_FILENO, &oldt);
    		newt = oldt;
    		newt.c_lflag &= ~(ICANON | ECHO);
    		tcsetattr(STDIN_FILENO, TCSANOW, &newt);
	
   		while (input.length() < 20) {
        		ssize_t bytes_read = read(STDIN_FILENO, &ch, 1); 
        		if (bytes_read <= 0) continue; 
       
        		if (ch >= '0' && ch <= '9') {
            			input.push_back(ch);
            			std::cout << '*' << std::flush; 
        		} else if ((ch == '\b' || ch == 127) && !input.empty()) {  
            			std::cout << "\b \b" << std::flush;
            			input.pop_back();
        		} else if (ch == '\n') {
            			break;
        		}
    		}
    		tcsetattr(STDIN_FILENO, TCSANOW, &oldt); 
	#endif

    	std::cout << std::endl; 
    	std::cout.sync_with_stdio(sync_status);
	
    	uint64_t recovery_pin;
    	
    	if (input.empty() || (input.length() == 20 && input > MAX_UINT64_STR)) {
        	recovery_pin = 0; 
    	} else {
        	recovery_pin = std::stoull(input); 
    	}
	// -----------
	
	while (value_bit_length) {
		image_vec[sodium_key_pos++] = (recovery_pin >> (value_bit_length -= 8)) & 0xff;
	}
	
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
	
	constexpr uint16_t TOTAL_PROFILE_HEADER_SEGMENTS_INDEX 	= 0x2C8;

	const uint16_t 
		ENCRYPTED_FILE_START_INDEX	= hasBlueskyOption ? 0x1D1 : 0x33B,
		FILE_SIZE_INDEX 		= hasBlueskyOption ? 0x1CD : 0x2CA,
		TOTAL_PROFILE_HEADER_SEGMENTS 	= (static_cast<uint16_t>(image_vec[TOTAL_PROFILE_HEADER_SEGMENTS_INDEX]) << 8) 
							| static_cast<uint16_t>(image_vec[TOTAL_PROFILE_HEADER_SEGMENTS_INDEX + 1]);

	constexpr uint32_t COMMON_DIFF_VAL = 65537; // Size difference between each icc segment profile header.

	uint32_t embedded_file_size = 0;
	
	for (uint8_t i = 0; i < 4; ++i) {
        	embedded_file_size = (embedded_file_size << 8) | static_cast<uint32_t>(image_vec[FILE_SIZE_INDEX + i]);
    	}
		
	int32_t last_segment_index = (TOTAL_PROFILE_HEADER_SEGMENTS - 1) * COMMON_DIFF_VAL - 0x16;
	
	// Check embedded data file for corruption, such as missing data segments.
	if (TOTAL_PROFILE_HEADER_SEGMENTS && !hasBlueskyOption) {
		if (last_segment_index > static_cast<int32_t>(image_vec.size()) || image_vec[last_segment_index] != 0xFF || image_vec[last_segment_index + 1] != 0xE2) {
			std::cerr << "\nFile Extraction Error: Missing segments detected. Embedded data file is corrupt!\n\n";
			std::exit(0);
		}
	}
	
	std::vector<uint8_t> tmp_vec(image_vec.begin() + ENCRYPTED_FILE_START_INDEX, image_vec.begin() + ENCRYPTED_FILE_START_INDEX + embedded_file_size);
	image_vec = std::move(tmp_vec);

	std::vector<uint8_t>decrypted_file_vec;

	if (hasBlueskyOption || !TOTAL_PROFILE_HEADER_SEGMENTS) {
		decrypted_file_vec.resize(image_vec.size() - crypto_secretbox_MACBYTES);
		if (crypto_secretbox_open_easy(decrypted_file_vec.data(), image_vec.data(), image_vec.size(), nonce.data(), key.data()) !=0 ) {
			std::cerr << "\nDecryption failed!" << std::endl;
		}
	} else {		
		const uint32_t ENCRYPTED_FILE_SIZE = static_cast<uint32_t>(image_vec.size());

		uint32_t 
			header_index = 0xFCB0, // The first split segment profile header location, this is after the main header/icc profile, which was previously removed.
			index_pos = 0;
	
			std::vector<uint8_t>sanitize_vec; 
			sanitize_vec.reserve(ENCRYPTED_FILE_SIZE);

			constexpr uint8_t PROFILE_HEADER_LENGTH	= 18;

			// We need to avoid including the icc segment profile headers within the decrypted output file.
			// Because we know the total number of profile headers and their location (common difference val), 
			// we can just skip the header bytes when copying the data to the sanitize vector.
        		// This is much faster than having to search for and then using something like vec.erase to remove the header string from the vector.

			while (ENCRYPTED_FILE_SIZE > index_pos) {
				sanitize_vec.emplace_back(image_vec[index_pos++]);
				if (index_pos == header_index) {
					index_pos += PROFILE_HEADER_LENGTH; // Skip the header bytes.
					header_index += COMMON_DIFF_VAL;
				}	
			}

		std::vector<uint8_t>().swap(image_vec);
		decrypted_file_vec.resize(sanitize_vec.size() - crypto_secretbox_MACBYTES);
		if (crypto_secretbox_open_easy(decrypted_file_vec.data(), sanitize_vec.data(), sanitize_vec.size(), nonce.data(), key.data()) !=0 ) {
			std::cerr << "\nDecryption failed!" << std::endl;
		}
		std::vector<uint8_t>().swap(sanitize_vec);
	}
	image_vec.swap(decrypted_file_vec);
	std::vector<uint8_t>().swap(decrypted_file_vec);
	// ----------------	
	
	// Uncompress the decrypted data file using zlib inflate.
	constexpr uint32_t BUFSIZE = 2 * 1024 * 1024;

    	std::vector<uint8_t> buffer(BUFSIZE); 
    	std::vector<uint8_t> inflate_vec;
    	inflate_vec.reserve(image_vec.size() + BUFSIZE);

    	z_stream strm = {};
    	strm.next_in = image_vec.data();
    	strm.avail_in = static_cast<uint32_t>(image_vec.size());
    	strm.next_out = buffer.data();
    	strm.avail_out = BUFSIZE;

    	if (inflateInit(&strm) != Z_OK) {
        	return 0;
    	}

    	while (strm.avail_in > 0) {
        	int ret = inflate(&strm, Z_NO_FLUSH);
        	if (ret == Z_STREAM_END) break;
        	if (ret != Z_OK) {
            		inflateEnd(&strm);
            		return 0; 
        	}

        	if (strm.avail_out == 0) {
            		inflate_vec.insert(inflate_vec.end(), buffer.begin(), buffer.end());
            		strm.next_out = buffer.data();
            		strm.avail_out = BUFSIZE;
        	}
    	}

    	int ret;
    	do {
        	ret = inflate(&strm, Z_FINISH);
        	size_t bytes_written = BUFSIZE - strm.avail_out;
        	inflate_vec.insert(inflate_vec.end(), buffer.begin(), buffer.begin() + bytes_written);
        	strm.next_out = buffer.data();
        	strm.avail_out = BUFSIZE;
    	} while (ret == Z_OK);

    	inflateEnd(&strm);

    	image_vec = std::move(inflate_vec);
    	std::vector<uint8_t>().swap(inflate_vec);
    	
	const uint32_t INFLATED_FILE_SIZE = static_cast<uint32_t>(image_vec.size());
	// -------------

	bool hasInflateFailed = !INFLATED_FILE_SIZE;
	
	std::streampos pin_attempts_index = JDVRIF_SIG_INDEX + INDEX_DIFF - 1;
			 
	if (hasInflateFailed) {	
		std::fstream file(IMAGE_FILENAME, std::ios::in | std::ios::out | std::ios::binary);
		
		if (pin_attempts_val == 0x90) {
			pin_attempts_val = 0;
		} else {
    			pin_attempts_val++;
		}

		if (pin_attempts_val > 2) {
			file.close();
			std::ofstream file(IMAGE_FILENAME, std::ios::out | std::ios::trunc | std::ios::binary);
		} else {
			file.seekp(pin_attempts_index);
			file.write(reinterpret_cast<char*>(&pin_attempts_val), sizeof(pin_attempts_val));
		}

		file.close();

		std::cerr << "\nFile Recovery Error: Invalid PIN or file is corrupt.\n\n";
		return 1;
	}

	if (pin_attempts_val != 0x90) {
		std::fstream file(IMAGE_FILENAME, std::ios::in | std::ios::out | std::ios::binary);
		
		uint8_t reset_pin_attempts_val = 0x90;

		file.seekp(pin_attempts_index);
		file.write(reinterpret_cast<char*>(&reset_pin_attempts_val), sizeof(reset_pin_attempts_val));

		file.close();
	}

	std::ofstream file_ofs(decrypted_filename, std::ios::binary);

	if (!file_ofs) {
		std::cerr << "\nWrite Error: Unable to write to file.\n\n";
		return 1;
	}

	file_ofs.write(reinterpret_cast<const char*>(image_vec.data()), INFLATED_FILE_SIZE);

	std::vector<uint8_t>().swap(image_vec);

	std::cout << "\nExtracted hidden file: " << decrypted_filename << " (" << INFLATED_FILE_SIZE << " bytes).\n\nComplete! Please check your file.\n\n";
	return 0;
}
