// Search and return a vector index position of given string signature.
template <uint8_t N>
uint32_t searchFunc(std::vector<uint8_t>& Vec, uint32_t start_index, const uint8_t INCREMENT_SEARCH_INDEX, const uint8_t (&SIG)[N]) {
	return static_cast<uint32_t>(std::search(Vec.begin() + start_index + INCREMENT_SEARCH_INDEX, Vec.end(), std::begin(SIG), std::end(SIG)) - Vec.begin());
}
