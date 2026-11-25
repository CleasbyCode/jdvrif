#include "twitter_steg.h"

#include "carrier_common.h"
#include "carrier_parallel.h"
#include "encryption_internal.h"
#include "signal_utils.h"
#include "twitter_jpeg_codec.h"
#include "twitter_juniward.h"
#include "twitter_stc.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using twitter_steg_internal::CoefficientImage;
using twitter_steg_internal::ImageInfo;
using carrier_common::crc32;
using carrier_common::CarrierPrf;

// JXSTEG4: keyed by the 128-bit carrier key through SipHash-2-4 (CarrierPrf).
// Positions are a keyed Fisher-Yates shuffle of the candidates
// (KeyedIndexPrefix, domain LAYOUT_DOMAIN), one SipHash draw per position.
// The magic sits inside the whitened, keyed header, so a header only parses
// under the key that wrote it.
constexpr std::array<Byte, 8> CARRIER_MAGIC{'J', 'X', 'S', 'T', 'E', 'G', '4', 0};
constexpr Byte CARRIER_VERSION = 4;

constexpr carrier_common::EnvelopeFormat TWITTER_ENVELOPE_FORMAT{
    .name = "X-Twitter",
    .magic = {'J', 'D', 'V', 'R', 'I', 'F', 'X', '2'},
    .kdf_metadata_bytes = KDF_METADATA_REGION_BYTES,
};

constexpr std::size_t
    CARRIER_HEADER_BITS = carrier_common::CARRIER_HEADER_BYTES * 8U,
    CARRIER_HEADER_SYMBOLS =
        (CARRIER_HEADER_BITS * twitter_steg_internal::kRateDenominator +
         twitter_steg_internal::kRateNumerator - 1U) /
        twitter_steg_internal::kRateNumerator;

constexpr std::uint64_t
    LAYOUT_DOMAIN = 0x4a58535445474c31ULL,
    HEADER_DOMAIN = 0x4a58535445474831ULL,
    PAYLOAD_DOMAIN = 0x4a58535445475031ULL,
    HEADER_DIRECTION_DOMAIN = 0x4a58535445474448ULL,
    PAYLOAD_DIRECTION_DOMAIN = 0x4a58535445474450ULL;

constexpr std::uint32_t MIN_CARRIER_DIMENSION = 400;

[[noreturn]] void fail(std::string_view message) {
    throw std::runtime_error(std::string(message));
}

// What extraction accepts: progressive YCbCr 4:2:0 with source-derived tables
// no finer than Q97, at least MIN_CARRIER_DIMENSION on each side.
[[nodiscard]] bool isCompatibleExtractCarrier(const ImageInfo& info) noexcept {
    return info.components == 3 &&
        info.is_ycbcr &&
        info.progressive &&
        info.is_420 &&
        info.estimated_quality >= 1 &&
        info.estimated_quality <= 97 &&
        info.width >= MIN_CARRIER_DIMENSION &&
        info.height >= MIN_CARRIER_DIMENSION;
}

// Extraction refuses anything else (isCompatibleExtractCarrier), so never
// produce it: a caller of this API that skipped the CLI's own cover checks
// would otherwise embed into an image it can never recover.
void validatePreparedCarrier(const ImageInfo& info) {
    if (isCompatibleExtractCarrier(info)) return;
    if (info.width < MIN_CARRIER_DIMENSION || info.height < MIN_CARRIER_DIMENSION) {
        fail(
            "Image Error: For platform compatibility, cover image dimensions "
            "must be at least 400x400 pixels.");
    }
    fail(
        "Internal Error: X-Twitter carrier preparation did not produce "
        "progressive YCbCr 4:2:0 with valid source-derived quantization "
        "tables no finer than Q97.");
}

// Embedding's rate budget uses the cover's nonzero AC count. The keyed shuffle
// preserves both tallies, so preparation and embedding agree on this capacity.
// Embedding can turn +/-1 into zero, however, so extraction must not reuse it.
[[nodiscard]] std::uint64_t calculateEmbeddingCapacity(
    const twitter_steg_internal::CarrierCandidateCounts& counts) {
    if (counts.candidate_count < CARRIER_HEADER_SYMBOLS) return 0;

    const std::uint64_t rate_budget =
        counts.nonzero_ac_count * twitter_steg_internal::kRateNumerator /
        twitter_steg_internal::kRateDenominator;
    if (rate_budget <= CARRIER_HEADER_BITS) return 0;

    const std::uint64_t remaining_candidates =
        counts.candidate_count - CARRIER_HEADER_SYMBOLS;
    const std::uint64_t candidate_bit_limit =
        remaining_candidates * twitter_steg_internal::kRateNumerator /
        twitter_steg_internal::kRateDenominator;
    const std::uint64_t payload_bits = std::min(
        rate_budget - CARRIER_HEADER_BITS,
        candidate_bit_limit);
    return payload_bits / 8U;
}

// The JXSTEG4 header layout (see carrier_common.h).
constexpr carrier_common::CarrierHeaderFormat HEADER_FORMAT{
        .name = "X-Twitter",
        .magic = CARRIER_MAGIC,
        .parameters = {
            CARRIER_VERSION,
            static_cast<Byte>(twitter_steg_internal::kStcConstraintHeight),
            static_cast<Byte>(twitter_steg_internal::kRateNumerator),
            static_cast<Byte>(twitter_steg_internal::kRateDenominator),
        },
};

[[nodiscard]] vBytes bytesToBits(std::span<const Byte> bytes) {
    if (bytes.size() > std::numeric_limits<std::size_t>::max() / 8U) {
        fail("File Size Error: X-Twitter carrier bit-vector size overflow.");
    }
    vBytes bits(bytes.size() * 8U);
    for (std::size_t bit = 0; bit < bits.size(); ++bit) {
        bits[bit] = static_cast<Byte>(
            (bytes[bit / 8U] >> (bit % 8U)) & 1U);
    }
    return bits;
}

[[nodiscard]] vBytes bitsToBytes(std::span<const Byte> bits) {
    if (bits.size() % 8U != 0) {
        fail("File Extraction Error: X-Twitter carrier bit count is invalid.");
    }
    vBytes bytes(bits.size() / 8U, 0);
    for (std::size_t bit = 0; bit < bits.size(); ++bit) {
        if (bits[bit] != 0) {
            bytes[bit / 8U] |= static_cast<Byte>(1U << (bit % 8U));
        }
    }
    return bytes;
}

void whitenBits(
    std::span<Byte> bits,
    const CarrierPrf& prf,
    std::uint64_t domain) noexcept {

    // One 64-bit keystream word per block of 64 bits; the last block may be short.
    for (const auto [block, block_bits] :
         bits | std::views::chunk(64) | std::views::enumerate) {
        const std::uint64_t stream = prf(domain, static_cast<std::uint64_t>(block));
        for (const auto [offset, bit] : block_bits | std::views::enumerate) {
            bit ^= static_cast<Byte>((stream >> offset) & 1U);
        }
    }
}

// The keyed carrier order over the candidates, drawn only as far as it is
// read. Only those entries are mapped to coefficient offsets, so neither the
// candidate list nor its permutation is ever built for the whole image.
class KeyedCandidateOrder {
public:
    KeyedCandidateOrder(const CoefficientImage& coefficients, const CarrierPrf& prf)
        : candidates_(coefficients), order_(candidates_.size(), prf, LAYOUT_DOMAIN) {}

    [[nodiscard]] std::uint64_t size() const noexcept { return candidates_.size(); }

    // Offsets of the order's entries [first, first + count): the carrier's
    // layout, so wiped wherever it is released.
    [[nodiscard]] carrier_common::WipedVector<std::uint32_t> offsets(std::size_t first, std::size_t count) {
        if (first > size() || count > size() - first) {
            fail(
                "Data File Size Error: X-Twitter carrier selection exceeds "
                "the cover image's coefficient capacity.");
        }
        order_.extendTo(first + count, throwIfSignalCancellationRequested);
        const std::span<const std::uint32_t> indices = order_.prefix().subspan(first, count);
        carrier_common::WipedVector<std::uint32_t> result(count);
        // Each entry is mapped on its own, so the threads cannot change it.
        carrier_parallel::forEach(0, static_cast<std::int64_t>(count), 1 << 16, [&](std::int64_t entry) {
            const auto at = static_cast<std::size_t>(entry);
            result[at] = candidates_[indices[at]];
        });
        return result;
    }

private:
    twitter_steg_internal::CandidateOffsets candidates_;
    carrier_common::KeyedIndexPrefix order_;
};

} // namespace

void inspectTwitterCover(std::span<const Byte> input_jpeg) {
    (void)twitter_steg_internal::inspectJpeg(input_jpeg);
}

template<class Input>
static TwitterPreparedCover prepareTwitterCoverImpl(Input&& input_jpeg) {
    throwIfSignalCancellationRequested();
    ImageInfo source_info;
    // The coefficient-domain fast path hands back the prepared coefficients;
    // only a pixel-domain re-encode needs the prepared image decoded again.
    CoefficientImage coefficients;
    vBytes prepared = twitter_steg_internal::prepareProgressiveSourceQuality(
        std::forward<Input>(input_jpeg), &coefficients, &source_info);
    throwIfSignalCancellationRequested();
    if (coefficients.luminance.empty()) {
        coefficients = twitter_steg_internal::readCoefficients(prepared);
    }
    validatePreparedCarrier(coefficients.info);
    // Capacity needs only the two tallies, which do not depend on the keyed
    // order, so skip building and permuting the offset array here.
    // embedTwitterPayload checks the payload against this capacity and builds
    // the keyed order it actually uses, which bounds every selection it draws.
    const std::uint64_t capacity = calculateEmbeddingCapacity(
        twitter_steg_internal::countCarrierCandidates(coefficients));
    if (capacity > std::numeric_limits<std::size_t>::max()) {
        fail("File Size Error: X-Twitter carrier capacity is too large.");
    }

    const std::uint32_t width = coefficients.info.width;
    const std::uint32_t height = coefficients.info.height;
    const int carrier_quality = coefficients.info.estimated_quality;
    return TwitterPreparedCover{
        .jpeg = std::move(prepared),
        .coefficients = std::move(coefficients),
        .width = width,
        .height = height,
        .payload_capacity = static_cast<std::size_t>(capacity),
        .source_quality = source_info.estimated_quality,
        .carrier_quality = carrier_quality,
    };
}

TwitterPreparedCover prepareTwitterCover(std::span<const Byte> input_jpeg) {
    return prepareTwitterCoverImpl(input_jpeg);
}

TwitterPreparedCover prepareTwitterCover(vBytes&& input_jpeg) {
    return prepareTwitterCoverImpl(std::move(input_jpeg));
}

std::size_t twitterEnvelopeSize(std::size_t encrypted_size) {
    return carrier_common::envelopeSize(TWITTER_ENVELOPE_FORMAT, encrypted_size);
}

vBytes makeTwitterEnvelope(
    std::span<const Byte> kdf_metadata,
    std::span<const Byte> encrypted_data,
    bool is_compressed) {

    return carrier_common::makeEnvelope(TWITTER_ENVELOPE_FORMAT, kdf_metadata, encrypted_data, is_compressed);
}

vBytes embedTwitterPayload(
    TwitterPreparedCover&& cover,
    const CarrierKey128& carrier_key,
    std::span<const Byte> payload) {

    if (payload.empty()) {
        fail("Internal Error: X-Twitter payload is empty.");
    }
    if (payload.size() > cover.payload_capacity) {
        fail(
            "Data File Size Error: Encrypted X-Twitter payload exceeds the "
            "cover image's theoretical J-UNIWARD/STC capacity.");
    }
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        fail("Data File Size Error: Encrypted X-Twitter payload exceeds its format limit.");
    }

    throwIfSignalCancellationRequested();
    // Taken over, not re-decoded: prepareTwitterCover already read these out
    // of the same JPEG, and applyParityBits mutates them below.
    CoefficientImage coefficients = std::move(cover.coefficients);
    validatePreparedCarrier(coefficients.info);
    const CarrierPrf prf = CarrierPrf::keyed(carrier_key);

    vBytes header = carrier_common::makeCarrierHeader(
        HEADER_FORMAT,
        static_cast<std::uint32_t>(payload.size()),
        crc32(payload));
    WipeBytesGuard header_wipe{header};
    vBytes header_bits = bytesToBits(header);
    WipeBytesGuard header_bits_wipe{header_bits};
    vBytes payload_bits = bytesToBits(payload);
    WipeBytesGuard payload_bits_wipe{payload_bits};
    whitenBits(header_bits, prf, HEADER_DOMAIN);
    whitenBits(payload_bits, prf, PAYLOAD_DOMAIN);

    const std::size_t header_symbols = CARRIER_HEADER_SYMBOLS;
    const std::size_t payload_symbols = static_cast<std::size_t>(
        twitter_steg_internal::requiredCoverSymbols(payload_bits.size()));
    if (payload_symbols > std::numeric_limits<std::size_t>::max() - header_symbols) {
        fail("File Size Error: X-Twitter carrier selection size overflow.");
    }
    const std::size_t selected_count = header_symbols + payload_symbols;
    // The order is a temporary: once the selected offsets are copied out, its
    // index array (4 bytes per candidate, 66 MiB at 4096x4096 when the payload
    // is large enough to make it dense) is released before the cost and
    // trellis stages allocate theirs.
    const carrier_common::WipedVector<std::uint32_t> selected_offset_list =
        KeyedCandidateOrder(coefficients, prf).offsets(0, selected_count);
    const std::span<const std::uint32_t> selected_offsets(selected_offset_list);

    // From the coefficients in hand, before they are embedded into: the same
    // pixels as decoding cover.jpeg again, without the second entropy decode.
    const vBytes luminance = twitter_steg_internal::luminanceFromCoefficients(coefficients);
    // Costs and parities in keyed order, and the trellis output: all wiped.
    std::vector<float> costs =
        twitter_steg_internal::computeJUniwardCosts(
            luminance,
            coefficients,
            selected_offsets);
    const carrier_common::WipeOnExit costs_wipe{costs};
    throwIfSignalCancellationRequested();

    vBytes cover_bits = twitter_steg_internal::coefficientParityBits(
        coefficients.luminance,
        selected_offsets);
    WipeBytesGuard cover_bits_wipe{cover_bits};
    twitter_steg_internal::StcEmbedResult header_result =
        twitter_steg_internal::stcEmbed(
            std::span<const Byte>(cover_bits).first(header_symbols),
            std::span<const float>(costs).first(header_symbols),
            header_bits);
    WipeBytesGuard header_stego_wipe{header_result.stego_bits};
    twitter_steg_internal::StcEmbedResult payload_result =
        twitter_steg_internal::stcEmbed(
            std::span<const Byte>(cover_bits).subspan(header_symbols),
            std::span<const float>(costs).subspan(header_symbols),
            payload_bits);
    WipeBytesGuard payload_stego_wipe{payload_result.stego_bits};

    const std::uint64_t header_changes =
        twitter_steg_internal::applyParityBits(
            coefficients.luminance,
            selected_offsets.first(header_symbols),
            header_result.stego_bits,
            prf(HEADER_DIRECTION_DOMAIN, 0));
    const std::uint64_t payload_changes =
        twitter_steg_internal::applyParityBits(
            coefficients.luminance,
            selected_offsets.subspan(header_symbols),
            payload_result.stego_bits,
            prf(PAYLOAD_DIRECTION_DOMAIN, 0));
    if (header_changes != header_result.changes ||
        payload_changes != payload_result.changes) {
        fail(
            "Internal Error: X-Twitter STC/coefficient change count mismatch.");
    }

    vBytes output = twitter_steg_internal::writeProgressiveCoefficients(
        cover.jpeg,
        coefficients.luminance);
    validatePreparedCarrier(twitter_steg_internal::inspectJpeg(output));
    throwIfSignalCancellationRequested();
    return output;
}

namespace {

// Decode the carrier from already-read coefficients.
// nullopt means "no header under this key"; anything past a parsed header that
// does not check out is corruption and throws.
[[nodiscard]] std::optional<vBytes> extractWithKey(
    const CoefficientImage& coefficients,
    const CarrierPrf& prf) {

    // The keyed order is fixed lazily: the header's positions first, the
    // payload's only once the header has declared its length.
    KeyedCandidateOrder order(coefficients, prf);

    constexpr std::size_t header_symbols = CARRIER_HEADER_SYMBOLS;
    const carrier_common::WipedVector<std::uint32_t> header_offsets = order.offsets(0, header_symbols);
    vBytes header_stego = twitter_steg_internal::coefficientParityBits(
        coefficients.luminance,
        header_offsets);
    WipeBytesGuard header_stego_wipe{header_stego};
    vBytes header_bits = twitter_steg_internal::stcExtract(
        header_stego,
        CARRIER_HEADER_BITS);
    WipeBytesGuard header_bits_wipe{header_bits};
    whitenBits(header_bits, prf, HEADER_DOMAIN);
    vBytes header_bytes = bitsToBytes(header_bits);
    WipeBytesGuard header_bytes_wipe{header_bytes};
    const std::optional<carrier_common::CarrierHeader> header =
        carrier_common::parseCarrierHeader(HEADER_FORMAT, header_bytes);
    if (!header) return std::nullopt;

    const std::uint64_t payload_bit_count =
        static_cast<std::uint64_t>(header->payload_size) * 8U;
    const std::uint64_t payload_symbol_count =
        twitter_steg_internal::requiredCoverSymbols(payload_bit_count);
    // Candidate positions are invariant under parity changes, unlike the
    // nonzero AC count used to budget concealment. Bound the declared length
    // against available STC symbols before allocating payload-sized buffers.
    if (payload_symbol_count >
        order.size() - header_symbols) {
        fail(
            "File Extraction Error: X-Twitter carrier declares a payload "
            "larger than its coefficient capacity.");
    }
    if (payload_bit_count > std::numeric_limits<std::size_t>::max() ||
        payload_symbol_count > std::numeric_limits<std::size_t>::max()) {
        fail("File Extraction Error: X-Twitter carrier payload is too large.");
    }

    const carrier_common::WipedVector<std::uint32_t> payload_offsets = order.offsets(
        header_symbols,
        static_cast<std::size_t>(payload_symbol_count));
    vBytes payload_stego = twitter_steg_internal::coefficientParityBits(
        coefficients.luminance,
        payload_offsets);
    WipeBytesGuard payload_stego_wipe{payload_stego};
    vBytes payload_bits = twitter_steg_internal::stcExtract(
        payload_stego,
        static_cast<std::size_t>(payload_bit_count));
    WipeBytesGuard payload_bits_wipe{payload_bits};
    whitenBits(payload_bits, prf, PAYLOAD_DOMAIN);
    vBytes payload = bitsToBytes(payload_bits);
    if (crc32(payload) != header->payload_crc) {
        carrier_common::wipeAndRelease(payload);
        fail("File Extraction Error: X-Twitter carrier payload failed its integrity check.");
    }
    return payload;
}

} // namespace

std::optional<vBytes> extractTwitterPayload(
    std::span<const Byte> input_jpeg,
    const CarrierKey128& carrier_key) {

    throwIfSignalCancellationRequested();
    ImageInfo info;
    try {
        info = twitter_steg_internal::inspectJpeg(input_jpeg);
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
    if (!isCompatibleExtractCarrier(info)) return std::nullopt;

    const CoefficientImage coefficients =
        twitter_steg_internal::readCoefficients(input_jpeg);
    std::optional<vBytes> payload = extractWithKey(coefficients, CarrierPrf::keyed(carrier_key));
    // A cancellation now must not drop the payload unwiped: the caller's
    // guard is not attached until it returns. One load decides both the wipe
    // and the throw, so a signal landing in between cannot split them.
    if (const int signal_number = pendingSignalCancellation(); signal_number != 0) {
        if (payload) carrier_common::wipeAndRelease(*payload);
        throw SignalCancellation(signal_number);
    }
    return payload;
}

std::optional<TwitterEncryptedEnvelope> extractTwitterEnvelope(
    std::span<const Byte> input_jpeg,
    const CarrierKey128& carrier_key) {

    std::optional<vBytes> carrier_payload = extractTwitterPayload(input_jpeg, carrier_key);
    if (!carrier_payload) return std::nullopt;
    // Its parts are copied into the envelope; this copy goes now.
    WipeBytesGuard carrier_payload_wipe{*carrier_payload};
    return carrier_common::parseEnvelope(TWITTER_ENVELOPE_FORMAT, *carrier_payload);
}
