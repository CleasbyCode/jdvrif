// Remove pre-existing EXIF and/or Color Profile segments from user's cover image.
void eraseSegments(std::vector<uint8_t>&vec) {
	constexpr std::array<uint8_t, 2>
		APP1_SIG { 0xFF, 0xE1 }, // EXIF SEGMENT MARKER.
		APP2_SIG { 0xFF, 0xE2 }; // ICC COLOR PROFILE SEGMENT MARKER.

	constexpr std::array<uint8_t, 4>
		DQT1_SIG { 0xFF, 0xDB, 0x00, 0x43 },
		DQT2_SIG { 0xFF, 0xDB, 0x00, 0x84 };

	const uint32_t APP1_POS = searchFunc(vec, 0, 0, APP1_SIG);

	if (vec.size() > APP1_POS) {
		const uint16_t APP1_BLOCK_SIZE = (static_cast<uint16_t>(vec[APP1_POS + 2]) << 8) | static_cast<uint16_t>(vec[APP1_POS + 3]);
		vec.erase(vec.begin() + APP1_POS, vec.begin() + APP1_POS + APP1_BLOCK_SIZE + 2);
	}

	const uint32_t APP2_POS = searchFunc(vec, 0, 0, APP2_SIG);
	if (vec.size() > APP2_POS) {
		const uint16_t APP2_BLOCK_SIZE = (static_cast<uint16_t>(vec[APP2_POS + 2]) << 8) | static_cast<uint16_t>(vec[APP2_POS + 3]);
		vec.erase(vec.begin() + APP2_POS, vec.begin() + APP2_POS + APP2_BLOCK_SIZE + 2);
	}

	const uint32_t
		DQT1_POS = searchFunc(vec, 0, 0, DQT1_SIG),
		DQT2_POS = searchFunc(vec, 0, 0, DQT2_SIG),
		DQT_POS  = std::min(DQT1_POS, DQT2_POS);

	vec.erase(vec.begin(), vec.begin() + DQT_POS);
}