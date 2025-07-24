#include "jdvIn.h"
#include "segmentsVec.h"   

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

#include <array>
#include <algorithm>
#include <utility>         
#include <fstream>          
#include <iostream>         
#include <filesystem>       
#include <random>       
#include <iterator>         

int jdvIn(const std::string& IMAGE_FILENAME, std::string& data_filename, ArgOption platform, bool isCompressedFile) {
	// Lambda function writes new values, such as segments lengths, etc. into the relevant vector index locations.	
	auto updateValue = [](std::vector<uint8_t>& vec, uint32_t insert_index, const uint64_t NEW_VALUE, uint8_t bits) {
		while (bits) { vec[insert_index++] = (NEW_VALUE >> (bits -= 8)) & 0xff; }	// Big-endian.
	};
	
	std::ifstream
		image_file_ifs(IMAGE_FILENAME, std::ios::binary),
		data_file_ifs(data_filename, std::ios::binary);

	if (!image_file_ifs || !data_file_ifs) {
		std::cerr << "\nRead File Error: Unable to read " << (!image_file_ifs ? "image file" : "data file") << ".\n\n";
		return 1;
	}

	const uintmax_t 
		IMAGE_FILE_SIZE = std::filesystem::file_size(IMAGE_FILENAME),
		DATA_FILE_SIZE = std::filesystem::file_size(data_filename);

	std::vector<uint8_t> image_vec(IMAGE_FILE_SIZE); 
	
	image_file_ifs.read(reinterpret_cast<char*>(image_vec.data()), IMAGE_FILE_SIZE);
	image_file_ifs.close();

	constexpr uint8_t SIG_LENGTH = 2;

	constexpr std::array<uint8_t, SIG_LENGTH>
		IMAGE_START_SIG	{ 0xFF, 0xD8 },
		IMAGE_END_SIG   { 0xFF, 0xD9 };

	if (!std::equal(IMAGE_START_SIG.begin(), IMAGE_START_SIG.end(), image_vec.begin()) || !std::equal(IMAGE_END_SIG.begin(), IMAGE_END_SIG.end(), image_vec.end() - 2)) {
        	std::cerr << "\nImage File Error: This is not a valid JPG image.\n\n";
		return 1;
	}
	
	bool hasBlueskyOption = (platform == ArgOption::Bluesky);
	
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
	
	std::filesystem::path file_path(data_filename);
    	data_filename = file_path.filename().string();

	constexpr uint8_t DATA_FILENAME_MAX_LENGTH = 20;

	const uint8_t DATA_FILENAME_LENGTH = static_cast<uint8_t>(data_filename.size());

	if (DATA_FILENAME_LENGTH > DATA_FILENAME_MAX_LENGTH) {
    		std::cerr << "\nData File Error: For compatibility requirements, length of data filename must not exceed 20 characters.\n\n";
    	 	return 1;
	}

	if (hasBlueskyOption) {
		segment_vec.swap(bluesky_exif_vec);	// Use the EXIF segment instead of the default color profile segment to store user data.
	}						// The color profile segment (FFE2) is removed by Bluesky, so we use EXIF.

	const uint16_t DATA_FILENAME_LENGTH_INDEX = hasBlueskyOption ? 0x160 : 0x2E6;

	segment_vec[DATA_FILENAME_LENGTH_INDEX] = DATA_FILENAME_LENGTH;	

	constexpr uint32_t LARGE_FILE_SIZE = 300 * 1024 * 1024;  

	if (DATA_FILE_SIZE > LARGE_FILE_SIZE) {
		std::cout << "\nPlease wait. Larger files will take longer to complete this process.\n";
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

    	if (FIRST_SIZE_OPTION > DATA_FILE_SIZE && isCompressedFile) {
        	compression_level = Z_NO_COMPRESSION;
    	} else if (SECOND_SIZE_OPTION > DATA_FILE_SIZE && isCompressedFile) {
        	compression_level = Z_BEST_SPEED;
    	} else if (isCompressedFile || DATA_FILE_SIZE > FIFTH_SIZE_OPTION) {
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
	
	bool shouldDisplayMastodonWarning = false; 

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
				std::cerr << "\nFile Size Error: Data file exceeds segment size limit.\n\n";
				return 1;
			}

			constexpr uint8_t SEGMENT_SIG_LENGTH = 2; // FFE1

			segment_size_field_index = 0x02;

			value_bit_length = 16;
			updateValue(bluesky_xmp_vec, segment_size_field_index, BLUESKY_XMP_VEC_SIZE - SEGMENT_SIG_LENGTH, value_bit_length);
			
			std::copy_n(bluesky_xmp_vec.begin(), BLUESKY_XMP_VEC_SIZE, std::back_inserter(segment_vec));
		}
		image_vec.insert(image_vec.begin(), segment_vec.begin(), segment_vec.end());
		std::cout << "\nBluesky option selected: Only post this \"file-embedded\" JPG image on Bluesky.\n\n"
			  	<< "Make sure to use the Python script \"bsky_post.py\" (found in the repo src folder)\nto post the image to Bluesky.\n";
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
				icc_segments_required       = (icc_profile_with_data_file_vec_size / icc_segment_data_size) + 1, // There will almost always be a remainder segment, so plus 1 here.
				icc_segment_remainder_size  = (icc_profile_with_data_file_vec_size % icc_segment_data_size) - (IMAGE_START_SIG_LENGTH + ICC_SEGMENT_SIG_LENGTH),
				icc_segments_sequence_val   = 1;
			
			constexpr uint16_t ICC_SEGMENTS_TOTAL_VAL_INDEX = 0x2E0;  // The value stored here is used by jdvout when extracting the data file.
			updateValue(segment_vec, ICC_SEGMENTS_TOTAL_VAL_INDEX, !icc_segment_remainder_size ? --icc_segments_required : icc_segments_required, value_bit_length);

			uint8_t 
				icc_segments_sequence_val_index = 0x11,
				icc_segment_remainder_size_index = 0x04;

			std::vector<uint8_t> icc_segment_header_vec { segment_vec.begin(), segment_vec.begin() + IMAGE_START_SIG_LENGTH + ICC_SEGMENT_SIG_LENGTH + ICC_SEGMENT_HEADER_LENGTH };

			// Because of some duplicate data, erase the first 20 bytes of segment_vec because they will be replaced when splitting the data file.
    			segment_vec.erase(segment_vec.begin(), segment_vec.begin() + (IMAGE_START_SIG_LENGTH + ICC_SEGMENT_SIG_LENGTH + ICC_SEGMENT_HEADER_LENGTH));

			data_file_vec.reserve(icc_profile_with_data_file_vec_size + (icc_segments_required * (ICC_SEGMENT_SIG_LENGTH + ICC_SEGMENT_HEADER_LENGTH)));

			uint32_t byte_index = 0;

			while (icc_segments_required--) {	
				if (!icc_segments_required) {
					if (icc_segment_remainder_size) {
						icc_segment_data_size = icc_segment_remainder_size;	
			   			updateValue(icc_segment_header_vec, icc_segment_remainder_size_index, (icc_segment_remainder_size + ICC_SEGMENT_HEADER_LENGTH), value_bit_length);
					} else {
						break;
					} 	
				}
				std::copy_n(icc_segment_header_vec.begin() + IMAGE_START_SIG_LENGTH, ICC_SEGMENT_SIG_LENGTH + ICC_SEGMENT_HEADER_LENGTH, std::back_inserter(data_file_vec));
				std::copy_n(segment_vec.begin() + byte_index, icc_segment_data_size, std::back_inserter(data_file_vec));
				updateValue(icc_segment_header_vec, icc_segments_sequence_val_index, ++icc_segments_sequence_val, value_bit_length);
				byte_index += icc_segment_data_size;
			}

			std::vector<uint8_t>().swap(segment_vec);
		
			// Insert the start of image sig bytes that were removed.
			data_file_vec.insert(data_file_vec.begin(), icc_segment_header_vec.begin(), icc_segment_header_vec.begin() + IMAGE_START_SIG_LENGTH);

			constexpr uint8_t MASTODON_SEGMENTS_LIMIT = 100;		   
			constexpr uint32_t MASTODON_IMAGE_UPLOAD_LIMIT = 16 * 1024 * 1024; 
					   
			// The warning is important because Mastodon will allow you to post an image that is greater than its 100 segments limit, as long as you do not exceed
			// the image size limit, which is 16MB. This seems fine until someone downloads/saves the image. Data segments over the limit will be truncated, so parts 
			// of the data file will be missing when an attempt is made to extract the (now corrupted) file from the image.
			shouldDisplayMastodonWarning = icc_segments_sequence_val > MASTODON_SEGMENTS_LIMIT && MASTODON_IMAGE_UPLOAD_LIMIT > icc_profile_with_data_file_vec_size;
			
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
		if (platform == ArgOption::Reddit) {
			image_vec.insert(image_vec.begin(), IMAGE_START_SIG.begin(), IMAGE_START_SIG.end());
			image_vec.insert(image_vec.end() - 2, 8000, 0x23);
			image_vec.insert(image_vec.end() - 2, data_file_vec.begin() + 2, data_file_vec.end());
			std::cout << "\nReddit option selected: Only post this file-embedded JPG image on Reddit.\n";
		} else {
			image_vec.insert(image_vec.begin(), data_file_vec.begin(), data_file_vec.end());
			if (shouldDisplayMastodonWarning) {
				std::cout << "\n**Warning**\n\nEmbedded image is not compatible with Mastodon. Image file exceeds platform's segments limit.\n";
			}
		}
		std::vector<uint8_t>().swap(data_file_vec);
	}	
	
    	std::uniform_int_distribution<> dist(10000, 99999);  

	const std::string OUTPUT_FILENAME = "jrif_" + std::to_string(dist(gen)) + ".jpg";

	std::ofstream file_ofs(OUTPUT_FILENAME, std::ios::binary);

	if (!file_ofs) {
		std::cerr << "\nWrite File Error: Unable to write to file.\n\n";
		return 1;
	}
	
	const uint32_t IMAGE_SIZE = static_cast<uint32_t>(image_vec.size());

	file_ofs.write(reinterpret_cast<const char*>(image_vec.data()), IMAGE_SIZE);
	
	std::vector<uint8_t>().swap(image_vec);
	
	std::cout << "\nSaved \"file-embedded\" JPG image: " << OUTPUT_FILENAME << " (" << IMAGE_SIZE << " bytes).\n";
	
	std::cout << "\nRecovery PIN: [***" << pin << "***]\n\nImportant: Keep your PIN safe, so that you can extract the hidden file.\n\nComplete!\n\n";
	return 0;
}
