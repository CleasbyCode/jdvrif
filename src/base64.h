#pragma once

#include "common.h"

#include <span>

// Padded base64 length of `binary_size` bytes (throws on overflow).
[[nodiscard]] std::size_t base64EncodedSize(std::size_t binary_size);
void binaryToBase64(std::span<const Byte> binary_data, vBytes& output_vec);
void appendBase64AsBinary(std::span<const Byte> base64_data, vBytes& destination_vec);
