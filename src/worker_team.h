#pragma once

#include <sched.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <exception>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace worker_team {

// CPUs this process may run on: its affinity mask, so taskset and cpusets are
// respected, unlike std::thread::hardware_concurrency(), which counts the
// machine's. At least 1. The kernel refuses (EINVAL) a mask smaller than its
// own CPU count, so a host past CPU_SETSIZE (1024) needs a larger one.
[[nodiscard]] inline unsigned availableCpus() noexcept {
    for (std::size_t cpus = CPU_SETSIZE; cpus <= std::size_t{1} << 20; cpus *= 2) {
        cpu_set_t* allowed = CPU_ALLOC(cpus);
        if (allowed == nullptr) return 1U;
        const std::size_t size = CPU_ALLOC_SIZE(cpus);
        CPU_ZERO_S(size, allowed);
        const bool read = sched_getaffinity(0, size, allowed) == 0;
        const int error = errno;
        const int count = read ? CPU_COUNT_S(size, allowed) : 0;
        CPU_FREE(allowed);
        if (read) return count > 0 ? static_cast<unsigned>(count) : 1U;
        if (error != EINVAL) break;
    }
    return 1U;
}

// Runs `work` on up to `threads` threads, the caller's own included, and
// returns once every one has finished. Thread creation, error capture and the
// join live here so each parallel stage gets the same failure handling; the
// stage keeps its own scheduling inside `work` (a shared queue it drains, and
// whatever per-thread state it sets up once, such as a z_stream).
//
// - Refused thread creation (a process or cgroup limit) only reduces the team:
//   the caller and any workers already started drain the same queue, so the
//   work is done with fewer threads rather than abandoned. Standard threads
//   report the refusal by exception; OpenMP's runtime would exit the process.
// - Any other failure to start a thread (std::bad_alloc for its state) is
//   treated like a worker's exception: started workers are stopped, not left
//   to drain the queue under memory pressure.
// - The first exception any thread raises sets `failed` and is rethrown after
//   every thread has joined, so no worker outlives the caller's stack frame.
//   `work` must check `failed` and stop taking items once it is set.
template<typename Work>
void run(std::size_t threads, std::atomic<bool>& failed, Work&& work) {
    if (threads <= 1) {
        work();
        return;
    }
    std::exception_ptr first_error;
    std::mutex error_mutex;
    const auto record_failure = [&]() noexcept {
        const std::lock_guard lock(error_mutex);
        if (!first_error) first_error = std::current_exception();
        failed.store(true, std::memory_order_relaxed);
    };
    const auto guarded = [&] {
        try {
            work();
        } catch (...) {
            record_failure();
        }
    };
    {
        std::vector<std::jthread> team;
        team.reserve(threads - 1U);
        for (std::size_t index = 1; index < threads; ++index) {
            try {
                team.emplace_back(guarded);
            } catch (const std::system_error&) {
                break;
            } catch (...) {
                record_failure();
                break;
            }
        }
        // After a failed start the team is already stopping: don't set up the
        // caller's own share of the work (a z_stream, say) just to drop it.
        if (!failed.load(std::memory_order_relaxed)) guarded();
    }
    if (first_error) std::rethrow_exception(first_error);
}

} // namespace worker_team
