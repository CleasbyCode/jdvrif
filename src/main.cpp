//	JPG Data Vehicle (jdvin v4.5) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023

//	Compile program (Linux):

//	$ sudo apt-get install libsodium-dev
//	$ sudo apt-get install libturbojpeg0-dev

//	$ chmod +x compile_jdvrif.sh
//	$ ./compile_jdvrif.sh
	
//	$ Compilation successful. Executable 'jdvrif' created.
//	$ sudo cp jdvrif /usr/bin
//	$ jdvrif

#include <turbojpeg.h>

// This software is based in part on the work of the Independent JPEG Group.
// Copyright (C) 2009-2024 D. R. Commander. All Rights Reserved.
// Copyright (C) 2015 Viktor Szathmáry. All Rights Reserved.
// https://github.com/libjpeg-turbo/libjpeg-turbo

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

#define SODIUM_STATIC
#include <sodium.h>

// This project uses libsodium (https://libsodium.org/) for cryptographic functions.
// Copyright (c) 2013-2025 Frank Denis <github@pureftpd.org>

#ifdef _WIN32
	#include <conio.h>
#else
	#include <termios.h>
	#include <unistd.h>
#endif

#include "segmentsVec.h" 
#include "fileChecks.h" 

#include <array>
#include <algorithm>
#include <utility>        
#include <cstddef>
#include <cstdlib>
#include <stdexcept>
#include <fstream>          
#include <iostream>         
#include <filesystem>       
#include <random>       
#include <iterator>   

int main(int argc, char** argv) {
	try {
		ProgramArgs args = ProgramArgs::parse(argc, argv);
		
		std::ifstream image_file_ifs(args.image_file, std::ios::binary);
        	
        	if (!image_file_ifs) {
        		throw std::runtime_error("Read File Error: Unable to read image file. Check the filename and try again.");
		}
		
		std::filesystem::path image_path(args.image_file), data_path(args.data_file);
        		
        	const uintmax_t IMAGE_FILE_SIZE = std::filesystem::file_size(image_path);
        	
        	constexpr uint32_t LARGE_FILE_SIZE = 300 * 1024 * 1024; 
        	
		const std::string LARGE_FILE_MSG = "\nPlease wait. Larger files will take longer to complete this process.\n";
		
        	validateImageFile(args.image_file, args.mode, args.platform, IMAGE_FILE_SIZE);
        	
        	std::vector<uint8_t> image_vec(IMAGE_FILE_SIZE); 
        	
		image_file_ifs.read(reinterpret_cast<char*>(image_vec.data()), IMAGE_FILE_SIZE);
		image_file_ifs.close();
        	
        	if (args.mode == ArgMode::conceal) {  // Embed data file section code.
        	
        		std::ifstream data_file_ifs(args.data_file, std::ios::binary);

			if (!data_file_ifs) {
				throw std::runtime_error("Read File Error: Unable to read data file. Check the filename and try again.");
			}
				
        		const uintmax_t DATA_FILE_SIZE = std::filesystem::file_size(data_path);
        
        		validateDataFile(args.data_file, args.platform, IMAGE_FILE_SIZE, DATA_FILE_SIZE);
        		
        		bool isCompressed = isCompressedFile(data_path.extension().string());
        		
        		// Lambda function writes new values, such as segments lengths, etc. into the relevant vector index locations.	
			auto updateValue = [](std::vector<uint8_t>& vec, uint32_t insert_index, const uint64_t NEW_VALUE, uint8_t bits) {
				while (bits) { vec[insert_index++] = (NEW_VALUE >> (bits -= 8)) & 0xff; }	// Big-endian.
			};
	
			constexpr uint8_t SIG_LENGTH = 2;

			constexpr std::array<uint8_t, SIG_LENGTH>
				IMAGE_START_SIG	{ 0xFF, 0xD8 },
				IMAGE_END_SIG   { 0xFF, 0xD9 };

			if (!std::equal(IMAGE_START_SIG.begin(), IMAGE_START_SIG.end(), image_vec.begin()) || !std::equal(IMAGE_END_SIG.begin(), IMAGE_END_SIG.end(), image_vec.end() - 2)) {
        			throw std::runtime_error("Image File Error: This is not a valid JPG image.");
			}
	
			bool 
				hasBlueskyOption = (args.platform == ArgOption::bluesky),
				hasRedditOption = (args.platform == ArgOption::reddit),
				hasNoneOption = (args.platform == ArgOption::none);
	
			// To improve compatibility, default re-encode image to JPG Progressive format with a quality value set at 97 with no chroma subsampling.
			// If Bluesky option, re-encode to standard Baseline format with a quality value set at 85.
			tjhandle decompressor = tjInitDecompress();
    			if (!decompressor) {
        			throw std::runtime_error("tjInitDecompress() failed.");
    			}

    			int width = 0, height = 0, jpegSubsamp = 0, jpegColorspace = 0;
    			if (tjDecompressHeader3(decompressor, image_vec.data(), static_cast<unsigned long>(image_vec.size()), &width, &height, &jpegSubsamp, &jpegColorspace) != 0) {
        			tjDestroy(decompressor);
        			throw std::runtime_error(std::string("tjDecompressHeader3: ") + tjGetErrorStr());
    			}

    			std::vector<uint8_t> decoded_image_vec(width * height * 3); 
    			if (tjDecompress2(decompressor, image_vec.data(),static_cast<unsigned long>(image_vec.size()), decoded_image_vec.data(), width, 0, height, TJPF_RGB, 0) != 0) {
        			tjDestroy(decompressor);
        			throw std::runtime_error(std::string("tjDecompress2: ") + tjGetErrorStr());
    			}
    			tjDestroy(decompressor);
    			tjhandle compressor = tjInitCompress();
    			if (!compressor) {
        			throw std::runtime_error("tjInitCompress() failed.");
    			}

    			const uint8_t JPG_QUALITY_VAL = hasBlueskyOption ? 85 : 97;

    			unsigned char* jpegBuf = nullptr;
    			unsigned long jpegSize = 0;

    			int flags = hasBlueskyOption ? TJFLAG_ACCURATEDCT : TJFLAG_PROGRESSIVE | TJFLAG_ACCURATEDCT;

    			if (tjCompress2(compressor, decoded_image_vec.data(), width, 0, height, TJPF_RGB, &jpegBuf, &jpegSize, TJSAMP_444, JPG_QUALITY_VAL, flags) != 0) {
        			tjDestroy(compressor);
        			throw std::runtime_error(std::string("tjCompress2: ") + tjGetErrorStr());
    			}
    			tjDestroy(compressor);

    			std::vector<uint8_t> output_image_vec(jpegBuf, jpegBuf + jpegSize);
    			tjFree(jpegBuf);
    			image_vec.swap(output_image_vec);
    			std::vector<uint8_t>().swap(output_image_vec);
			// ------------
	
			// Remove superfluous segments from cover image. (EXIF, ICC color profile, etc).
			constexpr std::array<uint8_t, 2>
				APP1_SIG { 0xFF, 0xE1 }, // EXIF SEGMENT MARKER.
				APP2_SIG { 0xFF, 0xE2 }; // ICC COLOR PROFILE SEGMENT MARKER.

			constexpr std::array<uint8_t, 4>
				DQT1_SIG { 0xFF, 0xDB, 0x00, 0x43 },
				DQT2_SIG { 0xFF, 0xDB, 0x00, 0x84 };
		
			auto searchSig = []<typename T, size_t N>(std::vector<uint8_t>& vec, const std::array<T, N>& SIG) -> uint32_t {
    				return static_cast<uint32_t>(std::search(vec.begin(), vec.end(), SIG.begin(), SIG.end()) - vec.begin());
			};

			const uint32_t APP1_POS = searchSig(image_vec, APP1_SIG);

			if (image_vec.size() > APP1_POS) {
				const uint16_t APP1_BLOCK_SIZE = (static_cast<uint16_t>(image_vec[APP1_POS + 2]) << 8) | static_cast<uint16_t>(image_vec[APP1_POS + 3]);
				image_vec.erase(image_vec.begin() + APP1_POS, image_vec.begin() + APP1_POS + APP1_BLOCK_SIZE + 2);
			}

			const uint32_t APP2_POS = searchSig(image_vec, APP2_SIG);
			if (image_vec.size() > APP2_POS) {
				const uint16_t APP2_BLOCK_SIZE = (static_cast<uint16_t>(image_vec[APP2_POS + 2]) << 8) | static_cast<uint16_t>(image_vec[APP2_POS + 3]);
				image_vec.erase(image_vec.begin() + APP2_POS, image_vec.begin() + APP2_POS + APP2_BLOCK_SIZE + 2);
			}

			const uint32_t
				DQT1_POS = searchSig(image_vec, DQT1_SIG),
				DQT2_POS = searchSig(image_vec, DQT2_SIG),
				DQT_POS  = std::min(DQT1_POS, DQT2_POS);

			image_vec.erase(image_vec.begin(), image_vec.begin() + DQT_POS);
			// ------------
	
    			std::string data_filename = data_path.filename().string();

			constexpr uint8_t DATA_FILENAME_MAX_LENGTH = 20;

			const uint8_t DATA_FILENAME_LENGTH = static_cast<uint8_t>(data_filename.size());

			if (DATA_FILENAME_LENGTH > DATA_FILENAME_MAX_LENGTH) {
    				throw std::runtime_error("Data File Error: For compatibility requirements, length of data filename must not exceed 20 characters.");
			}

			if (hasBlueskyOption) {
				segment_vec.swap(bluesky_exif_vec);	// Use the EXIF segment instead of the default color profile segment to store user data.
			}						// The color profile segment (FFE2) is removed by Bluesky, so we use EXIF.

			const uint16_t DATA_FILENAME_LENGTH_INDEX = hasBlueskyOption ? 0x160 : 0x2E6;

			segment_vec[DATA_FILENAME_LENGTH_INDEX] = DATA_FILENAME_LENGTH;	 

			if (DATA_FILE_SIZE > LARGE_FILE_SIZE) {
				std::cout << LARGE_FILE_MSG;
			}
	
			std::vector<uint8_t> data_file_vec(DATA_FILE_SIZE); 

			data_file_ifs.read(reinterpret_cast<char*>(data_file_vec.data()), DATA_FILE_SIZE);
			data_file_ifs.close();
	
			// Compress data file using zlib deflate.	
			constexpr uint32_t 
        			FIFTH_SIZE_OPTION   = 800 * 1024 * 1024,
        			FOURTH_SIZE_OPTION  = 450 * 1024 * 1024,
        			THIRD_SIZE_OPTION   = 200 * 1024 * 1024,
        			SECOND_SIZE_OPTION  = 100 * 1024 * 1024,
        			FIRST_SIZE_OPTION   = 5 * 1024 * 1024,
        			BUFSIZE             = 2 * 1024 * 1024;

    			std::vector<uint8_t> buffer(BUFSIZE); 
    			std::vector<uint8_t> deflate_vec;
    			deflate_vec.reserve(DATA_FILE_SIZE + BUFSIZE);

    			z_stream strm = {};
    			strm.next_in = data_file_vec.data();
    			strm.avail_in = static_cast<uint32_t>(DATA_FILE_SIZE);
    			strm.next_out = buffer.data();
    			strm.avail_out = BUFSIZE;

    			int compression_level = Z_DEFAULT_COMPRESSION;

    			if (FIRST_SIZE_OPTION > DATA_FILE_SIZE && isCompressed) {
        			compression_level = Z_NO_COMPRESSION;
    			} else if (SECOND_SIZE_OPTION > DATA_FILE_SIZE && isCompressed) {
        			compression_level = Z_BEST_SPEED;
    			} else if (isCompressed || DATA_FILE_SIZE > FIFTH_SIZE_OPTION) {
        			compression_level = Z_NO_COMPRESSION;
    			} else if (DATA_FILE_SIZE > FOURTH_SIZE_OPTION) {
        			compression_level = Z_BEST_SPEED;
    			} else if (DATA_FILE_SIZE > THIRD_SIZE_OPTION) {
        			compression_level = Z_DEFAULT_COMPRESSION;
    			} else {
        			compression_level = Z_BEST_COMPRESSION;
    			}

    			deflateInit(&strm, compression_level);

    			while (strm.avail_in > 0) {
        			int ret = deflate(&strm, Z_NO_FLUSH);
        			if (ret != Z_OK) break;

        			if (strm.avail_out == 0) {
            				deflate_vec.insert(deflate_vec.end(), buffer.begin(), buffer.end());
            				strm.next_out = buffer.data();
            				strm.avail_out = BUFSIZE;
        			}
    			}

    			int ret;
    			do {
        			ret = deflate(&strm, Z_FINISH);
        			size_t bytes_written = BUFSIZE - strm.avail_out;
        			deflate_vec.insert(deflate_vec.end(), buffer.begin(), buffer.begin() + bytes_written);
        			strm.next_out = buffer.data();
        			strm.avail_out = BUFSIZE;
    			} while (ret == Z_OK);

    			deflateEnd(&strm);

    			data_file_vec = std::move(deflate_vec);
    			std::vector<uint8_t>().swap(deflate_vec);
			// ------------
	
			// Encrypt data file using the Libsodium cryptographic library
			std::random_device rd;
 			std::mt19937 gen(rd());
			std::uniform_int_distribution<unsigned short> dis(1, 255); 
		
			constexpr uint8_t XOR_KEY_LENGTH = 24;
	
			uint16_t
				data_filename_xor_key_index = hasBlueskyOption ? 0x175 : 0x2FB,
				data_filename_index = hasBlueskyOption ? 0x161: 0x2E7;
		
			uint8_t
				value_bit_length = 32, 
				data_filename_length = segment_vec[data_filename_index - 1],
				data_filename_char_pos = 0;

				std::generate_n(segment_vec.begin() + data_filename_xor_key_index, XOR_KEY_LENGTH, [&dis, &gen]() { return static_cast<uint8_t>(dis(gen)); });

			std::transform(
        			data_filename.begin() + data_filename_char_pos, data_filename.begin() + data_filename_char_pos + data_filename_length,
        			segment_vec.begin() + data_filename_xor_key_index, segment_vec.begin() + data_filename_index,
        			[](char a, uint8_t b) { return static_cast<uint8_t>(a) ^ b; }
    			);	
	
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
				constexpr uint16_t EXIF_SEGMENT_DATA_SIZE_LIMIT = 65027; // + With EXIF overhead segment data (511) - four bytes we don't count (FFD8 FFE1),  
								         		 // = Max. segment size 65534 (0xFFFE). Can't have 65535 (0xFFFF) as Bluesky will strip the EXIF segment.
				const uint32_t ENCRYPTED_VEC_SIZE = static_cast<uint32_t>(encrypted_vec.size());
		
				uint16_t compressed_file_size_index = 0x1CD;
		
				value_bit_length = 32;					 	 
		
				updateValue(segment_vec, compressed_file_size_index, ENCRYPTED_VEC_SIZE, value_bit_length);

				// Split the data file if it exceeds the max compressed EXIF capacity of ~64KB. 
				// We can then use the second segment (XMP) for the remaining data.

				if (ENCRYPTED_VEC_SIZE > EXIF_SEGMENT_DATA_SIZE_LIMIT) {
					segment_vec.insert(segment_vec.begin() + EXIF_SEGMENT_DATA_INSERT_INDEX, encrypted_vec.begin(), encrypted_vec.begin() + EXIF_SEGMENT_DATA_SIZE_LIMIT);

					const uint32_t REMAINING_DATA_SIZE = ENCRYPTED_VEC_SIZE - EXIF_SEGMENT_DATA_SIZE_LIMIT;
			
					std::vector<uint8_t> tmp_xmp_vec(REMAINING_DATA_SIZE);
			
					std::copy_n(encrypted_vec.begin() + EXIF_SEGMENT_DATA_SIZE_LIMIT, REMAINING_DATA_SIZE, tmp_xmp_vec.begin());
			
					// We can only store Base64 encoded data in the XMP segment, so convert the binary data here.
					static constexpr uint8_t base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    					uint32_t input_size = static_cast<uint32_t>(tmp_xmp_vec.size());
    					uint32_t output_size = ((input_size + 2) / 3) * 4; 

    					std::vector<uint8_t> temp_vec(output_size); 

    					uint32_t j = 0;
    					for (uint32_t i = 0; i < input_size; i += 3) {
        					uint32_t octet_a = tmp_xmp_vec[i];
        					uint32_t octet_b = (i + 1 < input_size) ? tmp_xmp_vec[i + 1] : 0;
        					uint32_t octet_c = (i + 2 < input_size) ? tmp_xmp_vec[i + 2] : 0;
        			
        					uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        					temp_vec[j++] = base64_table[(triple >> 18) & 0x3F];
        					temp_vec[j++] = base64_table[(triple >> 12) & 0x3F];
        					temp_vec[j++] = (i + 1 < input_size) ? base64_table[(triple >> 6) & 0x3F] : '=';
        					temp_vec[j++] = (i + 2 < input_size) ? base64_table[triple & 0x3F] : '=';
    					}
    					tmp_xmp_vec.swap(temp_vec);
    					std::vector<uint8_t>().swap(temp_vec);
					// ------------
			
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
	
			uint64_t pin = 0;
	
			for (uint8_t i = 0; i < 8; ++i) {
        			pin = (pin << 8) | static_cast<uint64_t>(segment_vec[SODIUM_KEY_INDEX + i]);
    			}
	
			uint16_t 
				sodium_xor_key_pos = SODIUM_KEY_INDEX,
				sodium_key_pos = SODIUM_KEY_INDEX;

			uint8_t sodium_keys_length = 48;
	
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

			updateValue(segment_vec, sodium_key_pos, RANDOM_VAL, value_bit_length);
			// ------------
	
			std::vector<uint8_t>().swap(data_file_vec);

			value_bit_length = 16;

			if (hasBlueskyOption) {	 // We can store binary data within the first (EXIF) segment, with a max compressed storage capacity close to ~64KB. See encryptFile.cpp
				constexpr uint8_t MARKER_BYTES_VAL = 4; // FFD8, FFE1

				const uint32_t EXIF_SEGMENT_SIZE = static_cast<uint32_t>(segment_vec.size() - MARKER_BYTES_VAL);

				uint8_t	
					segment_size_field_index = 0x04,  
					exif_segment_xres_offset_field_index = 0x2A,
					exif_segment_yres_offset_field_index = 0x36, 
					exif_segment_artist_size_field_index = 0x4A,
					exif_segment_subifd_offset_field_index = 0x5A;

				const uint16_t	
					EXIF_XRES_OFFSET   = EXIF_SEGMENT_SIZE - 0x36,
					EXIF_YRES_OFFSET   = EXIF_SEGMENT_SIZE - 0x2E,
					EXIF_SUBIFD_OFFSET = EXIF_SEGMENT_SIZE - 0x26,
					EXIF_ARTIST_SIZE   = EXIF_SEGMENT_SIZE - 0x8C;

				updateValue(segment_vec, segment_size_field_index, EXIF_SEGMENT_SIZE, value_bit_length);
		
				value_bit_length = 32;

				updateValue(segment_vec, exif_segment_xres_offset_field_index, EXIF_XRES_OFFSET, value_bit_length);
				updateValue(segment_vec, exif_segment_yres_offset_field_index, EXIF_YRES_OFFSET, value_bit_length);
				updateValue(segment_vec, exif_segment_artist_size_field_index, EXIF_ARTIST_SIZE, value_bit_length); 
				updateValue(segment_vec, exif_segment_subifd_offset_field_index, EXIF_SUBIFD_OFFSET, value_bit_length);

				constexpr uint16_t BLUESKY_XMP_VEC_DEFAULT_SIZE = 405;  // XMP segment size without user data.
		
				const uint32_t BLUESKY_XMP_VEC_SIZE = static_cast<uint32_t>(bluesky_xmp_vec.size());

				// Are we using the second (XMP) segment?
				if (BLUESKY_XMP_VEC_SIZE > BLUESKY_XMP_VEC_DEFAULT_SIZE) {

					// Size includes segment SIG two bytes (don't count). Bluesky will strip XMP data segment greater than 60031 bytes (0xEA7F).
					// With the overhead of the XMP default segment data (405 bytes) and the Base64 encoding overhead (~33%),
					// The max compressed data storage in this segment is probably around ~40KB. 

 					constexpr uint16_t XMP_SEGMENT_SIZE_LIMIT = 60033;  

					if (BLUESKY_XMP_VEC_SIZE > XMP_SEGMENT_SIZE_LIMIT) {
						throw std::runtime_error("File Size Error: Data file exceeds segment size limit for Bluesky.");
					}

					constexpr uint8_t SEGMENT_SIG_LENGTH = 2; // FFE1

					segment_size_field_index = 0x02;

					value_bit_length = 16;
					updateValue(bluesky_xmp_vec, segment_size_field_index, BLUESKY_XMP_VEC_SIZE - SEGMENT_SIG_LENGTH, value_bit_length);
			
					std::copy_n(bluesky_xmp_vec.begin(), BLUESKY_XMP_VEC_SIZE, std::back_inserter(segment_vec));
				}
				image_vec.insert(image_vec.begin(), segment_vec.begin(), segment_vec.end());
					platforms_vec[0] = std::move(platforms_vec[2]);
					platforms_vec.resize(1);
			} else {
				// Default segment_vec uses color profile segment (FFE2) to store data file. If required, split data file and use multiple segments for these larger files.
				constexpr uint8_t
				IMAGE_START_SIG_LENGTH	  = 2,
				ICC_SEGMENT_SIG_LENGTH	  = 2,
				ICC_SEGMENT_HEADER_LENGTH = 16;

				uint16_t icc_segment_data_size = 65519;  // Max. data for each segment (Not including header and signature bytes).

				uint32_t 
					icc_profile_with_data_file_vec_size = static_cast<uint32_t>(segment_vec.size()),
					max_first_segment_size = icc_segment_data_size + IMAGE_START_SIG_LENGTH + ICC_SEGMENT_SIG_LENGTH + ICC_SEGMENT_HEADER_LENGTH;
		
				if (icc_profile_with_data_file_vec_size > max_first_segment_size) { 
					// Data file is too large for a single icc segment, so split data file in to multiple icc segments.
					constexpr uint8_t LIBSODIUM_MACBYTES = 16;
					// 16 byte authentication tag used by libsodium. Don't count these bytes as part of the data file, as they will be removed during the decryption process.
	
					icc_profile_with_data_file_vec_size -= LIBSODIUM_MACBYTES;

					uint16_t 
						icc_segments_required       = (icc_profile_with_data_file_vec_size / icc_segment_data_size) + 1, // Usually a remainder segment, so plus 1 here.
						icc_segment_remainder_size  = (icc_profile_with_data_file_vec_size % icc_segment_data_size) - (IMAGE_START_SIG_LENGTH + ICC_SEGMENT_SIG_LENGTH),
						icc_segments_sequence_val   = 1;
			
					constexpr uint16_t ICC_SEGMENTS_TOTAL_VAL_INDEX = 0x2E0;  // The value stored here is used by jdvout when extracting the data file.
					updateValue(segment_vec, ICC_SEGMENTS_TOTAL_VAL_INDEX, !icc_segment_remainder_size ? --icc_segments_required : icc_segments_required, value_bit_length);

					uint8_t 
						icc_segments_sequence_val_index = 0x11,
						icc_segment_remainder_size_index = 0x04;

					std::vector<uint8_t> icc_segment_header_vec { 
						segment_vec.begin(), segment_vec.begin() + IMAGE_START_SIG_LENGTH + ICC_SEGMENT_SIG_LENGTH + ICC_SEGMENT_HEADER_LENGTH 
					};

					// Because of some duplicate data, erase the first 20 bytes of segment_vec because they will be replaced when splitting the data file.
    					segment_vec.erase(segment_vec.begin(), segment_vec.begin() + (IMAGE_START_SIG_LENGTH + ICC_SEGMENT_SIG_LENGTH + ICC_SEGMENT_HEADER_LENGTH));

					data_file_vec.reserve(icc_profile_with_data_file_vec_size + (icc_segments_required * (ICC_SEGMENT_SIG_LENGTH + ICC_SEGMENT_HEADER_LENGTH)));

					uint32_t byte_index = 0;

					while (icc_segments_required--) {	
						if (!icc_segments_required) {
							if (icc_segment_remainder_size) {
								icc_segment_data_size = icc_segment_remainder_size;	
			   					updateValue(icc_segment_header_vec, icc_segment_remainder_size_index, 
			   						(icc_segment_remainder_size + ICC_SEGMENT_HEADER_LENGTH), value_bit_length);
							} else {
								break;
							}	 	
						}
						std::copy_n(icc_segment_header_vec.begin() + IMAGE_START_SIG_LENGTH,
							 ICC_SEGMENT_SIG_LENGTH + ICC_SEGMENT_HEADER_LENGTH, std::back_inserter(data_file_vec));
						std::copy_n(segment_vec.begin() + byte_index, icc_segment_data_size, std::back_inserter(data_file_vec));
						updateValue(icc_segment_header_vec, icc_segments_sequence_val_index, ++icc_segments_sequence_val, value_bit_length);
						byte_index += icc_segment_data_size;
					}

					std::vector<uint8_t>().swap(segment_vec);
		
					// Insert the start of image sig bytes that were removed.
					data_file_vec.insert(data_file_vec.begin(), icc_segment_header_vec.begin(), icc_segment_header_vec.begin() + IMAGE_START_SIG_LENGTH);

				} else {  
					// Data file is small enough to fit within a single icc profile segment.
					constexpr uint8_t
						ICC_SEGMENT_HEADER_SIZE_INDEX 	= 0x04, 
						ICC_PROFILE_SIZE_INDEX  	= 0x16, 
						ICC_PROFILE_SIZE_DIFF   	= 16;
			
					const uint16_t 
						SEGMENT_SIZE 	 = icc_profile_with_data_file_vec_size - (IMAGE_START_SIG_LENGTH + ICC_SEGMENT_SIG_LENGTH),
						ICC_SEGMENT_SIZE = SEGMENT_SIZE - ICC_PROFILE_SIZE_DIFF;

					updateValue(segment_vec, ICC_SEGMENT_HEADER_SIZE_INDEX, SEGMENT_SIZE, value_bit_length);
					updateValue(segment_vec, ICC_PROFILE_SIZE_INDEX, ICC_SEGMENT_SIZE, value_bit_length);

					data_file_vec = std::move(segment_vec);
				}
		
				value_bit_length = 32; 

				constexpr uint16_t 
					DEFLATED_DATA_FILE_SIZE_INDEX = 0x2E2,	// The size value stored here is used by jdvout when extracting the data file.
					ICC_PROFILE_DATA_SIZE = 851; // Color profile data size, not including user data file size.
	
				updateValue(data_file_vec, DEFLATED_DATA_FILE_SIZE_INDEX, static_cast<uint32_t>(data_file_vec.size()) - ICC_PROFILE_DATA_SIZE, value_bit_length);
				// -------
		
				image_vec.reserve(IMAGE_FILE_SIZE + data_file_vec.size());	
				if (hasRedditOption) {
					image_vec.insert(image_vec.begin(), IMAGE_START_SIG.begin(), IMAGE_START_SIG.end());
					image_vec.insert(image_vec.end() - 2, 8000, 0x23);
					image_vec.insert(image_vec.end() - 2, data_file_vec.begin() + 2, data_file_vec.end());
					platforms_vec[0] = std::move(platforms_vec[4]);
					platforms_vec.resize(1);
				} else {
					platforms_vec.erase(platforms_vec.begin() + 4); 
					platforms_vec.erase(platforms_vec.begin() + 2);
					image_vec.insert(image_vec.begin(), data_file_vec.begin(), data_file_vec.end());
				}
				std::vector<uint8_t>().swap(data_file_vec);
			}	
	
    			std::uniform_int_distribution<> dist(10000, 99999);  

			const std::string OUTPUT_FILENAME = "jrif_" + std::to_string(dist(gen)) + ".jpg";

			std::ofstream file_ofs(OUTPUT_FILENAME, std::ios::binary);

			if (!file_ofs) {
				throw std::runtime_error("Write File Error: Unable to write to file.");
			}
	
			const uint32_t IMAGE_SIZE = static_cast<uint32_t>(image_vec.size());

			file_ofs.write(reinterpret_cast<const char*>(image_vec.data()), IMAGE_SIZE);
			
			if (hasNoneOption) {
				const uint32_t 
					FLICKR_MAX_IMAGE_SIZE = 200 * 1024 * 1024,
					IMGPILE_MAX_IMAGE_SIZE = 100 * 1024 * 1024,
					IMGBB_POSTIMAGE_MAX_IMAGE_SIZE = 32 * 1024 * 1024,
					MASTODON_MAX_IMAGE_SIZE = 16 * 1024 * 1024,
					TWITTER_MAX_IMAGE_SIZE = 5 * 1024 * 1024;
					
				const uint16_t 
					TWITTER_MAX_DATA_SIZE = 10 * 1024,
					TUMBLR_MAX_DATA_SIZE =  64 * 1024 - 2,
					FIRST_SEGMENT_SIZE = (image_vec[0x04] << 8) | image_vec[0x05],
					TOTAL_SEGMENTS = (image_vec[0x2E0] << 8) | image_vec[0x2E1];
				
				const uint8_t MASTODON_MAX_SEGMENTS = 100;
				
				std::vector<std::string> filtered_platforms;

				for (const std::string& platform : platforms_vec) {
    					if (platform == "X-Twitter" && (FIRST_SEGMENT_SIZE > TWITTER_MAX_DATA_SIZE || IMAGE_SIZE > TWITTER_MAX_IMAGE_SIZE)) {
        					continue;
    					}
    					if (platform == "Tumblr" && (FIRST_SEGMENT_SIZE > TUMBLR_MAX_DATA_SIZE)) {
        					continue;
    					}
    					if (platform == "Mastodon" && (TOTAL_SEGMENTS > MASTODON_MAX_SEGMENTS || IMAGE_SIZE > MASTODON_MAX_IMAGE_SIZE)) {
        					continue;
    					}
    					if ((platform == "ImgBB" || platform == "PostImage") && (IMAGE_SIZE > IMGBB_POSTIMAGE_MAX_IMAGE_SIZE)) {
        					continue;
    					}
    					if (platform == "ImgPile" && IMAGE_SIZE > IMGPILE_MAX_IMAGE_SIZE) {
        					continue;
    					}
    					if (platform == "Flickr" && IMAGE_SIZE > FLICKR_MAX_IMAGE_SIZE) {
        					continue;
    					}
    					
					filtered_platforms.push_back(platform);
				}
					if (filtered_platforms.empty()) {
    						filtered_platforms.push_back("\b\bUnknown!\n\n Due to the large file size of the output JPG image, I'm unaware of any\n compatible platforms that this image can be posted on. Local use only?");
					}
					platforms_vec = std::move(filtered_platforms);
			}
			
			std::cout << "\nPlatform compatibility for output image:-\n\n";
			
			for (const auto& s : platforms_vec) {
        			std::cout << " ✔ "<< s << '\n' ;
   		 	}	
			
			std::vector<uint8_t>().swap(image_vec);
	
			std::cout << "\nSaved \"file-embedded\" JPG image: " << OUTPUT_FILENAME << " (" << IMAGE_SIZE << " bytes).\n";
	
			std::cout << "\nRecovery PIN: [***" << pin << "***]\n\nImportant: Keep your PIN safe, so that you can extract the hidden file.\n\nComplete!\n\n";
			return 0;
        	} else {
        		// Recover data file section code.
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
				throw std::runtime_error("Image File Error: Signature check failure. This is not a valid jdvrif \"file-embedded\" image.");
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
		
		if (IMAGE_FILE_SIZE > LARGE_FILE_SIZE) {
			std::cout << LARGE_FILE_MSG;
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
				throw std::runtime_error("File Extraction Error: Missing segments detected. Embedded data file is corrupt!");
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
			std::fstream file(args.image_file, std::ios::in | std::ios::out | std::ios::binary);
		
			if (pin_attempts_val == 0x90) {
				pin_attempts_val = 0;
			} else {
    				pin_attempts_val++;
			}

			if (pin_attempts_val > 2) {
				file.close();
				std::ofstream file(args.image_file, std::ios::out | std::ios::trunc | std::ios::binary);
			} else {
				file.seekp(pin_attempts_index);
				file.write(reinterpret_cast<char*>(&pin_attempts_val), sizeof(pin_attempts_val));
			}

			file.close();

			throw std::runtime_error("File Recovery Error: Invalid PIN or file is corrupt.");
		}

		if (pin_attempts_val != 0x90) {
			std::fstream file(args.image_file, std::ios::in | std::ios::out | std::ios::binary);
		
			uint8_t reset_pin_attempts_val = 0x90;

			file.seekp(pin_attempts_index);
			file.write(reinterpret_cast<char*>(&reset_pin_attempts_val), sizeof(reset_pin_attempts_val));

			file.close();
		}

		std::ofstream file_ofs(decrypted_filename, std::ios::binary);

		if (!file_ofs) {
			throw std::runtime_error("Write Error: Unable to write to file.");
		}

		file_ofs.write(reinterpret_cast<const char*>(image_vec.data()), INFLATED_FILE_SIZE);

		std::vector<uint8_t>().swap(image_vec);

		std::cout << "\nExtracted hidden file: " << decrypted_filename << " (" << INFLATED_FILE_SIZE << " bytes).\n\nComplete! Please check your file.\n\n";
		return 0;		
        	}
   	}
	catch (const std::runtime_error& e) {
        	std::cerr << "\n" << e.what() << "\n\n";
        	return 1;
    	}
}
