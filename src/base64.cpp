#include "base64.h"
#include "file_utils.h"

#include <sodium.h>

#include <algorithm>
#include <stdexcept>

// Thin wrappers over libsodium's standard padded base64 (RFC 4648 section 4,
// sodium_base64_VARIANT_ORIGINAL). Only the Bluesky XMP overflow segment uses
// base64, capped at a few tens of KiB per image.
//
// sodium_base642bin with no `ignore` set and no `b64_end` out-pointer rejects
// missing or surplus '=' padding, non-zero bits left over in the final
// quartet, and any input after the padding. It does NOT reliably reject bytes
// outside the alphabet: libsodium 1.0.18 decodes every byte >= 0x80 as '/'.
// So the alphabet is checked here first.

namespace {
[[nodiscard]] constexpr bool isBase64Char(Byte c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '+' || c == '/' || c == '=';
}
} // namespace

void binaryToBase64(std::span<const Byte> binary_data, vBytes& output_vec) {
    if (binary_data.empty()) return;

    const std::size_t base_offset = output_vec.size();
    const std::size_t output_size = checkedMul(
        checkedAdd(binary_data.size(), 2, "Base64 encode size overflow") / 3,
        4,
        "Base64 encode size overflow");
    // libsodium NUL-terminates, so give it one byte more and drop it after.
    output_vec.resize(checkedAdd(
        checkedAdd(base_offset, output_size, "Base64 encode destination size overflow"),
        1,
        "Base64 encode destination size overflow"));
    sodium_bin2base64(
        reinterpret_cast<char*>(output_vec.data() + base_offset), output_size + 1,
        binary_data.data(), binary_data.size(),
        sodium_base64_VARIANT_ORIGINAL);
    output_vec.pop_back();
}

void appendBase64AsBinary(std::span<const Byte> base64_data, vBytes& destination_vec) {
    if (base64_data.empty() || base64_data.size() % 4 != 0) {
        throw std::invalid_argument("Base64 input size must be a multiple of 4 and non-empty");
    }
    if (!std::ranges::all_of(base64_data, isBase64Char)) {
        throw std::invalid_argument("Invalid Base64 character encountered");
    }

    // Size for the unpadded maximum, then trim to what was actually decoded.
    const std::size_t base_offset = destination_vec.size();
    const std::size_t max_decoded = base64_data.size() / 4 * 3;
    destination_vec.resize(checkedAdd(base_offset, max_decoded, "Base64 decode destination size overflow"));

    std::size_t decoded = 0;
    if (sodium_base642bin(
            destination_vec.data() + base_offset, max_decoded,
            reinterpret_cast<const char*>(base64_data.data()), base64_data.size(),
            nullptr, &decoded, nullptr,
            sodium_base64_VARIANT_ORIGINAL) != 0) {
        // Leave the destination exactly as the caller passed it.
        destination_vec.resize(base_offset);
        throw std::invalid_argument("Invalid Base64 input");
    }
    destination_vec.resize(base_offset + decoded);
}
