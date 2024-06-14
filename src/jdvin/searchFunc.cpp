template <uint_fast8_t N>
uint_fast32_t searchFunc(std::vector<uint_fast8_t>& Vec, uint_fast32_t val_a, uint_fast8_t val_b, const uint_fast8_t (&SIG)[N]) {
	uint_fast32_t SIG_POS = static_cast<uint_fast32_t>(std::search(Vec.begin() + val_a + val_b, Vec.end(), std::begin(SIG), std::end(SIG)) - Vec.begin());
	return SIG_POS;
}
