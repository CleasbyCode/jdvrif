#include "binary_io.h"
#include "file_utils.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <limits>
#include <stdexcept>

namespace {
template <typename T>
[[nodiscard]] T swapEndian(T value) {
    if constexpr (std::endian::native == std::endian::little) return std::byteswap(value);
    else return value;
}

template <typename T>
[[nodiscard]] T checkedNarrow(std::size_t value, std::size_t length) {
    if (value > static_cast<std::size_t>(std::numeric_limits<T>::max())) throw std::out_of_range(std::format(
        "updateValue: value {} exceeds {}-byte field capacity", value, length));
    return static_cast<T>(value);
}
}

std::optional<std::size_t> searchSig(std::span<const Byte> v, std::span<const Byte> sig, std::size_t limit) {
    if (sig.empty() || v.empty() || sig.size() > v.size() || (limit != 0 && limit < sig.size())) return std::nullopt;
    const auto search_span = (limit == 0 || limit >= v.size()) ? v : v.first(limit);
    const auto it = std::ranges::search(search_span, sig);
    if (it.empty()) return std::nullopt;
    return static_cast<std::size_t>(it.begin() - v.begin());
}

void updateValue(std::span<Byte> data, std::size_t index, std::size_t value, std::size_t length) {
    if (!spanHasRange(data, index, length)) throw std::out_of_range("updateValue: index out of bounds");

    auto write = [&]<typename T>() {
        const T encoded = swapEndian(checkedNarrow<T>(value, length));
        std::memcpy(data.data() + index, &encoded, sizeof(T));
    };

    switch (length) {
        case 2: write.operator()<uint16_t>(); return;
        case 4: write.operator()<uint32_t>(); return;
        case 8: write.operator()<uint64_t>(); return;
        default: throw std::invalid_argument(std::format("updateValue: unsupported length {}", length));
    }
}

std::size_t getValue(std::span<const Byte> data, std::size_t index, std::size_t length) {
    if (!spanHasRange(data, index, length)) throw std::out_of_range("getValue: index out of bounds");

    auto read = [&]<typename T>() -> std::size_t {
        T encoded{};
        std::memcpy(&encoded, data.data() + index, sizeof(T));
        return static_cast<std::size_t>(swapEndian(encoded));
    };

    switch (length) {
        case 2: return read.operator()<uint16_t>();
        case 4: return read.operator()<uint32_t>();
        case 8: return read.operator()<uint64_t>();
        default: throw std::invalid_argument(std::format("getValue: unsupported length {}", length));
    }
}
