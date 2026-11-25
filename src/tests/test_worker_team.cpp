// worker_team failure handling: a std::bad_alloc while starting a worker must
// stop the workers already running, and availableCpus() must read affinity
// masks larger than cpu_set_t. Link with -Wl,--wrap=sched_getaffinity.
// The replacement operator new below allocates with malloc; GCC cannot see
// that and flags the matching free.
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"

#include "worker_team.h"

#include <sched.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <thread>

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// Allocation control for the main thread only. Once `main_allocations_left`
// runs out, the next main-thread allocation waits (up to 5 s) for a worker to
// be running, so there is one to stop, and then throws std::bad_alloc.
std::thread::id main_thread;
std::atomic<bool> allocation_armed{false};
std::atomic<long> main_allocations_left{0};
std::atomic<long> main_allocations_seen{0};
std::atomic<int> workers_running{0};
std::atomic<bool> caller_ran_work{false};

// Affinity emulation for a host with more CPUs than cpu_set_t can hold.
bool emulate_large_host = false;
constexpr std::size_t LARGE_HOST_CPUS = 1500;

} // namespace

void* operator new(std::size_t size) {
    if (std::this_thread::get_id() == main_thread) {
        main_allocations_seen.fetch_add(1, std::memory_order_relaxed);
        if (allocation_armed.load() && main_allocations_left.fetch_sub(1) <= 0) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (workers_running.load() == 0 && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            throw std::bad_alloc();
        }
    }
    if (void* block = std::malloc(size == 0 ? 1 : size)) return block;
    throw std::bad_alloc();
}
void operator delete(void* block) noexcept { std::free(block); }
void operator delete(void* block, std::size_t) noexcept { std::free(block); }

extern "C" int __real_sched_getaffinity(pid_t, std::size_t, cpu_set_t*);
extern "C" int __wrap_sched_getaffinity(pid_t pid, std::size_t size, cpu_set_t* mask) {
    if (!emulate_large_host) return __real_sched_getaffinity(pid, size, mask);
    // The kernel's rule: a mask too small for its CPU count is EINVAL.
    if (size * 8 < LARGE_HOST_CPUS) {
        errno = EINVAL;
        return -1;
    }
    std::memset(mask, 0, size);
    for (std::size_t cpu = 0; cpu < LARGE_HOST_CPUS; ++cpu) CPU_SET_S(cpu, size, mask);
    return 0;
}

namespace {

// How many main-thread allocations starting one std::jthread takes.
long allocationsPerThreadStart() {
    const long before = main_allocations_seen.load();
    std::jthread probe([] {});
    return main_allocations_seen.load() - before;
}

void checkBadAllocStopsStartedWorkers() {
    constexpr int ITEMS = 400;
    std::atomic<int> next{0};
    std::atomic<bool> failed{false};
    const auto work = [&] {
        if (std::this_thread::get_id() == main_thread) caller_ran_work = true;
        workers_running.fetch_add(1);
        while (!failed.load(std::memory_order_relaxed)) {
            if (next.fetch_add(1) >= ITEMS) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    // run() reserves the team (one allocation) and starts the first worker
    // normally; starting the second runs out of memory.
    main_allocations_left = 1 + allocationsPerThreadStart();
    allocation_armed = true;
    bool saw_bad_alloc = false;
    try {
        worker_team::run(3, failed, work);
    } catch (const std::bad_alloc&) {
        saw_bad_alloc = true;
    }
    allocation_armed = false;

    expect(saw_bad_alloc, "bad_alloc starting a worker is reported");
    expect(failed.load(), "bad_alloc starting a worker sets failed");
    expect(next.load() < ITEMS, "started worker stops instead of draining the queue");
    expect(!caller_ran_work.load(), "caller does not start its own share after a failed spawn");
}

void checkWorkerErrorStillReported() {
    std::atomic<bool> failed{false};
    bool saw_error = false;
    try {
        worker_team::run(3, failed, [] { throw std::runtime_error("worker"); });
    } catch (const std::runtime_error&) {
        saw_error = true;
    }
    expect(saw_error && failed.load(), "worker exception is rethrown");
}

void checkLargeAffinityMask() {
    emulate_large_host = true;
    const unsigned cpus = worker_team::availableCpus();
    emulate_large_host = false;
    expect(cpus == LARGE_HOST_CPUS, "CPUs beyond cpu_set_t are counted");
    expect(worker_team::availableCpus() >= 1, "ordinary host reports at least one CPU");
}

} // namespace

int main() {
    main_thread = std::this_thread::get_id();
    checkBadAllocStopsStartedWorkers();
    checkWorkerErrorStillReported();
    checkLargeAffinityMask();
    if (failures != 0) return EXIT_FAILURE;
    std::puts("worker_team tests passed.");
    return EXIT_SUCCESS;
}
