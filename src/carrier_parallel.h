#pragma once

#include "signal_utils.h"
#include "worker_team.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>

namespace carrier_parallel {

namespace detail {

// A setting in the form OpenMP takes it, read the way libgomp reads it:
// positive decimal integers (a leading '+' allowed), surrounding whitespace
// allowed, separated by commas when `list` is set (OMP_NUM_THREADS may give
// one per nesting level; only the first applies here). Unset, empty or
// malformed -- a zero, a minus sign, trailing text -- reads as no setting, as
// libgomp ignores it then too.
[[nodiscard]] inline std::optional<unsigned> positiveSetting(const char* name, bool list) noexcept {
    const char* text = std::getenv(name);
    if (text == nullptr) return std::nullopt;
    // strtoul reports an out-of-range count through errno (ERANGE, clamped
    // below anyway); a settings lookup must not leave that behind.
    struct ErrnoRestore {
        int saved = errno;
        ~ErrnoRestore() { errno = saved; }
    } const errno_restore;
    std::optional<unsigned> first;
    const char* at = text;
    for (;;) {
        while (std::isspace(static_cast<unsigned char>(*at))) ++at;
        if (*at == '+') ++at;
        if (!std::isdigit(static_cast<unsigned char>(*at))) return std::nullopt;
        char* end = nullptr;
        const unsigned long value = std::strtoul(at, &end, 10);
        if (value == 0) return std::nullopt;
        if (!first) first = static_cast<unsigned>(std::min<unsigned long>(value, UINT_MAX));
        at = end;
        while (std::isspace(static_cast<unsigned char>(*at))) ++at;
        if (*at == '\0') return first;
        if (!list || *at != ',') return std::nullopt;
        ++at;
    }
}

} // namespace detail

// How many threads a carrier stage uses. The OpenMP environment settings are
// still honoured -- the stages ran under OpenMP once, and OMP_NUM_THREADS is
// how a run is pinned to fewer threads -- but read here, so the runtime is no
// longer linked: the threads themselves are standard C++ ones (see
// worker_team for why). As under libgomp, the result is the smallest of
// OMP_NUM_THREADS (default: the CPUs this process may run on),
// OMP_THREAD_LIMIT (default: none) and those CPUs. (Checked against libgomp's
// own answers across valid, malformed and empty settings and CPU masks. The
// one deliberate difference: libgomp wraps counts of 2^31 and up through an
// int, so OMP_NUM_THREADS=2147483648 gives it one thread; here it means "as
// many as the CPUs allow".)
//
// A build defining JDVRIF_SERIAL_CARRIERS runs every stage on the calling
// thread alone, whatever the environment says.
[[nodiscard]] inline unsigned threadCount() noexcept {
#ifdef JDVRIF_SERIAL_CARRIERS
    return 1;
#else
    const unsigned cpus = worker_team::availableCpus();
    const unsigned requested = detail::positiveSetting("OMP_NUM_THREADS", true).value_or(cpus);
    const unsigned limit = detail::positiveSetting("OMP_THREAD_LIMIT", false).value_or(UINT_MAX);
    return std::max(1U, std::min({requested, limit, cpus}));
#endif
}

// Independent iterations, with the same dynamic chunks as the former OpenMP
// loops, run on a worker_team (see there for refused threads and errors).
// Never retry completed iterations. A pending cancellation stops every thread
// taking new chunks and is thrown here once they have joined.
// Bounds must be nonnegative and chunk positive (all callers use constants).
template<class Work>
void forEach(std::int64_t begin, std::int64_t end, std::int64_t chunk, Work&& work) {
    throwIfSignalCancellationRequested();
    if (begin < 0 || chunk <= 0) throw std::logic_error("Internal Error: Invalid parallel loop bounds.");
    if (begin >= end) return;
    const auto chunks = 1 + (end - begin - 1) / chunk;
    const auto threads = std::min<std::int64_t>(static_cast<std::int64_t>(threadCount()), chunks);
    // Each thread's last fetch_add can move `next` up to one chunk past `end`.
    if ((std::numeric_limits<std::int64_t>::max() - end) / chunk < threads) {
        throw std::logic_error("Internal Error: Invalid parallel loop bounds.");
    }
    std::atomic<std::int64_t> next{begin};
    std::atomic<bool> failed{false};
    worker_team::run(static_cast<std::size_t>(threads), failed, [&] {
        while (!failed.load(std::memory_order_relaxed) && pendingSignalCancellation() == 0) {
            const auto first = next.fetch_add(chunk, std::memory_order_relaxed);
            if (first >= end) break;
            const auto last = first + std::min(chunk, end - first);
            for (auto index = first; index < last; ++index) {
                if (failed.load(std::memory_order_relaxed) || pendingSignalCancellation() != 0) break;
                work(index);
            }
        }
    });
    throwIfSignalCancellationRequested();
}

} // namespace carrier_parallel
