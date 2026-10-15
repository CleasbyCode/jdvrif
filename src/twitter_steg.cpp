#include "twitter_steg.h"

#include "carrier_common.h"
#include "encryption_internal.h"
#include "file_utils.h"
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
#include <vector>

namespace {

using twitter_steg_internal::CarrierLayout;
using twitter_steg_internal::CoefficientImage;
using twitter_steg_internal::ImageInfo;
using carrier_common::crc32;
using carrier_common::mix64;

// Carrier formats this build understands. Conceal writes only the current one.
// The two share every layout rule; they differ in the key that drives them:
//
//   JXSTEG3 (current)  keyed by the Argon2id carrier key.
//   JXSTEG2 (v9.0)     read-only; keyed by the cheap BLAKE2b key -- see
//                      CarrierKeys in carrier_common.h.
//
// The magic sits inside the whitened, keyed header, so a header can only ever
// parse under the format/key pair that wrote it.
struct CarrierFormat {
    std::array<Byte, 8> magic;
    Byte version;
};

constexpr CarrierFormat CARRIER_FORMAT_CURRENT{
    .magic = {'J', 'X', 'S', 'T', 'E', 'G', '3', 0},
    .version = 3,
};
constexpr CarrierFormat CARRIER_FORMAT_LEGACY_V2{
    .magic = {'J', 'X', 'S', 'T', 'E', 'G', '2', 0},
    .version = 2,
};

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

struct Capacity {
    std::uint64_t candidate_count{0};
    std::uint64_t nonzero_ac_count{0};
    std::uint64_t payload_bytes{0};
};

[[noreturn]] void fail(std::string_view message) {
    throw std::runtime_error(std::string(message));
}

void validatePreparedCarrier(const ImageInfo& info) {
    if (info.components != 3 || !info.progressive ||
        !info.is_ycbcr || !info.is_420 ||
        info.estimated_quality < 1 || info.estimated_quality > 97) {
        fail(
            "Internal Error: X-Twitter carrier preparation did not produce "
            "progressive YCbCr 4:2:0 with valid source-derived quantization "
            "tables no finer than Q97.");
    }
}

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

// Embedding's rate budget uses the cover's nonzero AC count. The keyed shuffle
// preserves both tallies, so preparation and embedding agree on this capacity.
// Embedding can turn +/-1 into zero, however, so extraction must not reuse it.
[[nodiscard]] Capacity calculateEmbeddingCapacity(
    std::uint64_t candidate_count,
    std::uint64_t nonzero_ac_count) {
    Capacity capacity{
        .candidate_count = candidate_count,
        .nonzero_ac_count = nonzero_ac_count,
        .payload_bytes = 0,
    };
    if (capacity.candidate_count < CARRIER_HEADER_SYMBOLS) return capacity;

    const std::uint64_t rate_budget =
        capacity.nonzero_ac_count * twitter_steg_internal::kRateNumerator /
        twitter_steg_internal::kRateDenominator;
    if (rate_budget <= CARRIER_HEADER_BITS) return capacity;

    const std::uint64_t remaining_candidates =
        capacity.candidate_count - CARRIER_HEADER_SYMBOLS;
    const std::uint64_t candidate_bit_limit =
        remaining_candidates * twitter_steg_internal::kRateNumerator /
        twitter_steg_internal::kRateDenominator;
    const std::uint64_t payload_bits = std::min(
        rate_budget - CARRIER_HEADER_BITS,
        candidate_bit_limit);
    capacity.payload_bytes = payload_bits / 8U;
    return capacity;
}

[[nodiscard]] Capacity calculateEmbeddingCapacity(const CarrierLayout& layout) {
    return calculateEmbeddingCapacity(
        layout.coefficient_offsets.size(),
        layout.nonzero_ac_count);
}

// The JXSTEG header layout (see carrier_common.h) under one format version.
[[nodiscard]] constexpr carrier_common::CarrierHeaderFormat headerFormat(const CarrierFormat& format) {
    return carrier_common::CarrierHeaderFormat{
        .name = "X-Twitter",
        .magic = format.magic,
        .parameters = {
            format.version,
            static_cast<Byte>(twitter_steg_internal::kStcConstraintHeight),
            static_cast<Byte>(twitter_steg_internal::kRateNumerator),
            static_cast<Byte>(twitter_steg_internal::kRateDenominator),
        },
    };
}

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
    std::uint64_t carrier_key,
    std::uint64_t domain) noexcept {

    // One 64-bit keystream word per block of 64 bits; the last block may be short.
    for (const auto [block, block_bits] :
         bits | std::views::chunk(64) | std::views::enumerate) {
        const std::uint64_t stream = mix64(
            carrier_key ^ domain ^ static_cast<std::uint64_t>(block));
        for (const auto [offset, bit] : block_bits | std::views::enumerate) {
            bit ^= static_cast<Byte>((stream >> offset) & 1U);
        }
    }
}

[[nodiscard]] std::span<const std::uint32_t> candidateSpan(
    const CarrierLayout& layout,
    std::size_t offset,
    std::size_t count) {

    if (offset > layout.coefficient_offsets.size() ||
        count > layout.coefficient_offsets.size() - offset) {
        fail(
            "Data File Size Error: X-Twitter carrier selection exceeds "
            "the cover image's coefficient capacity.");
    }
    return std::span<const std::uint32_t>(layout.coefficient_offsets)
        .subspan(offset, count);
}

} // namespace

TwitterCoverDimensions inspectTwitterCover(std::span<const Byte> input_jpeg) {
    const ImageInfo info = twitter_steg_internal::inspectJpeg(input_jpeg);
    return TwitterCoverDimensions{
        .width = info.width,
        .height = info.height,
    };
}

TwitterPreparedCover prepareTwitterCover(std::span<const Byte> input_jpeg) {
    throwIfSignalCancellationRequested();
    const ImageInfo source_info = twitter_steg_internal::inspectJpeg(
        input_jpeg);
    vBytes prepared = twitter_steg_internal::prepareProgressiveSourceQuality(
        input_jpeg);
    throwIfSignalCancellationRequested();

    CoefficientImage coefficients =
        twitter_steg_internal::readCoefficients(prepared);
    validatePreparedCarrier(coefficients.info);
    // Capacity needs only the two tallies, so skip building and permuting the
    // offset array here; embedTwitterPayload builds the keyed one it actually
    // uses, and re-checks the payload against it.
    const twitter_steg_internal::CarrierCandidateCounts counts =
        twitter_steg_internal::countCarrierCandidates(coefficients);
    const Capacity capacity = calculateEmbeddingCapacity(
        counts.candidate_count,
        counts.nonzero_ac_count);
    if (capacity.payload_bytes > std::numeric_limits<std::size_t>::max()) {
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
        .candidate_count = capacity.candidate_count,
        .nonzero_ac_count = capacity.nonzero_ac_count,
        .payload_capacity = static_cast<std::size_t>(capacity.payload_bytes),
        .source_quality = source_info.estimated_quality,
        .carrier_quality = carrier_quality,
    };
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
    const TwitterPreparedCover& cover,
    std::uint64_t carrier_key,
    std::span<const Byte> payload) {

    if (payload.size() > cover.payload_capacity) {
        fail(
            "Data File Size Error: Encrypted X-Twitter payload exceeds the "
            "cover image's theoretical J-UNIWARD/STC capacity.");
    }
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        fail("Data File Size Error: Encrypted X-Twitter payload exceeds its format limit.");
    }

    throwIfSignalCancellationRequested();
    // Copied, not re-decoded: prepareTwitterCover already read these out of the
    // same JPEG, and applyParityBits mutates them below. A copy of the
    // luminance array is far cheaper than a second progressive decode.
    CoefficientImage coefficients = cover.coefficients;
    validatePreparedCarrier(coefficients.info);
    const CarrierLayout layout = twitter_steg_internal::makeCarrierLayout(
        coefficients,
        mix64(carrier_key ^ LAYOUT_DOMAIN));

    const Capacity keyed_capacity = calculateEmbeddingCapacity(layout);
    if (payload.size() > keyed_capacity.payload_bytes) {
        fail(
            "Data File Size Error: Encrypted X-Twitter payload exceeds the "
            "cover image's theoretical J-UNIWARD/STC capacity.");
    }

    vBytes header_bits = bytesToBits(carrier_common::makeCarrierHeader(
        headerFormat(CARRIER_FORMAT_CURRENT),
        static_cast<std::uint32_t>(payload.size()),
        crc32(payload)));
    WipeBytesGuard header_bits_wipe{header_bits};
    vBytes payload_bits = bytesToBits(payload);
    WipeBytesGuard payload_bits_wipe{payload_bits};
    whitenBits(header_bits, carrier_key, HEADER_DOMAIN);
    whitenBits(payload_bits, carrier_key, PAYLOAD_DOMAIN);

    const std::size_t header_symbols = static_cast<std::size_t>(
        twitter_steg_internal::requiredCoverSymbols(header_bits.size()));
    const std::size_t payload_symbols = static_cast<std::size_t>(
        twitter_steg_internal::requiredCoverSymbols(payload_bits.size()));
    const std::size_t selected_count = checkedAdd(
        header_symbols,
        payload_symbols,
        "File Size Error: X-Twitter carrier selection size overflow.");
    const auto selected_offsets = candidateSpan(layout, 0, selected_count);

    const vBytes luminance = twitter_steg_internal::decodeLuminance(
        cover.jpeg);
    const std::vector<float> costs =
        twitter_steg_internal::computeJUniwardCosts(
            luminance,
            coefficients,
            selected_offsets);
    throwIfSignalCancellationRequested();

    const vBytes cover_bits = twitter_steg_internal::coefficientParityBits(
        coefficients.luminance,
        selected_offsets);
    const twitter_steg_internal::StcEmbedResult header_result =
        twitter_steg_internal::stcEmbed(
            std::span<const Byte>(cover_bits).first(header_symbols),
            std::span<const float>(costs).first(header_symbols),
            header_bits);
    const twitter_steg_internal::StcEmbedResult payload_result =
        twitter_steg_internal::stcEmbed(
            std::span<const Byte>(cover_bits).subspan(header_symbols),
            std::span<const float>(costs).subspan(header_symbols),
            payload_bits);

    const std::uint64_t header_changes =
        twitter_steg_internal::applyParityBits(
            coefficients.luminance,
            selected_offsets.first(header_symbols),
            header_result.stego_bits,
            mix64(carrier_key ^ HEADER_DIRECTION_DOMAIN));
    const std::uint64_t payload_changes =
        twitter_steg_internal::applyParityBits(
            coefficients.luminance,
            selected_offsets.subspan(header_symbols),
            payload_result.stego_bits,
            mix64(carrier_key ^ PAYLOAD_DIRECTION_DOMAIN));
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

// Decode the carrier under one format/key pair from already-read coefficients.
// nullopt means "no header under this key"; anything past a parsed header that
// does not check out is corruption and throws.
[[nodiscard]] std::optional<vBytes> extractWithFormat(
    const CoefficientImage& coefficients,
    const CarrierFormat& format,
    std::uint64_t carrier_key) {

    const CarrierLayout layout = twitter_steg_internal::makeCarrierLayout(
        coefficients,
        mix64(carrier_key ^ LAYOUT_DOMAIN));

    constexpr std::size_t header_symbols = CARRIER_HEADER_SYMBOLS;
    const auto header_offsets = candidateSpan(layout, 0, header_symbols);
    const vBytes header_stego = twitter_steg_internal::coefficientParityBits(
        coefficients.luminance,
        header_offsets);
    vBytes header_bits = twitter_steg_internal::stcExtract(
        header_stego,
        CARRIER_HEADER_BITS);
    WipeBytesGuard header_bits_wipe{header_bits};
    whitenBits(header_bits, carrier_key, HEADER_DOMAIN);
    const std::optional<carrier_common::CarrierHeader> header =
        carrier_common::parseCarrierHeader(headerFormat(format), bitsToBytes(header_bits));
    if (!header) return std::nullopt;

    const std::uint64_t payload_bit_count =
        static_cast<std::uint64_t>(header->payload_size) * 8U;
    const std::uint64_t payload_symbol_count =
        twitter_steg_internal::requiredCoverSymbols(payload_bit_count);
    // Candidate positions are invariant under parity changes, unlike the
    // nonzero AC count used to budget concealment. Bound the declared length
    // against available STC symbols before allocating payload-sized buffers.
    if (payload_symbol_count >
        layout.coefficient_offsets.size() - header_symbols) {
        fail(
            "File Extraction Error: X-Twitter carrier declares a payload "
            "larger than its coefficient capacity.");
    }
    if (payload_bit_count > std::numeric_limits<std::size_t>::max() ||
        payload_symbol_count > std::numeric_limits<std::size_t>::max()) {
        fail("File Extraction Error: X-Twitter carrier payload is too large.");
    }

    const auto payload_offsets = candidateSpan(
        layout,
        header_symbols,
        static_cast<std::size_t>(payload_symbol_count));
    const vBytes payload_stego = twitter_steg_internal::coefficientParityBits(
        coefficients.luminance,
        payload_offsets);
    vBytes payload_bits = twitter_steg_internal::stcExtract(
        payload_stego,
        static_cast<std::size_t>(payload_bit_count));
    WipeBytesGuard payload_bits_wipe{payload_bits};
    whitenBits(payload_bits, carrier_key, PAYLOAD_DOMAIN);
    vBytes payload = bitsToBytes(payload_bits);
    if (crc32(payload) != header->payload_crc) {
        fail("File Extraction Error: X-Twitter carrier payload failed its integrity check.");
    }
    return payload;
}

} // namespace

std::optional<vBytes> extractTwitterPayload(
    std::span<const Byte> input_jpeg,
    const CarrierKeys& keys) {

    throwIfSignalCancellationRequested();
    ImageInfo info;
    try {
        info = twitter_steg_internal::inspectJpeg(input_jpeg);
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
    if (!isCompatibleExtractCarrier(info)) return std::nullopt;

    // One coefficient decode serves both formats: current under the Argon2id
    // key first, then the v9.0 format under the legacy key.
    const CoefficientImage coefficients =
        twitter_steg_internal::readCoefficients(input_jpeg);
    std::optional<vBytes> payload =
        extractWithFormat(coefficients, CARRIER_FORMAT_CURRENT, keys.current);
    if (!payload) {
        payload = extractWithFormat(coefficients, CARRIER_FORMAT_LEGACY_V2, keys.legacy);
    }
    throwIfSignalCancellationRequested();
    return payload;
}

std::optional<TwitterEncryptedEnvelope> extractTwitterEnvelope(
    std::span<const Byte> input_jpeg,
    const CarrierKeys& keys) {

    const std::optional<vBytes> carrier_payload = extractTwitterPayload(input_jpeg, keys);
    if (!carrier_payload) return std::nullopt;
    return carrier_common::parseEnvelope(TWITTER_ENVELOPE_FORMAT, *carrier_payload);
}
