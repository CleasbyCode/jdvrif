#include "common.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {
[[noreturn]] void fail(const char* msg) {
    std::fprintf(stderr, "%s\n", msg);
    std::exit(1);
}
} // namespace

int main() {
    std::string input;
    input.reserve(20);
    input = "12345";
    input.push_back('6');
    input.pop_back();

    try {
        WipeStringGuard guard(input);
        throw std::runtime_error("cancel");
    } catch (const std::runtime_error&) {
    }

    if (!input.empty()) {
        fail("WipeStringGuard left PIN digits in the string after unwind");
    }

    char ch = '7';
    try {
        WipePodGuard<char> guard(ch);
        throw std::runtime_error("cancel");
    } catch (const std::runtime_error&) {
    }

    if (ch != '\0') {
        fail("WipePodGuard left the last PIN byte after unwind");
    }

    return 0;
}
