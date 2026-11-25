#include "file_utils.h"
#include "key_input.h"
#include "signal_utils.h"

#include <cerrno>
#include <csignal>
#include <pthread.h>
#include <cstdio>
#include <fcntl.h>
#include <sys/wait.h>
#include <string_view>
#include <utility>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace {
enum class Injection { none, before_wait, after_ready, drain_ready };
Injection injection = Injection::none;
bool stop_after_raw = false;     // stop once raw mode is set, before read-back
bool stop_after_verify = false;  // stop once raw mode is confirmed, before reading
int eintr_raw = 0;               // raw-mode sets to fail with EINTR, unapplied
bool xoff_flush_restore = false; // a flushing restore waits, as on output held
                                 // by XOFF, until a signal interrupts it
bool typeahead_cancel = false;   // SIGTERM once input is ready, before it is read
int eintr_cancel_from = 0;       // raw-mode sets from this one on: SIGTERM, then EINTR
bool continue_before_wait = false; // a SIGCONT pending as the first wait starts
bool cancel_at_snapshot = false; // SIGTERM as the settings to restore are read
bool lose_foreground_when_ready = false; // hand the terminal to the session leader
                                         // between readiness and the read
bool always_eio = false; // every read fails with EIO, as on a hung-up terminal
int blip_foreground = 0; // lose the foreground for one read, then take it back
                         // (2: with the SIGCONT `fg` would send)
int raw_sets = 0;                // raw-mode sets attempted
}

extern "C" int __real_tcsetattr(int, int, const termios*);
extern "C" int __real_tcgetattr(int, termios*);

// Stop the process as KEY entry first enters raw mode, before any read: the
// windows where a shell's cooked-mode restore must still be caught.
extern "C" int __wrap_tcsetattr(int fd, int actions, const termios* settings) {
    if ((settings->c_lflag & ECHO) == 0) ++raw_sets;
    // A cancellation signal interrupting tcsetattr, as when it is stopped on
    // SIGTTOU or held by XOFF and the user presses Ctrl+C.
    if ((settings->c_lflag & ECHO) == 0 && eintr_cancel_from > 0 && raw_sets >= eintr_cancel_from) {
        std::raise(SIGTERM);
        errno = EINTR;
        return -1;
    }
    // A signal interrupting tcsetattr: nothing applied, EINTR.
    if ((settings->c_lflag & ECHO) == 0 && eintr_raw > 0) {
        --eintr_raw;
        errno = EINTR;
        return -1;
    }
    if ((settings->c_lflag & ECHO) != 0 && actions == TCSAFLUSH && xoff_flush_restore) {
        pause();
        errno = EINTR;
        return -1;
    }
    const int result = __real_tcsetattr(fd, actions, settings);
    if (stop_after_raw && result == 0 && (settings->c_lflag & ECHO) == 0) {
        stop_after_raw = false;
        std::raise(SIGSTOP);
    }
    return result;
}

extern "C" int __wrap_tcgetattr(int fd, termios* settings) {
    const int result = __real_tcgetattr(fd, settings);
    // A cancellation landing after the foreground wait, before raw mode.
    if (cancel_at_snapshot) {
        cancel_at_snapshot = false;
        std::raise(SIGTERM);
    }
    if (stop_after_verify && result == 0 && (settings->c_lflag & ECHO) == 0) {
        stop_after_verify = false;
        std::raise(SIGSTOP);
    }
    return result;
}

extern "C" ssize_t __real_read(int, void*, size_t);

// Another process's tcsetpgrp, landing after every foreground check, just
// before the read itself.
extern "C" ssize_t __wrap_read(int fd, void* buffer, size_t count) {
    if (fd == STDIN_FILENO && lose_foreground_when_ready) {
        lose_foreground_when_ready = false;
        if (tcsetpgrp(STDIN_FILENO, getsid(0)) != 0) return -1;
    }
    if (fd == STDIN_FILENO && always_eio) {
        errno = EIO;
        return -1;
    }
    if (fd == STDIN_FILENO && blip_foreground != 0) {
        // The read fails with EIO (SIGTTIN is blocked), and the foreground is
        // back before the caller looks at errno.
        const int blip = std::exchange(blip_foreground, 0);
        if (tcsetpgrp(STDIN_FILENO, getsid(0)) != 0) return -1;
        const ssize_t result = __real_read(fd, buffer, count);
        const int read_errno = errno;
        sigset_t ttou, previous;
        sigemptyset(&ttou);
        sigaddset(&ttou, SIGTTOU);
        pthread_sigmask(SIG_BLOCK, &ttou, &previous);
        const int taken_back = tcsetpgrp(STDIN_FILENO, getpgrp());
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
        if (taken_back != 0) return -1;
        if (blip == 2) std::raise(SIGCONT);  // blocked by the read loop: pending
        errno = read_errno;
        return result;
    }
    return __real_read(fd, buffer, count);
}

extern "C" int __real_pselect(int, fd_set*, fd_set*, fd_set*,
                              const timespec*, const sigset_t*);

// Deterministic signal delivery at the race windows that ordinary PTY tests
// rarely hit. The drain case leaves an open, empty pipe after readiness.
extern "C" int __wrap_pselect(int count, fd_set* read_fds, fd_set* write_fds,
                              fd_set* except_fds, const timespec* timeout,
                              const sigset_t* mask) {
    if (continue_before_wait) {
        // Blocked by the read loop, so delivered when pselect unmasks it:
        // the loop sees a resume and re-applies raw mode.
        continue_before_wait = false;
        std::raise(SIGCONT);
    }
    if (injection == Injection::before_wait) {
        injection = Injection::none;
        std::raise(SIGINT);
    }
    const int result = __real_pselect(count, read_fds, write_fds, except_fds, timeout, mask);
    if (result > 0 && typeahead_cancel) {
        // Queued, and delivered as the read loop next unmasks: the typed
        // bytes stay unread.
        typeahead_cancel = false;
        std::raise(SIGTERM);
    }
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

    if (mode == "open-input") {
        // Must refuse a FIFO (or anything non-regular) without blocking in open.
        if (argc != 3) return 2;
        try {
            const InputFile input(argv[2], "refused non-regular input");
            std::puts("opened");
            return 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
            return 1;
        }
    }
    if (mode == "reraise-blocked") {
        // The signal being re-raised is blocked (as during KEY entry): the
        // process must still be ended by it, not exit with 128 + signal.
        sigset_t blocked;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGTERM);
        if (pthread_sigmask(SIG_BLOCK, &blocked, nullptr) != 0) return 2;
        reraiseSignalAfterCleanup(SIGTERM);
    }

    if (mode == "noctty") {
        // A session leader with no controlling terminal opens a terminal swapped
        // in for an input path: the open must be refused as not a regular file
        // and must not have made that terminal the session's controlling one.
        if (argc != 3) return 2;
        const pid_t child = fork();
        if (child == 0) {
            if (setsid() < 0) _exit(3);
            try {
                const InputFile input(argv[2], "refused non-regular input");
                _exit(4);  // a terminal accepted as an input file
            } catch (const std::exception&) {
            }
            const int tty = open("/dev/tty", O_RDWR | O_NOCTTY);
            _exit(tty >= 0 ? 5 : 0);
        }
        int status = 0;
        if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status)) return 2;
        if (WEXITSTATUS(status) == 5) std::puts("acquired a controlling terminal");
        else if (WEXITSTATUS(status) == 0) std::puts("no controlling terminal acquired");
        return WEXITSTATUS(status);
    }

    if (mode == "filename") {
        if (argc != 3) return 2;
        std::puts(hasSafeEmbeddedFilename(argv[2]) ? "safe" : "unsafe");
        return 0;
    }

    installProcessSignalHandlers();
    if (mode == "dispositions") {
        // Which cancellation signals got the handler: one the parent left
        // ignored must stay ignored.
        for (const auto& [signal_number, name] : {std::pair{SIGHUP, "HUP"}, std::pair{SIGINT, "INT"},
                                                  std::pair{SIGQUIT, "QUIT"}, std::pair{SIGTERM, "TERM"}}) {
            struct sigaction current {};
            if (sigaction(signal_number, nullptr, &current) != 0) return 2;
            std::printf("%s=%s\n", name, current.sa_handler == SIG_IGN ? "ignored"
                                       : current.sa_handler == SIG_DFL ? "default" : "handled");
        }
        return 0;
    }
    if (mode == "pending") std::raise(SIGINT);
    else if (mode == "wait-race") injection = Injection::before_wait;
    else if (mode == "ready-race") injection = Injection::after_ready;
    else if (mode == "drained-race") injection = Injection::drain_ready;
    else if (mode == "stop-after-raw") stop_after_raw = true;
    else if (mode == "stop-after-verify") stop_after_verify = true;
    else if (mode == "eintr-raw") eintr_raw = 2;
    else if (mode == "xoff-restore") xoff_flush_restore = true;
    else if (mode == "xoff-cancel") {
        eintr_cancel_from = 1;
        xoff_flush_restore = true;
    }
    else if (mode == "typeahead-cancel") typeahead_cancel = true;
    else if (mode == "eintr-cancel") eintr_cancel_from = 1;
    else if (mode == "reapply-cancel") {
        // The re-apply after a resume is the second raw-mode set.
        eintr_cancel_from = 2;
        continue_before_wait = true;
    }
    else if (mode == "snapshot-cancel") cancel_at_snapshot = true;
    else if (mode == "key-lose-fg") lose_foreground_when_ready = true;
    else if (mode == "key-blip-fg") blip_foreground = 1;
    else if (mode == "key-eio") always_eio = true;
    else if (mode == "key-blip-cont") blip_foreground = 2;
    // Unbuffered, every echo is its own write: one that fails surfaces at once.
    else if (mode == "key-unbuffered") std::setvbuf(stdout, nullptr, _IONBF, 0);
    else if (mode != "key") return 2;

    const int original_flags = fcntl(STDIN_FILENO, F_GETFL);
    try {
        const RecoveryKey recovery_key = getRecoveryKey();
        if ((!mode.starts_with("key") && !mode.starts_with("stop-after-") && !mode.starts_with("eintr-") &&
             mode != "xoff-restore") ||
            fcntl(STDIN_FILENO, F_GETFL) != original_flags) return 1;
        std::printf("parsed ");
        for (const Byte b : recovery_key.secret) std::printf("%02x", b);
        std::printf("\n");
        return 0;
    } catch (const SignalCancellation& cancellation) {
        if (mode == "eintr-cancel" || mode == "reapply-cancel" || mode == "snapshot-cancel" ||
            mode == "xoff-cancel" || mode == "typeahead-cancel") {
            std::printf("cancelled SIGTERM after %d raw-mode sets\n", raw_sets);
            return cancellation.signalNumber() == SIGTERM ? 0 : 1;
        }
        // As main does: end by the signal, without exit's stdio flush (a
        // buffered prompt would stop a `stty tostop` background job again).
        if (mode.starts_with("key") && cancellation.signalNumber() != SIGINT)
            reraiseSignalAfterCleanup(cancellation.signalNumber());
        if (cancellation.signalNumber() != SIGINT ||
            fcntl(STDIN_FILENO, F_GETFL) != original_flags) return 1;
        std::puts("cancelled SIGINT; input flags restored");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
