#include "jdvOut.h"
#include "searchFunc.h"
#include "fromBase64.h"
#include "inflateFile.h"
#include "decryptFile.h"

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
				
	const uint32_t 
		JDVRIF_SIG_INDEX	= searchFunc(image_vec, 0, 0, JDVRIF_SIG),
		ICC_PROFILE_SIG_INDEX 	= searchFunc(image_vec, 0, 0, ICC_PROFILE_SIG);

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

		const uint32_t XMP_SIG_INDEX = searchFunc(image_vec, 0, 0, XMP_SIG);

		if (XMP_SIG_INDEX != image_vec.size()) { // Found XMP segment.
			const uint32_t 
				XMP_CREATOR_SIG_INDEX = searchFunc(image_vec, XMP_SIG_INDEX, 0, XMP_CREATOR_SIG),
				BEGIN_BASE64_DATA_INDEX = XMP_CREATOR_SIG_INDEX + SIG_LENGTH + 1;
			
			constexpr uint8_t END_BASE64_DATA_SIG = 0x3C;
			const uint32_t 
				END_BASE64_DATA_SIG_INDEX = static_cast<uint32_t>(std::find(image_vec.begin() + BEGIN_BASE64_DATA_INDEX,
											image_vec.end(), END_BASE64_DATA_SIG) - image_vec.begin()),
				BASE64_DATA_SIZE = END_BASE64_DATA_SIG_INDEX - BEGIN_BASE64_DATA_INDEX;
	
			std::vector<uint8_t> base64_data_vec(BASE64_DATA_SIZE);
			std::copy_n(image_vec.begin() + BEGIN_BASE64_DATA_INDEX, BASE64_DATA_SIZE, base64_data_vec.begin());

			// Convert back to binary.
			convertFromBase64(base64_data_vec);

			const uint32_t END_OF_EXIF_DATA_INDEX = XMP_SIG_INDEX - 0x32;

			// Now append the XMP binary data to the EXIF binary segment data, so that we have the complete, single data file.
			std::copy_n(base64_data_vec.begin(), base64_data_vec.size(), image_vec.begin() + END_OF_EXIF_DATA_INDEX);
		}
	}
	constexpr uint32_t LARGE_FILE_SIZE = 300 * 1024 * 1024;

	if (IMAGE_FILE_SIZE > LARGE_FILE_SIZE) {
		std::cout << "\nPlease wait. Larger files will take longer to complete this process.\n";
	}
	
	const std::string DECRYPTED_FILENAME = decryptFile(image_vec, hasBlueskyOption);	
	
	const uint32_t INFLATED_FILE_SIZE = inflateFile(image_vec);

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

	std::ofstream file_ofs(DECRYPTED_FILENAME, std::ios::binary);

	if (!file_ofs) {
		std::cerr << "\nWrite Error: Unable to write to file.\n\n";
		return 1;
	}

	file_ofs.write(reinterpret_cast<const char*>(image_vec.data()), INFLATED_FILE_SIZE);

	std::vector<uint8_t>().swap(image_vec);

	std::cout << "\nExtracted hidden file: " << DECRYPTED_FILENAME << " (" << INFLATED_FILE_SIZE << " bytes).\n\nComplete! Please check your file.\n\n";
	return 0;
}
