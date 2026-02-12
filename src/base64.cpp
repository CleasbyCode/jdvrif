#include "base64.h"

#include <stdexcept>
#include <string_view>

void binaryToBase64(std::span<const Byte> binary_data, vBytes& output_vec) {
    const std::size_t
        INPUT_SIZE  = binary_data.size(),
        OUTPUT_SIZE = ((INPUT_SIZE + 2) / 3) * 4,
        BASE_OFFSET = output_vec.size();

    static constexpr std::string_view BASE64_TABLE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    output_vec.resize(BASE_OFFSET + OUTPUT_SIZE);

    std::size_t out = BASE_OFFSET;
    for (std::size_t i = 0; i < INPUT_SIZE; i += 3) {
        const Byte
            A = binary_data[i],
            B = (i + 1 < INPUT_SIZE) ? binary_data[i + 1] : 0,
            C = (i + 2 < INPUT_SIZE) ? binary_data[i + 2] : 0;

        const uint32_t triple = (static_cast<uint32_t>(A) << 16) | (static_cast<uint32_t>(B) << 8)  | C;

        output_vec[out++] = BASE64_TABLE[(triple >> 18) & 0x3F];
        output_vec[out++] = BASE64_TABLE[(triple >> 12) & 0x3F];
        output_vec[out++] = (i + 1 < INPUT_SIZE) ? BASE64_TABLE[(triple >> 6) & 0x3F] : '=';
        output_vec[out++] = (i + 2 < INPUT_SIZE) ? BASE64_TABLE[triple & 0x3F] : '=';
    }
}

void appendBase64AsBinary(std::span<const Byte> base64_data, vBytes& destination_vec) {
    const std::size_t INPUT_SIZE = base64_data.size();

    if (INPUT_SIZE == 0 || (INPUT_SIZE % 4) != 0) {
        throw std::invalid_argument("Base64 input size must be a multiple of 4 and non-empty");
    }

    static constexpr auto BASE64_TABLE = std::to_array<int8_t>({
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
        -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
        -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
    });

    destination_vec.reserve(destination_vec.size() + (INPUT_SIZE * 3 / 4));

    for (std::size_t i = 0; i < INPUT_SIZE; i += 4) {
        const Byte
            C0 = base64_data[i],
            C1 = base64_data[i + 1],
            C2 = base64_data[i + 2],
            C3 = base64_data[i + 3];

        const bool
            P2 = (C2 == '='),
            P3 = (C3 == '=');

        if (P2 && !P3) {
            throw std::invalid_argument("Invalid Base64 padding: '==' required when third char is '='");
        }
        if ((P2 || P3) && (i + 4 < INPUT_SIZE)) {
            throw std::invalid_argument("Padding '=' may only appear in the final quartet");
        }

        const int
            V0 = BASE64_TABLE[C0],
            V1 = BASE64_TABLE[C1],
            V2 = P2 ? 0 : BASE64_TABLE[C2],
            V3 = P3 ? 0 : BASE64_TABLE[C3];

        if (V0 < 0 || V1 < 0 || (!P2 && V2 < 0) || (!P3 && V3 < 0)) {
            throw std::invalid_argument("Invalid Base64 character encountered");
        }

        const uint32_t TRIPLE = (static_cast<uint32_t>(V0) << 18) |
                                (static_cast<uint32_t>(V1) << 12) |
                                (static_cast<uint32_t>(V2) << 6) |
                                static_cast<uint32_t>(V3);

        destination_vec.emplace_back(static_cast<Byte>((TRIPLE >> 16) & 0xFF));

        if (!P2) destination_vec.emplace_back(static_cast<Byte>((TRIPLE >> 8) & 0xFF));
        if (!P3) destination_vec.emplace_back(static_cast<Byte>(TRIPLE & 0xFF));
    }
}
