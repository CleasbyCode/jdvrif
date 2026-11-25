#include "signal_utils.h"

#include <array>
#include <atomic>
#include <csignal>
#include <pthread.h>
#include <cstdlib>
#include <stdexcept>

namespace {
// Signal handlers can run on any worker thread. Lock-free atomics are safe
// both in the handler and across threads; volatile sig_atomic_t is not.
static_assert(std::atomic<int>::is_always_lock_free,
              "Signal cancellation requires lock-free atomic<int>.");
constinit std::atomic<int> pending_signal{0};

extern "C" void requestCancellation(int signal_number) noexcept {
    // Keep the first signal even when handlers execute concurrently. The flag
    // publishes no other data, so relaxed ordering is sufficient.
    int expected = 0;
    (void)pending_signal.compare_exchange_strong(
        expected, signal_number, std::memory_order_relaxed, std::memory_order_relaxed);
}

void installHandler(int signal_number, void (*handler)(int)) {
    // A signal the parent left ignored stays ignored: `nohup` relies on it for
    // SIGHUP, and a non-interactive shell's `cmd &` for SIGINT and SIGQUIT.
    struct sigaction inherited {};
    if (sigaction(signal_number, nullptr, &inherited) == 0 && inherited.sa_handler == SIG_IGN) {
        return;
    }
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

int pendingSignalCancellation() noexcept {
    return pending_signal.load(std::memory_order_relaxed);
}

void throwIfSignalCancellationRequested() {
    const int signal_number = pendingSignalCancellation();
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
    // A blocked signal would only be left pending by raise, and the process
    // would leave through _Exit with an ordinary exit status rather than be
    // ended by the signal its parent should see.
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, signal_number);
    (void)pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
    (void)std::raise(signal_number);
    std::_Exit(128 + signal_number);
}
