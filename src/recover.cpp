#include "recover.h"
#include "recover_internal.h"
#include "embedded_layout.h"
#include "file_utils.h"
#include "encryption_internal.h"
#include "pin_input.h"
#include "recover_modes.h"
#include "reddit_steg.h"
#include "twitter_steg.h"

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

    // Reddit and X-Twitter strip APP metadata, so their jdvrif identities live
    // in keyed DCT-carried headers plus inner JDVRIFR1/JDVRIFX2 encrypted
    // envelopes. Keep this last so metadata formats retain their established
    // routing, and bound the whole-image read to Reddit's larger upload limit.
    //
    // The carrier's coefficient positions derive from the recovery PIN, so there
    // is no way to answer "is there a payload here?" before asking for it. That
    // is the property being bought: a wrong PIN and an ordinary JPEG are
    // indistinguishable to anyone but the holder, so they share one message.
    //
    // Asking first also means a hostile image cannot make this process decode a
    // carrier at all until a PIN has been entered. The work is still bounded
    // once it starts: each codec caps dimensions/coefficient counts, each
    // carrier caps its declared payload against coefficient capacity, and the
    // platform upload limits cap the read below. Keep those caps in mind before
    // raising any of them.
    if (image_file_size <= REDDIT_UPLOAD_SIZE_LIMIT) {
        SecurePin recovery_pin = getPin();
        const vBytes image = readFile(
            image_file_path,
            FileTypeCheck::embedded_image);
        const std::uint64_t carrier_key =
            deriveCarrierKeyFromPin(recovery_pin);

        if (image_file_size <= TWITTER_UPLOAD_SIZE_LIMIT) {
            if (auto envelope = extractTwitterEnvelope(image, carrier_key)) {
                recoverFromTwitterPath(
                    std::move(*envelope),
                    std::move(recovery_pin));
                return;
            }
        }
        if (auto envelope = extractRedditEnvelope(image, carrier_key)) {
            recoverFromRedditPath(std::move(*envelope), std::move(recovery_pin));
            return;
        }
        throw std::runtime_error(
            "File Extraction Error: Invalid PIN, or this is not a valid jdvrif \"file-embedded\" image.");
    }

    throw std::runtime_error("Image File Error: Signature check failure. This is not a valid jdvrif \"file-embedded\" image.");
}
