bool hasValidFilename(const std::string& filename) {
	return std::all_of(filename.begin(), filename.end(), 
        	[](char c) { return std::isalnum(c) || c == '.' || c == '/' || c == '\\' || c == '-' || c == '_' || c == '@' || c == '%'; });
}

bool isValidImageExtension(const std::string& ext) {
	static const std::set<std::string> validExtensions = {".jpg", ".jpeg", ".jfif"};
    	return validExtensions.count(ext) > 0;
}

void validateFiles(const std::string& image_file) {
	std::filesystem::path image_path(image_file);

    	std::string image_ext = image_path.extension().string();

    	if (!isValidImageExtension(image_ext)) {
        	throw std::runtime_error("File Type Error: Invalid image extension. Only expecting \".jpg\", \".jpeg\", or \".jfif\".");
    	}

    	if (!std::filesystem::exists(image_path)) {
        	throw std::runtime_error("Image File Error: File not found. Check the filename and try again.");
    	}

   	if (std::filesystem::file_size(image_path) == 0) {
		throw std::runtime_error("Image File Error: File is empty.");
    	}
    	
    	constexpr uintmax_t MAX_FILE_SIZE = 3ULL * 1024 * 1024 * 1024;   
	
   	if (std::filesystem::file_size(image_path) > MAX_FILE_SIZE) {
   		throw std::runtime_error("File Size Error: Image file exceeds maximum size limit.");
   	}
}
