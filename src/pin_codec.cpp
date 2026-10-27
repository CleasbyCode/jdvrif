#include "pin_codec.h"

#include <sodium.h>

#include <cstring>

namespace {

constexpr std::string_view ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
constexpr std::size_t BODY_CHARS = 22;
constexpr std::uint32_t CHECK_MODULUS = 58 * 58;
constexpr char CHECK_DOMAIN[] = "jdvrif pin check v1";
constexpr std::size_t CHECK_DOMAIN_LEN = sizeof(CHECK_DOMAIN) - 1;
constexpr std::size_t MAX_LEGACY_DIGITS = 20;

[[nodiscard]] int base58Digit(char c) noexcept {
    const auto pos = ALPHABET.find(c);
    return pos == std::string_view::npos ? -1 : static_cast<int>(pos);
}

[[nodiscard]] std::uint32_t checkValue(const Pin128Bytes& secret) noexcept {
    std::array<Byte, CHECK_DOMAIN_LEN + PIN128_BYTES> msg{};
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

void encodePin128Display(const Pin128Bytes& secret, std::array<char, PIN128_DISPLAY_CHARS>& out) {
    std::array<char, PIN128_TEXT_CHARS> chars{};
    Pin128Bytes work = secret;

    for (std::size_t i = BODY_CHARS; i-- > 0;) {
        std::uint32_t rem = 0;
        for (auto& b : work) {
            const std::uint32_t cur = (rem << 8) | b;
            b = static_cast<Byte>(cur / 58);
            rem = cur % 58;
        }
        chars[i] = ALPHABET[rem];
    }
    const std::uint32_t c = checkValue(secret);
    chars[BODY_CHARS] = ALPHABET[c / 58];
    chars[BODY_CHARS + 1] = ALPHABET[c % 58];

    std::size_t o = 0;
    for (std::size_t i = 0; i < PIN128_TEXT_CHARS; ++i) {
        if (i != 0 && i % 4 == 0) out[o++] = '-';
        out[o++] = chars[i];
    }
    wipeValue(work);
    wipeValue(chars);
}

std::expected<SecurePin, PinParseError> parseRecoveryPinText(std::string_view text) {
    std::array<char, PIN128_TEXT_CHARS> sig{};
    WipePodGuard<std::array<char, PIN128_TEXT_CHARS>> sig_wipe{sig};
    std::size_t n = 0;
    bool all_digits = true;
    for (const char c : text) {
        if (isPinSeparator(c)) continue;
        if (n == sig.size()) return std::unexpected(PinParseError::malformed);
        sig[n++] = c;
        if (c < '0' || c > '9') all_digits = false;
    }

    // 1. Legacy decimal.
    if (all_digits && n >= 1 && n <= MAX_LEGACY_DIGITS) {
        if (sig[0] == '0') return std::unexpected(PinParseError::malformed);
        std::uint64_t v = 0;
        WipePodGuard<std::uint64_t> v_wipe{v};
        for (std::size_t i = 0; i < n; ++i) {
            const auto d = static_cast<std::uint64_t>(sig[i] - '0');
            if (v > (UINT64_MAX - d) / 10) return std::unexpected(PinParseError::malformed);
            v = v * 10 + d;
        }
        return SecurePin::legacy(v);
    }

    // 2. 128-bit Base58.
    if (n != PIN128_TEXT_CHARS) return std::unexpected(PinParseError::malformed);

    std::array<int, PIN128_TEXT_CHARS> digits{};
    WipePodGuard<std::array<int, PIN128_TEXT_CHARS>> digits_wipe{digits};
    for (std::size_t i = 0; i < n; ++i) {
        digits[i] = base58Digit(sig[i]);
        if (digits[i] < 0) return std::unexpected(PinParseError::malformed);
    }

    Pin128Bytes secret{};
    WipePodGuard<Pin128Bytes> secret_wipe{secret};
    for (std::size_t i = 0; i < BODY_CHARS; ++i) {
        std::uint32_t carry = static_cast<std::uint32_t>(digits[i]);
        for (std::size_t j = secret.size(); j-- > 0;) {
            const std::uint32_t cur = static_cast<std::uint32_t>(secret[j]) * 58 + carry;
            secret[j] = static_cast<Byte>(cur & 0xff);
            carry = cur >> 8;
        }
        if (carry != 0) return std::unexpected(PinParseError::malformed);
    }

    std::uint8_t any = 0;
    for (const Byte b : secret) any |= b;
    if (any == 0) return std::unexpected(PinParseError::malformed);

    const auto expected_check = static_cast<std::uint32_t>(digits[BODY_CHARS] * 58 + digits[BODY_CHARS + 1]);
    if (checkValue(secret) != expected_check) return std::unexpected(PinParseError::checksum);

    return SecurePin::modern(secret);
}
