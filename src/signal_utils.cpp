#include "signal_utils.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <stdexcept>

namespace {
volatile std::sig_atomic_t pending_signal = 0;

extern "C" void requestCancellation(int signal_number) noexcept {
    if (pending_signal == 0) pending_signal = signal_number;
}

void installHandler(int signal_number, void (*handler)(int)) {
    struct sigaction action {};
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0; // deliberately interrupt blocking syscalls
    // No common.h here (signal_utils stays free of the sodium/filesystem
    // headers everything else pulls in), so throw directly rather than throwIf.
    if (sigaction(signal_number, &action, nullptr) != 0) {
        throw std::runtime_error("Signal Error: Failed to install process signal handler.");
    }
}
} // namespace

void installProcessSignalHandlers() {
    installHandler(SIGPIPE, SIG_IGN);
    // SIGTSTP is deliberately absent: it keeps its default disposition so
    // Ctrl+Z performs a genuine stop. Job-control shells save/restore termios
    // around stopped jobs, and blocked reads resume on SIGCONT.
    for (const int signal_number : std::to_array({SIGHUP, SIGINT, SIGQUIT, SIGTERM})) {
        installHandler(signal_number, requestCancellation);
    }
}

void throwIfSignalCancellationRequested() {
    const int signal_number = pending_signal;
    if (signal_number != 0) {
        throw SignalCancellation(signal_number);
    }
}

[[noreturn]] void reraiseSignalAfterCleanup(int signal_number) noexcept {
    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    (void)sigaction(signal_number, &action, nullptr);
    (void)std::raise(signal_number);
    std::_Exit(128 + signal_number);
}
