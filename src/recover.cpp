#include "recover.h"
#include "recover_internal.h"
#include "embedded_layout.h"
#include "file_utils.h"
#include "recover_modes.h"
#include "reddit_steg.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>

namespace {
// jdvrif always writes the ICC template at file offset 0, so the profile and
// jdvrif signatures can only ever sit at two fixed absolute offsets -- which is
// exactly what recoverFromIccPath re-checks before it will use the layout.
// Verifying them in place costs one short read; scanning for them costs a pass
// over an input that may be gigabytes and can never accept a hit anywhere else.
[[nodiscard]] bool hasEmbeddedIccProfile(const fs::path& image_file_path, std::size_t image_file_size) {
    constexpr std::size_t PREFIX_BYTES =
        DEFAULT_JDVRIF_SIGNATURE_INDEX_ABS + JDVRIF_SIGNATURE.size();
    static_assert(DEFAULT_ICC_SIGNATURE_INDEX_ABS + ICC_PROFILE_SIGNATURE.size() <= PREFIX_BYTES);

    if (image_file_size < PREFIX_BYTES) return false;

    std::array<Byte, PREFIX_BYTES> prefix{};
    std::ifstream input = openBinaryInputOrThrow(image_file_path, "Read Error: Failed to open image file.");
    readExactAt(input, 0, std::span<Byte>(prefix));

    return std::equal(
               ICC_PROFILE_SIGNATURE.begin(), ICC_PROFILE_SIGNATURE.end(),
               prefix.begin() + static_cast<std::ptrdiff_t>(DEFAULT_ICC_SIGNATURE_INDEX_ABS)) &&
           std::equal(
               JDVRIF_SIGNATURE.begin(), JDVRIF_SIGNATURE.end(),
               prefix.begin() + static_cast<std::ptrdiff_t>(DEFAULT_JDVRIF_SIGNATURE_INDEX_ABS));
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
    if (hasEmbeddedIccProfile(image_file_path, image_file_size)) {
        recoverFromIccPath(image_file_path, image_file_size, DEFAULT_ICC_SIGNATURE_INDEX_ABS);
        return;
    }

    if (auto jdvrif_sig_opt = findBlueskyHeaderSignature(image_file_path, image_file_size)) {
        recoverFromBlueskyPath(image_file_path, image_file_size, *jdvrif_sig_opt);
        return;
    }

    // Reddit strips APP metadata, so its jdvrif identity lives in two robust
    // DCT-carried layers: the rqsteg header/CRC and the inner JDVRIFR1
    // encrypted-envelope marker/CRC. Keep this last so metadata formats retain
    // their established routing, and bound the whole-image read to Reddit's
    // upload limit.
    //
    // This format cannot follow the "validate declared size, then PIN, then
    // extract" ordering the ICC and Bluesky paths use (see
    // recoverFromCipherExtractor). Its declared payload size lives in the DCT
    // coefficients themselves, so reaching it means decoding the carrier and
    // reading the header out of it -- work that necessarily happens before a
    // PIN can be asked for, vouched for only by the header's CRC, which any
    // attacker can compute. That is deliberate and bounded rather than
    // overlooked: MAX_IMAGE_PIXELS caps the coefficient count, carrierCapacity()
    // caps the declared payload against it, and REDDIT_UPLOAD_SIZE_LIMIT caps
    // the read below -- together holding the pre-PIN cost of a hostile image to
    // roughly a quarter of a gigabyte and under a second. Keep those three caps
    // in mind before raising any of them.
    if (image_file_size <= REDDIT_UPLOAD_SIZE_LIMIT) {
        const vBytes image = readFile(
            image_file_path,
            FileTypeCheck::embedded_image);
        if (auto envelope = extractRedditEnvelope(image)) {
            recoverFromRedditPath(std::move(*envelope));
            return;
        }
    }

    throw std::runtime_error("Image File Error: Signature check failure. This is not a valid jdvrif \"file-embedded\" image.");
}
