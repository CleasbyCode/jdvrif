// Vectors for the 128-bit recovery KEY codec (D2/D3), shared with the web and Rust ports.
#include "key_codec.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace {
int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

RecoveryKeyBytes fromHex(std::string_view hex) {
    RecoveryKeyBytes out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<Byte>(std::stoi(std::string(hex.substr(i * 2, 2)), nullptr, 16));
    }
    return out;
}

struct Vector { const char* hex; const char* text; };
constexpr Vector VECTORS[] = {
    {"00000000000000000000000000000001", "1111-1111-1111-1111-1111-126X"},
    {"000102030405060708090a0b0c0d0e0f", "112d-rXXU-ifSr-RnXL-GbXg-8Egs"},
    {"ffffffffffffffffffffffffffffffff", "YcVf-xkQb-6JRz-qk5k-F2tN-LvUM"},
    {"8f3a1c5e9b2d4f60718293a4b5c6d7e8", "Jgoe-TsV4-hWLD-SMP9-PJt7-nPTd"},
    // Check-value boundaries: 0 ("11") and 3363 ("zz").
    {"000000000000000000000000000003d3", "1111-1111-1111-1111-1111-Ht11"},
    {"00000000000000000000000000000abf", "1111-1111-1111-1111-1111-pSzz"},
};

void expectModern(std::string_view text, const RecoveryKeyBytes& secret, const char* what) {
    auto r = parseRecoveryKeyText(text);
    expect(r.has_value() && r->kind == RecoveryKey::Kind::base58_128 && r->secret == secret, what);
}

void expectError(std::string_view text, KeyParseError e, const char* what) {
    auto r = parseRecoveryKeyText(text);
    expect(!r.has_value() && r.error() == e, what);
}

std::string stripDashes(std::string_view s) {
    std::string o;
    for (char c : s) if (c != '-') o += c;
    return o;
}
}  // namespace

int main() {
    if (sodium_init() < 0) return 2;

    for (const auto& v : VECTORS) {
        const RecoveryKeyBytes secret = fromHex(v.hex);
        std::array<char, RECOVERY_KEY_DISPLAY_CHARS> disp{};
        encodeRecoveryKeyDisplay(secret, disp);
        expect(std::string_view(disp.data(), disp.size()) == v.text, "encode display");
        expectModern(v.text, secret, "parse dashed");
        expectModern(stripDashes(v.text), secret, "parse undashed");
        std::string spaced = v.text;
        spaced.insert(2, " ");
        spaced.insert(8, "\t");
        spaced = " " + spaced + " ";
        expectModern(spaced, secret, "parse with spaces/tabs");
    }

    expectError("112d-rXXU-ifSr-RnXL-GbXg-8Egt", KeyParseError::checksum, "last char changed");
    expectError("112D-rXXU-ifSr-RnXL-GbXg-8Egs", KeyParseError::checksum, "case flip");
    expectError("112d-rXXU-ifSr-RnXL-GbXh-8Egs", KeyParseError::checksum, "body char changed");
    expectError("YcVf-xkQb-6JRz-qk5k-F2tN-LwUM", KeyParseError::malformed, "body is 2^128");
    expectError("1111-1111-1111-1111-1111-11xx", KeyParseError::malformed, "all-zero secret");
    expectError("112d-rXXU-ifSr-RnXL-GbXg-8Eg0", KeyParseError::malformed, "zero char");
    expectError("112d-rXXU-ifSr-RnXL-GbXg-8Eg", KeyParseError::malformed, "23 chars");
    expectError("112d-rXXU-ifSr-RnXL-GbXg-8Egss", KeyParseError::malformed, "25 chars");

    for (const char* bad : {"O12d-rXXU-ifSr-RnXL-GbXg-8Egs", "I12d-rXXU-ifSr-RnXL-GbXg-8Egs",
                            "l12d-rXXU-ifSr-RnXL-GbXg-8Egs"}) {
        expectError(bad, KeyParseError::malformed, "look-alike");
    }

    // Pre-v10 decimal PINs are no longer accepted.
    for (const char* bad : {"15753985819510608748", "18446744073709551615", "1575-3985",
                            "18446744073709551616", "0", "0123", "", "12O4", "99999999999999999999"}) {
        expectError(bad, KeyParseError::malformed, bad);
    }

    RecoveryKey a = RecoveryKey::modern(fromHex(VECTORS[1].hex));
    RecoveryKey b = std::move(a);
    expect(a.kind == RecoveryKey::Kind::none && a.secret == RecoveryKeyBytes{} && b.kind == RecoveryKey::Kind::base58_128,
           "move wipes source");

    if (failures != 0) return 1;
    std::puts("KEY codec vectors pass");
    return 0;
}
