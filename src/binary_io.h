#pragma once

#include "common.h"

#include <optional>
#include <span>

// Default limit of 0 means "Search Whole File".
// Any other value means "Search ONLY up to this limit".
[[nodiscard]] std::optional<std::size_t> searchSig(std::span<const Byte> v, std::span<const Byte> sig, std::size_t limit = 0);

// Write an integer as big-endian bytes into `vec` at the given byte offset.
// Length must be 2, 4, or 8.
void updateValue(vBytes& vec, std::size_t index, std::size_t value, std::size_t length = 2);

// Read a big-endian integer from `data` at the given byte offset.
// Length must be 2, 4, or 8.
[[nodiscard]] std::size_t getValue(std::span<const Byte> data, std::size_t index, std::size_t length = 2);
