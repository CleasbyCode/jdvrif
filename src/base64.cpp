#include "base64.h"
#include "file_utils.h"

#include <stdexcept>
#include <string_view>

// Only the Bluesky XMP overflow segment uses base64, and it is capped at a few
// tens of KiB per image, so a plain scalar codec is all this needs.

namespace {

constexpr std::string_view BASE64_TABLE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr Byte BASE64_PAD = static_cast<Byte>('=');

constexpr auto BASE64_DECODE_TABLE = [] {
    std::array<int8_t, 256> table{};
    table.fill(-1);
    for (std::size_t i = 0; i < BASE64_TABLE.size(); ++i) {
        table[static_cast<Byte>(BASE64_TABLE[i])] = static_cast<int8_t>(i);
    }
    return table;
}();

void binaryToBase64Scalar(std::span<const Byte> binary_data, Byte* output) {
    const std::size_t input_size = binary_data.size();
    const std::size_t full_groups = input_size / 3;
    const Byte* input = binary_data.data();

    for (std::size_t i = 0; i < full_groups; ++i) {
        const std::uint32_t triple =
            (static_cast<std::uint32_t>(input[0]) << 16) |
            (static_cast<std::uint32_t>(input[1]) << 8) |
            static_cast<std::uint32_t>(input[2]);

        *output++ = static_cast<Byte>(BASE64_TABLE[(triple >> 18) & 0x3F]);
        *output++ = static_cast<Byte>(BASE64_TABLE[(triple >> 12) & 0x3F]);
        *output++ = static_cast<Byte>(BASE64_TABLE[(triple >> 6) & 0x3F]);
        *output++ = static_cast<Byte>(BASE64_TABLE[triple & 0x3F]);
        input += 3;
    }

    // Tail of 1 or 2 bytes: the missing input byte(s) contribute zero bits and
    // are represented by '=' in the output quartet.
    const std::size_t tail = input_size - full_groups * 3;
    if (tail == 0) return;

    const std::uint32_t triple =
        (static_cast<std::uint32_t>(input[0]) << 16) |
        (tail == 2 ? static_cast<std::uint32_t>(input[1]) << 8 : 0);
    *output++ = static_cast<Byte>(BASE64_TABLE[(triple >> 18) & 0x3F]);
    *output++ = static_cast<Byte>(BASE64_TABLE[(triple >> 12) & 0x3F]);
    *output++ = (tail == 2) ? static_cast<Byte>(BASE64_TABLE[(triple >> 6) & 0x3F]) : BASE64_PAD;
    *output = BASE64_PAD;
}

void decodeBase64Quartet(Byte c0, Byte c1, Byte c2, Byte c3, Byte*& output) {
    const int v0 = BASE64_DECODE_TABLE[c0];
    const int v1 = BASE64_DECODE_TABLE[c1];
    const int v2 = BASE64_DECODE_TABLE[c2];
    const int v3 = BASE64_DECODE_TABLE[c3];
    if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) {
        throw std::invalid_argument("Invalid Base64 character encountered");
    }

    const uint32_t triple =
        (static_cast<uint32_t>(v0) << 18) |
        (static_cast<uint32_t>(v1) << 12) |
        (static_cast<uint32_t>(v2) << 6) |
        static_cast<uint32_t>(v3);
    *output++ = static_cast<Byte>((triple >> 16) & 0xFF);
    *output++ = static_cast<Byte>((triple >> 8) & 0xFF);
    *output++ = static_cast<Byte>(triple & 0xFF);
}

void decodeFinalBase64Quartet(Byte c0, Byte c1, Byte c2, Byte c3, Byte*& output) {
    const bool p2 = (c2 == BASE64_PAD);
    const bool p3 = (c3 == BASE64_PAD);

    if (p2 && !p3) {
        throw std::invalid_argument("Invalid Base64 padding: '==' required when third char is '='");
    }

    const int v0 = BASE64_DECODE_TABLE[c0];
    const int v1 = BASE64_DECODE_TABLE[c1];
    const int v2 = p2 ? 0 : BASE64_DECODE_TABLE[c2];
    const int v3 = p3 ? 0 : BASE64_DECODE_TABLE[c3];
    if (v0 < 0 || v1 < 0 || (!p2 && v2 < 0) || (!p3 && v3 < 0)) {
        throw std::invalid_argument("Invalid Base64 character encountered");
    }
    if ((p2 && (v1 & 0x0F) != 0) || (!p2 && p3 && (v2 & 0x03) != 0)) {
        throw std::invalid_argument("Invalid Base64 padding bits");
    }

    const uint32_t triple =
        (static_cast<uint32_t>(v0) << 18) |
        (static_cast<uint32_t>(v1) << 12) |
        (static_cast<uint32_t>(v2) << 6) |
        static_cast<uint32_t>(v3);
    *output++ = static_cast<Byte>((triple >> 16) & 0xFF);
    if (!p2) {
        *output++ = static_cast<Byte>((triple >> 8) & 0xFF);
    }
    if (!p3) {
        *output++ = static_cast<Byte>(triple & 0xFF);
    }
}

[[nodiscard]] std::size_t decodedBase64Size(std::span<const Byte> base64_data) {
    const std::size_t input_size = base64_data.size();
    if (input_size == 0 || (input_size % 4) != 0) {
        throw std::invalid_argument("Base64 input size must be a multiple of 4 and non-empty");
    }

    const bool has_one_pad = base64_data[input_size - 1] == BASE64_PAD;
    const bool has_two_pad = base64_data[input_size - 2] == BASE64_PAD;
    if (has_two_pad && !has_one_pad) {
        throw std::invalid_argument("Invalid Base64 padding: '==' required when third char is '='");
    }

    const std::size_t padding = has_two_pad ? 2 : (has_one_pad ? 1 : 0);
    return (input_size / 4) * 3 - padding;
}

} // namespace

void binaryToBase64(std::span<const Byte> binary_data, vBytes& output_vec) {
    const std::size_t base_offset = output_vec.size();
    const std::size_t output_size = checkedMul(
        checkedAdd(binary_data.size(), 2, "Base64 encode size overflow") / 3,
        4,
        "Base64 encode size overflow");
    const std::size_t final_size = checkedAdd(base_offset, output_size, "Base64 encode destination size overflow");

    output_vec.resize(final_size);
    if (output_size == 0) return;

    binaryToBase64Scalar(binary_data, output_vec.data() + static_cast<std::ptrdiff_t>(base_offset));
}

void appendBase64AsBinary(std::span<const Byte> base64_data, vBytes& destination_vec) {
    // Rejects empty and non-multiple-of-4 input, so at least one quartet exists.
    const std::size_t decoded_size = decodedBase64Size(base64_data);
    const std::size_t base_offset  = destination_vec.size();
    const std::size_t final_size   = checkedAdd(base_offset, decoded_size, "Base64 decode destination size overflow");

    destination_vec.resize(final_size);
    Byte* output = destination_vec.data() + static_cast<std::ptrdiff_t>(base_offset);
    const Byte* const expected_output_end = destination_vec.data() + static_cast<std::ptrdiff_t>(final_size);

    try {
        // Full quartets (no padding) up to but not including the last 4 chars,
        // then the final quartet via the padding-aware decoder.
        const std::size_t final_index = base64_data.size() - 4;
        for (std::size_t index = 0; index < final_index; index += 4) {
            decodeBase64Quartet(
                base64_data[index],
                base64_data[index + 1],
                base64_data[index + 2],
                base64_data[index + 3],
                output);
        }

        decodeFinalBase64Quartet(
            base64_data[final_index],
            base64_data[final_index + 1],
            base64_data[final_index + 2],
            base64_data[final_index + 3],
            output);

        throwIf(output != expected_output_end, "Internal Error: Base64 decoded size mismatch.");
    } catch (...) {
        destination_vec.resize(base_offset);
        throw;
    }
}
