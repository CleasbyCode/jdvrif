#include "recover_output.h"

#include <array>
#include <charconv>
#include <format>
#include <print>
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
    if (ec != std::errc{}) {
        throw std::runtime_error("Write Error: Unable to create a unique output filename.");
    }

    next_name.append(stem);
    next_name.push_back('_');
    next_name.append(attempt_buf.data(), ptr);
    next_name.append(ext);
    return fs::path(next_name);
}
} // namespace

fs::path validatedRecoveryPath(std::string decrypted_filename) {
    if (decrypted_filename.empty()) {
        throw std::runtime_error("File Extraction Error: Recovered filename is unsafe.");
    }

    fs::path parsed(std::move(decrypted_filename));
    if (parsed.has_root_path() ||
        parsed.has_parent_path() ||
        parsed != parsed.filename() ||
        !hasSafeEmbeddedFilename(parsed)) {
        throw std::runtime_error("File Extraction Error: Recovered filename is unsafe.");
    }

    return parsed.filename();
}

fs::path tempRecoveryPath(const fs::path& output_path) {
    constexpr std::size_t MAX_ATTEMPTS = 1024;
    const fs::path parent = output_path.parent_path();
    const std::string base = output_path.filename().string();
    return uniqueRandomizedPathOrThrow(
        parent,
        std::format(".{}.jdvrif_tmp_", base),
        "",
        MAX_ATTEMPTS,
        "Write Error: Unable to allocate a temporary output filename.");
}

fs::path commitRecoveredOutput(TempFileCleanupGuard& staged_file, const fs::path& base_output_path) {
    constexpr std::size_t MAX_ATTEMPTS = 10000;

    for (std::size_t attempt = 0; attempt <= MAX_ATTEMPTS; ++attempt) {
        const fs::path candidate = makeRecoveryCandidate(base_output_path, attempt);
        if (tryCommitStagedFileNoReplace(
                staged_file.path,
                candidate,
                "Write Error: Failed to commit recovered file")) {
            staged_file.dismiss();
            return candidate;
        }
    }

    throw std::runtime_error("Write Error: Unable to create a unique output filename.");
}

void printRecoverySuccess(const fs::path& output_path, std::size_t output_size) {
    std::println("\nExtracted hidden file: {} ({} bytes).\n\nComplete! Please check your file.\n",
                output_path.string(), output_size);
}
