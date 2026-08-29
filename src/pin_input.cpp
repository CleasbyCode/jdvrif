#include "pin_input.h"
#include "common.h"
#include "signal_utils.h"

#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <termios.h>
#include <unistd.h>

namespace {
// Deliberately says nothing about which rule the input broke: the shape of a
// recovery PIN is public, but how far a given attempt got is not worth echoing.
constexpr const char* PIN_FORMAT_ERROR =
    "PIN Entry Error: That is not a well-formed recovery PIN. "
    "A jdvrif PIN is a number of up to 20 digits, with no leading zero, "
    "exactly as it was printed when the file was concealed.";

constexpr const char* TERMINAL_MODE_ERROR =
    "Terminal Error: Unable to disable terminal echo for PIN entry. "
    "Refusing to read the recovery PIN in the clear.";

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
        // Piped/redirected stdin has no echo to suppress and no terminal state
        // to restore, so there is nothing to do -- and nothing to fail closed on.
        if (!isatty(STDIN_FILENO)) return;

        // From here stdin *is* a terminal, so a failure to establish raw mode
        // would leave ECHO on and print the PIN into the scrollback. Refuse
        // rather than silently reading the PIN in the clear.
        throwIf(tcgetattr(STDIN_FILENO, &old) != 0, TERMINAL_MODE_ERROR);
        raw = old;
        const auto mask = static_cast<tcflag_t>(ICANON | ECHO);
        raw.c_lflag &= static_cast<tcflag_t>(~mask);
        throwIf(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0, TERMINAL_MODE_ERROR);
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
    // Digits typed past MAX_PIN_LENGTH are not stored, but they are counted, so
    // that a backspace undoes the keystroke the user actually made last. Without
    // this the buffer silently stops matching what was typed: backspacing back
    // under the limit would hand out a PIN built from the first digits entered.
    std::size_t dropped_digits = 0;
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
                ++dropped_digits;   // counted, not stored, and not echoed
                continue;
            }
            input.push_back(ch);
            if (is_tty) {
                std::print("*");
                std::fflush(stdout);
            }
        } else if (ch == '\b' || ch == 127) {
            if (dropped_digits > 0) {
                // Undo an over-limit digit. Nothing was echoed for it, so
                // nothing is erased from the display either.
                --dropped_digits;
            } else if (!input.empty()) {
                if (is_tty) {
                    std::print("\b \b");
                    std::fflush(stdout);
                }
                input.pop_back();
            }
        }
    }

    std::println("");
    std::fflush(stdout);

    // Reject overlong and leading-zero input instead of silently truncating or
    // normalizing it: generated PINs never look like that, so such input is a
    // transcription error and must not derive a key the user believes is valid.
    // dropped_digits is non-zero only if digits past the limit are still
    // outstanding -- corrected ones have already been backspaced away above.
    //
    // Throw rather than hand back a zero PIN. A zero PIN is one generateRecoveryPin
    // never mints, so it could only ever fail -- but not before paying for an
    // Argon2id derivation and, on the ICC path, staging up to two gigabytes of
    // ciphertext, and then reporting the ambiguous "invalid PIN or file is
    // corrupt". Malformed input is knowable here, so say so here.
    //
    // Any leading '0' covers both cases at once: a minted PIN is a non-zero
    // uint64 in decimal, so it never starts with '0'. Testing the first digit
    // alone (rather than only multi-digit input) is what rejects a lone "0",
    // which is exactly the zero PIN this check exists to refuse.
    // The TermiosGuard and the wipe guards above all unwind on the way out.
    if (input.empty() || dropped_digits > 0
        || (input.length() == MAX_PIN_LENGTH && input > MAX_UINT64_STR)
        || input.front() == '0') {
        throw std::runtime_error(PIN_FORMAT_ERROR);
    }

    SecurePin result;
    auto [ptr, ec] = std::from_chars(input.data(), input.data() + input.size(), result.value);
    if (ec != std::errc{} || ptr != input.data() + input.size()) {
        result.wipe();
        throw std::runtime_error(PIN_FORMAT_ERROR);
    }

    return result;
}
