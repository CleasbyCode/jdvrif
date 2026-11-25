// file_utils fault paths:
// - an OutputFile whose drain fails partway must not rewrite the buffer from
//   its start when destroyed (that would duplicate the bytes already written);
// - a StagingFile whose constructor throws after opening must not leak the fd.
// Link with -Wl,--wrap=open.
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"

#include "file_utils.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <new>
#include <stdexcept>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// Set by the open() wrapper once an O_TMPFILE staging inode is open: every
// operator new after that throws until the test clears it.
bool fail_allocation_after_tmpfile = false;
bool allocations_fail = false;

} // namespace

void* operator new(std::size_t size) {
    if (allocations_fail) throw std::bad_alloc();
    if (void* block = std::malloc(size == 0 ? 1 : size)) return block;
    throw std::bad_alloc();
}
void operator delete(void* block) noexcept { std::free(block); }
void operator delete(void* block, std::size_t) noexcept { std::free(block); }

extern "C" int __real_open(const char* path, int flags, ...);
extern "C" int __wrap_open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }
    const int fd = __real_open(path, flags, mode);
    if (fd >= 0 && (flags & O_TMPFILE) == O_TMPFILE && fail_allocation_after_tmpfile) {
        allocations_fail = true;
    }
    return fd;
}

namespace {

std::size_t openDescriptorCount() {
    std::size_t count = 0;
    if (DIR* dir = ::opendir("/proc/self/fd")) {
        while (::readdir(dir) != nullptr) ++count;
        ::closedir(dir);
    }
    return count;
}

std::size_t stagedSize(const StagingFile& staged) {
    struct stat status {};
    expect(::fstat(staged.fd(), &status) == 0, "fstat of the staging inode");
    return static_cast<std::size_t>(status.st_size);
}

void checkFailedDrainIsNotRetried(const std::filesystem::path& dir) {
    StagingFile staged(dir, "partial");
    // Writes past RLIMIT_FSIZE stop short and then fail with EFBIG.
    std::signal(SIGXFSZ, SIG_IGN);
    rlimit original{};
    ::getrlimit(RLIMIT_FSIZE, &original);
    {
        OutputFile output(staged, 64);
        const vBytes first(40, 'A');
        output.write(first, "write");      // buffered
        rlimit limited = original;
        limited.rlim_cur = 10;
        ::setrlimit(RLIMIT_FSIZE, &limited);
        bool failed = false;
        try {
            output.write(vBytes(30, 'B'), "write");  // drains the 40: 10 land, then EFBIG
        } catch (const std::runtime_error&) {
            failed = true;
        }
        expect(failed, "drain past the file-size limit fails");
        ::setrlimit(RLIMIT_FSIZE, &original);
    }   // destructor: must not write the 40 buffered bytes again
    expect(stagedSize(staged) == 10, "failed drain is not repeated by the destructor");
}

// An OutputFile destroyed without close() is on an error path (a failed
// authentication or inflate, a cancellation): its buffered bytes, recovered
// plaintext in the decrypt paths, are discarded rather than written out.
void checkDestructorDiscardsBufferedTail(const std::filesystem::path& dir) {
    StagingFile staged(dir, "discarded_tail");
    {
        OutputFile output(staged, 64);
        output.write(vBytes(100, 'A'), "write");   // at least the buffer: written through
        output.write(vBytes(20, 'B'), "write");    // buffered
    }
    expect(stagedSize(staged) == 100, "destructor discards the buffered tail instead of flushing it");
}

void checkStagingFileClosesFdOnThrow(const std::filesystem::path& dir) {
    const std::size_t before = openDescriptorCount();
    bool threw = false;
    fail_allocation_after_tmpfile = true;
    try {
        StagingFile staged(dir, "fault");
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    allocations_fail = false;
    fail_allocation_after_tmpfile = false;
    expect(threw, "allocation failure after the staging open is reported");
    expect(openDescriptorCount() == before, "staging fd is closed when the constructor throws");
}

} // namespace

int main() {
    char pattern[] = "/tmp/jdvrif-output-faults.XXXXXX";
    const char* made = ::mkdtemp(pattern);
    if (made == nullptr) return EXIT_FAILURE;
    const std::filesystem::path dir(made);
    // Push the staging fd past 9, so its /proc path no longer fits in the
    // small-string buffer and formatting it has to allocate.
    for (int index = 0; index < 12; ++index) (void)::dup(STDERR_FILENO);
    checkFailedDrainIsNotRetried(dir);
    checkDestructorDiscardsBufferedTail(dir);
    checkStagingFileClosesFdOnThrow(dir);
    std::filesystem::remove_all(dir);
    if (failures != 0) return EXIT_FAILURE;
    std::puts("OutputFile/StagingFile fault tests passed.");
    return EXIT_SUCCESS;
}
