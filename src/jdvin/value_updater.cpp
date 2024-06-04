void valueUpdater(std::vector<uint_fast8_t>& vec, uint_fast32_t value_insert_index, const uint_fast32_t VALUE, uint_fast8_t bits) {
	while (bits) {
		static_cast<uint_fast32_t>(vec[value_insert_index++] = (VALUE >> (bits -= 8)) & 0xff);
	}
}
