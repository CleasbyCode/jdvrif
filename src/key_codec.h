#pragma once

#include "common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

inline constexpr std::size_t RECOVERY_KEY_BYTES = 16;
inline constexpr std::size_t RECOVERY_KEY_TEXT_CHARS = 24;
inline constexpr std::size_t RECOVERY_KEY_DISPLAY_CHARS = 29;

using RecoveryKeyBytes = std::array<Byte, RECOVERY_KEY_BYTES>;

enum class KeyParseError : std::uint8_t { malformed, checksum };

[[nodiscard]] constexpr bool isKeySeparator(char c) noexcept {
    return c == ' ' || c == '\t' || c == '-';
}

// D2 display form (XXXX-XXXX-XXXX-XXXX-XXXX-XXXX), no NUL terminator.
void encodeRecoveryKeyDisplay(const RecoveryKeyBytes& secret, std::array<char, RECOVERY_KEY_DISPLAY_CHARS>& out);

// Skips separators anywhere in `text`, then accepts a 24-character Base58 KEY
// with a valid check pair. Never allocates; temporaries are wiped.
[[nodiscard]] std::expected<RecoveryKey, KeyParseError> parseRecoveryKeyText(std::string_view text);
