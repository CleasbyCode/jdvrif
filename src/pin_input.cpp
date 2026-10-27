#include "pin_input.h"
#include "common.h"
#include "pin_codec.h"
#include "signal_utils.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <format>
#include <print>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <sys/select.h>
#include <system_error>
#include <termios.h>
#include <unistd.h>

namespace {
// Deliberately says nothing about which rule the input broke: the shape of a
// recovery KEY/PIN is public, but how far a given attempt got is not worth
// echoing. Each prompt describes only the forms its image can take.
[[nodiscard]] const char* formatError(PinPrompt prompt) noexcept {
    switch (prompt) {
        case PinPrompt::key:
            return "KEY Entry Error: That is not a well-formed recovery KEY.\n\n"
                   "A jdvrif KEY is 24 letters and digits, printed in groups of four "
                   "(XXXX-XXXX-XXXX-XXXX-XXXX-XXXX).\n\n"
                   "Enter it exactly as it was printed when the file was concealed.";
        case PinPrompt::pin:
            return "PIN Entry Error: That is not a well-formed recovery PIN.\n\n"
                   "This image was made before jdvrif v10 and uses a PIN of up to 20 digits.\n\n"
                   "Enter it exactly as it was printed when the file was concealed.";
        case PinPrompt::key_or_pin:
            break;
    }
    return "KEY/PIN Entry Error: That is not a well-formed recovery KEY/PIN.\n\n"
           "A jdvrif KEY is 24 letters and digits, printed in groups of four "
           "(XXXX-XXXX-XXXX-XXXX-XXXX-XXXX);\n"
           "Images made before jdvrif v10 use a PIN of up to 20 digits.\n\n"
           "Enter it exactly as it was printed when the file was concealed.";
}

// A 24-character KEY whose check pair does not match its body: a typo the
// user can find, reported before any key derivation. An image made before v10
// takes no KEY at all, so there the entry is simply not a well-formed PIN.
[[nodiscard]] const char* checkError(PinPrompt prompt) noexcept {
    switch (prompt) {
        case PinPrompt::key:
            return "KEY Entry Error: That KEY has a typo: its last two characters do not "
                   "match the rest. Check each character, including upper and lower case.";
        case PinPrompt::pin:
            return formatError(prompt);
        case PinPrompt::key_or_pin:
            break;
    }
    return "KEY/PIN Entry Error: That KEY has a typo: its last two characters do not "
           "match the rest. Check each character, including upper and lower case.";
}

[[noreturn]] void throwTerminalModeError(const char* word) {
    throwError(std::format(
        "Terminal Error: Unable to disable terminal echo for {0} entry. "
        "Refusing to read the recovery {0} in the clear.", word));
}

volatile std::sig_atomic_t continue_received = 0;

extern "C" void noteTerminalContinue(int) noexcept {
    continue_received = 1;
}

struct TermiosGuard {
    termios old{};
    termios raw{};
    struct sigaction old_continue_action{};
    const char* word;
    bool active{false};
    bool continue_hooked{false};

    TermiosGuard(const TermiosGuard&) = delete;
    TermiosGuard& operator=(const TermiosGuard&) = delete;
    TermiosGuard(TermiosGuard&&) = delete;
    TermiosGuard& operator=(TermiosGuard&&) = delete;

    explicit TermiosGuard(const char* prompt_word) : word(prompt_word) {
        // Piped/redirected stdin has no echo to suppress and no terminal state
        // to restore, so there is nothing to do -- and nothing to fail closed on.
        if (!isatty(STDIN_FILENO)) return;

        // From here stdin *is* a terminal, so a failure to establish raw mode
        // would leave ECHO on and print the PIN into the scrollback. Refuse
        // rather than silently reading the PIN in the clear.
        if (tcgetattr(STDIN_FILENO, &old) != 0) throwTerminalModeError(word);
        raw = old;
        const auto mask = static_cast<tcflag_t>(ICANON | ECHO);
        raw.c_lflag &= static_cast<tcflag_t>(~mask);
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) throwTerminalModeError(word);
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
            if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) throwTerminalModeError(word);
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

    explicit PinSignalBlock(const char* word) {
        sigset_t blocked;
        sigemptyset(&blocked);
        for (const int signal_number : {SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGCONT, SIGTSTP}) {
            sigaddset(&blocked, signal_number);
        }
        if (sigprocmask(SIG_BLOCK, &blocked, &previous_mask) != 0) {
            throwError(std::format("Signal Error: Failed to protect {} input wait.", word));
        }
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

    explicit NonblockingStdinGuard(const char* word) : previous_flags(fcntl(STDIN_FILENO, F_GETFL)) {
        if (previous_flags < 0) {
            throwError(std::format("Read Error: Failed to inspect {} input mode.", word));
        }
        if ((previous_flags & O_NONBLOCK) == 0) {
            if (fcntl(STDIN_FILENO, F_SETFL, previous_flags | O_NONBLOCK) != 0) {
                throwError(std::format("Read Error: Failed to prepare {} input mode.", word));
            }
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
    const char* word = termios_guard.word;
    PinSignalBlock signal_block(word);
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
            throwError(std::format("Read Error: Failed to wait for {} input.", word));
        }
        if (ready == 0) continue;

        // Readiness can become stale, including when a resume flushes input.
        // Never block in read while cancellation signals are masked. Only
        // change fd flags around read, restoring them before unmasking or
        // waiting again so a suspended job leaves stdin usable by its shell.
        NonblockingStdinGuard nonblocking_guard(word);
        const ssize_t bytes_read = read(STDIN_FILENO, &ch, 1);
        if (bytes_read < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            throwError(std::format("Read Error: Failed to read {} input.", word));
        }
        return bytes_read;
    }
}
} // namespace

SecurePin getPin(PinPrompt prompt) {
    // 24 characters plus 5 group dashes, with room to spare (D3).
    constexpr std::size_t MAX_PIN_KEYSTROKES = 32;

    throwIfSignalCancellationRequested();

    std::print("\n{}: ", pinPromptWord(prompt));
    std::fflush(stdout);

    // A fixed stack buffer, never a heap string: nothing here can leave a copy
    // of the PIN behind in a freed allocation. Wiped on every exit path.
    std::array<char, MAX_PIN_KEYSTROKES> input{};
    std::size_t input_len = 0;
    // Keystrokes typed past MAX_PIN_KEYSTROKES are not stored, but they are
    // counted, so that a backspace undoes the keystroke the user actually made
    // last. Without this the buffer silently stops matching what was typed:
    // backspacing back under the limit would hand out a PIN built from the
    // first characters entered.
    std::size_t dropped_keys = 0;
    char ch{};
    const bool is_tty = (isatty(STDIN_FILENO) != 0);

    WipePodGuard<std::array<char, MAX_PIN_KEYSTROKES>> wipe_input{input};
    WipePodGuard<std::size_t> wipe_input_len{input_len};
    WipePodGuard<char> wipe_ch{ch};

    TermiosGuard termios_guard(pinPromptWord(prompt));
    while (true) {
        const ssize_t bytes_read = readPinByte(ch, termios_guard);
        // readPinByte restores the signal mask before returning, delivering
        // any cancellation that arrived after its wait reported readiness.
        throwIfSignalCancellationRequested();
        if (bytes_read == 0) break;
        if (ch == '\n' || ch == '\r') break;
        // Spaces and tabs are ignored, so a PIN pasted with stray whitespace
        // still works (the web variant's PIN file is trimmed the same way).
        if (ch == ' ' || ch == '\t') continue;
        if (ch == '\b' || ch == 127) {
            if (dropped_keys > 0) {
                // Undo an over-limit keystroke. Nothing was echoed for it, so
                // nothing is erased from the display either.
                --dropped_keys;
            } else if (input_len > 0) {
                // One display cell per stored keystroke, dash or not.
                if (is_tty) {
                    std::print("\b \b");
                    std::fflush(stdout);
                }
                input[--input_len] = 0;
            }
            continue;
        }
        // Every other keystroke is kept -- including the group dash, so that
        // "XXXX-" then one backspace removes the dash the user typed, not the
        // character before it -- and so that a stray character rejects the PIN
        // below instead of being silently dropped: "12O4" is a mistyped PIN.
        if (input_len >= input.size()) {
            ++dropped_keys;   // counted, not stored, and not echoed
            continue;
        }
        input[input_len++] = ch;
        if (is_tty) {
            // Dashes are layout, not secret, and echoing them lets the user
            // keep their place in the groups.
            if (ch == '-') {
                std::print("-");
            } else {
                std::print("*");
            }
            std::fflush(stdout);
        }
    }

    std::println("");
    std::fflush(stdout);

    // Reject malformed input here instead of silently truncating or
    // normalizing it: such input is a transcription error and must not derive
    // a key the user believes is valid. dropped_keys is non-zero only if
    // keystrokes past the limit are still outstanding -- corrected ones have
    // already been backspaced away above.
    //
    // Knowable mistakes are reported now rather than after an Argon2id run
    // and, on the ICC path, staging up to two gigabytes of ciphertext, only to
    // end in the ambiguous "invalid PIN or file is corrupt". The codec owns the
    // grammar (legacy decimal or 24-character Base58 with a check pair).
    // The TermiosGuard and the wipe guards above all unwind on the way out.
    if (dropped_keys > 0) {
        throw std::runtime_error(formatError(prompt));
    }
    auto parsed = parseRecoveryPinText(std::string_view(input.data(), input_len));
    if (!parsed) {
        throw std::runtime_error(
            parsed.error() == PinParseError::checksum ? checkError(prompt) : formatError(prompt));
    }
    return std::move(*parsed);
}
