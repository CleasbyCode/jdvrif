// The replacement operator new allocates with malloc; GCC cannot see that.
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"

#include "compression.h"
#include "file_utils.h"
#include "signal_utils.h"

#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>

// head-oom: the first slice-output allocation (just over 1 MiB) stalls, so the
// other workers fill every slot and wait behind it, then fails.
std::atomic<bool> stall_first_output{false};

void* operator new(std::size_t size) {
    constexpr std::size_t slice = 1024 * 1024;
    if (size > slice && size < slice + 32 * 1024 && stall_first_output.exchange(false)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        throw std::bad_alloc();
    }
    if (void* block = std::malloc(size == 0 ? 1 : size)) return block;
    throw std::bad_alloc();
}
void operator delete(void* block) noexcept { std::free(block); }
void operator delete(void* block, std::size_t) noexcept { std::free(block); }

// argv: input output expected_size policy, where policy is keep or discard,
// or one of two fault modes that must end the run promptly, not hang workers
// waiting for a free slot:
//   cancel       SIGINT arrives mid-run; SignalCancellation must come out;
//   write-limit  RLIMIT_FSIZE fails an output write mid-run;
//   head-oom     the first slice's output buffer cannot be allocated, after
//                the other workers are already waiting behind that slice.
int main(int argc, char** argv) {
    try {
        if (argc != 5) return 2;
        const std::string_view policy = argv[4];
        std::jthread interrupter;
        if (policy == "cancel") {
            installProcessSignalHandlers();
            interrupter = std::jthread([] {
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                ::kill(::getpid(), SIGINT);
            });
        } else if (policy == "write-limit") {
            ::signal(SIGXFSZ, SIG_IGN);
            const rlimit limit{4U * 1024U * 1024U, RLIM_INFINITY};
            ::setrlimit(RLIMIT_FSIZE, &limit);
        }
        StagingFile stage({}, "deflate-thread-test");
        if (policy == "head-oom") stall_first_output = true;
        const auto started = std::chrono::steady_clock::now();
        try {
            const bool kept = zlibCompressFileToPath(
                argv[1], stage, std::stoull(argv[3]),
                policy == "discard" ? UnshrunkOutput::discard : UnshrunkOutput::keep);
            if (kept) fs::copy_file(stage.path(), argv[2]);
            std::cout << (kept ? "kept" : "discarded") << '\n';
        } catch (const std::bad_alloc&) {
            std::cout << "out of memory\n";
        } catch (const SignalCancellation&) {
            const auto elapsed = std::chrono::steady_clock::now() - started;
            std::cout << "cancelled in "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms\n";
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
