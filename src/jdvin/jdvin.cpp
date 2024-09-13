uint_fast8_t jdvIn(const std::string& IMAGE_FILENAME, std::string& data_filename, bool isRedditOption) {

	constexpr uint_fast32_t
		MAX_FILE_SIZE = 1078800000, // Slightly over 1GB.
		MAX_FILE_SIZE_REDDIT = 20971520, 
		LARGE_FILE_SIZE = 104857600;

	constexpr uint_fast8_t JPG_MIN_FILE_SIZE = 134;

	const size_t 
		IMAGE_FILE_SIZE = std::filesystem::file_size(IMAGE_FILENAME),
		DATA_FILE_SIZE = std::filesystem::file_size(data_filename),
		COMBINED_FILE_SIZE = DATA_FILE_SIZE + IMAGE_FILE_SIZE;

	if (COMBINED_FILE_SIZE > MAX_FILE_SIZE
		 || (isRedditOption && COMBINED_FILE_SIZE > MAX_FILE_SIZE_REDDIT)
		 || JPG_MIN_FILE_SIZE > IMAGE_FILE_SIZE) {		
		std::cerr << "\nFile Size Error: " 
			<< (JPG_MIN_FILE_SIZE > IMAGE_FILE_SIZE
        			? "Image is too small to be a valid JPG image"
	        		: "Combined size of image and data file exceeds the maximum limit of "
        	    		+ std::string(isRedditOption 
                			? "20MB"
	                		: "1GB"))
			<< ".\n\n";
    		return 1;
	}
	
	std::ifstream
		image_file_ifs(IMAGE_FILENAME, std::ios::binary),
		data_file_ifs(data_filename, std::ios::binary);

	if (!image_file_ifs || !data_file_ifs) {
		std::cerr << "\nRead File Error: Unable to read " << (!image_file_ifs 
				? "image file" 
				: "data file") 
			<< ".\n\n";
		return 1;
	}

	std::vector<uint_fast8_t>Image_Vec((std::istreambuf_iterator<char>(image_file_ifs)), std::istreambuf_iterator<char>());

	constexpr uint_fast8_t
		SOI_SIG[]	{ 0xFF, 0xD8 },
		EOI_SIG[] 	{ 0xFF, 0xD9 };

	if (!std::equal(std::begin(SOI_SIG), std::end(SOI_SIG), std::begin(Image_Vec)) 
		|| !std::equal(std::begin(EOI_SIG), std::end(EOI_SIG), std::end(Image_Vec) - 2)) {
        	std::cerr << "\nImage File Error: This is not a valid JPG image.\n\n";
		return 1;
	}
	
	bool isKdak_Profile = false;

	eraseSegments(Image_Vec, isKdak_Profile);
	
	if (isKdak_Profile) {
		Profile_Vec.swap(Profile_Kdak_Vec);
	}

	const uint_fast8_t LAST_SLASH_POS = static_cast<uint_fast8_t>(data_filename.find_last_of("\\/"));

	if (LAST_SLASH_POS <= data_filename.length()) {
		const std::string_view NO_SLASH_NAME(data_filename.c_str() + (LAST_SLASH_POS + 1), data_filename.length() - (LAST_SLASH_POS + 1));
		data_filename = NO_SLASH_NAME;
	}

	constexpr uint_fast8_t MAX_FILENAME_LENGTH = 23;

	const uint_fast8_t DATA_FILENAME_LENGTH = static_cast<uint_fast8_t>(data_filename.length());

	if (DATA_FILENAME_LENGTH > DATA_FILE_SIZE || DATA_FILENAME_LENGTH > MAX_FILENAME_LENGTH) {
    		std::cerr << "\nData File Error: " 
              		<< (DATA_FILENAME_LENGTH > MAX_FILENAME_LENGTH 
                	  ? "Length of data filename is too long.\n\nFor compatibility requirements, length of data filename must be under 24 characters"
                  	  : "Size of data file is too small.\n\nFor compatibility requirements, data file size must be greater than the length of the filename")
              		<< ".\n\n";
    	 	return 1;
	}

	if (DATA_FILE_SIZE > LARGE_FILE_SIZE) {
		std::cout << "\nPlease wait. Larger files will take longer to process.\n";
	}

	std::vector<uint_fast8_t>File_Vec((std::istreambuf_iterator<char>(data_file_ifs)), std::istreambuf_iterator<char>());

	std::reverse(File_Vec.begin(), File_Vec.end());
	
	deflateFile(File_Vec);
	
	if (File_Vec.empty()) {
		std::cerr << "\nFile Size Error: File is zero bytes. Compression failure.\n\n";
		return 1;
	}
	
	encryptFile(Profile_Vec, File_Vec, data_filename);

	File_Vec.clear();
	File_Vec.shrink_to_fit();
	
	insertProfileHeaders(Profile_Vec, File_Vec);

	constexpr uint_fast8_t PROFILE_HEADER_LENGTH = 18;

	if (isRedditOption) {
		Image_Vec.insert(Image_Vec.begin(), std::begin(SOI_SIG), std::end(SOI_SIG));
		Image_Vec.insert(Image_Vec.end() - 2, File_Vec.begin() + PROFILE_HEADER_LENGTH, File_Vec.end());
	} else {
		Image_Vec.insert(Image_Vec.begin(), File_Vec.begin(), File_Vec.end());
	}

	if (!writeFile(Image_Vec)) {
		return 1;
	}

	std::cout << ((isRedditOption) 
		?  "\n**Important**\n\nDue to your option selection, for compatibility reasons\nyou should only post this file-embedded JPG image on Reddit.\n\nComplete!\n\n"
		:  "\nComplete!\n\n");	

	return 0;
}
