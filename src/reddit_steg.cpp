#include "reddit_steg.h"
#include "carrier_common.h"
#include "carrier_parallel.h"
#include "jpeg_codec_common.h"

#include "encryption_internal.h"
#include "signal_utils.h"

#include <algorithm>
#include <array>
#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using carrier_common::CarrierPrf;
using carrier_common::crc32;
using carrier_common::scaledQuantTable;
using carrier_common::STD_LUMA_Q50;
using carrier_common::STD_CHROMA_Q50;

// RQSTEG3: positions, whitening and a per-coefficient QIM dither offset all
// derive from the 128-bit carrier key through SipHash-2-4 (CarrierPrf).
// Positions are a keyed Fisher-Yates shuffle of every candidate
// (KeyedIndexPrefix), one SipHash draw per position. The magic sits inside
// the whitened, keyed header, so a header only parses under the key that wrote it.
constexpr std::array<Byte, 8> CARRIER_MAGIC{'R', 'Q', 'S', 'T', 'E', 'G', '3', 0};
constexpr Byte CARRIER_VERSION = 3;

constexpr carrier_common::EnvelopeFormat REDDIT_ENVELOPE_FORMAT{
    .name = "Reddit",
    .magic = {'J', 'D', 'V', 'R', 'I', 'F', 'R', '1'},
    .kdf_metadata_bytes = KDF_METADATA_REGION_BYTES,
};

constexpr Byte
    HEADER_COPIES           = 17,
    PAYLOAD_COPIES          = 3;

constexpr int
    QIM_STEP           = 8,
    CARRIER_QUALITY    = 75,
    MIN_IMAGE_DIMENSION = 400;

// Carrier keying.
//
// Every coefficient position, whitening bit and dither offset derives,
// through CarrierPrf, from a 128-bit carrier key that is an Argon2id hash of
// the recovery KEY. Without the KEY the header cannot be located or
// read, and because the key costs a full Argon2id run, the header is no cheaper
// a KEY oracle than the encrypted envelope underneath it.
//
// What keying does NOT buy is undetectability. QIM moves every carrier
// coefficient onto a coarse lattice; the keyed dither spreads the lattice
// offsets uniformly so they do not cluster, but embedding still changes the coefficient statistics (most
// visibly, zero coefficients become rare in the carrier band as the payload
// grows). Statistical steganalysis can therefore still flag a carrier image.
// Positions and dither protect *where* the bits are, not *whether* they exist;
// the contents are secretstream-encrypted under the Argon2id key either way.
//
// HEADER_DOMAIN / PAYLOAD_DOMAIN key each carrier's whitening bit and dither
// (one hash per candidate position: bit 0 whitens, bits 1-3 dither). Position
// shuffles draw from separate domains, so no position draw ever reuses a whitening/dither word.
constexpr std::uint64_t
    HEADER_DOMAIN    = 0x4845414445527631ULL,
    PAYLOAD_DOMAIN   = 0x5041594c4f414431ULL,
    HEADER_LAYOUT_DOMAIN  = 0x5251534c41594831ULL, // "RQSLAYH1"
    PAYLOAD_LAYOUT_DOMAIN = 0x5251534c41595031ULL; // "RQSLAYP1"

constexpr std::size_t
    RQSTEG_HEADER_BITS      = carrier_common::CARRIER_HEADER_BYTES * 8;

// The header alone takes one luminance block per bit copy, so a cover with
// fewer blocks cannot carry it. MIN_SQUARE_COVER_SIDE is the smallest
// whole-block square that reaches that count, quoted in the error message.
constexpr std::uint64_t
    MIN_HEADER_LUMINANCE_BLOCKS = static_cast<std::uint64_t>(RQSTEG_HEADER_BITS) * HEADER_COPIES;

constexpr std::uint64_t MIN_SQUARE_COVER_SIDE = [] {
    std::uint64_t blocks_per_side = 1;
    while (blocks_per_side * blocks_per_side < MIN_HEADER_LUMINANCE_BLOCKS) ++blocks_per_side;
    return blocks_per_side * 8;
}();

// JPEG zig-zag position -> natural 8x8 coefficient-array position.
constexpr std::array<Byte, 64> ZIGZAG_TO_NATURAL{
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

constexpr std::array<Byte, 1> HEADER_FREQUENCIES{
    ZIGZAG_TO_NATURAL[4]
};

// Ten disjoint low/mid-frequency luminance AC lattices carry the payload.
constexpr std::array<Byte, 10> PAYLOAD_FREQUENCIES{
    ZIGZAG_TO_NATURAL[5],  ZIGZAG_TO_NATURAL[6],
    ZIGZAG_TO_NATURAL[7],  ZIGZAG_TO_NATURAL[8],
    ZIGZAG_TO_NATURAL[9],  ZIGZAG_TO_NATURAL[10],
    ZIGZAG_TO_NATURAL[11], ZIGZAG_TO_NATURAL[12],
    ZIGZAG_TO_NATURAL[13], ZIGZAG_TO_NATURAL[14]
};

struct ImageInfo : jpeg_codec::HeaderInfo {
    bool is_q75{false};
};

// One payload/header bit copy pinned to one luminance DCT coefficient.
//
// The block position is stored pre-split into row/column rather than as a flat
// block index: the two hot loops (applyCarriers / readCarriers) walk the image
// one block row at a time, so a flat index would cost a 64-bit division by a
// runtime divisor per carrier there. Splitting it here also gives makeCarriers
// the bucket key it needs to place carriers in row order without a comparison
// sort.
//
// Widths are checked against these field sizes in makeCarriers: at Reddit's
// 8192-pixel ceiling a block coordinate cannot exceed 1024, and bit_index is
// bounded by carrierCapacity() * 8. `dither` is the keyed QIM lattice offset
// (0..QIM_STEP-1).
struct Carrier {
    std::uint32_t bit_index{0};
    std::uint16_t block_row{0};
    std::uint16_t block_column{0};
    Byte coefficient_index{0};
    bool whitening_bit{false};
    Byte dither{0};
};

// 24 -> 12 bytes; the dither byte occupies what was padding. At
// REDDIT_COVER_IMAGE_LIMITS.max_pixels this is the program's largest
// allocation, so halving it halves the peak RSS of a Reddit conceal/recover.
static_assert(sizeof(Carrier) == 12, "Carrier is expected to stay a 12-byte record.");

struct DecodedSymbol {
    bool bit{false};
    int confidence{0};
};

// A bit's vote total: the sum of +-confidence over its copies. Confidence is
// at most QIM_STEP / 2 + 1 (two circular distances on a QIM_STEP lattice
// differ by at most QIM_STEP / 2), so even the header's 17 copies stay within
// a signed byte.
using Score = std::int8_t;
static_assert(static_cast<int>(std::max(HEADER_COPIES, PAYLOAD_COPIES)) * (QIM_STEP / 2 + 1) <=
                  std::numeric_limits<Score>::max(),
              "a bit's vote total must fit in Score");

// The C++ locals each codec entry point needs across a setjmp/longjmp round trip.
//
// libjpeg reports errors by longjmp'ing back to the setjmp point. An automatic
// object modified between setjmp and longjmp has an indeterminate value
// afterwards, and every object here has a non-trivial destructor that runs
// while fail()'s exception unwinds the setjmp frame -- destroying a container
// whose members are indeterminate is precisely what that rule forbids. Holding
// them on the heap behind a pointer established *before* setjmp puts them out
// of longjmp's reach: the pointer is never reassigned, so its value survives,
// and the objects it owns are untouched by the jump.
//
// jpeg_codec::CodecState and ErrorManager are already heap-allocated for the
// same reason; this extends the treatment to the rest. (ImageInfo and the plain
// scalars stay on the stack: they are trivially destructible and are never
// read on the longjmp path, so no indeterminate value is ever observed.)
//
// The carrier sets (keyed positions, whitening bits, dithers) and the vote
// totals use WipingAllocator, so they are zeroed whenever they release memory.
// The decoded bytes keep vBytes for the code they are handed to, and the
// destructor wipes what is left of them.
struct CodecLocals {
    CodecLocals() = default;
    CodecLocals(const CodecLocals&) = delete;
    CodecLocals& operator=(const CodecLocals&) = delete;
    ~CodecLocals() {
        carrier_common::wipeAndRelease(header_bytes);
        carrier_common::wipeAndRelease(payload_bytes);
    }

    // The header's carrier set, then the payload's: never both at once.
    carrier_common::WipedVector<Carrier> carriers;
    // readCarriers' per-bit vote totals. Kept here, not in that helper, because
    // its access_virt_barray calls can longjmp straight past a local vector.
    // One byte each (see Score): the carriers are row-ordered, so these are
    // read and written in random order, and a quarter of the footprint keeps
    // far more of them in cache.
    carrier_common::WipedVector<Score> scores;
    vBytes header_bytes;
    vBytes payload_bytes;
    std::optional<carrier_common::CarrierHeader> header;
};

constexpr const char* REDDIT_CORRUPT_ERROR =
    "File Extraction Error: Reddit embedded data is corrupt!";

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

// The RQSTEG3 header layout (see carrier_common.h).
constexpr carrier_common::CarrierHeaderFormat HEADER_FORMAT{
        .name = "Reddit",
        .magic = CARRIER_MAGIC,
        .parameters = {CARRIER_VERSION, static_cast<Byte>(QIM_STEP), PAYLOAD_COPIES, 0},
};

[[nodiscard]] bool getBit(std::span<const Byte> bytes, std::uint64_t bit_index) {
    const std::size_t byte_index = static_cast<std::size_t>(bit_index / 8U);
    const unsigned int shift = static_cast<unsigned int>(bit_index % 8U);
    return ((bytes[byte_index] >> shift) & 1U) != 0U;
}

void setBit(vBytes& bytes, std::uint64_t bit_index, bool value) {
    const std::size_t byte_index = static_cast<std::size_t>(bit_index / 8U);
    const unsigned int shift = static_cast<unsigned int>(bit_index % 8U);
    const Byte mask = static_cast<Byte>(1U << shift);
    if (value) {
        bytes[byte_index] |= mask;
    } else {
        bytes[byte_index] &= static_cast<Byte>(~mask);
    }
}

// Candidates are indexed as 32-bit values (KeyedIndexPrefix's domain limit).
constexpr std::uint64_t MAX_CANDIDATE_COUNT = std::numeric_limits<std::uint32_t>::max();

void requirePermutationDomain(std::uint64_t count) {
    if (count == 0) {
        fail("Image Error: Carrier image has no usable coefficient positions.");
    }
    if (count > MAX_CANDIDATE_COUNT) {
        fail("Internal Error: Reddit carrier permutation domain is too large.");
    }
}

// Build the carrier set already grouped by block row, ready for the row-walking
// loops below.
//
// A `candidate` in [0, candidate_count) encodes position as
//     candidate = ((row * blocks_wide) + column) * FrequencyCount + frequency
// so row = candidate / (blocks_wide * FrequencyCount) and the remainder splits
// into column and frequency by a compile-time constant divisor.
//
// Placement is a two-pass counting sort on that row, not a comparison sort:
// pass one tallies per-row bucket sizes, pass two recomputes each candidate and
// scatters it straight into its bucket. That is O(slots + blocks_high) into a
// single exact-sized allocation -- no temporary copy of the carrier array, and
// no O(n log n) pass over what is the program's largest allocation. Both passes
// run in parallel over chunks of slots without changing the result.
//
// Each candidate's whitening bit and QIM dither offset come
// from one keyed hash of its position, so they are fixed per coefficient and
// never depend on which bit happens to land there.
template<std::size_t FrequencyCount>
[[nodiscard]] carrier_common::WipedVector<Carrier> makeCarriers(
    const ImageInfo& info,
    const std::array<Byte, FrequencyCount>& frequencies,
    std::uint64_t bit_count,
    Byte copies,
    const CarrierPrf& prf,
    std::uint64_t domain,
    std::uint64_t layout_domain) {

    const std::uint64_t copy_count = copies;
    if (bit_count != 0 && copy_count > std::numeric_limits<std::uint64_t>::max() / bit_count) {
        fail("Internal Error: Reddit carrier-count overflow.");
    }
    const std::uint64_t slots = bit_count * copy_count;
    const std::uint64_t block_count = info.yBlockCount();
    if (block_count > std::numeric_limits<std::uint64_t>::max() / FrequencyCount) {
        fail("Internal Error: Reddit coefficient-count overflow.");
    }
    const std::uint64_t candidate_count =
        block_count * static_cast<std::uint64_t>(FrequencyCount);
    if (slots > candidate_count) {
        fail("Data File Size Error: Encrypted payload exceeds this cover image's C3 coefficient capacity.");
    }
    if (slots > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        fail("Internal Error: Reddit carrier allocation exceeds addressable memory.");
    }
    // Carrier's field widths (see its definition). Both hold with a wide margin
    // under REDDIT_COVER_IMAGE_LIMITS / carrierCapacity, so a failure here
    // means a limit elsewhere moved without this record following it.
    if (bit_count > std::numeric_limits<std::uint32_t>::max() ||
        info.y_blocks_wide > std::numeric_limits<std::uint16_t>::max() ||
        info.y_blocks_high > std::numeric_limits<std::uint16_t>::max()) {
        fail("Internal Error: Reddit carrier geometry exceeds its record layout.");
    }

    // requirePermutationDomain caps candidate_count below 2^32, so every candidate
    // fits in 32 bits and the row split below stays 32-bit arithmetic. It also
    // rejects an empty domain, which is the only way y_blocks_wide could be
    // zero -- so row_stride is a safe divisor from here on.
    //
    // Slot s goes to candidate shuffled[s]: the first `slots` entries of a
    // keyed shuffle of every candidate. Only that prefix is held (plus the
    // entries it displaced), unless it covers a large share of the domain.
    requirePermutationDomain(candidate_count);
    carrier_common::KeyedIndexPrefix shuffle(candidate_count, prf, layout_domain);
    shuffle.extendTo(static_cast<std::size_t>(slots), throwIfSignalCancellationRequested);
    const std::span<const std::uint32_t> shuffled = shuffle.prefix();
    const auto candidateAt = [&](std::uint64_t slot) -> std::uint32_t {
        return shuffled[static_cast<std::size_t>(slot)];
    };
    const std::uint32_t row_stride =
        static_cast<std::uint32_t>(FrequencyCount) * info.y_blocks_wide;

    // The slots are cut into contiguous chunks that run in parallel. Pass one
    // tallies each chunk's carriers per row; the cursor for (chunk, row) is
    // then the row's start plus the earlier chunks' tallies for that row, so
    // pass two lays the carriers out exactly as one sequential walk would --
    // each row in slot order -- whatever the thread count.
    // At least 64 Ki slots per chunk, and at most 256 chunks.
    const std::size_t rows = info.y_blocks_high;
    const std::uint64_t chunk_slots = std::max<std::uint64_t>(1U << 16U, (slots + 255U) / 256U);
    const std::size_t chunks = static_cast<std::size_t>((slots + chunk_slots - 1U) / chunk_slots);
    // Every chunk's row tallies, then its write cursors: about 1.3 MiB at the
    // size limit (1,024 block rows, 160 chunks).
    // How many carriers each row holds follows from the keyed layout too.
    carrier_common::WipedVector<std::size_t> cursors(chunks * rows, 0);
    const auto chunkSlots = [&](std::size_t chunk) {
        const std::uint64_t first = chunk * chunk_slots;
        return std::pair{first, std::min<std::uint64_t>(slots, first + chunk_slots)};
    };
    const auto chunk_count = static_cast<std::int64_t>(chunks);

    // Pass one: per-chunk row tallies.
    carrier_parallel::forEach(0, chunk_count, 1, [&](std::int64_t chunk_index) {
        const auto chunk = static_cast<std::size_t>(chunk_index);
        std::size_t* const tally = cursors.data() + chunk * rows;
        const auto [first, last] = chunkSlots(chunk);
        for (std::uint64_t slot = first; slot < last; ++slot) {
            ++tally[candidateAt(slot) / row_stride];
        }
    });
    // Row by row, chunk by chunk: tallies become write cursors.
    std::size_t next_start = 0;
    for (std::size_t row = 0; row < rows; ++row) {
        for (std::size_t chunk = 0; chunk < chunks; ++chunk) {
            std::size_t& entry = cursors[chunk * rows + row];
            const std::size_t count = entry;
            entry = next_start;
            next_start += count;
        }
    }

    // Pass two: each chunk scatters its carriers to its own cursors. Slot
    // s = bit * copies + copy, so a chunk's slots follow (bit, copy) order.
    carrier_common::WipedVector<Carrier> carriers(static_cast<std::size_t>(slots));
    carrier_parallel::forEach(0, chunk_count, 1, [&](std::int64_t chunk_index) {
        const auto chunk = static_cast<std::size_t>(chunk_index);
        std::size_t* const cursor = cursors.data() + chunk * rows;
        const auto [first, last] = chunkSlots(chunk);
        for (std::uint64_t slot = first; slot < last; ++slot) {
            const std::uint64_t bit = slot / copy_count;
            const std::uint32_t candidate = candidateAt(slot);
            const std::uint32_t row = candidate / row_stride;
            const std::uint32_t within = candidate - (row * row_stride);
            const std::uint32_t column = within / static_cast<std::uint32_t>(FrequencyCount);
            const std::size_t frequency =
                static_cast<std::size_t>(within % FrequencyCount);
            const std::uint64_t position_hash = prf(domain, candidate);
            carriers[cursor[row]++] = Carrier{
                .bit_index = static_cast<std::uint32_t>(bit),
                .block_row = static_cast<std::uint16_t>(row),
                .block_column = static_cast<std::uint16_t>(column),
                .coefficient_index = frequencies[frequency],
                .whitening_bit = (position_hash & 1U) != 0U,
                .dither = static_cast<Byte>((position_hash >> 1U) % QIM_STEP),
            };
        }
    });

    // Within a row the carriers stay in slot order. Sorting them by column
    // would change nothing: every carrier names a different coefficient, the
    // decoder's scores are integer sums, and a block row's coefficients fit in
    // the L2 cache whatever the order (measured: the sort took a third of an
    // embed at capacity and made the walks no faster).
    return carriers;
}

[[nodiscard]] int floorDivide(int numerator, int denominator) noexcept {
    int quotient = numerator / denominator;
    const int remainder = numerator % denominator;
    if (remainder != 0 && ((remainder < 0) != (denominator < 0))) {
        --quotient;
    }
    return quotient;
}

// Bit 0 sits on the lattice QIM_STEP/4 + dither (mod QIM_STEP), bit 1 on
// 3*QIM_STEP/4 + dither. The two lattices stay QIM_STEP/2 apart whatever the
// dither, so robustness to requantization is unchanged; only the absolute
// residues move, and with a uniform keyed dither they no longer cluster.
[[nodiscard]] JCOEF qimEncode(JCOEF coefficient, bool bit, Byte dither) {
    constexpr int COEF_MIN = static_cast<int>(std::numeric_limits<JCOEF>::min());
    constexpr int COEF_MAX = static_cast<int>(std::numeric_limits<JCOEF>::max());

    const int value = coefficient;
    const int target = (bit ? (3 * QIM_STEP) / 4 : QIM_STEP / 4) + dither;
    const int quotient = floorDivide(value - target, QIM_STEP);
    const int lower = target + quotient * QIM_STEP;
    const int upper = lower + QIM_STEP;
    const int lower_distance = std::abs(value - lower);
    const int upper_distance = std::abs(value - upper);

    int chosen = lower;
    if (upper_distance < lower_distance) {
        chosen = upper;
    } else if (upper_distance == lower_distance) {
        if (value > 0 && upper > 0) {
            chosen = upper;
        } else if (value < 0 && lower < 0) {
            chosen = lower;
        } else if (std::abs(upper) < std::abs(lower)) {
            chosen = upper;
        }
    }

    // Never clamp into range: clamping moves the value off the QIM lattice,
    // which silently flips the bit this call was asked to encode. `lower` and
    // `upper` are both on the lattice and straddle `value`, so if one falls
    // outside JCOEF the other is a valid substitute carrying the same bit --
    // lower >= value - QIM_STEP and upper <= value + QIM_STEP, and `value` is
    // itself a JCOEF. Unreachable at Q75 (the AC positions this carrier writes
    // stay under ~600 against a +/-32767 range), but a lattice point is the
    // only correct answer here, so take the fallback rather than a wrong bit.
    // The two are at most 2 * QIM_STEP apart, far less than the JCOEF range, so
    // at most one of them can fall outside it.
    static_assert(2 * QIM_STEP < COEF_MAX - COEF_MIN);
    if (chosen > COEF_MAX) {
        chosen = lower;
    } else if (chosen < COEF_MIN) {
        chosen = upper;
    }
    return static_cast<JCOEF>(chosen);
}

[[nodiscard]] int positiveModulo(int value, int modulus) noexcept {
    const int remainder = value % modulus;
    return remainder < 0 ? remainder + modulus : remainder;
}

[[nodiscard]] int circularDistance(int lhs, int rhs, int modulus) noexcept {
    const int direct = std::abs(lhs - rhs);
    return std::min(direct, modulus - direct);
}

[[nodiscard]] DecodedSymbol qimDecode(JCOEF coefficient, Byte dither) noexcept {
    const int residue = positiveModulo(coefficient - dither, QIM_STEP);
    const int target_zero = QIM_STEP / 4;
    const int target_one = (3 * QIM_STEP) / 4;
    const int zero_distance =
        circularDistance(residue, target_zero, QIM_STEP);
    const int one_distance =
        circularDistance(residue, target_one, QIM_STEP);
    return DecodedSymbol{
        .bit = one_distance < zero_distance,
        .confidence = std::abs(one_distance - zero_distance) + 1,
    };
}

void installStandardQ75QuantTables(
    jpeg_compress_struct& destination) {

    // Scale 100 with baseline forcing stores each value as given: the Q75
    // tables are already scaled and lie within 1..255.
    jpeg_add_quant_table(
        &destination,
        0,
        scaledQuantTable(STD_LUMA_Q50, CARRIER_QUALITY).data(),
        100,
        TRUE);
    jpeg_add_quant_table(
        &destination,
        1,
        scaledQuantTable(STD_CHROMA_Q50, CARRIER_QUALITY).data(),
        100,
        TRUE);

    // Do not inherit component/table choices from an implementation-specific
    // jpeg_set_defaults(). The carrier format requires Y=0, Cb=1, Cr=1.
    destination.comp_info[0].quant_tbl_no = 0;
    destination.comp_info[1].quant_tbl_no = 1;
    destination.comp_info[2].quant_tbl_no = 1;
}

[[nodiscard]] bool quantTableMatches(
    const JQUANT_TBL* table,
    const std::array<unsigned int, 64>& expected) {

    if (table == nullptr) return false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (table->quantval[i] != expected[i]) return false;
    }
    return true;
}

[[nodiscard]] ImageInfo imageInfoFromHeader(const jpeg_decompress_struct& source) {
    const auto header = jpeg_codec::headerInfo(source);
    const bool has_three_components = header.components == 3;
    const auto expected_luma = scaledQuantTable(STD_LUMA_Q50, CARRIER_QUALITY);
    const auto expected_chroma = scaledQuantTable(STD_CHROMA_Q50, CARRIER_QUALITY);
    // Check the table each component is actually bound to: an encoder may
    // use any DQT slots, and tables merely present in slots 0/1 say nothing
    // about how Y, Cb and Cr were quantized.
    const bool is_q75 = has_three_components &&
        quantTableMatches(jpeg_codec::componentQuantTable(source, 0), expected_luma) &&
        quantTableMatches(jpeg_codec::componentQuantTable(source, 1), expected_chroma) &&
        quantTableMatches(jpeg_codec::componentQuantTable(source, 2), expected_chroma);

    ImageInfo info{header, is_q75};
    if (!has_three_components) {
        info.y_blocks_wide = 0;
        info.y_blocks_high = 0;
    }
    return info;
}

void validateCoverInput(const jpeg_decompress_struct& source) {
    const std::uint64_t width = source.image_width;
    const std::uint64_t height = source.image_height;
    if (source.num_components != 1 && source.num_components != 3) {
        fail("Image Error: Unsupported JPEG color space. CMYK/YCCK cover images must be converted to RGB before use.");
    }
    if (width < MIN_IMAGE_DIMENSION || height < MIN_IMAGE_DIMENSION) {
        fail("Image Error: For platform compatibility, cover image dimensions must be at least 400x400 pixels.");
    }
    // The carrier keeps the cover's dimensions, so its luminance block count
    // is known here. Say what is needed now, rather than leaving validateCarrier
    // to refuse the transcoded carrier without any guidance.
    const std::uint64_t luminance_blocks = ((width + 7) / 8) * ((height + 7) / 8);
    if (luminance_blocks < MIN_HEADER_LUMINANCE_BLOCKS) {
        fail("Image Error: Cover image is too small for the Reddit carrier. It has " +
             std::to_string(luminance_blocks) + " 8x8 pixel blocks and at least " +
             std::to_string(MIN_HEADER_LUMINANCE_BLOCKS) + " are needed: use a cover of " +
             std::to_string(MIN_SQUARE_COVER_SIDE) + "x" + std::to_string(MIN_SQUARE_COVER_SIDE) +
             " pixels or more.");
    }
    if (width > REDDIT_COVER_IMAGE_LIMITS.max_dimension ||
        height > REDDIT_COVER_IMAGE_LIMITS.max_dimension ||
        width * height > REDDIT_COVER_IMAGE_LIMITS.max_pixels) {
        fail("Image Error: Cover image dimensions exceed jdvrif's safe image limit.");
    }
}

[[nodiscard]] bool isCompatibleCarrier(const ImageInfo& info) noexcept {
    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(info.width) * info.height;
    return info.components == 3 &&
        !info.progressive &&
        info.is_ycbcr &&
        info.is_420 &&
        info.is_q75 &&
        info.width >= MIN_IMAGE_DIMENSION &&
        info.height >= MIN_IMAGE_DIMENSION &&
        info.width <= REDDIT_COVER_IMAGE_LIMITS.max_dimension &&
        info.height <= REDDIT_COVER_IMAGE_LIMITS.max_dimension &&
        pixel_count <= REDDIT_COVER_IMAGE_LIMITS.max_pixels &&
        info.yBlockCount() >= MIN_HEADER_LUMINANCE_BLOCKS;
}

void validateCarrier(const ImageInfo& info) {
    if (info.components != 3) {
        fail("Internal Error: Reddit carrier is not a three-component colour JPEG.");
    }
    if (info.progressive) {
        fail("Internal Error: Reddit carrier is not a baseline JPEG.");
    }
    if (!info.is_ycbcr) {
        fail("Internal Error: Reddit carrier is not a YCbCr JPEG.");
    }
    if (!info.is_420) {
        fail("Internal Error: Reddit carrier does not use YCbCr 4:2:0 subsampling.");
    }
    if (!info.is_q75) {
        fail("Internal Error: Reddit carrier does not use the standard quality-75 quantization tables.");
    }
    // The carrier keeps the cover's dimensions, which validateCoverInput
    // already bounded; re-check them here so validateCarrier accepts exactly
    // what isCompatibleCarrier will recover.
    const std::uint64_t pixel_count =
        static_cast<std::uint64_t>(info.width) * info.height;
    if (info.width < MIN_IMAGE_DIMENSION || info.height < MIN_IMAGE_DIMENSION) {
        fail("Internal Error: Reddit carrier is smaller than 400x400 pixels.");
    }
    if (info.width > REDDIT_COVER_IMAGE_LIMITS.max_dimension ||
        info.height > REDDIT_COVER_IMAGE_LIMITS.max_dimension ||
        pixel_count > REDDIT_COVER_IMAGE_LIMITS.max_pixels) {
        fail("Internal Error: Reddit carrier exceeds jdvrif's safe image limit.");
    }
    if (info.yBlockCount() < MIN_HEADER_LUMINANCE_BLOCKS) {
        fail("Image Error: Cover image is too small for the robust Reddit carrier header.");
    }
}

[[nodiscard]] ImageInfo inspectJpeg(std::span<const Byte> input) {
    return jpeg_codec::inspectHeader(input, imageInfoFromHeader);
}

[[nodiscard]] vBytes transcodeCarrier(std::span<const Byte> input) {
    return jpeg_codec::transcodeRgb420(input, validateCoverInput,
        [](const jpeg_decompress_struct&, jpeg_compress_struct& destination) {
            // The carrier requires exact IJG Q75 tables, baseline coding,
            // and no JFIF or Adobe markers.
            installStandardQ75QuantTables(destination);
            destination.optimize_coding = FALSE;
            destination.write_JFIF_header = FALSE;
            destination.write_Adobe_marker = FALSE;
        }, "JPEG error while preparing Reddit cover: ");
}

// makeCarriers already emits carriers grouped by block_row, so both walks
// below just stream the array. Order within a row does not matter.
void applyCarriers(
    jpeg_decompress_struct& source,
    jvirt_barray_ptr* coefficient_arrays,
    std::span<const Carrier> carriers,
    std::span<const Byte> bits) {

    const jpeg_component_info& y = source.comp_info[0];
    std::size_t carrier_index = 0;
    for (JDIMENSION row = 0;
         row < y.height_in_blocks && carrier_index < carriers.size();
         ++row) {

        throwIfSignalCancellationRequested();
        JBLOCKARRAY block_row = (*source.mem->access_virt_barray)(
            reinterpret_cast<j_common_ptr>(&source),
            coefficient_arrays[0],
            row,
            1,
            TRUE);
        while (carrier_index < carriers.size()) {
            const Carrier& carrier = carriers[carrier_index];
            if (carrier.block_row != row) break;

            const JDIMENSION column = static_cast<JDIMENSION>(carrier.block_column);
            const bool logical_bit = getBit(bits, carrier.bit_index);
            const bool encoded_bit = logical_bit ^ carrier.whitening_bit;
            JCOEF& coefficient =
                block_row[0][column][carrier.coefficient_index];
            coefficient = qimEncode(coefficient, encoded_bit, carrier.dither);
            ++carrier_index;
        }
    }
    if (carrier_index != carriers.size()) {
        fail("Internal Error: Reddit carrier index exceeded image bounds.");
    }
}

[[nodiscard]] vBytes readCarriers(
    jpeg_decompress_struct& source,
    jvirt_barray_ptr* coefficient_arrays,
    std::span<const Carrier> carriers,
    std::uint64_t bit_count,
    carrier_common::WipedVector<Score>& scores) {

    if (bit_count > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max())) {
        fail(REDDIT_CORRUPT_ERROR);
    }
    scores.assign(static_cast<std::size_t>(bit_count), 0);
    const jpeg_component_info& y = source.comp_info[0];

    std::size_t carrier_index = 0;
    for (JDIMENSION row = 0;
         row < y.height_in_blocks && carrier_index < carriers.size();
         ++row) {

        throwIfSignalCancellationRequested();
        JBLOCKARRAY block_row = (*source.mem->access_virt_barray)(
            reinterpret_cast<j_common_ptr>(&source),
            coefficient_arrays[0],
            row,
            1,
            FALSE);
        while (carrier_index < carriers.size()) {
            const Carrier& carrier = carriers[carrier_index];
            if (carrier.block_row != row) break;

            const JDIMENSION column = static_cast<JDIMENSION>(carrier.block_column);
            DecodedSymbol symbol = qimDecode(
                block_row[0][column][carrier.coefficient_index],
                carrier.dither);
            symbol.bit = symbol.bit ^ carrier.whitening_bit;
            const std::size_t bit =
                static_cast<std::size_t>(carrier.bit_index);
            scores[bit] = static_cast<Score>(scores[bit] +
                (symbol.bit ? symbol.confidence : -symbol.confidence));
            ++carrier_index;
        }
    }
    if (carrier_index != carriers.size()) {
        fail(REDDIT_CORRUPT_ERROR);
    }

    vBytes decoded(static_cast<std::size_t>((bit_count + 7U) / 8U), 0);
    for (std::uint64_t bit = 0; bit < bit_count; ++bit) {
        setBit(
            decoded,
            bit,
            scores[static_cast<std::size_t>(bit)] >= 0);
    }
    return decoded;
}

[[nodiscard]] vBytes embedCarrier(
    std::span<const Byte> input,
    std::span<const Byte> header,
    std::span<const Byte> payload,
    const CarrierPrf& prf) {

    throwIfSignalCancellationRequested();
    jpeg_codec::requireMemorySource(input);
    auto error = std::make_unique<jpeg_codec::ErrorManager>();
    auto state = std::make_unique<jpeg_codec::CodecState>();
    auto locals = std::make_unique<CodecLocals>();

    jpeg_codec::install(*error);
    if (setjmp(error->jump) != 0) {
        jpeg_codec::throwAfterLongjmp(*error, *state, "JPEG error while embedding Reddit payload: ");
    }

    state->openSource(*error, input);
    jpeg_read_header(&state->source, TRUE);
    const ImageInfo info = imageInfoFromHeader(state->source);
    validateCarrier(info);
    jvirt_barray_ptr* arrays = jpeg_read_coefficients(&state->source);

    locals->carriers = makeCarriers(
        info,
        HEADER_FREQUENCIES,
        RQSTEG_HEADER_BITS,
        HEADER_COPIES,
        prf,
        HEADER_DOMAIN,
        HEADER_LAYOUT_DOMAIN);
    applyCarriers(state->source, arrays, locals->carriers, header);
    // Released before the payload set is built. The two sets never share a
    // coefficient, and at the capacity ceiling the payload array is the
    // largest allocation the program makes -- no reason to hold both.
    carrier_common::WipedVector<Carrier>{}.swap(locals->carriers);

    const std::uint64_t payload_bits =
        static_cast<std::uint64_t>(payload.size()) * 8U;
    locals->carriers = makeCarriers(
        info,
        PAYLOAD_FREQUENCIES,
        payload_bits,
        PAYLOAD_COPIES,
        prf,
        PAYLOAD_DOMAIN,
        PAYLOAD_LAYOUT_DOMAIN);
    applyCarriers(state->source, arrays, locals->carriers, payload);
    carrier_common::WipedVector<Carrier>{}.swap(locals->carriers);

    state->openDestination(*error);
    jpeg_copy_critical_parameters(&state->source, &state->destination);
    state->destination.write_JFIF_header = FALSE;
    state->destination.write_Adobe_marker = FALSE;
    state->destination.optimize_coding = FALSE;
    jpeg_write_coefficients(&state->destination, arrays);
    jpeg_finish_compress(&state->destination);
    jpeg_finish_decompress(&state->source);
    // libjpeg is done: nothing can longjmp past here, and the state's
    // destructor releases the codecs.
    return state->takeOutput();
}

[[nodiscard]] std::size_t carrierCapacity(std::uint64_t block_count) {
    if (block_count >
        std::numeric_limits<std::uint64_t>::max() / PAYLOAD_FREQUENCIES.size()) {
        fail("Internal Error: Reddit payload capacity overflow.");
    }
    const std::uint64_t capacity =
        (block_count * PAYLOAD_FREQUENCIES.size()) / PAYLOAD_COPIES / 8U;
    if (capacity > std::numeric_limits<std::size_t>::max()) {
        fail("Internal Error: Reddit payload capacity exceeds addressable memory.");
    }
    return static_cast<std::size_t>(capacity);
}

// Decode the carrier from already-read coefficient arrays, leaving the payload
// in locals.payload_bytes. Returns false when no header parses under this key; anything past a parsed header that does not
// check out is corruption and throws. Kept out of extractCarrierPayload so that
// function's setjmp frame holds no per-attempt locals.
[[nodiscard]] bool extractWithKey(
    jpeg_decompress_struct& source,
    jvirt_barray_ptr* arrays,
    const ImageInfo& info,
    const CarrierPrf& prf,
    CodecLocals& locals) {

    locals.carriers = makeCarriers(
        info,
        HEADER_FREQUENCIES,
        RQSTEG_HEADER_BITS,
        HEADER_COPIES,
        prf,
        HEADER_DOMAIN,
        HEADER_LAYOUT_DOMAIN);
    locals.header_bytes = readCarriers(
        source,
        arrays,
        locals.carriers,
        RQSTEG_HEADER_BITS,
        locals.scores);
    carrier_common::WipedVector<Carrier>{}.swap(locals.carriers);
    locals.header = carrier_common::parseCarrierHeader(HEADER_FORMAT, locals.header_bytes);
    if (!locals.header) return false;

    if (locals.header->payload_size > carrierCapacity(info.yBlockCount())) {
        fail("File Extraction Error: Reddit carrier declares a payload larger than its coefficient capacity.");
    }
    // Everything from here on is sized by an attacker-controlled field that
    // only a CRC has vouched for so far -- see extractRedditEnvelope's caller
    // in recover.cpp for why that ordering is unavoidable for this format, and
    // carrierCapacity() for the ceiling that bounds it.
    const std::uint64_t payload_bits =
        static_cast<std::uint64_t>(locals.header->payload_size) * 8U;
    locals.carriers = makeCarriers(
        info,
        PAYLOAD_FREQUENCIES,
        payload_bits,
        PAYLOAD_COPIES,
        prf,
        PAYLOAD_DOMAIN,
        PAYLOAD_LAYOUT_DOMAIN);
    locals.payload_bytes = readCarriers(
        source,
        arrays,
        locals.carriers,
        payload_bits,
        locals.scores);
    carrier_common::WipedVector<Carrier>{}.swap(locals.carriers);
    carrier_common::WipedVector<Score>{}.swap(locals.scores);
    if (crc32(locals.payload_bytes) != locals.header->payload_crc) {
        fail("File Extraction Error: Reddit carrier payload failed its integrity check.");
    }
    return true;
}

[[nodiscard]] std::optional<vBytes> extractCarrierPayload(
    std::span<const Byte> input,
    const CarrierKey128& key) {

    if (input.size() < 2 || input[0] != 0xFF || input[1] != 0xD8) {
        return std::nullopt;
    }
    throwIfSignalCancellationRequested();
    jpeg_codec::requireMemorySource(input);

    auto error = std::make_unique<jpeg_codec::ErrorManager>();
    auto state = std::make_unique<jpeg_codec::CodecState>();
    auto locals = std::make_unique<CodecLocals>();
    // Built before setjmp: a longjmp out of a decode must land inside their
    // lifetime, never skip a destructor (it wipes its key copy).
    const CarrierPrf prf = CarrierPrf::keyed(key);

    jpeg_codec::install(*error);
    if (setjmp(error->jump) != 0) {
        jpeg_codec::throwAfterLongjmp(*error, *state, "JPEG error while extracting Reddit payload: ");
    }

    state->openSource(*error, input);
    jpeg_read_header(&state->source, TRUE);
    const ImageInfo info = imageInfoFromHeader(state->source);
    if (!isCompatibleCarrier(info)) {
        state->cleanup();
        return std::nullopt;
    }

    jvirt_barray_ptr* arrays = jpeg_read_coefficients(&state->source);
    const bool found = extractWithKey(state->source, arrays, info, prf, *locals);

    jpeg_finish_decompress(&state->source);
    state->cleanup();
    if (!found) return std::nullopt;
    return std::move(locals->payload_bytes);
}

} // namespace

RedditPreparedCover prepareRedditCover(std::span<const Byte> input_jpeg) {
    vBytes prepared = transcodeCarrier(input_jpeg);
    const ImageInfo info = inspectJpeg(prepared);
    validateCarrier(info);
    return RedditPreparedCover{
        .jpeg = std::move(prepared),
        .width = info.width,
        .height = info.height,
        .payload_capacity = carrierCapacity(info.yBlockCount()),
    };
}

std::size_t redditEnvelopeSize(std::size_t encrypted_size) {
    return carrier_common::envelopeSize(REDDIT_ENVELOPE_FORMAT, encrypted_size);
}

vBytes makeRedditEnvelope(
    std::span<const Byte> kdf_metadata,
    std::span<const Byte> encrypted_data,
    bool is_compressed) {

    return carrier_common::makeEnvelope(REDDIT_ENVELOPE_FORMAT, kdf_metadata, encrypted_data, is_compressed);
}

vBytes embedRedditPayload(
    const RedditPreparedCover& cover,
    const CarrierKey128& carrier_key,
    std::span<const Byte> payload) {

    if (payload.size() > cover.payload_capacity) {
        fail("Data File Size Error: Encrypted Reddit payload exceeds the cover image's theoretical C3 capacity.");
    }
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        fail("Data File Size Error: Encrypted Reddit payload exceeds its format limit.");
    }

    // The plaintext header (length and CRC), wiped like X-Twitter's.
    vBytes header = carrier_common::makeCarrierHeader(
        HEADER_FORMAT,
        static_cast<std::uint32_t>(payload.size()),
        crc32(payload));
    const WipeBytesGuard header_wipe{header};
    // Outlives embedCarrier's setjmp frame, so no longjmp skips its destructor.
    const CarrierPrf prf = CarrierPrf::keyed(carrier_key);
    return embedCarrier(cover.jpeg, header, payload, prf);
}

RedditPreparedCover measureRedditCover(std::span<const Byte> input_jpeg) {
    throwIfSignalCancellationRequested();
    jpeg_codec::requireMemorySource(input_jpeg);
    auto error = std::make_unique<jpeg_codec::ErrorManager>();
    auto state = std::make_unique<jpeg_codec::CodecState>();
    // Keep the owner alive across longjmp; only the heap vector changes below.
    const auto row = std::make_unique<std::vector<Byte>>();

    jpeg_codec::install(*error);
    jpeg_codec::rejectWarnings(*error);
    if (setjmp(error->jump) != 0) {
        jpeg_codec::throwAfterLongjmp(*error, *state, "JPEG error while preparing Reddit cover: ");
    }

    state->openSource(*error, input_jpeg);
    jpeg_read_header(&state->source, TRUE);
    validateCoverInput(state->source);
    const std::uint32_t width = static_cast<std::uint32_t>(state->source.image_width);
    const std::uint32_t height = static_cast<std::uint32_t>(state->source.image_height);
    // Every scan is still entropy-decoded, which is where a truncated or
    // corrupt cover shows. What follows it in the transcode cannot fail, so
    // it is cut to the minimum: luminance only, at 1/8 scale, where the
    // inverse DCT is one value per block, into a single row that is thrown
    // away. A baseline cover streams through in constant memory.
    state->source.out_color_space = JCS_GRAYSCALE;
    state->source.scale_num = 1;
    state->source.scale_denom = 8;
    jpeg_start_decompress(&state->source);
    jpeg_codec::scanRows(state->source, *row);
    jpeg_finish_decompress(&state->source);
    state->cleanup();
    throwIfSignalCancellationRequested();

    // The carrier is the cover at the same size in 4:2:0, where luminance has
    // one block per 8x8 pixels, rounded up (validateCoverInput checked the
    // header fits in them).
    const std::uint64_t luminance_blocks =
        ((static_cast<std::uint64_t>(width) + 7U) / 8U) * ((static_cast<std::uint64_t>(height) + 7U) / 8U);
    return RedditPreparedCover{
        .jpeg = {},
        .width = width,
        .height = height,
        .payload_capacity = carrierCapacity(luminance_blocks),
    };
}

std::optional<RedditEncryptedEnvelope> extractRedditEnvelope(
    std::span<const Byte> input_jpeg,
    const CarrierKey128& carrier_key) {

    std::optional<vBytes> carrier_payload = extractCarrierPayload(input_jpeg, carrier_key);
    if (!carrier_payload) return std::nullopt;
    // Its parts are copied into the envelope; this copy goes now.
    const carrier_common::WipeOnExit carrier_payload_wipe{*carrier_payload};
    return carrier_common::parseEnvelope(REDDIT_ENVELOPE_FORMAT, *carrier_payload);
}
