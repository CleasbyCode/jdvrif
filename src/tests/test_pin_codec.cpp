// Vectors for the 128-bit recovery PIN codec (D2/D3), shared with the web and Rust ports.
#include "pin_codec.h"

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

Pin128Bytes fromHex(std::string_view hex) {
    Pin128Bytes out{};
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

void expectModern(std::string_view text, const Pin128Bytes& secret, const char* what) {
    auto r = parseRecoveryPinText(text);
    expect(r.has_value() && r->kind == SecurePin::Kind::base58_128 && r->secret == secret, what);
}

void expectError(std::string_view text, PinParseError e, const char* what) {
    auto r = parseRecoveryPinText(text);
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
        const Pin128Bytes secret = fromHex(v.hex);
        std::array<char, PIN128_DISPLAY_CHARS> disp{};
        encodePin128Display(secret, disp);
        expect(std::string_view(disp.data(), disp.size()) == v.text, "encode display");
        expectModern(v.text, secret, "parse dashed");
        expectModern(stripDashes(v.text), secret, "parse undashed");
        std::string spaced = v.text;
        spaced.insert(2, " ");
        spaced.insert(8, "\t");
        spaced = " " + spaced + " ";
        expectModern(spaced, secret, "parse with spaces/tabs");
    }

    expectError("112d-rXXU-ifSr-RnXL-GbXg-8Egt", PinParseError::checksum, "last char changed");
    expectError("112D-rXXU-ifSr-RnXL-GbXg-8Egs", PinParseError::checksum, "case flip");
    expectError("112d-rXXU-ifSr-RnXL-GbXh-8Egs", PinParseError::checksum, "body char changed");
    expectError("YcVf-xkQb-6JRz-qk5k-F2tN-LwUM", PinParseError::malformed, "body is 2^128");
    expectError("1111-1111-1111-1111-1111-11xx", PinParseError::malformed, "all-zero secret");
    expectError("112d-rXXU-ifSr-RnXL-GbXg-8Eg0", PinParseError::malformed, "zero char");
    expectError("112d-rXXU-ifSr-RnXL-GbXg-8Eg", PinParseError::malformed, "23 chars");
    expectError("112d-rXXU-ifSr-RnXL-GbXg-8Egss", PinParseError::malformed, "25 chars");

    for (const char* bad : {"O12d-rXXU-ifSr-RnXL-GbXg-8Egs", "I12d-rXXU-ifSr-RnXL-GbXg-8Egs",
                            "l12d-rXXU-ifSr-RnXL-GbXg-8Egs"}) {
        expectError(bad, PinParseError::malformed, "look-alike");
    }

    auto legacy = parseRecoveryPinText("15753985819510608748");
    expect(legacy && legacy->kind == SecurePin::Kind::legacy_decimal &&
           legacy->legacy_value == 15753985819510608748ULL, "legacy decimal");
    auto max = parseRecoveryPinText("18446744073709551615");
    expect(max && max->kind == SecurePin::Kind::legacy_decimal &&
           max->legacy_value == UINT64_MAX, "legacy max");
    auto dashed = parseRecoveryPinText("1575-3985");
    expect(dashed && dashed->legacy_value == 15753985ULL, "legacy with dash");
    for (const char* bad : {"18446744073709551616", "0", "0123", "", "12O4", "99999999999999999999"}) {
        expectError(bad, PinParseError::malformed, bad);
    }

    SecurePin a = SecurePin::modern(fromHex(VECTORS[1].hex));
    SecurePin b = std::move(a);
    expect(a.kind == SecurePin::Kind::none && a.secret == Pin128Bytes{} && b.kind == SecurePin::Kind::base58_128,
           "move wipes source");

    if (failures != 0) return 1;
    std::puts("pin codec vectors pass");
    return 0;
}
