#include "recover_output.h"

#include <array>
#include <charconv>
#include <string_view>
#include <stdexcept>
#include <system_error>

namespace {
[[nodiscard]] fs::path makeRecoveryCandidate(const fs::path& base_path, std::size_t attempt) {
    if (attempt == 0) return base_path;

    std::string stem = base_path.stem().string();
    if (stem.empty()) stem = "recovered";
    const std::string ext = base_path.extension().string();
    std::string next_name;
    next_name.reserve(stem.size() + 1 + 20 + ext.size());

    std::array<char, 32> attempt_buf{};
    const auto [ptr, ec] = std::to_chars(attempt_buf.data(), attempt_buf.data() + attempt_buf.size(), attempt);
    throwIf(ec != std::errc{}, "Write Error: Unable to create a unique output filename.");

    const std::string_view number(attempt_buf.data(), static_cast<std::size_t>(ptr - attempt_buf.data()));

    // A recovered name may be up to 255 bytes (its length field is one byte),
    // which is also Linux's NAME_MAX, so "_N" can push it past what the
    // filesystem accepts. Cut the stem back to fit, never inside a UTF-8
    // character. An extension too long to leave the stem a byte is treated
    // as part of the stem.
    constexpr std::size_t NAME_MAX_BYTES = 255;
    const std::size_t suffix_bytes = 1 + number.size();
    std::string tail = ext;
    if (suffix_bytes + tail.size() >= NAME_MAX_BYTES) {
        stem += tail;
        tail.clear();
    }
    const std::size_t stem_budget = NAME_MAX_BYTES - suffix_bytes - tail.size();
    if (stem.size() > stem_budget) {
        std::size_t cut = stem_budget;
        while (cut > 0 && (static_cast<unsigned char>(stem[cut]) & 0xC0U) == 0x80U) --cut;
        stem.resize(cut);
    }

    next_name.append(stem);
    next_name.push_back('_');
    next_name.append(number);
    next_name.append(tail);
    return fs::path(next_name);
}
} // namespace

fs::path validatedRecoveryPath(std::string_view decrypted_filename) {
    // Checked where it lies, so a refused name is never copied: the only copy
    // is DecryptResult's, which its destructor wipes. No '/' means no root or
    // parent: the name is its own filename component.
    throwIf(!isSafeEmbeddedFilename(decrypted_filename),
        "File Extraction Error: Recovered filename is unsafe.");
    return fs::path(decrypted_filename);
}

fs::path commitRecoveredOutput(const StagingFile& staged_file, const fs::path& base_output_path) {
    constexpr std::size_t MAX_ATTEMPTS = 10000;

    for (std::size_t attempt = 0; attempt <= MAX_ATTEMPTS; ++attempt) {
        const fs::path candidate = makeRecoveryCandidate(base_output_path, attempt);
        if (tryCommitStagingFileNoReplace(staged_file, candidate, "Write Error: Failed to commit recovered file")) {
            return candidate;
        }
    }

    throw std::runtime_error("Write Error: Unable to create a unique output filename.");
}
