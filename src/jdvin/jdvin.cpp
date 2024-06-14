void startJdv(const std::string& IMAGE_FILENAME, std::string& data_filename, bool isRedditOption) {

	const size_t 
		TMP_IMAGE_FILE_SIZE = std::filesystem::file_size(IMAGE_FILENAME),
		TMP_DATA_FILE_SIZE = std::filesystem::file_size(data_filename),
		COMBINED_FILE_SIZE = TMP_DATA_FILE_SIZE + TMP_IMAGE_FILE_SIZE;

	constexpr uint_fast32_t
		MAX_FILE_SIZE = 209715200, 
		MAX_FILE_SIZE_REDDIT = 20971520, 
		LARGE_FILE_SIZE = 52428800;

	constexpr uint_fast8_t JPG_MIN_FILE_SIZE = 134;
	
	if (COMBINED_FILE_SIZE > MAX_FILE_SIZE
		 || (isRedditOption && COMBINED_FILE_SIZE > MAX_FILE_SIZE_REDDIT)
		 || JPG_MIN_FILE_SIZE > TMP_IMAGE_FILE_SIZE) {		
		std::cerr << "\nFile Size Error: " 
			<< (JPG_MIN_FILE_SIZE > TMP_IMAGE_FILE_SIZE
        			? "Image is too small to be a valid JPG image"
	        		: "Combined size of image and data file exceeds the maximum limit of "
        	    		+ std::string(isRedditOption 
                			? "20MB"
	                		: "200MB"))
			<< ".\n\n";

    		std::exit(EXIT_FAILURE);
	}
	
	std::ifstream
		image_file_ifs(IMAGE_FILENAME, std::ios::binary),
		data_file_ifs(data_filename, std::ios::binary);

	if (!image_file_ifs || !data_file_ifs) {
		std::cerr << "\nRead File Error: Unable to read " << (!image_file_ifs 
			? "image file" 
			: "data file") 
		<< ".\n\n";

		std::exit(EXIT_FAILURE);
	}

	std::vector<uint_fast8_t>Image_Vec((std::istreambuf_iterator<char>(image_file_ifs)), std::istreambuf_iterator<char>());

	bool isKdak_Profile = false;

	constexpr uint_fast8_t
		SOI_SIG[]	{ 0xFF, 0xD8 },
		EOI_SIG[] 	{ 0xFF, 0xD9 },
		APP1_SIG[] 	{ 0xFF, 0xE1 },
		APP2_SIG[]	{ 0XFF, 0xE2 },
		DQT1_SIG[]  	{ 0xFF, 0xDB, 0x00, 0x43 },
		DQT2_SIG[]	{ 0xFF, 0xDB, 0x00, 0x84 },
		KDAK_SIG[]	{ 0x4B, 0x4F, 0x44, 0x41, 0x52, 0x4F, 0x4D };
		
	if (!std::equal(std::begin(SOI_SIG), std::end(SOI_SIG), std::begin(Image_Vec)) 
				|| !std::equal(std::begin(EOI_SIG), std::end(EOI_SIG), std::end(Image_Vec) - 2)) {
        		std::cerr << "\nImage File Error: This is not a valid JPG image.\n\n";
			std::exit(EXIT_FAILURE);
	}

	const uint_fast32_t APP1_POS = searchFunc(Image_Vec, 0, 0, APP1_SIG);
	if (Image_Vec.size() > APP1_POS) {
		const uint_fast16_t APP1_BLOCK_SIZE = (static_cast<uint_fast16_t>(Image_Vec[APP1_POS + 2]) << 8) | static_cast<uint_fast16_t>(Image_Vec[APP1_POS + 3]);
		Image_Vec.erase(Image_Vec.begin() + APP1_POS, Image_Vec.begin() + APP1_POS + APP1_BLOCK_SIZE + 2);
	}

	const uint_fast32_t APP2_POS = searchFunc(Image_Vec, 0, 0, APP2_SIG);
	if (Image_Vec.size() > APP2_POS) {
		const uint_fast32_t KDAK_POS = searchFunc(Image_Vec, APP2_POS, 0, KDAK_SIG);
		if (Image_Vec.size() > KDAK_POS) {
			isKdak_Profile = true;	
		}
		const uint_fast16_t APP2_BLOCK_SIZE = (static_cast<uint_fast16_t>(Image_Vec[APP2_POS + 2]) << 8) | static_cast<uint_fast16_t>(Image_Vec[APP2_POS + 3]);
		Image_Vec.erase(Image_Vec.begin() + APP2_POS, Image_Vec.begin() + APP2_POS + APP2_BLOCK_SIZE + 2);
	}

	const uint_fast32_t
		DQT1_POS = searchFunc(Image_Vec, 0, 0, DQT1_SIG),
		DQT2_POS = searchFunc(Image_Vec, 0, 0, DQT2_SIG),
		DQT_POS = DQT1_POS > DQT2_POS ? DQT2_POS : DQT1_POS;
	
	Image_Vec.erase(Image_Vec.begin(), Image_Vec.begin() + DQT_POS);

	const uint_fast8_t LAST_SLASH_POS = static_cast<uint_fast8_t>(data_filename.find_last_of("\\/"));

	if (LAST_SLASH_POS <= data_filename.length()) {
		const std::string_view NO_SLASH_NAME(data_filename.c_str() + (LAST_SLASH_POS + 1), data_filename.length() - (LAST_SLASH_POS + 1));
		data_filename = NO_SLASH_NAME;
	}

	constexpr uint_fast8_t MAX_FILENAME_LENGTH = 23;

	const uint_fast8_t DATA_FILENAME_LENGTH = static_cast<uint_fast8_t>(data_filename.length());

	if (DATA_FILENAME_LENGTH > TMP_DATA_FILE_SIZE || DATA_FILENAME_LENGTH > MAX_FILENAME_LENGTH) {
    		std::cerr << "\nData File Error: " 
              		<< (DATA_FILENAME_LENGTH > MAX_FILENAME_LENGTH 
                	  ? "Length of data filename is too long.\n\nFor compatibility requirements, length of data filename must be under 24 characters"
                  	  : "Size of data file is too small.\n\nFor compatibility requirements, data file size must be greater than the length of the filename")
              		<< ".\n\n";

    	 	std::exit(EXIT_FAILURE);
	}

	std::cout << (TMP_DATA_FILE_SIZE > LARGE_FILE_SIZE ? "\nPlease wait. Larger files will take longer to process.\n" : "");

	std::vector<uint_fast8_t>File_Vec((std::istreambuf_iterator<char>(data_file_ifs)), std::istreambuf_iterator<char>());

	std::reverse(File_Vec.begin(), File_Vec.end());

	uint_fast32_t deflated_file_size = deflateFile(File_Vec);
	
	if (isKdak_Profile) {
		Profile_Vec.swap(Profile_Kdak_Vec);
	}

	Profile_Vec.reserve(deflated_file_size + Profile_Vec.size());

	encryptFile(Profile_Vec, File_Vec, data_filename);

	File_Vec.clear();
	File_Vec.shrink_to_fit();

	File_Vec.reserve(Profile_Vec.size());

	insertProfileHeaders(Profile_Vec, File_Vec, deflated_file_size);

	constexpr uint_fast8_t PROFILE_HEADER_LENGTH = 18;

	if (isRedditOption) {
		Image_Vec.insert(Image_Vec.begin(), std::begin(SOI_SIG), std::end(SOI_SIG));
		Image_Vec.insert(Image_Vec.end() - 2, File_Vec.begin() + PROFILE_HEADER_LENGTH, File_Vec.end());
	} else {
		Image_Vec.insert(Image_Vec.begin(), File_Vec.begin(), File_Vec.end());
	}

	srand((unsigned)time(NULL));  

	const std::string 
		TIME_VALUE = std::to_string(rand()),
		EMBEDDED_IMAGE_FILENAME = "jrif_" + TIME_VALUE.substr(0, 5) + ".jpg";

	std::ofstream file_ofs(EMBEDDED_IMAGE_FILENAME, std::ios::binary);

	if (!file_ofs) {
		std::cerr << "\nWrite Error: Unable to write to file.\n\n";
		std::exit(EXIT_FAILURE);
	}
	
	uint_fast32_t 
		EMBEDDED_IMAGE_SIZE = static_cast<uint_fast32_t>(Image_Vec.size()),
		REDDIT_SIZE = 20971520;

	file_ofs.write((char*)&Image_Vec[0], EMBEDDED_IMAGE_SIZE);

	std::cout << "\nSaved file-embedded JPG image: " + EMBEDDED_IMAGE_FILENAME + '\x20' + std::to_string(EMBEDDED_IMAGE_SIZE) + " Bytes.\n";

	if (isRedditOption && REDDIT_SIZE >= EMBEDDED_IMAGE_SIZE) {
		std::cout << "\n**Important**\n\nDue to your option selection, for compatibility reasons\nyou should only post this file-embedded JPG image on Reddit.\n\n";
	} else {
		std::cout << "\nComplete!\n\n";	
	}	
}
