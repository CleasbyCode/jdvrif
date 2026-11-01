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
#include <span>
#include <stdexcept>

namespace {
// jdvrif writes both metadata templates at file offset 0. Bluesky keeps that
// layout, so its signature is checked in place. The default ICC template may
// sit behind segments an upload pipeline prepended (see
// findDefaultIccSignatureIndex), so the prefix read covers that bounded walk.
// Scanning the whole input instead would cost a pass over a file that may be
// gigabytes and could never accept a hit anywhere else.
constexpr std::size_t SIGNATURE_PREFIX_BYTES = DEFAULT_SIGNATURE_PREFIX_BYTES;
static_assert(BLUESKY_JDVRIF_SIGNATURE_INDEX_ABS + JDVRIF_SIGNATURE.size() <= SIGNATURE_PREFIX_BYTES);

[[nodiscard]] bool hasSignatureAt(std::span<const Byte> prefix, std::size_t offset, std::span<const Byte> sig) {
    return spanHasRange(prefix, offset, sig.size()) &&
           std::ranges::equal(prefix.subspan(offset, sig.size()), sig);
}
} // namespace

void recoverData(const fs::path& image_file_path) {
    const std::size_t image_file_size = validateFileForRead(image_file_path, FileTypeCheck::embedded_image);

    vBytes prefix_buffer(std::min(image_file_size, SIGNATURE_PREFIX_BYTES));
    const std::span<Byte> prefix(prefix_buffer);
    readExactAt(image_file_path, 0, prefix);

    if (const auto icc_index = findDefaultIccSignatureIndex(prefix)) {
        recoverFromIccPath(image_file_path, image_file_size, *icc_index);
        return;
    }

    if (hasSignatureAt(prefix, BLUESKY_JDVRIF_SIGNATURE_INDEX_ABS, JDVRIF_SIGNATURE)) {
        recoverFromBlueskyPath(image_file_path, image_file_size, BLUESKY_JDVRIF_SIGNATURE_INDEX_ABS);
        return;
    }

    // Reddit and X-Twitter strip APP metadata, so their jdvrif identities live
    // in keyed DCT-carried headers plus inner JDVRIFR1/JDVRIFX2 encrypted
    // envelopes. Keep this last so metadata formats retain their established
    // routing, and bound the whole-image read to Reddit's larger upload limit.
    //
    // The carrier's coefficient positions derive from the recovery PIN, so the
    // PIN has to come first: without it there is nothing to locate. A wrong PIN
    // and an image with no carrier therefore share one message. That hides
    // where a payload is, not whether one exists -- see reddit_steg.cpp on what
    // statistical steganalysis can still see.
    //
    // The carrier key costs one Argon2id run, so testing a KEY guess against
    // the carrier costs as much as testing it against the envelope.
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
            FileTypeCheck::embedded_image,
            REDDIT_UPLOAD_SIZE_LIMIT);
        CarrierKey128 carrier_key = deriveCarrierKey128FromPin(recovery_pin);
        WipePodGuard<CarrierKey128> carrier_key_wipe{carrier_key};

        if (image.size() <= TWITTER_UPLOAD_SIZE_LIMIT) {
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
            "File Extraction Error: Invalid KEY, or this is not a valid jdvrif \"file-embedded\" image.");
    }

    throw std::runtime_error("Image File Error: Signature check failure. This is not a valid jdvrif \"file-embedded\" image.");
}
