// Forces sendfile to report EINVAL so sendFileRangeToFd takes its read/write
// fallback, then checks the bounce buffer is zeroed before it is released.
#include "file_utils.h"

#include <sodium.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <sys/sendfile.h>
#include <unistd.h>

namespace {
constexpr std::size_t kLength = 12'345;  // Distinctive buffer size to watch for.
bool watching = false;
bool observed = false;
bool found_plaintext = false;
}

extern "C" ssize_t __wrap_sendfile(int, int, off_t*, std::size_t) {
    errno = EINVAL;
    return -1;
}

extern "C" void __real__ZdlPvm(void* pointer, std::size_t size) noexcept;
extern "C" void __wrap__ZdlPvm(void* pointer, std::size_t size) noexcept {
    if (watching && pointer != nullptr && size == kLength) {
        observed = true;
        const auto* bytes = static_cast<const unsigned char*>(pointer);
        for (std::size_t index = 0; index < size; ++index) {
            if (bytes[index] != 0) found_plaintext = true;
        }
    }
    __real__ZdlPvm(pointer, size);
}

int main() {
    if (sodium_init() < 0) return 1;
    char in_name[] = "copy_fallback_inXXXXXX";
    char out_name[] = "copy_fallback_outXXXXXX";
    const int in_fd = ::mkstemp(in_name);
    const int out_fd = ::mkstemp(out_name);
    if (in_fd < 0 || out_fd < 0) return 1;
    ::unlink(in_name);
    ::unlink(out_name);

    const vBytes plaintext(kLength, 'S');
    if (::write(in_fd, plaintext.data(), plaintext.size()) != static_cast<ssize_t>(kLength)) return 1;

    watching = true;
    sendFileRangeToFd(out_fd, in_fd, 0, kLength, "copy failed");
    watching = false;

    vBytes copied(kLength);
    if (::pread(out_fd, copied.data(), copied.size(), 0) != static_cast<ssize_t>(kLength) ||
        copied != plaintext) {
        std::fputs("fallback copy produced wrong output\n", stderr);
        return 1;
    }
    if (!observed) {
        std::fputs("did not observe the fallback buffer release\n", stderr);
        return 1;
    }
    if (found_plaintext) {
        std::fputs("fallback copy released a buffer containing plaintext\n", stderr);
        return 1;
    }
    std::puts("sendfile fallback buffer wiped on release");
    return 0;
}
