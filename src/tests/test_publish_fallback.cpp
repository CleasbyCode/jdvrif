// Exercises the copy fallback of tryCommitStagingFileNoReplace -- the path taken
// when a staging inode cannot be linkat(2)-ed into place. linkat is forced to
// fail, and renameat2 / link are wrapped so each publish primitive (and their
// absence) can be simulated. Checks that the final name never holds a partial
// file, that existing names are never replaced, and that no hidden temporary
// is left behind.
#include "file_utils.h"
#include "signal_utils.h"

#include <sodium.h>

#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstring>
#include <sys/wait.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <fcntl.h>
#include <unistd.h>

namespace {
enum class FakeMode { real, unsupported, race, race_eperm };
FakeMode rename_mode = FakeMode::real;
FakeMode link_mode = FakeMode::real;
std::size_t expected_size = 0;
bool checked_before_publish = false;
bool bad_state_before_publish = false;
int failures = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

bool hasPublishTemp() {
    for (const auto& entry : fs::directory_iterator(".")) {
        if (entry.path().filename().string().starts_with(".jdvrif_publish_")) return true;
    }
    return false;
}

// The temporary must be complete and the final name still absent at the moment
// of publication.
void checkBeforePublish(const char* from, const char* to) {
    checked_before_publish = true;
    std::error_code ec;
    const bool racing = rename_mode == FakeMode::race || rename_mode == FakeMode::race_eperm;
    if (fs::exists(to, ec) && !racing) bad_state_before_publish = true;
    if (fs::file_size(from, ec) != expected_size || ec) bad_state_before_publish = true;
}
} // namespace

extern "C" int __wrap_linkat(int, const char*, int, const char*, int) {
    errno = EPERM;  // e.g. a filesystem without hard links
    return -1;
}

extern "C" int __real_renameat2(int, const char*, int, const char*, unsigned);
extern "C" int __wrap_renameat2(int olddirfd, const char* from, int newdirfd, const char* to, unsigned flags) {
    checkBeforePublish(from, to);
    if (rename_mode == FakeMode::unsupported) {
        errno = EINVAL;
        return -1;
    }
    if (rename_mode == FakeMode::race) {
        // Another process takes the name between the existence check and publish.
        std::ofstream(to, std::ios::binary) << "intruder";
    }
    if (rename_mode == FakeMode::race_eperm) {
        // As above, but the filesystem (FUSE, an LSM) reports the taken name
        // as EPERM rather than EEXIST.
        std::ofstream(to, std::ios::binary) << "intruder";
        errno = EPERM;
        return -1;
    }
    return __real_renameat2(olddirfd, from, newdirfd, to, flags);
}

// Interrupt the exclusive create of the final name, with SIGINT pending, as a
// Ctrl+C during a slow open(2) would.
bool interrupt_final_open = false;

extern "C" int __real_open(const char*, int, ...);
extern "C" int __wrap_open(const char* path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args;
        va_start(args, flags);
        mode = static_cast<mode_t>(va_arg(args, int));
        va_end(args);
    }
    if (interrupt_final_open && (flags & O_EXCL) != 0 && std::strcmp(path, "interrupted") == 0) {
        std::raise(SIGINT);
        errno = EINTR;
        return -1;
    }
    return __real_open(path, flags, mode);
}

extern "C" int __real_link(const char*, const char*);
extern "C" int __wrap_link(const char* from, const char* to) {
    if (link_mode == FakeMode::unsupported) {
        errno = EPERM;
        return -1;
    }
    return __real_link(from, to);
}

namespace {
void stage(StagingFile& staged, const std::string& content) {
    OutputFile out(staged, 4096);
    out.write(std::span<const Byte>(reinterpret_cast<const Byte*>(content.data()), content.size()), "write");
    out.close("close", true);
    expected_size = content.size();
}

void runPublish(const char* label, FakeMode rename, FakeMode link, bool expect_checked) {
    rename_mode = rename;
    link_mode = link;
    checked_before_publish = false;
    bad_state_before_publish = false;
    const fs::path out = std::string("out_") + label;
    const std::string content(70'000, label[0]);

    StagingFile staged({}, "test");
    stage(staged, content);
    const bool ok = tryCommitStagingFileNoReplace(staged, out, "commit");
    expect(ok, label);
    expect(slurp(out) == content, label);
    expect(!hasPublishTemp(), label);
    expect(checked_before_publish == expect_checked && !bad_state_before_publish, label);
}
} // namespace

int main() {
    if (sodium_init() < 0) return 1;

    runPublish("rename_noreplace", FakeMode::real, FakeMode::real, true);
    runPublish("link_fallback", FakeMode::unsupported, FakeMode::real, true);
    runPublish("direct_last_resort", FakeMode::unsupported, FakeMode::unsupported, true);

    // An existing destination is never touched, and nothing is copied.
    {
        rename_mode = FakeMode::real;
        checked_before_publish = false;
        std::ofstream("taken", std::ios::binary) << "keep";
        StagingFile staged({}, "test");
        stage(staged, std::string(5000, 'x'));
        expect(!tryCommitStagingFileNoReplace(staged, "taken", "commit"), "taken: returns false");
        expect(slurp("taken") == "keep", "taken: unchanged");
        expect(!checked_before_publish, "taken: no copy attempted");
        expect(!hasPublishTemp(), "taken: no temp left");
    }

    // The name is taken after the check but before the rename.
    {
        rename_mode = FakeMode::race;
        StagingFile staged({}, "test");
        stage(staged, std::string(5000, 'y'));
        expect(!tryCommitStagingFileNoReplace(staged, "raced", "commit"), "race: returns false");
        expect(slurp("raced") == "intruder", "race: intruder not replaced");
        expect(!hasPublishTemp(), "race: no temp left");
    }

    // Same race, reported as EPERM: still "name taken", not a write error.
    {
        rename_mode = FakeMode::race_eperm;
        StagingFile staged({}, "test");
        stage(staged, std::string(5000, 'z'));
        bool taken = false;
        try {
            taken = !tryCommitStagingFileNoReplace(staged, "raced_eperm", "commit");
        } catch (const std::exception&) {
        }
        expect(taken, "EPERM race: returns false instead of throwing");
        expect(slurp("raced_eperm") == "intruder", "EPERM race: intruder not replaced");
        expect(!hasPublishTemp(), "EPERM race: no temp left");
    }

    // Cancelled while creating the final name on the last-resort path: the
    // cancellation must come out as itself, not as a create error or "taken".
    {
        const pid_t child = fork();
        if (child == 0) {
            installProcessSignalHandlers();
            rename_mode = FakeMode::unsupported;
            link_mode = FakeMode::unsupported;
            StagingFile staged({}, "test");
            stage(staged, std::string(5000, 'c'));
            interrupt_final_open = true;
            try {
                (void)tryCommitStagingFileNoReplace(staged, "interrupted", "commit");
            } catch (const SignalCancellation& cancellation) {
                std::_Exit(cancellation.signalNumber() == SIGINT ? 0 : 3);
            } catch (...) {
                std::_Exit(4);
            }
            std::_Exit(5);
        }
        int status = 0;
        waitpid(child, &status, 0);
        expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
               "cancelled final create: SignalCancellation propagates");
        expect(!fs::exists("interrupted"), "cancelled final create: nothing published");
        expect(!hasPublishTemp(), "cancelled final create: no temp left");
    }

    if (failures != 0) return 1;
    std::puts("staging copy fallback publishes atomically without replacement");
    return 0;
}
