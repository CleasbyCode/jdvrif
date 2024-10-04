uint8_t jdvIn(const std::string& IMAGE_FILENAME, std::string& data_filename, bool isRedditOption, bool isCompressedFile) {
	constexpr uint32_t
		COMBINED_MAX_FILE_SIZE 	= 2147483648, 	// 2GB. (image + data file)
		MAX_FILE_SIZE_REDDIT 	= 20971520;	// 20MB. 	

	constexpr uint8_t JPG_MIN_FILE_SIZE = 134;

	const size_t 
		IMAGE_FILE_SIZE 	= std::filesystem::file_size(IMAGE_FILENAME),
		DATA_FILE_SIZE 		= std::filesystem::file_size(data_filename),
		COMBINED_FILE_SIZE 	= DATA_FILE_SIZE + IMAGE_FILE_SIZE;

	if (COMBINED_FILE_SIZE > COMBINED_MAX_FILE_SIZE
     		|| (DATA_FILE_SIZE == 0)
     		|| (isRedditOption && COMBINED_FILE_SIZE > MAX_FILE_SIZE_REDDIT)
     		|| JPG_MIN_FILE_SIZE > IMAGE_FILE_SIZE) {     
    		std::cerr << "\nFile Size Error: "
        		<< (JPG_MIN_FILE_SIZE > IMAGE_FILE_SIZE
            		? "Image is too small to be a valid JPG image"
            		: (DATA_FILE_SIZE == 0
                		? "Data file is empty"
                		: "Combined size of image and data file exceeds program maximum limit of "
                    		+ std::string(isRedditOption ? "20MB" : "2GB")))
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

	std::vector<uint8_t> Image_Vec;
	Image_Vec.reserve(COMBINED_FILE_SIZE); 
	
	std::copy(std::istreambuf_iterator<char>(image_file_ifs), std::istreambuf_iterator<char>(), std::back_inserter(Image_Vec));

	constexpr uint8_t
		SOI_SIG[]	{ 0xFF, 0xD8 },
		EOI_SIG[] 	{ 0xFF, 0xD9 };

	if (!std::equal(std::begin(SOI_SIG), std::end(SOI_SIG), std::begin(Image_Vec)) || !std::equal(std::begin(EOI_SIG), std::end(EOI_SIG), std::end(Image_Vec) - 2)) {
        	std::cerr << "\nImage File Error: This is not a valid JPG image.\n\n";
		return 1;
	}
	
	bool isKdakProfile = false;

	eraseSegments(Image_Vec, isKdakProfile);
	
	if (isKdakProfile) {
		Profile_Vec.swap(Profile_Kdak_Vec);
	}

	const uint8_t LAST_SLASH_POS = static_cast<uint8_t>(data_filename.find_last_of("\\/"));

	if (LAST_SLASH_POS <= data_filename.length()) {
		const std::string_view NO_SLASH_NAME(data_filename.c_str() + (LAST_SLASH_POS + 1), data_filename.length() - (LAST_SLASH_POS + 1));
		data_filename = NO_SLASH_NAME;
	}

	constexpr uint8_t MAX_FILENAME_LENGTH = 20;

	const uint8_t DATA_FILENAME_LENGTH = static_cast<uint8_t>(data_filename.length());

	if (DATA_FILENAME_LENGTH > MAX_FILENAME_LENGTH) {
    		std::cerr << "\nData File Error: Length of data filename is too long.\n\nFor compatibility requirements, length of data filename must not exceed 20 characters.\n\n";
    	 	return 1;
	}

	constexpr uint8_t PROFILE_NAME_LENGTH_INDEX = 0x50;
	
	Profile_Vec[PROFILE_NAME_LENGTH_INDEX] = DATA_FILENAME_LENGTH;

	constexpr uint32_t LARGE_FILE_SIZE = 104857600;	// 100MB.

	if (DATA_FILE_SIZE > LARGE_FILE_SIZE) {
		std::cout << "\nPlease wait. Larger files will take longer to complete this process.\n";
	}

	std::vector<uint8_t> File_Vec;
	File_Vec.reserve(COMBINED_FILE_SIZE); 

	std::copy(std::istreambuf_iterator<char>(data_file_ifs), std::istreambuf_iterator<char>(), std::back_inserter(File_Vec));
	
	std::reverse(File_Vec.begin(), File_Vec.end());
	
	uint32_t file_vec_size = deflateFile(File_Vec, isCompressedFile);
	
	if (!file_vec_size) {
		std::cerr << "\nFile Size Error: File is zero bytes. Probable compression failure.\n\n";
		return 1;
	}
	
	encryptFile(Profile_Vec, File_Vec, file_vec_size, data_filename);
	
	insertProfileHeaders(Profile_Vec, File_Vec);

	constexpr uint8_t PROFILE_HEADER_LENGTH = 18;
	
	if (isRedditOption) {
		Image_Vec.insert(Image_Vec.begin(), std::begin(SOI_SIG), std::end(SOI_SIG));
		Image_Vec.insert(Image_Vec.end() - 2, File_Vec.begin() + PROFILE_HEADER_LENGTH, File_Vec.end());
	} else {
		Image_Vec.insert(Image_Vec.begin(), File_Vec.begin(), File_Vec.end());
	}

	std::vector<uint8_t>().swap(File_Vec);

	if (!writeFile(Image_Vec)) {
		return 1;
	}

	std::cout << ((isRedditOption) 
		?  "\n**Important**\n\nDue to your option selection, for compatibility reasons\nyou should only post this file-embedded JPG image on Reddit.\n\nComplete!\n\n"
		:  "\nComplete!\n\n");	

	return 0;
}
