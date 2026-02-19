#include "base64.h"

#include <stdexcept>
#include <string_view>

void binaryToBase64(std::span<const Byte> binary_data, vBytes& output_vec) {
    const std::size_t
        input_size  = binary_data.size(),
        output_size = ((input_size + 2) / 3) * 4,
        base_offset = output_vec.size();

    static constexpr std::string_view BASE64_TABLE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    output_vec.resize(base_offset + output_size);

    std::size_t out = base_offset;
    for (std::size_t i = 0; i < input_size; i += 3) {
        const Byte
            a = binary_data[i],
            b = (i + 1 < input_size) ? binary_data[i + 1] : 0,
            c = (i + 2 < input_size) ? binary_data[i + 2] : 0;

        const uint32_t triple = (static_cast<uint32_t>(a) << 16) | (static_cast<uint32_t>(b) << 8)  | c;

        output_vec[out++] = BASE64_TABLE[(triple >> 18) & 0x3F];
        output_vec[out++] = BASE64_TABLE[(triple >> 12) & 0x3F];
        output_vec[out++] = (i + 1 < input_size) ? BASE64_TABLE[(triple >> 6) & 0x3F] : '=';
        output_vec[out++] = (i + 2 < input_size) ? BASE64_TABLE[triple & 0x3F] : '=';
    }
}

void appendBase64AsBinary(std::span<const Byte> base64_data, vBytes& destination_vec) {
    const std::size_t input_size = base64_data.size();

    if (input_size == 0 || (input_size % 4) != 0) {
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

    destination_vec.reserve(destination_vec.size() + (input_size * 3 / 4));

    for (std::size_t i = 0; i < input_size; i += 4) {
        const Byte
            c0 = base64_data[i],
            c1 = base64_data[i + 1],
            c2 = base64_data[i + 2],
            c3 = base64_data[i + 3];

        const bool
            p2 = (c2 == '='),
            p3 = (c3 == '=');

        if (p2 && !p3) {
            throw std::invalid_argument("Invalid Base64 padding: '==' required when third char is '='");
        }
        if ((p2 || p3) && (i + 4 < input_size)) {
            throw std::invalid_argument("Padding '=' may only appear in the final quartet");
        }

        const int
            v0 = BASE64_TABLE[c0],
            v1 = BASE64_TABLE[c1],
            v2 = p2 ? 0 : BASE64_TABLE[c2],
            v3 = p3 ? 0 : BASE64_TABLE[c3];

        if (v0 < 0 || v1 < 0 || (!p2 && v2 < 0) || (!p3 && v3 < 0)) {
            throw std::invalid_argument("Invalid Base64 character encountered");
        }

        const uint32_t triple = (static_cast<uint32_t>(v0) << 18) |
                                (static_cast<uint32_t>(v1) << 12) |
                                (static_cast<uint32_t>(v2) << 6) |
                                static_cast<uint32_t>(v3);

        destination_vec.emplace_back(static_cast<Byte>((triple >> 16) & 0xFF));

        if (!p2) destination_vec.emplace_back(static_cast<Byte>((triple >> 8) & 0xFF));
        if (!p3) destination_vec.emplace_back(static_cast<Byte>(triple & 0xFF));
    }
}
