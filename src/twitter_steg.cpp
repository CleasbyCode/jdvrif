#include "twitter_steg.h"

#include "encryption_internal.h"
#include "signal_utils.h"
#include "twitter_jpeg_codec.h"
#include "twitter_juniward.h"
#include "twitter_stc.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

using twitter_steg_internal::CarrierLayout;
using twitter_steg_internal::CoefficientImage;
using twitter_steg_internal::ImageInfo;

constexpr auto CARRIER_MAGIC = std::to_array<Byte>({
    'J', 'X', 'S', 'T', 'E', 'G', '2', 0
});
constexpr auto TWITTER_ENVELOPE_MAGIC = std::to_array<Byte>({
    'J', 'D', 'V', 'R', 'I', 'F', 'X', '2'
});

constexpr Byte
    CARRIER_VERSION = 2,
    ENVELOPE_FLAG_COMPRESSED = 1;

constexpr std::size_t
    CARRIER_HEADER_SIZE = 24,
    CARRIER_HEADER_BITS = CARRIER_HEADER_SIZE * 8U,
    CARRIER_HEADER_SYMBOLS =
        (CARRIER_HEADER_BITS * twitter_steg_internal::kRateDenominator +
         twitter_steg_internal::kRateNumerator - 1U) /
        twitter_steg_internal::kRateNumerator,
    ENVELOPE_PREFIX_BYTES = 16,
    TWITTER_ENVELOPE_HEADER_BYTES =
        ENVELOPE_PREFIX_BYTES + KDF_METADATA_REGION_BYTES;

constexpr std::uint64_t
    LAYOUT_DOMAIN = 0x4a58535445474c31ULL,
    HEADER_DOMAIN = 0x4a58535445474831ULL,
    PAYLOAD_DOMAIN = 0x4a58535445475031ULL,
    HEADER_DIRECTION_DOMAIN = 0x4a58535445474448ULL,
    PAYLOAD_DIRECTION_DOMAIN = 0x4a58535445474450ULL;

constexpr std::uint32_t MIN_CARRIER_DIMENSION = 400;
constexpr const char* TWITTER_CORRUPT_ERROR =
    "File Extraction Error: X-Twitter embedded data is corrupt!";

struct Capacity {
    std::uint64_t candidate_count{0};
    std::uint64_t nonzero_ac_count{0};
    std::uint64_t payload_bytes{0};
};

struct CarrierHeader {
    std::uint32_t payload_size{0};
    std::uint32_t payload_crc{0};
};

[[noreturn]] void fail(std::string_view message) {
    throw std::runtime_error(std::string(message));
}

[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] std::uint32_t crc32(std::span<const Byte> data) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const Byte byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

void appendU32(vBytes& output, std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<Byte>((value >> shift) & 0xffU));
    }
}

[[nodiscard]] std::uint32_t readU32(
    std::span<const Byte> input,
    std::size_t offset) {

    if (offset > input.size() || input.size() - offset < 4U) {
        fail("File Extraction Error: X-Twitter carrier header is truncated.");
    }
    std::uint32_t value = 0;
    for (unsigned int byte = 0; byte < 4; ++byte) {
        value |= static_cast<std::uint32_t>(input[offset + byte]) <<
            (byte * 8U);
    }
    return value;
}

[[nodiscard]] std::size_t checkedAdd(
    std::size_t lhs,
    std::size_t rhs,
    std::string_view error_message) {

    if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        fail(error_message);
    }
    return lhs + rhs;
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

// Depends only on the candidate tallies, both of which are invariant under the
// keyed shuffle -- so the unkeyed count in prepareTwitterCover and the keyed
// layout in embedTwitterPayload necessarily agree on capacity.
[[nodiscard]] Capacity calculateCapacity(
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

[[nodiscard]] Capacity calculateCapacity(const CarrierLayout& layout) {
    return calculateCapacity(
        layout.coefficient_offsets.size(),
        layout.nonzero_ac_count);
}

[[nodiscard]] vBytes makeCarrierHeader(
    std::uint32_t payload_size,
    std::uint32_t payload_crc) {

    vBytes header;
    header.reserve(CARRIER_HEADER_SIZE);
    header.insert(header.end(), CARRIER_MAGIC.begin(), CARRIER_MAGIC.end());
    header.push_back(CARRIER_VERSION);
    header.push_back(static_cast<Byte>(
        twitter_steg_internal::kStcConstraintHeight));
    header.push_back(static_cast<Byte>(twitter_steg_internal::kRateNumerator));
    header.push_back(static_cast<Byte>(
        twitter_steg_internal::kRateDenominator));
    appendU32(header, payload_size);
    appendU32(header, payload_crc);
    appendU32(header, crc32(header));
    if (header.size() != CARRIER_HEADER_SIZE) {
        fail("Internal Error: X-Twitter carrier header has an invalid size.");
    }
    return header;
}

[[nodiscard]] std::optional<CarrierHeader> parseCarrierHeader(
    std::span<const Byte> header) {

    if (header.size() != CARRIER_HEADER_SIZE) {
        fail("File Extraction Error: X-Twitter carrier header has an invalid size.");
    }
    if (!std::equal(CARRIER_MAGIC.begin(), CARRIER_MAGIC.end(), header.begin())) {
        return std::nullopt;
    }
    // CRC first, as in reddit_steg.cpp's parseCarrierHeader: until the header
    // has vouched for itself, a garbled version/rate byte says nothing about
    // the format and everything about the bits being wrong. Reporting it as
    // "unsupported carrier format" would send the user looking for a different
    // jdvrif release over what is really a damaged image.
    if (crc32(header.first(20)) != readU32(header, 20)) {
        fail("File Extraction Error: X-Twitter carrier header failed its integrity check.");
    }
    if (header[8] != CARRIER_VERSION ||
        header[9] != static_cast<Byte>(
            twitter_steg_internal::kStcConstraintHeight) ||
        header[10] != static_cast<Byte>(twitter_steg_internal::kRateNumerator) ||
        header[11] != static_cast<Byte>(
            twitter_steg_internal::kRateDenominator)) {
        fail("File Extraction Error: Unsupported X-Twitter carrier format.");
    }
    return CarrierHeader{
        .payload_size = readU32(header, 12),
        .payload_crc = readU32(header, 16),
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

    for (std::size_t block = 0; block * 64U < bits.size(); ++block) {
        const std::uint64_t stream = mix64(
            carrier_key ^ domain ^ static_cast<std::uint64_t>(block));
        const std::size_t first = block * 64U;
        const std::size_t count = std::min<std::size_t>(
            64U,
            bits.size() - first);
        for (std::size_t offset = 0; offset < count; ++offset) {
            bits[first + offset] ^= static_cast<Byte>(
                (stream >> offset) & 1U);
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
    const Capacity capacity = calculateCapacity(
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
    return checkedAdd(
        TWITTER_ENVELOPE_HEADER_BYTES,
        encrypted_size,
        "File Size Error: X-Twitter encrypted-envelope size overflow.");
}

vBytes makeTwitterEnvelope(
    std::span<const Byte> kdf_metadata,
    std::span<const Byte> encrypted_data,
    bool is_compressed) {

    if (kdf_metadata.size() != KDF_METADATA_REGION_BYTES) {
        fail("Internal Error: X-Twitter KDF metadata has an invalid size.");
    }
    if (encrypted_data.size() > std::numeric_limits<std::uint32_t>::max()) {
        fail("File Size Error: X-Twitter encrypted payload exceeds its format limit.");
    }

    vBytes envelope;
    envelope.reserve(twitterEnvelopeSize(encrypted_data.size()));
    envelope.insert(
        envelope.end(),
        TWITTER_ENVELOPE_MAGIC.begin(),
        TWITTER_ENVELOPE_MAGIC.end());
    envelope.push_back(is_compressed ? ENVELOPE_FLAG_COMPRESSED : 0);
    envelope.insert(envelope.end(), 3, 0);
    appendU32(envelope, static_cast<std::uint32_t>(encrypted_data.size()));
    envelope.insert(envelope.end(), kdf_metadata.begin(), kdf_metadata.end());
    envelope.insert(envelope.end(), encrypted_data.begin(), encrypted_data.end());
    if (envelope.size() != twitterEnvelopeSize(encrypted_data.size())) {
        fail("Internal Error: X-Twitter encrypted envelope has an invalid size.");
    }
    return envelope;
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

    const Capacity keyed_capacity = calculateCapacity(layout);
    if (payload.size() > keyed_capacity.payload_bytes) {
        fail(
            "Data File Size Error: Encrypted X-Twitter payload exceeds the "
            "cover image's theoretical J-UNIWARD/STC capacity.");
    }

    vBytes header_bits = bytesToBits(makeCarrierHeader(
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

std::optional<vBytes> extractTwitterPayload(
    std::span<const Byte> input_jpeg,
    std::uint64_t carrier_key) {

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
    const std::optional<CarrierHeader> header = parseCarrierHeader(
        bitsToBytes(header_bits));
    if (!header) return std::nullopt;

    const Capacity capacity = calculateCapacity(layout);
    if (header->payload_size > capacity.payload_bytes) {
        fail(
            "File Extraction Error: X-Twitter carrier declares a payload "
            "larger than its coefficient capacity.");
    }
    const std::uint64_t payload_bit_count =
        static_cast<std::uint64_t>(header->payload_size) * 8U;
    const std::uint64_t payload_symbol_count =
        twitter_steg_internal::requiredCoverSymbols(payload_bit_count);
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
    throwIfSignalCancellationRequested();
    return payload;
}

std::optional<TwitterEncryptedEnvelope> extractTwitterEnvelope(
    std::span<const Byte> input_jpeg,
    std::uint64_t carrier_key) {

    std::optional<vBytes> carrier_payload =
        extractTwitterPayload(input_jpeg, carrier_key);
    if (!carrier_payload) return std::nullopt;

    const std::span<const Byte> payload(*carrier_payload);
    if (payload.size() < TWITTER_ENVELOPE_MAGIC.size() ||
        !std::equal(
            TWITTER_ENVELOPE_MAGIC.begin(),
            TWITTER_ENVELOPE_MAGIC.end(),
            payload.begin())) {
        return std::nullopt;
    }
    if (payload.size() < TWITTER_ENVELOPE_HEADER_BYTES) {
        fail(TWITTER_CORRUPT_ERROR);
    }

    const Byte flags = payload[8];
    if ((flags & static_cast<Byte>(~ENVELOPE_FLAG_COMPRESSED)) != 0 ||
        payload[9] != 0 ||
        payload[10] != 0 ||
        payload[11] != 0) {
        fail("File Extraction Error: X-Twitter envelope contains unsupported flags.");
    }

    const std::size_t encrypted_size = readU32(payload, 12);
    const std::size_t expected_size = checkedAdd(
        TWITTER_ENVELOPE_HEADER_BYTES,
        encrypted_size,
        TWITTER_CORRUPT_ERROR);
    if (payload.size() != expected_size) {
        fail(TWITTER_CORRUPT_ERROR);
    }

    TwitterEncryptedEnvelope envelope;
    envelope.kdf_metadata.assign(
        payload.begin() + static_cast<std::ptrdiff_t>(ENVELOPE_PREFIX_BYTES),
        payload.begin() + static_cast<std::ptrdiff_t>(
            TWITTER_ENVELOPE_HEADER_BYTES));
    envelope.encrypted_data.assign(
        payload.begin() + static_cast<std::ptrdiff_t>(
            TWITTER_ENVELOPE_HEADER_BYTES),
        payload.end());
    envelope.is_compressed =
        (flags & ENVELOPE_FLAG_COMPRESSED) != 0;
    return envelope;
}
