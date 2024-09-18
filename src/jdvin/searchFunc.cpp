// Search and return a vector index position of given string signature.
template <uint_fast8_t N>
uint_fast32_t searchFunc(std::vector<uint_fast8_t>& Vec, uint_fast32_t start_index, const uint_fast8_t INCREMENT_SEARCH_INDEX, const uint_fast8_t (&SIG)[N]) {
	return static_cast<uint_fast32_t>(std::search(Vec.begin() + start_index + INCREMENT_SEARCH_INDEX, Vec.end(), std::begin(SIG), std::end(SIG)) - Vec.begin());
}
