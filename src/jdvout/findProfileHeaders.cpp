void findProfileHeaders(std::vector<uint_fast8_t>&Image_Vec, std::vector<uint_fast32_t>&Profile_Headers_Offset_Vec, uint_fast16_t profile_count) {

	constexpr uint_fast8_t 
		ICC_PROFILE_SIG[] { 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45},
		PROFILE_HEADER_LENGTH = 18;

	uint_fast32_t profile_header_index{}; 
	
	Profile_Headers_Offset_Vec.reserve(profile_count * PROFILE_HEADER_LENGTH);

	while (profile_count--) {
		profile_header_index = searchFunc(Image_Vec, profile_header_index, 5, ICC_PROFILE_SIG) - 4;
    		Profile_Headers_Offset_Vec.emplace_back(profile_header_index);
	}
}
