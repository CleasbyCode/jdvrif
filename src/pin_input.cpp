#include "pin_input.h"
#include "common.h"
#include "signal_utils.h"

#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <termios.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t continue_received = 0;

extern "C" void noteTerminalContinue(int) noexcept {
    continue_received = 1;
}

struct TermiosGuard {
    termios old{};
    termios raw{};
    struct sigaction old_continue_action{};
    bool active{false};
    bool continue_hooked{false};

    TermiosGuard(const TermiosGuard&) = delete;
    TermiosGuard& operator=(const TermiosGuard&) = delete;
    TermiosGuard(TermiosGuard&&) = delete;
    TermiosGuard& operator=(TermiosGuard&&) = delete;

    TermiosGuard() {
        if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &old) != 0) return;
        raw = old;
        const auto mask = static_cast<tcflag_t>(ICANON | ECHO);
        raw.c_lflag &= static_cast<tcflag_t>(~mask);
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return;
        active = true;

        // Shells are not obliged to preserve our raw modes across
        // SIGTSTP/SIGCONT (bash restores cooked mode, which would echo the
        // PIN), so flag the continue and let the read loop re-apply them.
        struct sigaction action {};
        action.sa_handler = noteTerminalContinue;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0; // interrupt read(2) so the loop notices the resume
        continue_hooked = (sigaction(SIGCONT, &action, &old_continue_action) == 0);
    }

    ~TermiosGuard() {
        if (continue_hooked) {
            sigaction(SIGCONT, &old_continue_action, nullptr);
        }
        if (active) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        }
    }

    void reapplyAfterContinue() {
        if (active && continue_received) {
            continue_received = 0;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        }
    }
};
} // namespace

SecurePin getPin() {
    constexpr auto MAX_UINT64_STR = std::string_view{"18446744073709551615"};
    constexpr std::size_t MAX_PIN_LENGTH = 20;

    std::print("\nPIN: ");
    std::fflush(stdout);

    std::string input;
    input.reserve(MAX_PIN_LENGTH);
    bool overlong = false;
    char ch{};
    const bool is_tty = (isatty(STDIN_FILENO) != 0);

    WipeStringGuard wipe_input{input};
    WipePodGuard<char> wipe_ch{ch};

    TermiosGuard termios_guard;
    while (true) {
        // Checked each iteration, not just on EINTR: a stop can land between
        // syscalls, in which case read(2) is re-entered without interruption.
        termios_guard.reapplyAfterContinue();
        ssize_t bytes_read = read(STDIN_FILENO, &ch, 1);
        if (bytes_read == 0) break;
        if (bytes_read < 0) {
            if (errno == EINTR) {
                throwIfSignalCancellationRequested();
                continue;
            }
            break;
        }
        if (ch == '\n' || ch == '\r') break;
        if (ch >= '0' && ch <= '9') {
            if (input.length() >= MAX_PIN_LENGTH) {
                overlong = true;
                continue;
            }
            input.push_back(ch);
            if (is_tty) {
                std::print("*");
                std::fflush(stdout);
            }
        } else if ((ch == '\b' || ch == 127) && !input.empty()) {
            if (is_tty) {
                std::print("\b \b");
                std::fflush(stdout);
            }
            input.pop_back();
            if (input.length() < MAX_PIN_LENGTH) overlong = false;
        }
    }

    std::println("");
    std::fflush(stdout);

    // Reject overlong and leading-zero input instead of silently truncating or
    // normalizing it: generated PINs never look like that, so such input is a
    // transcription error and must not derive a key the user believes is valid.
    if (input.empty() || overlong
        || (input.length() == MAX_PIN_LENGTH && input > MAX_UINT64_STR)
        || (input.length() > 1 && input.front() == '0')) {
        return SecurePin{};
    }

    SecurePin result;
    auto [ptr, ec] = std::from_chars(input.data(), input.data() + input.size(), result.value);
    if (ec != std::errc{} || ptr != input.data() + input.size()) {
        result.wipe();
        return SecurePin{};
    }

    return result;
}
