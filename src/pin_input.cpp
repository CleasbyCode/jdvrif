#include "pin_input.h"
#include "common.h"

#include <cerrno>
#include <charconv>
#include <cstdio>
#include <print>
#include <string>
#include <string_view>
#include <system_error>

std::uint64_t getPin() {
    constexpr auto MAX_UINT64_STR = std::string_view{"18446744073709551615"};
    constexpr std::size_t MAX_PIN_LENGTH = 20;

    std::print("\nPIN: ");
    std::fflush(stdout);

    std::string input;
    input.reserve(MAX_PIN_LENGTH);
    char ch;
    const bool is_tty = (isatty(STDIN_FILENO) != 0);

    TermiosGuard termios_guard;
    while (true) {
        ssize_t bytes_read = read(STDIN_FILENO, &ch, 1);
        if (bytes_read == 0) break;
        if (bytes_read < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ch == '\n' || ch == '\r') break;
        if (input.length() >= MAX_PIN_LENGTH) continue;
        if (ch >= '0' && ch <= '9') {
            input.push_back(ch);
            if (is_tty) { std::print("*"); std::fflush(stdout); }
        } else if ((ch == '\b' || ch == 127) && !input.empty()) {
            if (is_tty) { std::print("\b \b"); std::fflush(stdout); }
            input.pop_back();
        }
    }

    std::println("");
    std::fflush(stdout);

    auto wipe_input = [&]() {
        if (!input.empty()) sodium_memzero(input.data(), input.size());
        input.clear();
    };

    if (input.empty() || (input.length() == MAX_PIN_LENGTH && input > MAX_UINT64_STR)) { wipe_input(); return 0; }

    std::uint64_t result = 0;
    auto [ptr, ec] = std::from_chars(input.data(), input.data() + input.size(), result);
    if (ec != std::errc{} || ptr != input.data() + input.size()) { wipe_input(); return 0; }

    wipe_input();
    return result;
}
