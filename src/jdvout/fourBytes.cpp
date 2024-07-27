uint32_t getFourByteValue(const std::vector<uint8_t>& VEC, const uint32_t INDEX) {
	return	(static_cast<uint32_t>(VEC[INDEX]) << 24) |
		(static_cast<uint32_t>(VEC[INDEX + 1]) << 16) |
		(static_cast<uint32_t>(VEC[INDEX + 2]) << 8) |
		 static_cast<uint32_t>(VEC[INDEX + 3]); 
}
