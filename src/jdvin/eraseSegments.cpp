void eraseSegments(std::vector<uint8_t>&Vec) {
	constexpr uint8_t
		APP1_SIG[] 	{ 0xFF, 0xE1 },
		APP2_SIG[]	{ 0xFF, 0xE2 },
		DQT1_SIG[]  	{ 0xFF, 0xDB, 0x00, 0x43 },
		DQT2_SIG[]	{ 0xFF, 0xDB, 0x00, 0x84 };

	const uint32_t APP1_POS = searchFunc(Vec, 0, 0, APP1_SIG);

	if (Vec.size() > APP1_POS) {
		const uint16_t APP1_BLOCK_SIZE = (static_cast<uint16_t>(Vec[APP1_POS + 2]) << 8) | static_cast<uint16_t>(Vec[APP1_POS + 3]);
		Vec.erase(Vec.begin() + APP1_POS, Vec.begin() + APP1_POS + APP1_BLOCK_SIZE + 2);
	}

	const uint32_t APP2_POS = searchFunc(Vec, 0, 0, APP2_SIG);
	if (Vec.size() > APP2_POS) {
		const uint16_t APP2_BLOCK_SIZE = (static_cast<uint16_t>(Vec[APP2_POS + 2]) << 8) | static_cast<uint16_t>(Vec[APP2_POS + 3]);
		Vec.erase(Vec.begin() + APP2_POS, Vec.begin() + APP2_POS + APP2_BLOCK_SIZE + 2);
	}

	const uint32_t
		DQT1_POS = searchFunc(Vec, 0, 0, DQT1_SIG),
		DQT2_POS = searchFunc(Vec, 0, 0, DQT2_SIG),
		DQT_POS  = std::min(DQT1_POS, DQT2_POS);

	Vec.erase(Vec.begin(), Vec.begin() + DQT_POS);
}
