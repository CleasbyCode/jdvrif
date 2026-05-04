#include "base64.h"
#include "file_utils.h"

#include <stdexcept>
#include <string_view>

namespace {

constexpr std::string_view BASE64_TABLE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void binaryToBase64Scalar(std::span<const Byte> binary_data, Byte* output) {
    const std::size_t input_size = binary_data.size();
    const std::size_t full_groups = input_size / 3;
    const Byte* input = binary_data.data();

    for (std::size_t i = 0; i < full_groups; ++i) {
        const Byte a = input[0], b = input[1], c = input[2];

        const std::uint32_t triple = (static_cast<std::uint32_t>(a) << 16) | (static_cast<std::uint32_t>(b) << 8) | static_cast<std::uint32_t>(c);

        *output++ = static_cast<Byte>(BASE64_TABLE[(triple >> 18) & 0x3F]);
        *output++ = static_cast<Byte>(BASE64_TABLE[(triple >> 12) & 0x3F]);
        *output++ = static_cast<Byte>(BASE64_TABLE[(triple >> 6) & 0x3F]);
        *output++ = static_cast<Byte>(BASE64_TABLE[triple & 0x3F]);
        input += 3;
    }

    switch (input_size - full_groups * 3) {
        case 1: {
            const std::uint32_t triple = static_cast<std::uint32_t>(input[0]) << 16;
            *output++ = static_cast<Byte>(BASE64_TABLE[(triple >> 18) & 0x3F]);
            *output++ = static_cast<Byte>(BASE64_TABLE[(triple >> 12) & 0x3F]);
            *output++ = static_cast<Byte>('=');
            *output = static_cast<Byte>('=');
            return;
        }
        case 2: {
            const std::uint32_t triple = (static_cast<std::uint32_t>(input[0]) << 16) | (static_cast<std::uint32_t>(input[1]) << 8);
            *output++ = static_cast<Byte>(BASE64_TABLE[(triple >> 18) & 0x3F]);
            *output++ = static_cast<Byte>(BASE64_TABLE[(triple >> 12) & 0x3F]);
            *output++ = static_cast<Byte>(BASE64_TABLE[(triple >> 6) & 0x3F]);
            *output = static_cast<Byte>('=');
            return;
        }
        default:
            return;
    }
}

[[nodiscard]] int decodeValue(Byte c) { return BASE64_DECODE_TABLE[c]; }

void decodeBase64Quartet(Byte c0, Byte c1, Byte c2, Byte c3, Byte*& output) {
    const int v0 = decodeValue(c0), v1 = decodeValue(c1), v2 = decodeValue(c2), v3 = decodeValue(c3);
    if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) throw std::invalid_argument("Invalid Base64 character encountered");

    const uint32_t triple = (static_cast<uint32_t>(v0) << 18) | (static_cast<uint32_t>(v1) << 12) | (static_cast<uint32_t>(v2) << 6) | static_cast<uint32_t>(v3);
    *output++ = static_cast<Byte>((triple >> 16) & 0xFF);
    *output++ = static_cast<Byte>((triple >> 8) & 0xFF);
    *output++ = static_cast<Byte>(triple & 0xFF);
}

void decodeFinalBase64Quartet(Byte c0, Byte c1, Byte c2, Byte c3, Byte*& output) {
    const bool p2 = (c2 == static_cast<Byte>('='));
    const bool p3 = (c3 == static_cast<Byte>('='));

    if (p2 && !p3) {
        throw std::invalid_argument("Invalid Base64 padding: '==' required when third char is '='");
    }

    const int v0 = decodeValue(c0), v1 = decodeValue(c1), v2 = p2 ? 0 : decodeValue(c2), v3 = p3 ? 0 : decodeValue(c3);
    if (v0 < 0 || v1 < 0 || (!p2 && v2 < 0) || (!p3 && v3 < 0)) throw std::invalid_argument("Invalid Base64 character encountered");
    if ((p2 && (v1 & 0x0F) != 0) || (!p2 && p3 && (v2 & 0x03) != 0)) {
        throw std::invalid_argument("Invalid Base64 padding bits");
    }

    const uint32_t triple = (static_cast<uint32_t>(v0) << 18) | (static_cast<uint32_t>(v1) << 12) | (static_cast<uint32_t>(v2) << 6) | static_cast<uint32_t>(v3);
    *output++ = static_cast<Byte>((triple >> 16) & 0xFF);
    if (!p2) *output++ = static_cast<Byte>((triple >> 8) & 0xFF);
    if (!p3) *output++ = static_cast<Byte>(triple & 0xFF);
}

}

void binaryToBase64(std::span<const Byte> binary_data, vBytes& output_vec) {
    const std::size_t input_size = binary_data.size();
    const std::size_t base_offset = output_vec.size();
    const std::size_t output_size = checkedMul(
        checkedAdd(input_size, 2, "Base64 encode size overflow") / 3,
        4,
        "Base64 encode size overflow");
    const std::size_t final_size = checkedAdd(base_offset, output_size, "Base64 encode destination size overflow");

    output_vec.resize(final_size);
    if (output_size == 0) return;

    Byte* output = output_vec.data() + static_cast<std::ptrdiff_t>(base_offset);
    binaryToBase64Scalar(binary_data, output);
}

void appendBase64AsBinary(std::span<const Byte> base64_data, vBytes& destination_vec) {
    const std::size_t input_size = base64_data.size();

    if (input_size == 0 || (input_size % 4) != 0) {
        throw std::invalid_argument("Base64 input size must be a multiple of 4 and non-empty");
    }

    const std::size_t decoded_upper_bound = (input_size / 4) * 3;
    const bool has_one_pad = base64_data[input_size - 1] == static_cast<Byte>('=');
    const bool has_two_pad = base64_data[input_size - 2] == static_cast<Byte>('=');
    if (has_two_pad && !has_one_pad) {
        throw std::invalid_argument("Invalid Base64 padding: '==' required when third char is '='");
    }
    const std::size_t padding = has_two_pad ? 2 : (has_one_pad ? 1 : 0);
    const std::size_t decoded_size = decoded_upper_bound - padding;
    const std::size_t base_offset = destination_vec.size();
    const std::size_t final_size = checkedAdd(
        base_offset,
        decoded_size,
        "Base64 decode destination size overflow");

    destination_vec.resize(final_size);
    Byte* output = destination_vec.data() + static_cast<std::ptrdiff_t>(base_offset);

    try {
        const std::size_t full_quartets = input_size / 4 - 1;
        for (std::size_t i = 0; i < full_quartets; ++i) {
            const std::size_t index = i * 4;
            decodeBase64Quartet(base64_data[index], base64_data[index + 1], base64_data[index + 2], base64_data[index + 3], output);
        }

        const std::size_t final_index = input_size - 4;
        decodeFinalBase64Quartet(base64_data[final_index], base64_data[final_index + 1], base64_data[final_index + 2], base64_data[final_index + 3], output);
    } catch (...) {
        destination_vec.resize(base_offset);
        throw;
    }
}
