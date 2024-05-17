void findProfileHeaders(std::vector<uint_fast8_t>&Image_Vec, std::vector<uint_fast32_t>&Profile_Headers_Offset_Vec, std::string& image_file_name) {

	constexpr uint_fast8_t
		NAME_LENGTH_INDEX = 38,		
		NAME_INDEX = 39,			
		PROFILE_COUNT_INDEX = 96,	
		FILE_SIZE_INDEX = 102;		

	constexpr uint_fast16_t	FILE_INDEX = 621; 		

	const uint_fast8_t NAME_LENGTH = static_cast<uint_fast8_t>(Image_Vec[NAME_LENGTH_INDEX]);	
	
	const uint_fast32_t EMBEDDED_FILE_SIZE = (static_cast<uint_fast32_t>(Image_Vec[FILE_SIZE_INDEX]) << 24)
				| (static_cast<uint_fast32_t>(Image_Vec[FILE_SIZE_INDEX + 1]) << 16)
				| (static_cast<uint_fast32_t>(Image_Vec[FILE_SIZE_INDEX + 2]) << 8)
				| static_cast<uint_fast32_t>(Image_Vec[FILE_SIZE_INDEX + 3]);

	const std::string ICC_PROFILE_SIG = "ICC_PROFILE";

	uint_fast16_t profile_count = (static_cast<uint_fast16_t>(Image_Vec[PROFILE_COUNT_INDEX]) << 8)
				| static_cast<uint_fast16_t>(Image_Vec[PROFILE_COUNT_INDEX + 1]);	

	image_file_name = { Image_Vec.begin() + NAME_INDEX, Image_Vec.begin() + NAME_INDEX + Image_Vec[NAME_LENGTH_INDEX] };

	Image_Vec.erase(Image_Vec.begin(), Image_Vec.begin() + FILE_INDEX);

	uint_fast32_t profile_header_index = 0; 

	if (profile_count) { 
		while (profile_count--) {
			Profile_Headers_Offset_Vec.emplace_back(profile_header_index = static_cast<uint_fast32_t>(std::search(Image_Vec.begin() + profile_header_index + 5, Image_Vec.end(), ICC_PROFILE_SIG.begin(), ICC_PROFILE_SIG.end()) - Image_Vec.begin() - 4));
		}
	}

	Image_Vec.erase(Image_Vec.begin() + EMBEDDED_FILE_SIZE, Image_Vec.end());
}
