#include "signal_utils.h"

#include <array>
#include <atomic>
#include <csignal>
#include <iostream>
#include <latch>
#include <thread>
#include <vector>

int main() {
    installProcessSignalHandlers();
    if (pendingSignalCancellation() != 0) return 1;
    throwIfSignalCancellationRequested();
    std::raise(SIGPIPE);
    if (pendingSignalCancellation() != 0) return 1;

    constexpr auto signals = std::to_array({SIGHUP, SIGINT, SIGQUIT, SIGTERM});
    std::latch reading(signals.size());
    std::atomic<unsigned> observed{0};
    std::atomic<unsigned> unwound{0};
    std::atomic<bool> wrong_signal{false};
    {
        std::vector<std::jthread> readers;
        for (const int later_signal : signals) {
            readers.emplace_back([&, later_signal] {
                // Every worker has read the flag before the writer signals.
                if (pendingSignalCancellation() != 0) wrong_signal = true;
                reading.count_down();
                while (pendingSignalCancellation() == 0) std::this_thread::yield();
                if (pendingSignalCancellation() != SIGINT) wrong_signal = true;
                struct Guard {
                    std::atomic<unsigned>& count;
                    ~Guard() { ++count; }
                };
                try {
                    Guard guard{unwound};
                    throwIfSignalCancellationRequested();
                } catch (const SignalCancellation& error) {
                    if (error.signalNumber() == SIGINT) ++observed;
                    else wrong_signal = true;
                }
                // Concurrent handlers must preserve the already-recorded signal.
                for (unsigned repeat = 0; repeat < 1000; ++repeat) {
                    std::raise(later_signal);
                    if (pendingSignalCancellation() != SIGINT) wrong_signal = true;
                }
            });
        }
        std::jthread writer([&] {
            reading.wait();
            std::raise(SIGINT);
        });
    }
    if (wrong_signal || observed != signals.size() || unwound != signals.size() ||
        pendingSignalCancellation() != SIGINT) return 1;
    std::cout << "Signal threads: cancellation observed, first signal preserved, guards unwound\n";
}
