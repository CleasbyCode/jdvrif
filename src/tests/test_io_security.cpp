#include "file_utils.h"
#include "pin_input.h"
#include "signal_utils.h"

#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <string_view>
#include <sys/select.h>
#include <unistd.h>

namespace {
enum class Injection { none, before_wait, after_ready, drain_ready };
Injection injection = Injection::none;
}

extern "C" int __real_pselect(int, fd_set*, fd_set*, fd_set*,
                              const timespec*, const sigset_t*);

// Deterministic signal delivery at the race windows that ordinary PTY tests
// rarely hit. The drain case leaves an open, empty pipe after readiness.
extern "C" int __wrap_pselect(int count, fd_set* read_fds, fd_set* write_fds,
                              fd_set* except_fds, const timespec* timeout,
                              const sigset_t* mask) {
    if (injection == Injection::before_wait) {
        injection = Injection::none;
        std::raise(SIGINT);
    }
    const int result = __real_pselect(count, read_fds, write_fds, except_fds, timeout, mask);
    if (result > 0 && injection != Injection::none) {
        if (injection == Injection::drain_ready) {
            char discarded;
            if (read(STDIN_FILENO, &discarded, 1) != 1) return -1;
        }
        injection = Injection::none;
        std::raise(SIGINT);
    }
    return result;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string_view mode = argv[1];
    if (mode == "read") {
        if (argc != 4) return 2;
        try {
            const auto bytes = readFile(argv[2], FileTypeCheck::embedded_image, std::stoull(argv[3]));
            std::printf("read %zu bytes\n", bytes.size());
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }

    installProcessSignalHandlers();
    if (mode == "pending") std::raise(SIGINT);
    else if (mode == "wait-race") injection = Injection::before_wait;
    else if (mode == "ready-race") injection = Injection::after_ready;
    else if (mode == "drained-race") injection = Injection::drain_ready;
    else if (mode != "pin") return 2;

    const int original_flags = fcntl(STDIN_FILENO, F_GETFL);
    try {
        const SecurePin pin = getPin();
        if (mode != "pin" || fcntl(STDIN_FILENO, F_GETFL) != original_flags) return 1;
        std::printf("parsed %llu\n", static_cast<unsigned long long>(pin.value));
        return 0;
    } catch (const SignalCancellation& cancellation) {
        if (cancellation.signalNumber() != SIGINT ||
            fcntl(STDIN_FILENO, F_GETFL) != original_flags) return 1;
        std::puts("cancelled SIGINT; input flags restored");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
