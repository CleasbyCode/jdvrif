#pragma once

#include "common.h"

#include <optional>
#include <span>

[[nodiscard]] std::optional<std::size_t> searchSig(std::span<const Byte> v, std::span<const Byte> sig, std::size_t limit = 0);
void updateValue(std::span<Byte> data, std::size_t index, std::size_t value, std::size_t length = 2);
[[nodiscard]] std::size_t getValue(std::span<const Byte> data, std::size_t index, std::size_t length = 2);
