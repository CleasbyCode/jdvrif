#pragma once

#include "common.h"

#include <span>

void binaryToBase64(std::span<const Byte> binary_data, vBytes& output_vec);
void appendBase64AsBinary(std::span<const Byte> base64_data, vBytes& destination_vec);
