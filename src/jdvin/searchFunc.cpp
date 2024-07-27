template <uint8_t N>
uint32_t searchFunc(std::vector<uint8_t>& Vec, uint32_t val_a, uint8_t val_b, const uint8_t (&SIG)[N]) {
	uint32_t SIG_POS = static_cast<uint32_t>(std::search(Vec.begin() + val_a + val_b, Vec.end(), std::begin(SIG), std::end(SIG)) - Vec.begin());
	return SIG_POS;
}
