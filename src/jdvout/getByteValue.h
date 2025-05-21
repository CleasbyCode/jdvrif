#pragma once

#include <vector>    
#include <cstdint> 

template <typename T>
T getByteValue(const std::vector<uint8_t>& VEC, uint32_t INDEX) {
    constexpr size_t numBytes = sizeof(T);

    T value = 0;
    for (size_t i = 0; i < numBytes; ++i) {
        value |= static_cast<T>(VEC[INDEX + i]) << ((numBytes - 1 - i) * 8);
    }
    return value;
}
