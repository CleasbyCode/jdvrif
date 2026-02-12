#pragma once

#include "common.h"

#include <iostream>

struct SyncGuard {
    bool old_status;
    SyncGuard() : old_status(std::cout.sync_with_stdio(false)) {}
    ~SyncGuard() { std::cout.sync_with_stdio(old_status); }
};

#ifndef _WIN32
    struct TermiosGuard {
        termios old;
        TermiosGuard() {
            tcgetattr(STDIN_FILENO, &old);
            termios newt = old;
            newt.c_lflag &= ~(ICANON | ECHO);
            tcsetattr(STDIN_FILENO, TCSANOW, &newt);
        }
        ~TermiosGuard() { tcsetattr(STDIN_FILENO, TCSANOW, &old); }
    };
#endif

[[nodiscard]] std::size_t getPin();
