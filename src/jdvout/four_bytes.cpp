uint_fast32_t getFourByteValue(const std::vector<uint_fast8_t>& vec, uint_fast32_t index) {
	return	(static_cast<uint_fast32_t>(vec[index]) << 24) |
		(static_cast<uint_fast32_t>(vec[index + 1]) << 16) |
		(static_cast<uint_fast32_t>(vec[index + 2]) << 8) |
		static_cast<uint_fast32_t>(vec[index + 3]); 
}