#pragma once

#include <vector>
#include <array>
#include <cstdint>
#include <cstddef>
#include <algorithm> 

template <typename T, size_t N>
uint32_t searchFunc(std::vector<uint8_t>& vec, uint32_t start_index, const uint8_t INCREMENT_SEARCH_INDEX, const std::array<T, N>& SIG) {
    return static_cast<uint32_t>(std::search(vec.begin() + start_index + INCREMENT_SEARCH_INDEX, vec.end(), SIG.begin(), SIG.end()) - vec.begin());
}
