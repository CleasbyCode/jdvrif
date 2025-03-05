bool isCompressedFile(const std::string& extension) {
	static const std::set<std::string> compressedExtensions = {
		".zip", ".jar", ".rar", ".7z", ".bz2", ".gz", ".xz", ".tar",
        	".lz", ".lz4", ".cab", ".rpm", ".deb", ".mp4", ".mp3",
        	".jpg", ".png", ".ogg", ".flac"
	};
	return compressedExtensions.count(extension) > 0;
}

bool hasValidFilename(const std::string& filename) {
	return std::all_of(filename.begin(), filename.end(), 
        	[](char c) { return std::isalnum(c) || c == '.' || c == '/' || c == '\\' || c == '-' || c == '_' || c == '@' || c == '%'; });
}

bool isValidImageExtension(const std::string& ext) {
	static const std::set<std::string> validExtensions = {".jpg", ".jpeg", ".jfif"};
    	return validExtensions.count(ext) > 0;
}

void validateFiles(const std::string& image_file, const std::string& data_file, ArgOption platformOption) {
	std::filesystem::path image_path(image_file), data_path(data_file);

    	std::string image_ext = image_path.extension().string();

    	if (!isValidImageExtension(image_ext)) {
        	throw std::runtime_error("File Type Error: Invalid image extension. Only expecting \".jpg\", \".jpeg\", or \".jfif\".");
    	}

    	if (!std::filesystem::exists(image_path)) {
        	throw std::runtime_error("Image File Error: File not found. Check the filename and try again.");
    	}

    	if (!std::filesystem::exists(data_path) || !std::filesystem::is_regular_file(data_path)) {
        	throw std::runtime_error("Data File Error: File not found or not a regular file. Check the filename and try again.");
    	}

    	constexpr uint8_t MINIMUM_IMAGE_SIZE = 134;

    	if (MINIMUM_IMAGE_SIZE > std::filesystem::file_size(image_path)) {
        	throw std::runtime_error("Image File Error: Invalid file size.");
    	}

    	constexpr uintmax_t 
		MAX_SIZE_DEFAULT = 2ULL * 1024 * 1024 * 1024,   // 2GB (cover image + data file)
    		MAX_SIZE_REDDIT  = 20ULL * 1024 * 1024;         // 20MB ""

    	const uintmax_t COMBINED_FILE_SIZE = std::filesystem::file_size(data_path) + std::filesystem::file_size(image_path);
	
    	if (std::filesystem::file_size(data_path) == 0) {
		throw std::runtime_error("Data File Error: File is empty.");
    	}

    	bool hasRedditOption = (platformOption == ArgOption::Reddit);

   	if ((hasRedditOption && COMBINED_FILE_SIZE > MAX_SIZE_REDDIT) || (!hasRedditOption && COMBINED_FILE_SIZE > MAX_SIZE_DEFAULT)) {
   		throw std::runtime_error("File Size Error: Combined size of image and data file exceeds maximum size limit.");
   	}
}
