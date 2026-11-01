// Exercises the copy fallback of tryCommitStagingFileNoReplace -- the path taken
// when a staging inode cannot be linkat(2)-ed into place. linkat is forced to
// fail, and renameat2 / link are wrapped so each publish primitive (and their
// absence) can be simulated. Checks that the final name never holds a partial
// file, that existing names are never replaced, and that no hidden temporary
// is left behind.
#include "file_utils.h"

#include <sodium.h>

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <fcntl.h>
#include <unistd.h>

namespace {
enum class FakeMode { real, unsupported, race };
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
    if (fs::exists(to, ec) && rename_mode != FakeMode::race) bad_state_before_publish = true;
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
    return __real_renameat2(olddirfd, from, newdirfd, to, flags);
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
    OutputFile out(staged.path(), 4096, OutputCreate::existing_staging);
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

    if (failures != 0) return 1;
    std::puts("staging copy fallback publishes atomically without replacement");
    return 0;
}
