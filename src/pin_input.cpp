#include "pin_input.h"
#include "common.h"
#include "signal_utils.h"

#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/select.h>
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
            throwIf(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0, TERMINAL_MODE_ERROR);
        }
    }
};

// Keep cancellation and job-control signals blocked between checking their
// flags and reading a ready byte. pselect atomically restores the caller's
// mask while waiting, so a signal arriving just before the wait interrupts it.
// Deferring SIGTSTP until that wait also lets SIGCONT restore raw mode before
// the next read if the shell changed terminal settings while we were stopped.
struct PinSignalBlock {
    sigset_t previous_mask{};

    PinSignalBlock() {
        sigset_t blocked;
        sigemptyset(&blocked);
        for (const int signal_number : {SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGCONT, SIGTSTP}) {
            sigaddset(&blocked, signal_number);
        }
        throwIf(sigprocmask(SIG_BLOCK, &blocked, &previous_mask) != 0,
                "Signal Error: Failed to protect PIN input wait.");
    }

    ~PinSignalBlock() {
        (void)sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
    }

    PinSignalBlock(const PinSignalBlock&) = delete;
    PinSignalBlock& operator=(const PinSignalBlock&) = delete;
};

struct NonblockingStdinGuard {
    int previous_flags;
    bool changed{false};

    NonblockingStdinGuard() : previous_flags(fcntl(STDIN_FILENO, F_GETFL)) {
        throwIf(previous_flags < 0, "Read Error: Failed to inspect PIN input mode.");
        if ((previous_flags & O_NONBLOCK) == 0) {
            throwIf(fcntl(STDIN_FILENO, F_SETFL, previous_flags | O_NONBLOCK) != 0,
                    "Read Error: Failed to prepare PIN input mode.");
            changed = true;
        }
    }

    ~NonblockingStdinGuard() {
        if (changed) (void)fcntl(STDIN_FILENO, F_SETFL, previous_flags);
    }

    NonblockingStdinGuard(const NonblockingStdinGuard&) = delete;
    NonblockingStdinGuard& operator=(const NonblockingStdinGuard&) = delete;
};

[[nodiscard]] ssize_t readPinByte(char& ch, TermiosGuard& termios_guard) {
    PinSignalBlock signal_block;
    while (true) {
        throwIfSignalCancellationRequested();
        termios_guard.reapplyAfterContinue();

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(STDIN_FILENO, &readable);
        // The timeout also bounds cancellation latency if a process-directed
        // signal happens to be handled on another thread.
        constexpr timespec WAIT_INTERVAL{0, 100'000'000};
        const int ready = pselect(STDIN_FILENO + 1, &readable, nullptr, nullptr,
                                  &WAIT_INTERVAL, &signal_block.previous_mask);
        const int wait_errno = errno;
        throwIfSignalCancellationRequested();
        termios_guard.reapplyAfterContinue();
        if (ready < 0) {
            if (wait_errno == EINTR) continue;
            throwError("Read Error: Failed to wait for PIN input.");
        }
        if (ready == 0) continue;

        // Readiness can become stale, including when a resume flushes input.
        // Never block in read while cancellation signals are masked. Only
        // change fd flags around read, restoring them before unmasking or
        // waiting again so a suspended job leaves stdin usable by its shell.
        NonblockingStdinGuard nonblocking_guard;
        const ssize_t bytes_read = read(STDIN_FILENO, &ch, 1);
        if (bytes_read < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            throwError("Read Error: Failed to read PIN input.");
        }
        return bytes_read;
    }
}
} // namespace

SecurePin getPin() {
    constexpr auto MAX_UINT64_STR = std::string_view{"18446744073709551615"};
    constexpr std::size_t MAX_PIN_LENGTH = 20;

    throwIfSignalCancellationRequested();

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
        const ssize_t bytes_read = readPinByte(ch, termios_guard);
        // readPinByte restores the signal mask before returning, delivering
        // any cancellation that arrived after its wait reported readiness.
        throwIfSignalCancellationRequested();
        if (bytes_read == 0) break;
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
