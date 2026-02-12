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

void updateValue(vBytes& vec, std::size_t index, std::size_t value, std::size_t length) {
    if (index + length > vec.size()) {
        throw std::out_of_range("updateValue: Index out of bounds.");
    }
    switch (length) {
        case 2: {
            auto be = std::byteswap(static_cast<uint16_t>(value));
            std::memcpy(vec.data() + index, &be, 2);
            break;
        }
        case 4: {
            auto be = std::byteswap(static_cast<uint32_t>(value));
            std::memcpy(vec.data() + index, &be, 4);
            break;
        }
        case 8: {
            auto be = std::byteswap(static_cast<uint64_t>(value));
            std::memcpy(vec.data() + index, &be, 8);
            break;
        }
        default:
            throw std::invalid_argument("updateValue: length must be 2, 4, or 8.");
    }
}

std::size_t getValue(std::span<const Byte> data, std::size_t index, std::size_t length) {
    if (index + length > data.size()) {
        throw std::out_of_range("getValue: Index out of bounds.");
    }
    switch (length) {
        case 2: {
            uint16_t value;
            std::memcpy(&value, data.data() + index, 2);
            return std::byteswap(value);
        }
        case 4: {
            uint32_t value;
            std::memcpy(&value, data.data() + index, 4);
            return std::byteswap(value);
        }
        case 8: {
            uint64_t value;
            std::memcpy(&value, data.data() + index, 8);
            return std::byteswap(value);
        }
        default:
            throw std::invalid_argument("getValue: length must be 2, 4, or 8.");
    }
}
