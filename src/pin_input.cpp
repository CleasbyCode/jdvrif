#include "pin_input.h"

#include <charconv>
#include <cstdio>
#include <print>
#include <string>
#include <string_view>
#include <system_error>

std::size_t getPin() {
    constexpr auto MAX_UINT64_STR = std::string_view{"18446744073709551615"};
    constexpr std::size_t MAX_PIN_LENGTH = 20;

    std::print("\nPIN: ");
    std::fflush(stdout);

    std::string input;
    char ch;

    SyncGuard sync_guard;

    #ifdef _WIN32
        while (input.length() < MAX_PIN_LENGTH) {
            ch = _getch();
            if (ch >= '0' && ch <= '9') {
                input.push_back(ch);
                std::print("*");
                std::fflush(stdout);
            } else if (ch == '\b' && !input.empty()) {
                std::print("\b \b");
                std::fflush(stdout);
                input.pop_back();
            } else if (ch == '\r') {
                break;
            }
        }
    #else
        TermiosGuard termios_guard;
        while (input.length() < MAX_PIN_LENGTH) {
            ssize_t bytes_read = read(STDIN_FILENO, &ch, 1);
            if (bytes_read <= 0) continue;
            if (ch >= '0' && ch <= '9') {
                input.push_back(ch);
                std::print("*");
                std::fflush(stdout);
            } else if ((ch == '\b' || ch == 127) && !input.empty()) {
                std::print("\b \b");
                std::fflush(stdout);
                input.pop_back();
            } else if (ch == '\n') {
                break;
            }
        }
    #endif

    std::println("");
    std::fflush(stdout);

    if (input.empty() || (input.length() == MAX_PIN_LENGTH && input > MAX_UINT64_STR)) {
        return 0;
    }

    std::size_t result = 0;
    auto [ptr, ec] = std::from_chars(input.data(), input.data() + input.size(), result);
    if (ec != std::errc{}) return 0;

    return result;
}
