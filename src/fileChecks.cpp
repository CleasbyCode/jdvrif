#include "fileChecks.h"
#include <stdexcept>
#include <cctype>
#include <algorithm>
#include <set>
#include <cstdint>
#include <filesystem>

bool isCompressedFile(const std::string& extension) {
	static const std::set<std::string> compressed_extensions = {
		".zip", ".jar", ".rar", ".7z", ".bz2", ".gz", ".xz", ".tar",
        	".lz", ".lz4", ".cab", ".rpm", ".deb", ".mp4", ".mp3", ".exe",
        	".jpg", ".jpeg", ".jfif", ".png", ".webp", ".bmp", ".gif", ".ogg", ".flac"
	};
	return compressed_extensions.count(extension) > 0;
}

bool hasValidFilename(const std::string& filename) {
	return std::all_of(filename.begin(), filename.end(), 
        	[](char c) { return std::isalnum(c) || c == '.' || c == '/' || c == '\\' || c == '-' || c == '_' || c == '@' || c == '%'; });
}

bool hasValidImageExtension(const std::string& ext) {
	static const std::set<std::string> valid_extensions = {".jpg", ".jpeg", ".jfif"};
    	return valid_extensions.count(ext) > 0;
}

void validateImageFile(const std::string& image_file, ArgMode mode, ArgOption platform, uintmax_t IMAGE_FILE_SIZE) {
	std::filesystem::path image_path(image_file);

    	std::string image_ext = image_path.extension().string();

	if (!hasValidFilename(image_file)) {
            	throw std::runtime_error("Invalid Input Error: Unsupported characters in filename arguments.");
        }

    	if (!hasValidImageExtension(image_ext)) {
        	throw std::runtime_error("File Type Error: Invalid image extension. Only expecting \".jpg\", \".jpeg\", or \".jfif\".");
    	}

    	constexpr uint8_t MINIMUM_IMAGE_SIZE = 134;

    	if (MINIMUM_IMAGE_SIZE > IMAGE_FILE_SIZE) {
        	throw std::runtime_error("Image File Error: Invalid file size.");
    	}

    	constexpr uintmax_t 
		MAX_IMAGE_SIZE_BLUESKY = 800ULL * 1024,
		MAX_SIZE_RECOVER = 3ULL * 1024 * 1024 * 1024;    

    	bool 
    		isModeRecover = (mode == ArgMode::recover),
		hasBlueskyOption = (platform == ArgOption::bluesky);

	if (hasBlueskyOption && IMAGE_FILE_SIZE > MAX_IMAGE_SIZE_BLUESKY) {
		throw std::runtime_error("File Size Error: Image file exceeds maximum size limit for the Bluesky platform.");
	}

	if (isModeRecover && IMAGE_FILE_SIZE > MAX_SIZE_RECOVER) {
		throw std::runtime_error("File Size Error: Image file exceeds maximum default size limit for jdvrif.");
	}
}
	
void validateDataFile(const std::string& data_file, ArgOption platform, uintmax_t IMAGE_FILE_SIZE, uintmax_t DATA_FILE_SIZE) {

    	constexpr uintmax_t 
		MAX_DATA_SIZE_BLUESKY = 5ULL * 1024 * 1024,
		MAX_SIZE_CONCEAL = 2ULL * 1024 * 1024 * 1024,  		
    		MAX_SIZE_REDDIT  = 20ULL * 1024 * 1024;         

    	const uintmax_t COMBINED_FILE_SIZE = DATA_FILE_SIZE + IMAGE_FILE_SIZE;
	
    	if (!DATA_FILE_SIZE) {
		throw std::runtime_error("Data File Error: File is empty.");
    	}

    	bool 
		hasNoneOption   = (platform == ArgOption::none),
		hasRedditOption  = (platform == ArgOption::reddit),
		hasBlueskyOption = (platform == ArgOption::bluesky);

	if (hasBlueskyOption && DATA_FILE_SIZE > MAX_DATA_SIZE_BLUESKY) {
		throw std::runtime_error("Data File Size Error: File exceeds maximum size limit for the Bluesky platform.");
	}

   	if (hasRedditOption && COMBINED_FILE_SIZE > MAX_SIZE_REDDIT) {
   		throw std::runtime_error("File Size Error: Combined size of image and data file exceeds maximum size limit for the Reddit platform.");
   	}

	if (hasNoneOption && COMBINED_FILE_SIZE > MAX_SIZE_CONCEAL) {
		throw std::runtime_error("File Size Error: Combined size of image and data file exceeds maximum default size limit for jdvrif.");
	}
}
