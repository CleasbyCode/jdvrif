#pragma once

#include "common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

inline constexpr std::size_t PIN128_BYTES = 16;
inline constexpr std::size_t PIN128_TEXT_CHARS = 24;
inline constexpr std::size_t PIN128_DISPLAY_CHARS = 29;

using Pin128Bytes = std::array<Byte, PIN128_BYTES>;

enum class PinParseError : std::uint8_t { malformed, checksum };

[[nodiscard]] constexpr bool isPinSeparator(char c) noexcept {
    return c == ' ' || c == '\t' || c == '-';
}

// D2 display form (XXXX-XXXX-XXXX-XXXX-XXXX-XXXX), no NUL terminator.
void encodePin128Display(const Pin128Bytes& secret, std::array<char, PIN128_DISPLAY_CHARS>& out);

// Skips separators anywhere in `text`, then accepts either a legacy decimal PIN
// (1-20 digits, no leading zero, <= 2^64-1) or a 24-character Base58 PIN with a
// valid check pair. Never allocates; temporaries are wiped.
[[nodiscard]] std::expected<SecurePin, PinParseError> parseRecoveryPinText(std::string_view text);
