#include "fileChecks.h"
#include <stdexcept>
#include <cctype>
#include <algorithm>
#include <set>
#include <filesystem>
#include <fstream> 
#include <array>
#include <iostream>

bool isFileCompressed(const std::string& extension) {
	static const std::set<std::string> compressed_extensions = {
		".zip", ".jar", ".rar", ".7z", ".bz2", ".gz", ".xz", ".tar",
        	".lz", ".lz4", ".cab", ".rpm", ".deb", ".mp4", ".mp3", ".exe",
        	".jpg", ".jpeg", ".jfif", ".png", ".webp", ".bmp", ".gif", ".ogg", ".flac"
	};
	return compressed_extensions.count(extension) > 0;
}

bool hasValidFilename(const std::string& filename) {
	return std::all_of(filename.begin(), filename.end(), [](char c) { return std::isalnum(c) || c == '.' || c == '/' || c == '\\' || c == '-' || c == '_' || c == '@' || c == '%'; });
}

bool hasValidImageExtension(const std::string& ext) {
	static const std::set<std::string> valid_extensions = {".jpg", ".jpeg", ".jfif"};
    return valid_extensions.count(ext) > 0;
}

void validateImageFile(std::string& cover_image, ArgMode mode, ArgOption platform, uintmax_t& cover_image_size, std::vector<uint8_t>& cover_image_vec) {
	std::filesystem::path image_path(cover_image);

    std::string image_ext = image_path.extension().string();

	if (!std::filesystem::exists(image_path)) {
        throw std::runtime_error("Image File Error: File not found.");
    }

	if (!hasValidFilename(image_path.string())) {
    	throw std::runtime_error("Invalid Input Error: Unsupported characters in filename arguments.");
    }

    if (!hasValidImageExtension(image_ext)) {
        throw std::runtime_error("File Type Error: Invalid image extension. Only expecting \".jpg\", \".jpeg\", or \".jfif\".");
    }

	std::ifstream cover_image_ifs(image_path, std::ios::binary);
        	
    if (!cover_image_ifs) {
    	throw std::runtime_error("Read File Error: Unable to read image file. Check the filename and try again.");
	}

	cover_image_size = std::filesystem::file_size(image_path);

    constexpr uint8_t MINIMUM_IMAGE_SIZE = 134;

    if (MINIMUM_IMAGE_SIZE > cover_image_size) {
        throw std::runtime_error("Image File Error: Invalid file size.");
    }

    constexpr uintmax_t 
		MAX_IMAGE_SIZE_BLUESKY = 800ULL * 1024,
		MAX_SIZE_RECOVER = 3ULL * 1024 * 1024 * 1024;    
	
	if (platform == ArgOption::bluesky && cover_image_size > MAX_IMAGE_SIZE_BLUESKY) {
		throw std::runtime_error("File Size Error: Image file exceeds maximum size limit for the Bluesky platform.");
	}

	if (mode == ArgMode::recover && cover_image_size > MAX_SIZE_RECOVER) {
		throw std::runtime_error("File Size Error: Image file exceeds maximum default size limit for jdvrif.");
	}
	
	std::vector<uint8_t> tmp_vec(cover_image_size);
	
	cover_image_ifs.read(reinterpret_cast<char*>(tmp_vec.data()), cover_image_size);
	cover_image_ifs.close();
	
	cover_image_vec.swap(tmp_vec);
	std::vector<uint8_t>().swap(tmp_vec);
	
	constexpr std::array<uint8_t, 2>
		IMAGE_START_SIG	{ 0xFF, 0xD8 },
		IMAGE_END_SIG   { 0xFF, 0xD9 };

	if (!std::equal(IMAGE_START_SIG.begin(), IMAGE_START_SIG.end(), cover_image_vec.begin()) || !std::equal(IMAGE_END_SIG.begin(), IMAGE_END_SIG.end(), cover_image_vec.end() - 2)) {
    	throw std::runtime_error("Image File Error: This is not a valid JPG image.");
	}
}
	
void validateDataFile(std::string& data_filename, ArgOption platform, uintmax_t& cover_image_size, uintmax_t& data_file_size, std::vector<uint8_t>& data_file_vec, bool& isCompressedFile) {
	std::filesystem::path data_path(data_filename);
	
	if (!hasValidFilename(data_path.string())) {
        throw std::runtime_error("Invalid Input Error: Unsupported characters in filename arguments.");
    }
	
	if (!std::filesystem::exists(data_path) || !std::filesystem::is_regular_file(data_path)) {
        throw std::runtime_error("Data File Error: File not found or not a regular file.");
    }
	
	std::ifstream data_file_ifs(data_path, std::ios::binary);

	if (!data_file_ifs) {
		throw std::runtime_error("Read File Error: Unable to read data file. Check the filename and try again.");
	}
	
	constexpr uint8_t DATA_FILENAME_MAX_LENGTH = 20;
	
	data_filename = data_path.filename().string();

	if (data_filename.size() > DATA_FILENAME_MAX_LENGTH) {
    	throw std::runtime_error("Data File Error: For compatibility requirements, length of data filename must not exceed 20 characters.");
	}
		
	data_file_size = std::filesystem::file_size(data_path);	
			
    if (!data_file_size) {
		throw std::runtime_error("Data File Error: File is empty.");
    }

	isCompressedFile = isFileCompressed(data_path.extension().string());
	
	constexpr uintmax_t 
		MAX_DATA_SIZE_BLUESKY = 5ULL * 1024 * 1024,
		MAX_SIZE_CONCEAL = 2ULL * 1024 * 1024 * 1024,  		
    	MAX_SIZE_REDDIT  = 20ULL * 1024 * 1024;         

	const uintmax_t COMBINED_FILE_SIZE = data_file_size + cover_image_size;

	if (platform == ArgOption::bluesky && data_file_size > MAX_DATA_SIZE_BLUESKY) {
		throw std::runtime_error("Data File Size Error: File exceeds maximum size limit for the Bluesky platform.");
	}

   	if (platform == ArgOption::reddit && COMBINED_FILE_SIZE > MAX_SIZE_REDDIT) {
   		throw std::runtime_error("File Size Error: Combined size of image and data file exceeds maximum size limit for the Reddit platform.");
   	}

	if (platform == ArgOption::none && COMBINED_FILE_SIZE > MAX_SIZE_CONCEAL) {
		throw std::runtime_error("File Size Error: Combined size of image and data file exceeds maximum default size limit for jdvrif.");
	}
	
	std::vector<uint8_t> tmp_vec(data_file_size);

	data_file_ifs.read(reinterpret_cast<char*>(tmp_vec.data()), data_file_size);
	data_file_ifs.close();
	
	data_file_vec.swap(tmp_vec);
	std::vector<uint8_t>().swap(tmp_vec);
}
