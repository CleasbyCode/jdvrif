#pragma once

#include "signal_utils.h"

#include <csetjmp>
#include <cstddef>
#include <cstdio>
#include <stdexcept>

extern "C" {
#include <jpeglib.h>
}

namespace jpeg_safety {

// Ordinary progressive JPEGs use far fewer scans. Byte and dimension limits
// alone do not bound the work caused by repeatedly scanning the same coefficients.
inline constexpr int MAX_INPUT_SCANS = 100;

extern "C" inline void checkProgress(j_common_ptr common) noexcept;

// Keep this beside the error manager on the heap: libjpeg updates its counters
// after setjmp, and the stop reason must survive longjmp unchanged.
struct ProgressMonitor {
    jpeg_progress_mgr pub{};
    std::jmp_buf* jump;
    int cancellation_signal{0};
    bool scan_limit_exceeded{false};

    explicit ProgressMonitor(std::jmp_buf& error_jump) noexcept
        : jump(&error_jump) {
        pub.progress_monitor = checkProgress;
    }

    void throwIfStopped() const {
        if (cancellation_signal != 0) {
            throw SignalCancellation(cancellation_signal);
        }
        if (scan_limit_exceeded) {
            throw std::runtime_error("JPEG error: Progressive scan limit exceeded (maximum 100 scans).");
        }
    }
};

extern "C" inline void checkProgress(j_common_ptr common) noexcept {
    auto* monitor = reinterpret_cast<ProgressMonitor*>(common->progress);
    monitor->cancellation_signal = pendingSignalCancellation();
    if (monitor->cancellation_signal != 0) {
        // Do not unwind a C++ exception through libjpeg's C stack. The codec
        // entry point destroys its codec objects before throwing in C++.
        std::longjmp(*monitor->jump, 1);
    }
    if (common->is_decompressor &&
        reinterpret_cast<j_decompress_ptr>(common)->input_scan_number > MAX_INPUT_SCANS) {
        monitor->scan_limit_exceeded = true;
        std::longjmp(*monitor->jump, 1);
    }
}

} // namespace jpeg_safety
