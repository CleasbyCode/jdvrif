// Writes updated values, such as segment lengths, into the relevant vector index locations.

#include "valueUpdater.h"

void valueUpdater(std::vector<uint8_t>& vec, uint32_t value_index, const uint64_t VALUE, uint8_t bits) {
	while (bits) {
		vec[value_index++] = (VALUE >> (bits -= 8)) & 0xff;
	}
}
