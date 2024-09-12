// Search the "file-embedded" image for ICC Profile headers. Store index location of each found header within a vector.
// We will use these index positions to skip over the headers when decrypting the data file, so that they are not included within the restored data file.
void findProfileHeaders(std::vector<uint_fast8_t>&Image_Vec, std::vector<uint_fast32_t>&Profile_Headers_Index_Vec, uint_fast16_t profile_count) {

	constexpr uint_fast8_t 
		ICC_PROFILE_SIG[] { 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45 },
		NEXT_SEARCH_POS_INC = 5,
		INDEX_DIFF = 4;

	uint_fast32_t profile_header_index{}; 

	while (profile_count--) {
		profile_header_index = searchFunc(Image_Vec, profile_header_index, NEXT_SEARCH_POS_INC, ICC_PROFILE_SIG) - INDEX_DIFF;
    		Profile_Headers_Index_Vec.emplace_back(profile_header_index);
	}
}
