// Search the "file-embedded" image for ICC Profile headers. Store index location of each found header within a vector.
// We will use these index positions to skip over the headers when decrypting the data file, so that they are not included within the restored data file.
void findProfileHeaders(std::vector<uint8_t>&Image_Vec, uint32_t* Headers_Index_Arr, const uint16_t PROFILE_COUNT) {

	constexpr uint8_t	
		ICC_PROFILE_SIG[] { 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45 },
		NEXT_SEARCH_POS_INC 	= 5,
		INDEX_DIFF 		= 4;

	uint32_t header_index = 0;
		
	for (uint16_t i = 0; PROFILE_COUNT > i; ++i) {
		Headers_Index_Arr[i] = header_index = searchFunc(Image_Vec, header_index, NEXT_SEARCH_POS_INC, ICC_PROFILE_SIG) - INDEX_DIFF;
	}
}
