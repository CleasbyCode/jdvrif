// Writes updated values, such as segment size lengths and other values, into the relevant vector index locations.
void valueUpdater(std::vector<uint8_t>& vec, uint32_t value_insert_index, const uint32_t VALUE, uint8_t bits) {
	while (bits) {
		vec[value_insert_index++] = (VALUE >> (bits -= 8)) & 0xff;
	}
}
