#include "key_input.h"
#include "common.h"
#include "key_codec.h"
#include "signal_utils.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <pthread.h>
#include <cstdio>
#include <fcntl.h>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace {
// Deliberately says nothing about which rule the input broke: the shape of a
// recovery KEY is public, but how far a given attempt got is not worth echoing.
constexpr const char* FORMAT_ERROR =
    "KEY Entry Error: That is not a well-formed recovery KEY.\n\n"
    "A jdvrif KEY is 24 letters and digits, printed in groups of four "
    "(XXXX-XXXX-XXXX-XXXX-XXXX-XXXX).\n\n"
    "Enter it exactly as it was printed when the file was concealed.";

// A KEY whose check pair does not match its body: a typo the user can find,
// reported before any key derivation.
constexpr const char* CHECK_ERROR =
    "KEY Entry Error: That KEY has a typo: its last two characters do not "
    "match the rest. Check each character, including upper and lower case.";

[[noreturn]] void throwTerminalModeError() {
    throwError(
        "Terminal Error: Unable to disable terminal echo for KEY entry. "
        "Refusing to read the recovery KEY in the clear.");
}

volatile std::sig_atomic_t continue_received = 0;

// tcsetattr waits for pending output with TCSAFLUSH, and jdvrif's cancellation
// handlers are installed without SA_RESTART, so a signal can interrupt it.
// Retried a few times; returns the last result with errno intact. Entering raw
// mode stops retrying once a cancellation is pending, so that one SIGINT or
// SIGTERM ends a set that is stopped on SIGTTOU or waiting on output held back
// by XOFF. Restoring keeps retrying: it runs while a cancellation unwinds.
enum class OnCancellation { stop, retry };

[[nodiscard]] int setTerminal(const termios& settings, int when, OnCancellation on_cancellation) noexcept {
    constexpr int ATTEMPTS = 8;
    int rc = -1;
    for (int attempt = 0; attempt < ATTEMPTS; ++attempt) {
        rc = tcsetattr(STDIN_FILENO, when, &settings);
        if (rc == 0 || errno != EINTR) break;
        if (on_cancellation == OnCancellation::stop && pendingSignalCancellation() != 0) break;
    }
    return rc;
}

// Whether this process may change the terminal's settings without being
// stopped by SIGTTOU: it is in the foreground process group, or stdin is not
// this session's controlling terminal (no foreground group, no job control).
[[nodiscard]] bool inTerminalForeground() noexcept {
    const pid_t foreground = tcgetpgrp(STDIN_FILENO);
    return foreground < 0 || foreground == getpgrp();
}

// Puts the terminal back as it was found, if this process still owns it.
// Only the foreground process group may change a terminal's settings: a
// background one gets SIGTTOU, whose default action stops it. After Ctrl+Z
// and `kill %1` that would leave the cancelled job stopped instead of
// exiting -- and the shell has already restored its own settings when it took
// the terminal back, so a job that lost the foreground has nothing to undo.
// SIGTTOU is blocked across the check and the set, so losing the foreground
// in between cannot stop the process either. A terminal that is not this
// session's controlling terminal has no foreground group (tcgetpgrp fails)
// and no job control, so it is always restored.
void restoreTerminal(const termios& old) noexcept {
    sigset_t ttou;
    sigemptyset(&ttou);
    sigaddset(&ttou, SIGTTOU);
    sigset_t previous;
    const bool blocked = pthread_sigmask(SIG_BLOCK, &ttou, &previous) == 0;
    if (inTerminalForeground()) {
        // Not TCSAFLUSH: it waits for output to drain, and output held back by
        // XOFF (Ctrl+S) would hang the restore -- the very wait a cancellation
        // may just have broken out of. Set at once, then drop typed-ahead KEY
        // characters (which never waits), so the shell cannot read them.
        (void)setTerminal(old, TCSANOW, OnCancellation::retry);
        (void)tcflush(STDIN_FILENO, TCIFLUSH);
    }
    if (blocked) (void)pthread_sigmask(SIG_SETMASK, &previous, nullptr);
}

// Returns once this process is in the terminal's foreground. In the
// background, tcflush stops it on SIGTTOU -- the usual job-control wait --
// until `fg` resumes it; the input it discards is flushed again on entering
// raw mode anyway. A cancellation during the wait ends it. If SIGTTOU is
// ignored or blocked, or the process group is orphaned, there is no stop to
// wait in: return, and leave the settings to what tcgetattr reports.
// A cancellation already pending ends it first: with `stty tostop`, a job
// started in the background stops on SIGTTOU writing the prompt, and
// `kill %1` resumes it there with SIGTERM pending -- tcflush would only stop
// it again, now with nothing left to deliver.
void waitForTerminalForeground() {
    throwIfSignalCancellationRequested();
    while (!inTerminalForeground()) {
        if (tcflush(STDIN_FILENO, TCIFLUSH) == 0 || errno != EINTR) return;
        throwIfSignalCancellationRequested();
    }
}

extern "C" void noteTerminalContinue(int) noexcept {
    continue_received = 1;
}

// Unblocks, for its lifetime, the cancellation signals the caller's mask
// left deliverable; one already pending is handled as it is unblocked.
class CancellationWindow {
public:
    explicit CancellationWindow(const sigset_t& caller_mask) noexcept {
        sigset_t cancellation;
        sigemptyset(&cancellation);
        for (const int signal_number : {SIGHUP, SIGINT, SIGQUIT, SIGTERM}) {
            if (sigismember(&caller_mask, signal_number) == 0) sigaddset(&cancellation, signal_number);
        }
        opened_ = pthread_sigmask(SIG_UNBLOCK, &cancellation, &saved_) == 0;
    }
    ~CancellationWindow() {
        if (opened_) (void)pthread_sigmask(SIG_SETMASK, &saved_, nullptr);
    }
    CancellationWindow(const CancellationWindow&) = delete;
    CancellationWindow& operator=(const CancellationWindow&) = delete;

private:
    sigset_t saved_{};
    bool opened_{false};
};

struct TermiosGuard {
    termios old{};
    termios raw{};
    struct sigaction old_continue_action{};
    // Stdin is a terminal, in raw mode with the SIGCONT hook: both set
    // together on the constructor's one successful exit for a terminal (a
    // throw undoes both by hand and skips the destructor).
    bool active{false};

    TermiosGuard(const TermiosGuard&) = delete;
    TermiosGuard& operator=(const TermiosGuard&) = delete;
    TermiosGuard(TermiosGuard&&) = delete;
    TermiosGuard& operator=(TermiosGuard&&) = delete;

    TermiosGuard() {
        // Piped/redirected stdin has no echo to suppress and no terminal state
        // to restore, so there is nothing to do -- and nothing to fail closed on.
        if (!isatty(STDIN_FILENO)) return;

        // Shells are not obliged to preserve our raw modes across
        // SIGTSTP/SIGCONT (bash restores cooked mode, which would echo the
        // KEY), so flag the continue and let the read loop re-apply them.
        // Hook SIGCONT *before* entering raw mode: a stop landing between the
        // two would otherwise resume in cooked mode with nothing flagged.
        // From here stdin *is* a terminal, so a failure to establish raw mode
        // would leave ECHO on and print the KEY into the scrollback: refuse
        // rather than silently read the KEY in the clear.
        continue_received = 0;
        struct sigaction action {};
        action.sa_handler = noteTerminalContinue;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0; // interrupt pselect(2) so the loop notices the resume
        if (sigaction(SIGCONT, &action, &old_continue_action) != 0) throwTerminalModeError();

        try {
            // Started in the background (`jdvrif recover img.jpg &`), the
            // terminal holds the shell's line-editor modes (no ICANON, no
            // ECHO), which tcgetattr would happily report: restoring those
            // after `fg` would leave the terminal raw and silent. Wait for
            // the foreground first, then take the settings to restore.
            waitForTerminalForeground();
            if (tcgetattr(STDIN_FILENO, &old) != 0) throwTerminalModeError();
        } catch (...) {
            (void)sigaction(SIGCONT, &old_continue_action, nullptr);
            throw;
        }
        // The wait's own resume is not one the read loop has to answer.
        continue_received = 0;
        raw = old;
        const auto mask = static_cast<tcflag_t>(ICANON | ECHO);
        raw.c_lflag &= static_cast<tcflag_t>(~mask);
        // In canonical mode the kernel ignores VMIN/VTIME, so whatever an
        // earlier program left there only bites once ICANON is cleared:
        // VMIN > 1 keeps pselect from reporting a single keystroke, and
        // VMIN = VTIME = 0 makes a stale-readiness read return 0 (EOF)
        // instead of EAGAIN. One byte, no timer, is what the loop expects.
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;

        // A tcsetattr that "succeeded" may have applied only part of raw, and
        // a throw from a constructor skips the destructor: undo by hand.
        try {
            applyRaw();
        } catch (...) {
            restoreTerminal(old);
            (void)sigaction(SIGCONT, &old_continue_action, nullptr);
            throw;
        }
        active = true;
    }

    ~TermiosGuard() {
        if (!active) return;
        (void)sigaction(SIGCONT, &old_continue_action, nullptr);
        restoreTerminal(old);
    }

    // POSIX tcsetattr reports success if *any* requested change took
    // effect, so read the settings back and refuse unless all of them did.
    // A stop between the set and the read-back lets the shell restore cooked
    // mode, so try again a few times rather than abort an entry the user
    // merely suspended; a terminal that truly refuses keeps refusing. (The
    // SIGCONT flag is not consulted here: its handler may run on another
    // thread after this one has already resumed.)
    void applyRaw() const {
        // A cancellation that arrived since the last check ends entry here,
        // before the terminal is switched: the set itself may block.
        throwIfSignalCancellationRequested();
        constexpr int ATTEMPTS = 4;
        for (int attempt = 0; attempt < ATTEMPTS; ++attempt) {
            if (setTerminal(raw, TCSAFLUSH, OnCancellation::stop) != 0) {
                // Interrupted by a cancellation signal: cancel, as the read
                // loop would. Interrupted throughout otherwise: a fault.
                if (errno == EINTR) throwIfSignalCancellationRequested();
                throwTerminalModeError();
            }
            termios applied{};
            if (tcgetattr(STDIN_FILENO, &applied) != 0) throwTerminalModeError();
            const auto mask = static_cast<tcflag_t>(ICANON | ECHO);
            if ((applied.c_lflag & mask) == 0
                && applied.c_cc[VMIN] == 1 && applied.c_cc[VTIME] == 0) {
                return;
            }
        }
        throwTerminalModeError();
    }

    // Returns true if raw mode was re-applied. The TCSAFLUSH discards any
    // input pselect may have just reported, so the caller must wait again.
    // A job resumed in the background (Ctrl+Z, then `bg`) leaves the resume
    // pending instead: tcsetattr would stop it on SIGTTOU, so the caller
    // waits until `fg` sends another SIGCONT.
    //
    // The read loop keeps cancellation masked outside pselect, but the set
    // runs with it deliverable again (`caller_mask`, the mask from before the
    // loop blocked it): TCSAFLUSH can wait indefinitely on output held back
    // by XOFF (Ctrl+S), and the foreground can still be lost between the
    // check and the set (SIGSTOP, or another process's tcsetpgrp), stopping
    // it on SIGTTOU. Either way one Ctrl+C or `kill %1` must end it. SIGTSTP
    // and SIGCONT stay blocked, so a Ctrl+Z still waits for pselect.
    [[nodiscard]] bool reapplyAfterContinue(const sigset_t& caller_mask) {
        if (!active || !continue_received || !inTerminalForeground()) return false;
        continue_received = 0;
        const CancellationWindow window(caller_mask);
        throwIfSignalCancellationRequested();
        applyRaw();
        return true;
    }

    // Handles the next wait as a resume: raw mode is re-applied first.
    void noteResume() noexcept {
        if (active) continue_received = 1;
    }

    // A resume has not yet been followed by raw mode, or the terminal now
    // belongs to another process group: never read until raw mode is back.
    // The foreground can be lost without a stop (another process's
    // tcsetpgrp; a start in the background with SIGTTOU ignored; SIGSTOP and
    // `bg` while SIGCONT is blocked), and a read from the background stops on
    // SIGTTIN -- with stdin's O_NONBLOCK on the open file description the
    // shell shares, and cancellation masked, so `kill %1` could not end it.
    // Whoever had the terminal meanwhile may have changed its settings, so
    // the return to the foreground is handled as a resume.
    [[nodiscard]] bool awaitingForeground() noexcept {
        if (!active) return false;
        if (!inTerminalForeground()) continue_received = 1;
        return continue_received != 0;
    }
};

// Keep cancellation and job-control signals blocked between checking their
// flags and reading a ready byte. pselect atomically restores the caller's
// mask while waiting, so a signal arriving just before the wait interrupts it.
// Deferring SIGTSTP until that wait also lets SIGCONT restore raw mode before
// the next read if the shell changed terminal settings while we were stopped.
// SIGTTIN is blocked so that a read which loses the foreground after the
// check fails with EIO instead of stopping the job mid-read.
struct KeyEntrySignalBlock {
    sigset_t previous_mask{};

    KeyEntrySignalBlock() {
        sigset_t blocked;
        sigemptyset(&blocked);
        for (const int signal_number : {SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGCONT, SIGTSTP, SIGTTIN}) {
            sigaddset(&blocked, signal_number);
        }
        if (sigprocmask(SIG_BLOCK, &blocked, &previous_mask) != 0) {
            throwError("Signal Error: Failed to protect KEY input wait.");
        }
    }

    ~KeyEntrySignalBlock() {
        (void)sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
    }

    KeyEntrySignalBlock(const KeyEntrySignalBlock&) = delete;
    KeyEntrySignalBlock& operator=(const KeyEntrySignalBlock&) = delete;
};

struct NonblockingStdinGuard {
    int previous_flags;
    bool changed{false};

    NonblockingStdinGuard() : previous_flags(fcntl(STDIN_FILENO, F_GETFL)) {
        if (previous_flags < 0) {
            throwError("Read Error: Failed to inspect KEY input mode.");
        }
        if ((previous_flags & O_NONBLOCK) == 0) {
            if (fcntl(STDIN_FILENO, F_SETFL, previous_flags | O_NONBLOCK) != 0) {
                throwError("Read Error: Failed to prepare KEY input mode.");
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

[[nodiscard]] ssize_t readKeyByte(char& ch, TermiosGuard& termios_guard) {
    KeyEntrySignalBlock signal_block;
    bool eio_retried = false;
    while (true) {
        throwIfSignalCancellationRequested();
        (void)termios_guard.reapplyAfterContinue(signal_block.previous_mask);
        // In the background, only wait: for `fg`, or for cancellation.
        const bool background = termios_guard.awaitingForeground();

        fd_set readable;
        FD_ZERO(&readable);
        if (!background) FD_SET(STDIN_FILENO, &readable);
        // The timeout also bounds cancellation latency if a process-directed
        // signal happens to be handled on another thread.
        constexpr timespec WAIT_INTERVAL{0, 100'000'000};
        const int ready = pselect(background ? 0 : STDIN_FILENO + 1,
                                  background ? nullptr : &readable, nullptr, nullptr,
                                  &WAIT_INTERVAL, &signal_block.previous_mask);
        const int wait_errno = errno;
        throwIfSignalCancellationRequested();
        // A resume flushed the input, so any readiness just reported is stale.
        if (termios_guard.reapplyAfterContinue(signal_block.previous_mask)) continue;
        if (background || termios_guard.awaitingForeground()) continue;
        if (ready < 0) {
            if (wait_errno == EINTR) continue;
            throwError("Read Error: Failed to wait for KEY input.");
        }
        if (ready == 0) continue;

        // Readiness can become stale, including when a resume flushes input.
        // Never block in read while cancellation signals are masked. Only
        // change fd flags around read, restoring them before unmasking or
        // waiting again so a suspended job leaves stdin usable by its shell.
        const NonblockingStdinGuard nonblocking_guard;
        const ssize_t bytes_read = read(STDIN_FILENO, &ch, 1);
        if (bytes_read < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            // The foreground was lost after the check (SIGTTIN is blocked, so
            // the read refuses rather than stops): wait for it in pselect.
            // It may already be back -- `fg`, whose SIGCONT is still blocked
            // here, or another process's tcsetpgrp, which sends none -- so
            // one EIO in the foreground is handled as a resume too: raw mode
            // is re-applied and the wait starts again. Another one is a real
            // fault (a hung-up terminal fails every read).
            if (errno == EIO) {
                if (termios_guard.awaitingForeground()) continue;
                if (!eio_retried) {
                    eio_retried = true;
                    termios_guard.noteResume();
                    continue;
                }
            }
            throwError("Read Error: Failed to read KEY input.");
        }
        return bytes_read;
    }
}
} // namespace

RecoveryKey getRecoveryKey() {
    // 24 characters plus 5 group dashes, with room to spare (D3).
    constexpr std::size_t MAX_KEY_KEYSTROKES = 32;

    throwIfSignalCancellationRequested();

    // Not std::print, which throws when the write fails: with `stty tostop` a
    // background job stops on SIGTTOU here, and `kill %1` resumes it with the
    // write interrupted. That must surface as the pending cancellation (the
    // guard checks it first), not as an error whose message would stop the
    // job again on its way to the terminal. The echo below is written the same
    // way: a stop and resume (SIGCONT is hooked without SA_RESTART) while it
    // waits on output held back by XOFF interrupts it too, and a lost echo
    // must not abort entry.
    std::fputs("\nKEY: ", stdout);
    std::fflush(stdout);

    // Declared before the KEY's buffers, so it is destroyed after them: the
    // typed KEY is wiped before anything waits on the terminal.
    TermiosGuard termios_guard;

    // A fixed stack buffer, never a heap string: nothing here can leave a copy
    // of the KEY behind in a freed allocation. Wiped on every exit path.
    std::array<char, MAX_KEY_KEYSTROKES> input{};
    std::size_t input_len = 0;
    // Keystrokes typed past MAX_KEY_KEYSTROKES are not stored, but they are
    // counted, so that a backspace undoes the keystroke the user actually made
    // last. Without this the buffer silently stops matching what was typed:
    // backspacing back under the limit would hand out a KEY built from the
    // first characters entered.
    std::size_t dropped_keys = 0;
    char ch{};

    WipePodGuard<std::array<char, MAX_KEY_KEYSTROKES>> wipe_input{input};
    WipePodGuard<std::size_t> wipe_input_len{input_len};
    WipePodGuard<char> wipe_ch{ch};
    while (true) {
        const ssize_t bytes_read = readKeyByte(ch, termios_guard);
        // readKeyByte restores the signal mask before returning, delivering
        // any cancellation that arrived after its wait reported readiness.
        throwIfSignalCancellationRequested();
        if (bytes_read == 0) break;
        if (ch == '\n' || ch == '\r') break;
        // Spaces and tabs are ignored, so a KEY pasted with stray whitespace
        // still works (the web variant's KEY file is trimmed the same way).
        if (ch == ' ' || ch == '\t') continue;
        if (ch == '\b' || ch == 127) {
            if (dropped_keys > 0) {
                // Undo an over-limit keystroke. Nothing was echoed for it, so
                // nothing is erased from the display either.
                --dropped_keys;
            } else if (input_len > 0) {
                // One display cell per stored keystroke, dash or not.
                if (termios_guard.active) {
                    std::fputs("\b \b", stdout);
                    std::fflush(stdout);
                }
                input[--input_len] = 0;
            }
            continue;
        }
        // Every other keystroke is kept -- including the group dash, so that
        // "XXXX-" then one backspace removes the dash the user typed, not the
        // character before it -- and so that a stray character rejects the KEY
        // below instead of being silently dropped: "12O4" is a mistyped KEY.
        if (input_len >= input.size()) {
            ++dropped_keys;   // counted, not stored, and not echoed
            continue;
        }
        input[input_len++] = ch;
        if (termios_guard.active) {
            // Dashes are layout, not secret, and echoing them lets the user
            // keep their place in the groups.
            std::fputc(ch == '-' ? '-' : '*', stdout);
            std::fflush(stdout);
        }
    }

    std::fputc('\n', stdout);
    std::fflush(stdout);
    // A cancellation that interrupted the echo (held back by XOFF, say) ends
    // entry now, before anything else waits on the same held output.
    throwIfSignalCancellationRequested();

    // Reject malformed input here instead of silently truncating or
    // normalizing it: such input is a transcription error and must not derive
    // a key the user believes is valid. dropped_keys is non-zero only if
    // keystrokes past the limit are still outstanding -- corrected ones have
    // already been backspaced away above.
    //
    // Knowable mistakes are reported now rather than after an Argon2id run
    // and, on the ICC path, staging up to two gigabytes of ciphertext, only to
    // end in the ambiguous "invalid KEY or file is corrupt". The codec owns the
    // grammar (24-character Base58 with a check pair).
    // The TermiosGuard and the wipe guards above all unwind on the way out.
    if (dropped_keys > 0) {
        throw std::runtime_error(FORMAT_ERROR);
    }
    auto parsed = parseRecoveryKeyText(std::string_view(input.data(), input_len));
    if (!parsed) {
        throw std::runtime_error(
            parsed.error() == KeyParseError::checksum ? CHECK_ERROR : FORMAT_ERROR);
    }
    return std::move(*parsed);
}
