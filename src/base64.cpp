#include "base64.h"
#include "file_utils.h"

#include <stdexcept>
#include <string_view>

namespace {

constexpr std::string_view BASE64_TABLE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void binaryToBase64Scalar(std::span<const Byte> binary_data, Byte* output) {
    const std::size_t input_size = binary_data.size();
    std::size_t out = 0;

    for (std::size_t i = 0; i < input_size; i += 3) {
        const Byte a = binary_data[i], b = (i + 1 < input_size) ? binary_data[i + 1] : 0, c = (i + 2 < input_size) ? binary_data[i + 2] : 0;

        const std::uint32_t triple = (static_cast<std::uint32_t>(a) << 16) | (static_cast<std::uint32_t>(b) << 8) | static_cast<std::uint32_t>(c);

        output[out++] = static_cast<Byte>(BASE64_TABLE[(triple >> 18) & 0x3F]);
        output[out++] = static_cast<Byte>(BASE64_TABLE[(triple >> 12) & 0x3F]);
        output[out++] = (i + 1 < input_size)
            ? static_cast<Byte>(BASE64_TABLE[(triple >> 6) & 0x3F])
            : static_cast<Byte>('=');
        output[out++] = (i + 2 < input_size)
            ? static_cast<Byte>(BASE64_TABLE[triple & 0x3F])
            : static_cast<Byte>('=');
    }
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
    const std::size_t reserve_target = checkedAdd(
        destination_vec.size(),
        decoded_upper_bound,
        "Base64 decode destination size overflow");
    destination_vec.reserve(reserve_target);

    for (std::size_t i = 0; i < input_size; i += 4) {
        const Byte c0 = base64_data[i], c1 = base64_data[i + 1], c2 = base64_data[i + 2], c3 = base64_data[i + 3];

        const bool p2 = (c2 == '='), p3 = (c3 == '=');

        if (p2 && !p3) {
            throw std::invalid_argument("Invalid Base64 padding: '==' required when third char is '='");
        }
        if ((p2 || p3) && (i + 4 < input_size)) {
            throw std::invalid_argument("Padding '=' may only appear in the final quartet");
        }

        const int v0 = BASE64_DECODE_TABLE[c0], v1 = BASE64_DECODE_TABLE[c1], v2 = p2 ? 0 : BASE64_DECODE_TABLE[c2], v3 = p3 ? 0 : BASE64_DECODE_TABLE[c3];

        if (v0 < 0 || v1 < 0 || (!p2 && v2 < 0) || (!p3 && v3 < 0)) throw std::invalid_argument("Invalid Base64 character encountered");

        const uint32_t triple = (static_cast<uint32_t>(v0) << 18) | (static_cast<uint32_t>(v1) << 12) | (static_cast<uint32_t>(v2) << 6) | static_cast<uint32_t>(v3);

        destination_vec.emplace_back(static_cast<Byte>((triple >> 16) & 0xFF));

        if (!p2) destination_vec.emplace_back(static_cast<Byte>((triple >> 8) & 0xFF));
        if (!p3) destination_vec.emplace_back(static_cast<Byte>(triple & 0xFF));
    }
}
