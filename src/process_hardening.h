#pragma once

#include <cstdio>
#include <sys/prctl.h>
#include <sys/resource.h>

// Keeps the process's memory -- the KEY, derived keys and decrypted data --
// out of core dumps, and stops other processes running as the same user from
// attaching to it with ptrace or reading /proc/<pid>/mem. Both settings are
// per-process and end when it exits. RLIMIT_CORE alone is not enough, because
// a core_pattern pipe handler (systemd-coredump, apport) ignores it;
// PR_SET_DUMPABLE 0 suppresses those as well. The process can still reach its
// own /proc/self/fd entries, which the staging files rely on.
//
// Best effort: if either call is refused (a seccomp filter, say), a warning
// goes to stderr and the program carries on rather than refusing to run. This
// does not keep memory out of swap, nor out of reach of root.
inline void disableCoreDumps() noexcept {
    const rlimit no_core{0, 0};
    const bool core_ok = setrlimit(RLIMIT_CORE, &no_core) == 0;
    const bool dumpable_ok = prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) == 0;
    if (core_ok && dumpable_ok) return;

    const char* refused = !core_ok && !dumpable_ok ? "RLIMIT_CORE 0 and PR_SET_DUMPABLE 0"
                        : !core_ok                 ? "RLIMIT_CORE 0"
                                                   : "PR_SET_DUMPABLE 0";
    // fprintf, not std::print: this must not throw.
    (void)std::fprintf(stderr,
        "Warning: The system refused %s, so core dumps or other processes may "
        "expose this run's memory. Continuing anyway.\n", refused);
}
