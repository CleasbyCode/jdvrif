uint_fast32_t getFourByteValue(const std::vector<uint_fast8_t>& VEC, const uint_fast32_t INDEX) {
	return	(static_cast<uint_fast32_t>(VEC[INDEX]) << 24) |
		(static_cast<uint_fast32_t>(VEC[INDEX + 1]) << 16) |
		(static_cast<uint_fast32_t>(VEC[INDEX + 2]) << 8) |
		 static_cast<uint_fast32_t>(VEC[INDEX + 3]); 
}