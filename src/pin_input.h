#pragma once

#include <cstddef>
#include <termios.h>
#include <unistd.h>

struct TermiosGuard {
    termios old{};
    bool active{false};

    TermiosGuard(const TermiosGuard&) = delete;
    TermiosGuard& operator=(const TermiosGuard&) = delete;
    TermiosGuard(TermiosGuard&&) = delete;
    TermiosGuard& operator=(TermiosGuard&&) = delete;

    TermiosGuard() {
        if (!isatty(STDIN_FILENO)) {
            return;
        }
        if (tcgetattr(STDIN_FILENO, &old) != 0) {
            return;
        }
        termios newt = old;
        const auto mask = static_cast<tcflag_t>(ICANON | ECHO);
        newt.c_lflag &= static_cast<tcflag_t>(~mask);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &newt) == 0) {
            active = true;
        }
    }
    ~TermiosGuard() {
        if (active) {
            tcsetattr(STDIN_FILENO, TCSANOW, &old);
        }
    }
};
[[nodiscard]] std::size_t getPin();
