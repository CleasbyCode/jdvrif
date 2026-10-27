// base64.cpp is a thin wrapper over libsodium's padded codec. These pin the
// behaviour the Bluesky XMP segment depends on, including the strictness that
// libsodium alone does not supply (1.0.18 decodes every byte >= 0x80 as '/').
#include "base64.h"

#include <sodium.h>

#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace {
int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

vBytes bytes(std::string_view text) { return vBytes(text.begin(), text.end()); }

bool decodes(std::string_view encoded, vBytes& out) {
    try {
        appendBase64AsBinary(bytes(encoded), out);
        return true;
    } catch (const std::invalid_argument&) {
        return false;
    }
}

void expectRejected(std::string_view encoded, const char* what) {
    vBytes out = bytes("keep");
    expect(!decodes(encoded, out), what);
    expect(out == bytes("keep"), "a rejected decode must leave the destination untouched");
}
} // namespace

int main() {
    if (sodium_init() < 0) return 1;

    // RFC 4648 section 10 vectors, appended after existing content.
    constexpr std::string_view plain[] = {"f", "fo", "foo", "foob", "fooba", "foobar"};
    constexpr std::string_view coded[] = {"Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (std::size_t i = 0; i < std::size(plain); ++i) {
        vBytes encoded = bytes(">");
        binaryToBase64(bytes(plain[i]), encoded);
        expect(encoded == bytes(std::string(">") + std::string(coded[i])), "encode matches RFC 4648");
        vBytes decoded = bytes("<");
        expect(decodes(coded[i], decoded) && decoded == bytes(std::string("<") + std::string(plain[i])),
               "decode matches RFC 4648");
    }
    vBytes empty_out = bytes("x");
    binaryToBase64({}, empty_out);
    expect(empty_out == bytes("x"), "encoding nothing appends nothing");

    expectRejected("", "empty input");
    expectRejected("Zm9", "length not a multiple of 4");
    expectRejected("Zm9vY", "length not a multiple of 4");
    expectRejected("Zg=", "short padding");
    expectRejected("Z===", "too much padding");
    expectRejected("Zm=v", "padding inside a quartet");
    expectRejected("Zg==Zm9v", "data after padding");
    expectRejected("Zh==", "non-zero padding bits (1-byte tail)");
    expectRejected("Zm9=", "non-zero padding bits (2-byte tail)");
    expectRejected("Zm 9", "whitespace");
    expectRejected("Zm9v\n", "trailing newline");
    expectRejected("Zm-_", "URL-safe alphabet");
    expectRejected(std::string_view("Zm\0v", 4), "NUL byte");
    for (int c = 0x80; c <= 0xFF; ++c) {
        const char quartet[4] = {static_cast<char>(c), 'A', 'A', 'A'};
        vBytes out;
        if (decodes(std::string_view(quartet, 4), out)) {
            std::fprintf(stderr, "FAIL: high byte 0x%02X accepted\n", c);
            ++failures;
        }
    }

    if (failures != 0) return 1;
    std::puts("base64 vectors, strictness and rollback checks passed");
    return 0;
}
