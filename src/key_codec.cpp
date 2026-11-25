#include "key_codec.h"

#include <sodium.h>

#include <cstring>

namespace {
// The KEY's Base58 characters, without the display form's group dashes.
constexpr std::size_t RECOVERY_KEY_TEXT_CHARS = 24;

// Typed or pasted between the KEY's characters, and ignored.
[[nodiscard]] constexpr bool isKeySeparator(char c) noexcept {
    return c == ' ' || c == '\t' || c == '-';
}

constexpr std::string_view ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
constexpr std::size_t BODY_CHARS = 22;
constexpr std::uint32_t CHECK_MODULUS = 58 * 58;
// Part of the KEY format: every KEY's check pair is hashed under this label,
// so it keeps its original wording ("pin") byte-for-byte.
constexpr char CHECK_DOMAIN[] = "jdvrif pin check v1";
constexpr std::size_t CHECK_DOMAIN_LEN = sizeof(CHECK_DOMAIN) - 1;

// The KEY's characters and digits are the secret, so the two alphabet
// mappings below compare against every entry and select with masks: no
// branch, early exit or table index depends on them. The empty asm keeps the
// compiler from turning a mask back into a branch or a lookup. (Dividing by
// the constant 58 compiles to a multiply and shift, also constant time.)
[[nodiscard]] std::uint32_t valueBarrier(std::uint32_t value) noexcept {
    asm volatile("" : "+r"(value));
    return value;
}

// All ones when a == b, zero otherwise; both below 2^31.
[[nodiscard]] std::uint32_t equalMask(std::uint32_t a, std::uint32_t b) noexcept {
    return valueBarrier(0U - (((a ^ b) - 1U) >> 31U));
}

// The digit of `c`, or -1 when it is not a Base58 character.
[[nodiscard]] int base58Digit(char c) noexcept {
    const auto byte = static_cast<std::uint32_t>(static_cast<unsigned char>(c));
    std::uint32_t digit = 0;
    std::uint32_t found = 0;
    for (std::uint32_t index = 0; index < ALPHABET.size(); ++index) {
        const std::uint32_t mask =
            equalMask(static_cast<std::uint32_t>(static_cast<unsigned char>(ALPHABET[index])), byte);
        digit |= index & mask;
        found |= mask;
    }
    return static_cast<int>(digit | ~found);  // -1 when nothing matched
}

// The character for `digit` (0..57).
[[nodiscard]] char base58Char(std::uint32_t digit) noexcept {
    std::uint32_t out = 0;
    for (std::uint32_t index = 0; index < ALPHABET.size(); ++index) {
        out |= static_cast<std::uint32_t>(static_cast<unsigned char>(ALPHABET[index])) & equalMask(index, digit);
    }
    return static_cast<char>(out);
}

[[nodiscard]] std::uint32_t checkValue(const RecoveryKeyBytes& secret) noexcept {
    std::array<Byte, CHECK_DOMAIN_LEN + RECOVERY_KEY_BYTES> msg{};
    std::memcpy(msg.data(), CHECK_DOMAIN, CHECK_DOMAIN_LEN);
    std::memcpy(msg.data() + CHECK_DOMAIN_LEN, secret.data(), secret.size());
    std::array<Byte, 32> hash{};
    crypto_generichash(hash.data(), hash.size(), msg.data(), msg.size(), nullptr, 0);
    const std::uint32_t be = (static_cast<std::uint32_t>(hash[0]) << 24) |
                             (static_cast<std::uint32_t>(hash[1]) << 16) |
                             (static_cast<std::uint32_t>(hash[2]) << 8) |
                             static_cast<std::uint32_t>(hash[3]);
    wipeValue(msg);
    wipeValue(hash);
    return be % CHECK_MODULUS;
}

}  // namespace

void encodeRecoveryKeyDisplay(const RecoveryKeyBytes& secret, std::array<char, RECOVERY_KEY_DISPLAY_CHARS>& out) {
    std::array<char, RECOVERY_KEY_TEXT_CHARS> chars{};
    RecoveryKeyBytes work = secret;

    for (std::size_t i = BODY_CHARS; i-- > 0;) {
        std::uint32_t rem = 0;
        for (auto& b : work) {
            const std::uint32_t cur = (rem << 8) | b;
            b = static_cast<Byte>(cur / 58);
            rem = cur % 58;
        }
        chars[i] = base58Char(rem);
    }
    const std::uint32_t c = checkValue(secret);
    chars[BODY_CHARS] = base58Char(c / 58);
    chars[BODY_CHARS + 1] = base58Char(c % 58);

    std::size_t o = 0;
    for (std::size_t i = 0; i < RECOVERY_KEY_TEXT_CHARS; ++i) {
        if (i != 0 && i % 4 == 0) out[o++] = '-';
        out[o++] = chars[i];
    }
    wipeValue(work);
    wipeValue(chars);
}

std::expected<RecoveryKey, KeyParseError> parseRecoveryKeyText(std::string_view text) {
    std::array<char, RECOVERY_KEY_TEXT_CHARS> sig{};
    WipePodGuard<std::array<char, RECOVERY_KEY_TEXT_CHARS>> sig_wipe{sig};
    std::size_t n = 0;
    for (const char c : text) {
        if (isKeySeparator(c)) continue;
        if (n == sig.size()) return std::unexpected(KeyParseError::malformed);
        sig[n++] = c;
    }

    if (n != RECOVERY_KEY_TEXT_CHARS) return std::unexpected(KeyParseError::malformed);

    std::array<int, RECOVERY_KEY_TEXT_CHARS> digits{};
    WipePodGuard<std::array<int, RECOVERY_KEY_TEXT_CHARS>> digits_wipe{digits};
    // Every character is decoded before the verdict, so where a stray
    // character sits does not show in the timing either.
    std::uint32_t invalid = 0;
    for (std::size_t i = 0; i < n; ++i) {
        digits[i] = base58Digit(sig[i]);
        invalid |= static_cast<std::uint32_t>(digits[i]) >> 31U;
    }
    if (invalid != 0) return std::unexpected(KeyParseError::malformed);

    RecoveryKeyBytes secret{};
    WipePodGuard<RecoveryKeyBytes> secret_wipe{secret};
    for (std::size_t i = 0; i < BODY_CHARS; ++i) {
        std::uint32_t carry = static_cast<std::uint32_t>(digits[i]);
        for (std::size_t j = secret.size(); j-- > 0;) {
            const std::uint32_t cur = static_cast<std::uint32_t>(secret[j]) * 58 + carry;
            secret[j] = static_cast<Byte>(cur & 0xff);
            carry = cur >> 8;
        }
        if (carry != 0) return std::unexpected(KeyParseError::malformed);
    }

    std::uint8_t any = 0;
    for (const Byte b : secret) any |= b;
    if (any == 0) return std::unexpected(KeyParseError::malformed);

    const auto expected_check = static_cast<std::uint32_t>(digits[BODY_CHARS] * 58 + digits[BODY_CHARS + 1]);
    if (checkValue(secret) != expected_check) return std::unexpected(KeyParseError::checksum);

    return RecoveryKey::fromSecret(secret);
}
