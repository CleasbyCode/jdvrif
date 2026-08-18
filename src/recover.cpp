#include "recover.h"
#include "recover_internal.h"
#include "embedded_layout.h"
#include "file_utils.h"
#include "recover_modes.h"

#include <algorithm>
#include <optional>
#include <stdexcept>

namespace {
[[nodiscard]] std::optional<std::size_t> findEmbeddedIccProfile(const fs::path& image_file_path) {
    return findSignaturePairInFile(
        image_file_path,
        ICC_PROFILE_SIGNATURE,
        JDVRIF_SIGNATURE,
        JDVRIF_TO_ICC_SIGNATURE_OFFSET);
}

[[nodiscard]] std::optional<std::size_t> findBlueskyHeaderSignature(
    const fs::path& image_file_path,
    std::size_t image_file_size) {

    const std::size_t header_search_limit = std::min(
        image_file_size,
        BLUESKY_CIPHER_LAYOUT.encrypted_payload_start_index);
    return findSignatureInFile(image_file_path, JDVRIF_SIGNATURE, header_search_limit, 0);
}
} // namespace

void recoverData(const fs::path& image_file_path) {
    const std::size_t image_file_size = validateFileForRead(image_file_path, FileTypeCheck::embedded_image);
    if (auto icc_opt = findEmbeddedIccProfile(image_file_path)) {
        recoverFromIccPath(image_file_path, image_file_size, *icc_opt);
        return;
    }

    if (auto jdvrif_sig_opt = findBlueskyHeaderSignature(image_file_path, image_file_size)) {
        recoverFromBlueskyPath(image_file_path, image_file_size, *jdvrif_sig_opt);
        return;
    }

    throw std::runtime_error("Image File Error: Signature check failure. This is not a valid jdvrif \"file-embedded\" image.");
}
