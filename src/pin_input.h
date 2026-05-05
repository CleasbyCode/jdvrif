#pragma once

#include <cstddef>
#include <cstdint>
#include <termios.h>
#include <unistd.h>

struct TermiosGuard {
    termios old{}; bool active{false};
    TermiosGuard(const TermiosGuard&) = delete; TermiosGuard& operator=(const TermiosGuard&) = delete;
    TermiosGuard(TermiosGuard&&) = delete; TermiosGuard& operator=(TermiosGuard&&) = delete;

    TermiosGuard() {
        if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &old) != 0) return;
        termios newt = old;
        const auto mask = static_cast<tcflag_t>(ICANON | ECHO);
        newt.c_lflag &= static_cast<tcflag_t>(~mask);
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &newt) == 0) active = true;
    }
    ~TermiosGuard() { if (active) tcsetattr(STDIN_FILENO, TCSAFLUSH, &old); }
};
[[nodiscard]] std::uint64_t getPin();
