#include "file_utils.h"
#include "recover_output.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <malloc.h>
#include <new>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

// While armed, every block freed through operator delete is searched for the
// refused recovered name: a refusal must not free a copy of it unwiped (the
// caller's own copy, in DecryptResult, is wiped by its destructor).
namespace {
const char* watched_name = nullptr;
bool freed_name_copy = false;
}

void operator delete(void* block) noexcept {
    if (block != nullptr && watched_name != nullptr &&
        memmem(block, malloc_usable_size(block), watched_name, std::strlen(watched_name)) != nullptr) {
        freed_name_copy = true;
    }
    std::free(block);
}
void operator delete(void* block, std::size_t) noexcept { operator delete(block); }

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void checkRejected(const std::string& name) {
    require(!hasSafeEmbeddedFilename(name), "unsafe embedded filename accepted");
    bool rejected = false;
    try {
        (void)validatedRecoveryPath(name);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "unsafe recovery filename accepted");
    const std::string shown = displaySafeFilename(name);
    require(shown != name && shown.find("\\x") != std::string::npos,
            "unsafe filename was not escaped");
    require(displaySafePath(fs::path("parent") / name) == "parent/" + shown,
            "unsafe path was not escaped");
    require(displaySafePath(fs::path(name) / "ok.txt") == shown + "/ok.txt",
            "unsafe parent directory was not escaped");
}
} // namespace

int main() {
    try {
        // Each reported family, plus other default-ignorables and whitespace.
        for (const std::string hidden : {
                 "\u200B", "\u200C", "\u200D", "\u2060", "\uFEFF",
                 "\u2028", "\u2029", "\u3000", "\U000E0001", "\U000E0020",
                 "\U000E007F", "\u00AD", "\u034F", "\u061C", "\u115F",
                 "\u1160", "\u17B4", "\u17B5", "\u180B", "\u180E", "\u180F",
                 "\u200E", "\u200F", "\u202A", "\u202E", "\u2061", "\u206F",
                 "\u3164", "\uFE00", "\uFE0F", "\uFFA0", "\uFFF0", "\uFFF8",
                 "\U0001BCA0", "\U0001BCA3", "\U0001D173", "\U0001D17A",
                 "\U000E0100", "\U000E01EF", "\U000E0FFF", "\u00A0",
                 "\u1680", "\u2000", "\u200A", "\u202F", "\u205F", "\u009B"}) {
            checkRejected(hidden + "invoice.pdf");
            checkRejected("invoice" + hidden + ".pdf");
            checkRejected("doc.pdf" + hidden);
        }
        for (const std::string name : {"invoice.pdf", "my invoice.pdf", "caf\u00E9.txt",
                                      "cafe\u0301.txt", "\u65E5\u672C.txt",
                                      "\u0645\u0644\u0641.txt", "emoji\U0001F600.txt"}) {
            require(hasSafeEmbeddedFilename(name), "ordinary filename rejected");
            require(validatedRecoveryPath(name) == fs::path(name), "recovery renamed filename");
            require(displaySafeFilename(name) == name, "ordinary filename escaped");
        }
        for (const std::string name : {"x\xFF.txt", "x\xC0\xAE.txt", "x\xED\xA0\x80.txt",
                                      "x\xF4\x90\x80\x80.txt", "x\xE2\x80.txt", "x\x1B.txt"}) {
            checkRejected(name);
        }
        require(displaySafeFilename("invoice\u200B.pdf") == "invoice\\xE2\\x80\\x8B.pdf",
                "zero-width space escape mismatch");
        require(displaySafeFilename("doc.pdf\u3000") == "doc.pdf\\xE3\\x80\\x80",
                "ideographic space escape mismatch");
        for (const char* name : {"", ".hidden", "-option", "trail ", "trail.", "../a", "a/b"}) {
            bool rejected = false;
            try { (void)validatedRecoveryPath(name); }
            catch (const std::runtime_error&) { rejected = true; }
            require(rejected, "existing recovery restriction regressed");
        }
        // A refused recovered name is checked where it lies: no copy of it
        // is made, so none is freed unwiped.
        {
            const std::string refused = "SECRET-RECOVERED-NAME-\x1B[31m-long-enough-for-the-heap.txt";
            watched_name = refused.c_str();
            bool rejected = false;
            try { (void)validatedRecoveryPath(refused); }
            catch (const std::runtime_error&) { rejected = true; }
            watched_name = nullptr;
            require(rejected, "unsafe recovery filename accepted");
            require(!freed_name_copy, "refusing a recovered filename freed an unwiped copy of it");
        }
        // A recovered name may be up to 255 bytes, Linux's NAME_MAX. When it
        // is taken, the numbered candidates must still fit: the stem is cut
        // back, never inside a UTF-8 character, and the extension kept.
        {
            const fs::path previous = fs::current_path();
            const fs::path dir = fs::temp_directory_path() /
                ("jdvrif-name-max-" + std::to_string(::getpid()));
            fs::create_directories(dir);
            fs::current_path(dir);
            struct Restore {
                fs::path previous, dir;
                ~Restore() {
                    std::error_code ec;
                    fs::current_path(previous, ec);
                    fs::remove_all(dir, ec);
                }
            } restore{previous, dir};

            std::string accented;
            while (accented.size() + 2 + 4 <= 255) accented += "\u00E9";  // 2 bytes each
            for (const std::string& base : {std::string(251, 'a') + ".txt", accented + ".txt",
                                             std::string(255, 'b'), "c." + std::string(253, 'e')}) {
                require(base.size() <= 255 && validatedRecoveryPath(base) == fs::path(base),
                        "long test name rejected");
                std::ofstream(base) << "taken";
                for (int copy = 1; copy <= 2; ++copy) {
                    StagingFile staged({}, "t");
                    const fs::path committed = commitRecoveredOutput(staged, base);
                    const std::string name = committed.string();
                    require(name.size() <= 255, "numbered recovery name exceeds NAME_MAX");
                    require(name.ends_with("_" + std::to_string(copy) + (base.ends_with(".txt") ? ".txt" : "")),
                            "numbered recovery name lost its number or extension");
                    require(hasSafeEmbeddedFilename(name), "numbered recovery name is not valid UTF-8 text");
                    require(fs::exists(committed), "numbered recovery file missing");
                }
            }
        }

        // The top-level error sink: newlines kept, each line escaped on its
        // own, safe lines (already-escaped paths included) unchanged.
        require(terminalSafeMessage("Error: Line one.\n          Line \"two\" \\xE2.") ==
                    "Error: Line one.\n          Line \"two\" \\xE2.",
                "safe multi-line message changed");
        require(terminalSafeMessage("Bad: \x1B[31mred\nfine \u2713") == "Bad: \\x1B[31mred\nfine \u2713",
                "control sequence in a message not escaped");
        require(terminalSafeMessage("abc\u202Edef") == "abc\\xE2\\x80\\xAEdef", "BiDi override in a message not escaped");

        std::cout << "Filename safety tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
