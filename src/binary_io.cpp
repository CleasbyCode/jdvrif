#include "binary_io.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

std::optional<std::size_t> searchSig(std::span<const Byte> v, std::span<const Byte> sig, std::size_t limit) {
    auto search_span = (limit == 0 || limit > v.size())
        ? v
        : v.first(limit);

    auto it = std::ranges::search(search_span, sig);

    if (it.empty()) return std::nullopt;
    return static_cast<std::size_t>(it.begin() - v.begin());
}

void updateValue(std::span<Byte> data, std::size_t index, std::size_t value, std::size_t length) {

	static_assert(std::endian::native == std::endian::little,
	              "byteswap logic assumes little-endian host");

	if (index + length > data.size()) {
		throw std::out_of_range("updateValue: Index out of bounds.");
	}

	auto write = [&]<typename T>(T val) {
		val = std::byteswap(val);
		std::memcpy(data.data() + index, &val, sizeof(T));
	};

	switch (length) {
		case 2: write(static_cast<uint16_t>(value)); break;
		case 4: write(static_cast<uint32_t>(value)); break;
		case 8: write(static_cast<uint64_t>(value)); break;
		default:
			throw std::invalid_argument(
				std::format("updateValue: unsupported length {}", length));
	}
}

std::size_t getValue(std::span<const Byte> data, std::size_t index, std::size_t length) {

	static_assert(std::endian::native == std::endian::little,
	              "byteswap logic assumes little-endian host");

	if (index + length > data.size()) {
		throw std::out_of_range("getValue: index out of bounds");
	}

	auto read = [&]<typename T>() -> T {
		T val;
		std::memcpy(&val, data.data() + index, sizeof(T));
		return std::byteswap(val);
	};

	switch (length) {
		case 2: return read.operator()<uint16_t>();
		case 4: return read.operator()<uint32_t>();
		case 8: return read.operator()<uint64_t>();
		default:
			throw std::invalid_argument(
				std::format("getValue: unsupported length {}", length));
	}
}

