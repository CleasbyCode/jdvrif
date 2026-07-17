#pragma once

#include <exception>

class SignalCancellation final : public std::exception {
public:
    explicit SignalCancellation(int signal_number) noexcept
        : signal_number_(signal_number) {}

    [[nodiscard]] int signalNumber() const noexcept { return signal_number_; }
    [[nodiscard]] const char* what() const noexcept override {
        return "Operation interrupted by signal";
    }

private:
    int signal_number_;
};

// Converts the common terminating/job-control signals into a cooperative
// cancellation request. Normal code checks that request and throws, allowing
// termios and temporary-file RAII guards to unwind before the signal is
// re-raised with its default disposition. SIGPIPE is ignored so output errors
// likewise unwind instead of terminating at an arbitrary write.
void installProcessSignalHandlers();
void throwIfSignalCancellationRequested();
[[noreturn]] void reraiseSignalAfterCleanup(int signal_number) noexcept;
